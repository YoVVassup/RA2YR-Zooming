#include "RenderZoom.hpp"
#include "Zoomer.hpp"
#include "ScalerConflict.hpp"
#include "TextDefer.hpp"
#include "GameAddrs.hpp"
#include "Log.h"

#include <cstring>
#include <vector>

namespace
{
	// Surface vtable slots (YRpp Surface.h).
	constexpr int VT_LOCK = 23;
	constexpr int VT_UNLOCK = 24;
	constexpr int VT_GET_BYTES_PER_PIXEL = 28;
	constexpr int VT_GET_PITCH = 29;
	constexpr int VT_GET_WIDTH = 31;
	constexpr int VT_GET_HEIGHT = 32;

	// The vanilla view rect is observed at most four times a second, on
	// every frame whose view dimensions are not substituted (see PreRender).
	constexpr DWORD VIEW_RECT_POLL_MS = 250;

	// How often the scaler-fingerprint hook sites are re-checked for a
	// foreign runtime patch (MOExperience re-patches when ScaleBar toggles).
	constexpr DWORD PATCH_SCAN_MS = 1000;

	constexpr float ZOOM_EPSILON = 1.001f;

	using SurfaceIntFn = int(__thiscall*)(void*);
	using SurfaceLockFn = void*(__thiscall*)(void*, int, int);
	using SurfaceUnlockFn = bool(__thiscall*)(void*);
	using CalcViewportCellsFn = void(__thiscall*)(void* self);

	bool g_enabled = false;

	// Scratch buffer for the stretch: it holds the source rect the engine
	// rendered, so it never grows past the full view and shrinks with the
	// zoom factor in practice.
	std::vector<unsigned char> g_buffer;

	// True while DSurface::ViewBounds.W/H hold the substituted source rect
	// sizes, i.e. between a successful PreRender and the PostRender that
	// restores them. The vanilla pair is saved next to it.
	bool g_substituted = false;
	int g_savedViewW = 0;
	int g_savedViewH = 0;

	// True when the previous frame was magnified: its stretch is still on
	// screen, so the first vanilla frame repaints everything.
	bool g_magnifiedLastFrame = false;

	DWORD g_lastPatchScanMs = 0;

	RECT g_viewRect = {};
	bool g_viewRectValid = false;
	DWORD g_lastViewPoll = 0;

	// Entry counters of the two ClampCoordMap hooks (Main.cpp). A rejected
	// camera shift logs them: zero means the syhks declaration never made
	// it into this process and the game clamped against the full view.
	unsigned long g_clampHookCallsW = 0;
	unsigned long g_clampHookCallsH = 0;

	// Indirection over the hard coded game globals: the defaults are the
	// production addresses, SetTestGameRegion (VIEWCTRL_TEST) redirects them
	// into a region the test owns so IsGameReadable accepts it.
	HMODULE g_gameModuleOverride = nullptr;
	const void* g_compositeSlot = reinterpret_cast<const void*>(GameAddr::DSurface_Composite);
	const void* g_viewBounds = reinterpret_cast<const void*>(GameAddr::DSurface_ViewBounds);
	const void* g_windowBounds = reinterpret_cast<const void*>(GameAddr::DSurface_WindowBounds);

	// TacticalClass's own view_bound cache (RectangleStruct at
	// GameAddr::TacticalClass_viewBound). Set_View_Dimensions writes it
	// together with DSurface::ViewBounds; CalcViewportCells builds the
	// viewport origin from its W/H, so a substitution that touches only the
	// DSurface pair leaves the engine rendering the small rect at the
	// full-view origin (map cut off at the bottom and right). These three
	// follow the viewBounds indirection so tests can redirect them.
	const void* g_viewBound = reinterpret_cast<const void*>(GameAddr::TacticalClass_viewBound);
	const void* g_tacticalInstanceSlot = reinterpret_cast<const void*>(GameAddr::TacticalClass_Instance);
	CalcViewportCellsFn g_calcViewportCells =
		reinterpret_cast<CalcViewportCellsFn>(GameAddr::TacticalClass_CalcViewportCells);

	// ClampCoordMap's five map cell-extent inputs ([0x87F8DC, 0x87F8E4,
	// 0x87F8E8, 0x87F8EC, 0x87F8F0]): LogCameraClampState reads them for
	// the diagnostic. Unreadable outside gamemd.exe (logged as zeros then);
	// SetMapBoundsForTest (VIEWCTRL_TEST) points them at readable memory so
	// the read path runs under unit tests. Null restores the defaults.
	constexpr uintptr_t kMapAddrDefaults[5] = {
		0x87F8DCu, 0x87F8E4u, 0x87F8E8u, 0x87F8ECu, 0x87F8F0u
	};
	uintptr_t g_mapAddrs[5] = {
		kMapAddrDefaults[0], kMapAddrDefaults[1], kMapAddrDefaults[2],
		kMapAddrDefaults[3], kMapAddrDefaults[4]
	};

	bool IsReadable(const void* address, size_t bytes)
	{
		if (!address || bytes == 0) return false;

		MEMORY_BASIC_INFORMATION mbi = {};
		if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
		if (mbi.State != MEM_COMMIT) return false;
		if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;

		const auto* start = static_cast<const BYTE*>(address);
		const auto* regionEnd = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
		return start + bytes <= regionEnd;
	}

	void* CompositeSurface()
	{
		if (!RenderZoom::IsGameReadable(g_compositeSlot, sizeof(void*)))
			return nullptr;
		return *reinterpret_cast<void* const*>(g_compositeSlot);
	}

	void** VTable(void* object)
	{
		if (!object || !IsReadable(object, sizeof(void*))) return nullptr;

		void** vtable = *reinterpret_cast<void***>(object);
		if (!IsReadable(vtable, sizeof(void*) * (VT_GET_HEIGHT + 1))) return nullptr;
		return vtable;
	}

	bool ReadGameViewRect(RECT& out, int* windowW, int* windowH)
	{
		if (!RenderZoom::IsGameReadable(g_viewBounds, 4 * sizeof(int))) return false;
		if (!RenderZoom::IsGameReadable(g_windowBounds, 4 * sizeof(int))) return false;

		// Both globals are RectangleStruct {X, Y, W, H}. Read them as ints and
		// convert explicitly to {left=X, top=Y, right=X+W, bottom=Y+H} below: a
		// memcpy straight into a RECT would take W/H for right/bottom and only
		// looks correct while X=Y=0.
		int view[4] = {};
		int window[4] = {};
		memcpy(view, g_viewBounds, sizeof(view));
		memcpy(window, g_windowBounds, sizeof(window));

		if (windowW) *windowW = window[2];
		if (windowH) *windowH = window[3];

		// Only a view rect that is smaller than the window rect describes the
		// tactical view — the sidebar is what makes it smaller. Otherwise the
		// surface has no separate view area and nothing is magnified.
		if (!(view[2] < window[2] || view[3] < window[3])) return false;

		out.left = view[0];
		out.top = view[1];
		out.right = view[0] + view[2];
		out.bottom = view[1] + view[3];
		return out.left < out.right && out.top < out.bottom;
	}

	// The substituted slots: DSurface::ViewBounds.W / .H at +8 / +12.
	bool ReadViewDims(int& w, int& h)
	{
		if (!RenderZoom::IsGameReadable(g_viewBounds, 4 * sizeof(int))) return false;
		const int* p = reinterpret_cast<const int*>(g_viewBounds);
		w = p[2];
		h = p[3];
		return true;
	}

	bool WriteViewDims(int w, int h)
	{
		if (!RenderZoom::IsGameReadable(g_viewBounds, 4 * sizeof(int))) return false;

		// IsGameReadable only rejects guard / no-access pages; a read-only
		// view of the game region would pass it and fault on the store.
		MEMORY_BASIC_INFORMATION mbi = {};
		if (VirtualQuery(g_viewBounds, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
		const DWORD prot = mbi.Protect & 0xFF;
		if (prot != PAGE_READWRITE && prot != PAGE_EXECUTE_READWRITE) return false;

		int* p = reinterpret_cast<int*>(const_cast<void*>(g_viewBounds));
		p[2] = w;
		p[3] = h;
		return true;
	}

	// The W/H half of TacticalClass::view_bound (GameAddr 0xB0CE28 + 8/12).
	// Written together with WriteViewDims by the game's own Set_View_Dimensions
	// path (0x4A8960 -> 0x6D5F60); the plugin has to keep the pair in lockstep
	// or CalcViewportCells keeps the full-view viewport origin.
	bool WriteViewBoundCache(int w, int h)
	{
		if (!RenderZoom::IsGameReadable(g_viewBound, 4 * sizeof(int))) return false;

		MEMORY_BASIC_INFORMATION mbi = {};
		if (VirtualQuery(g_viewBound, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
		const DWORD prot = mbi.Protect & 0xFF;
		if (prot != PAGE_READWRITE && prot != PAGE_EXECUTE_READWRITE) return false;

		int* p = reinterpret_cast<int*>(const_cast<void*>(g_viewBound));
		p[2] = w;
		p[3] = h;
		return true;
	}

	// TacticalClass::CalcViewportCells (GameAddr 0x6D8B30): TacticalPos
	// (this+0xB0/0xB4) = view center (this+0xD64/0xD68) - view_bound.W/H / 2.
	// The engine runs it whenever the camera or the view dimensions change
	// (SetCameraPosition, Set_View_Dimensions); substituting the dims behind
	// its back needs the same recompute or the small render stays anchored at
	// the full-view origin. Optional out params report the resulting origin.
	bool RecalcViewport(int* outPosX, int* outPosY)
	{
		if (outPosX) *outPosX = 0;
		if (outPosY) *outPosY = 0;
		if (!g_calcViewportCells) return false;
		if (!RenderZoom::IsGameReadable(g_tacticalInstanceSlot, sizeof(void*)))
			return false;

		void* instance = *reinterpret_cast<void* const*>(
			const_cast<void*>(g_tacticalInstanceSlot));
		// The instance is HEAP allocated (operator new of 0xE18 bytes at
		// 0x687604, stored into the slot), so IsGameReadable's
		// AllocationBase == gamemd.exe test rejects it by design — the same
		// reason GameCamera dereferences TacticalClass::Instance with only a
		// null check. The plain committed-page test is the right guard here;
		// with the strict one this whole function silently no-oped and the
		// full-view origin bug survived the first fix attempt.
		// Size through this+0xD8C: the tail fields CalcViewportCells derives
		// from the cache (scroll extents) sit right after the camera center.
		if (!instance || !IsReadable(instance, 0xD90)) return false;

		g_calcViewportCells(instance);
		if (outPosX)
			*outPosX = *reinterpret_cast<const int*>(
				static_cast<const BYTE*>(instance) + 0xB0);
		if (outPosY)
			*outPosY = *reinterpret_cast<const int*>(
				static_cast<const BYTE*>(instance) + 0xB4);
		return true;
	}

	bool RecalcViewport()
	{
		return RecalcViewport(nullptr, nullptr);
	}

	bool LockView(const RECT& view, void*& pixels, int& pitch)
	{
		void* surface = CompositeSurface();
		void** vtable = VTable(surface);
		if (!vtable) return false;

		const auto getBytesPerPixel = reinterpret_cast<SurfaceIntFn>(vtable[VT_GET_BYTES_PER_PIXEL]);
		const auto getPitch = reinterpret_cast<SurfaceIntFn>(vtable[VT_GET_PITCH]);
		const auto getWidth = reinterpret_cast<SurfaceIntFn>(vtable[VT_GET_WIDTH]);
		const auto getHeight = reinterpret_cast<SurfaceIntFn>(vtable[VT_GET_HEIGHT]);
		const auto lock = reinterpret_cast<SurfaceLockFn>(vtable[VT_LOCK]);
		if (!getBytesPerPixel || !getPitch || !getWidth || !getHeight || !lock) return false;

		// Only 16 bit surfaces are stretched here.
		if (getBytesPerPixel(surface) != 2) return false;
		if (view.left < 0 || view.top < 0) return false;
		if (view.right > getWidth(surface) || view.bottom > getHeight(surface)) return false;

		pitch = getPitch(surface);
		if (pitch <= 0 || (pitch & 1) != 0) return false;

		pixels = lock(surface, 0, 0);
		return pixels != nullptr;
	}

	void UnlockSurface()
	{
		void* surface = CompositeSurface();
		void** vtable = VTable(surface);
		if (!vtable) return;

		const auto unlock = reinterpret_cast<SurfaceUnlockFn>(vtable[VT_UNLOCK]);
		if (unlock) unlock(surface);
	}

	// Puts the vanilla view dimensions back. Only meaningful while a
	// substitution is active; silently succeeds when the globals are gone.
	// The TacticalClass view_bound cache goes back with them and the
	// viewport origin is recomputed, so the between-frame coordinate paths
	// (clicks, HUD, edge scroll) see one consistent vanilla pair again.
	void RestoreViewDims()
	{
		if (WriteViewDims(g_savedViewW, g_savedViewH))
		{
			WriteViewBoundCache(g_savedViewW, g_savedViewH);
			RecalcViewport();
			g_substituted = false;
		}
	}

	// Everything the replaced "mov ebx, [0x886FA8]" / "[0x886FAC]" would have
	// loaded. ClampCoordMap uses this value to clamp the camera against the
	// map. While magnified the displayed window is the smaller source rect
	// centered at the camera, so the bound has to use THAT size: a full view
	// bound would reject the focus shifts a zoom-in asks for near the map
	// edge (the focus point would drift off the cursor). The bound tightens
	// back to the full view as the zoom returns to 1 — every camera write
	// goes through ClampCoordMap with the then-current zoom, and the zoom
	// decrease path in ApplyCameraStep re-runs the position through the
	// clamp even when the focus itself does not move.
	//
	// The source size is derived straight from the dimension read above:
	// while a frame substitution is active the global already holds the
	// source rect size (PreRender wrote it), otherwise it holds the full
	// view and the zoom factor scales it down. The polled view rect is
	// deliberately not consulted — it can still be empty on the first zoom
	// of a session (the game may sit at a screen where view == window and
	// the poll rejects the rect, then the user zooms before the next native
	// frame), and waiting for it would clamp against the full view: the
	// invisible wall at the map corner.
	int ClampViewDimension(bool width)
	{
		static int lastWidth = 640;
		static int lastHeight = 400;

		const auto* address = reinterpret_cast<const BYTE*>(g_viewBounds)
			+ (width ? 2 : 3) * sizeof(int);
		if (!RenderZoom::IsGameReadable(address, sizeof(int)))
			return width ? lastWidth : lastHeight;
		const int gameDim = *reinterpret_cast<const int*>(address);
		(width ? lastWidth : lastHeight) = gameDim;

		const float zoom = Zoomer::CurrentZoom();
		if (zoom <= ZOOM_EPSILON) return gameDim;

		// PreRender substituted this frame: the global already holds the
		// source rect size the bound needs.
		if (g_substituted) return gameDim;

		// Full view still in place: the window actually displayed at this
		// zoom is the view scaled by 1/zoom (rounded half up, matching
		// ComputeSourceRect; a +-1 px difference does not move the bound).
		const int srcDim = (int)floorf((float)gameDim / zoom + 0.5f);
		return RenderZoom::ClampDimension(gameDim, srcDim, true);
	}

	// Shuts the plugin down from inside the game: unsubstitutes the view
	// dimensions, disarms the hooks and drops the window subclass.
	void Disable(const char* reason)
	{
		LOG("RenderZoom: disabled (%s)", reason);
		if (g_substituted)
			RestoreViewDims();
		g_enabled = false;
		Zoomer::Shutdown();
		RenderZoom::ResetFrameState();
	}
}

// The hard coded game globals may only be dereferenced inside gamemd.exe.
// Outside of the game (unit tests, a different host exe) they can point at
// unrelated memory, so they are rejected there before anything is read.
bool RenderZoom::IsGameReadable(const void* address, size_t bytes)
{
	const HMODULE game = g_gameModuleOverride
		? g_gameModuleOverride
		: GetModuleHandleA("gamemd.exe");
	if (!game) return false;
	if (!IsReadable(address, bytes)) return false;

	MEMORY_BASIC_INFORMATION mbi = {};
	if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
	return mbi.AllocationBase == game;
}

void RenderZoom::Init()
{
	ResetFrameState();

	char path[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	_strlwr(path);
	const bool isGame = (strstr(path, "gamemd.exe") != nullptr);

	g_enabled = isGame;
	LOG("RenderZoom: enabled=%d (gamemd=%d)", g_enabled, isGame);
}

void RenderZoom::Shutdown()
{
	ResetFrameState();
	g_enabled = false;
}

#ifdef VIEWCTRL_TEST
void RenderZoom::SetEnabled(bool enabled)
{
	g_enabled = enabled;
}

bool RenderZoom::IsEnabled()
{
	return g_enabled;
}

void RenderZoom::SetTestGameRegion(void* moduleBase, const void* compositeSlot, const void* viewBounds, const void* windowBounds)
{
	if (moduleBase)
	{
		g_gameModuleOverride = static_cast<HMODULE>(moduleBase);
		if (compositeSlot) g_compositeSlot = compositeSlot;
		if (viewBounds) g_viewBounds = viewBounds;
		if (windowBounds) g_windowBounds = windowBounds;
		return;
	}

	g_gameModuleOverride = nullptr;
	g_compositeSlot = reinterpret_cast<const void*>(GameAddr::DSurface_Composite);
	g_viewBounds = reinterpret_cast<const void*>(GameAddr::DSurface_ViewBounds);
	g_windowBounds = reinterpret_cast<const void*>(GameAddr::DSurface_WindowBounds);
}

void RenderZoom::SetViewBoundForTest(const void* viewBound,
	void* tacticalInstanceSlot, void* calcViewportCells)
{
	g_viewBound = viewBound
		? viewBound
		: reinterpret_cast<const void*>(GameAddr::TacticalClass_viewBound);
	g_tacticalInstanceSlot = tacticalInstanceSlot
		? tacticalInstanceSlot
		: reinterpret_cast<const void*>(GameAddr::TacticalClass_Instance);
	g_calcViewportCells = calcViewportCells
		? reinterpret_cast<CalcViewportCellsFn>(calcViewportCells)
		: reinterpret_cast<CalcViewportCellsFn>(GameAddr::TacticalClass_CalcViewportCells);
}

void RenderZoom::SetMapBoundsForTest(const void* addrs[5])
{
	if (!addrs)
	{
		for (int i = 0; i < 5; ++i)
			g_mapAddrs[i] = kMapAddrDefaults[i];
		return;
	}
	for (int i = 0; i < 5; ++i)
		g_mapAddrs[i] = reinterpret_cast<uintptr_t>(addrs[i]);
}

const void* RenderZoom::TestWindowBoundsAddress()
{
	return g_windowBounds;
}

void RenderZoom::SetLastPatchScanForTest(DWORD ms)
{
	g_lastPatchScanMs = ms;
}
#endif

void RenderZoom::ResetFrameState()
{
	g_buffer.clear();
	g_substituted = false;
	g_savedViewW = 0;
	g_savedViewH = 0;
	g_magnifiedLastFrame = false;
	g_lastPatchScanMs = 0;
	g_viewRect = {};
	g_viewRectValid = false;
	g_lastViewPoll = 0;
	TextDefer::Reset();
}

bool RenderZoom::PreRender()
{
	if (!g_enabled) return false;

	// A frame never inherits text records: a PostRender that did not run
	// must not feed a stale region into this frame's capture.
	TextDefer::Reset();

	// Advance the zoom lerp and the game camera before the frame is drawn,
	// so the substituted view below matches the camera move of this very
	// frame.
	Zoomer::TickFrame();

	const DWORD now = GetTickCount();

	// L3: a foreign module that patched one of our render sites after
	// Syringe installed our hooks (MOExperience re-patches at runtime).
	if (now - g_lastPatchScanMs >= PATCH_SCAN_MS)
	{
		g_lastPatchScanMs = now;
		if (ScalerConflict::RuntimePatchPresent())
		{
			Disable("a foreign module patched a hook site at runtime");
			return false;
		}
	}

	const float zoom = Zoomer::CurrentZoom();
	const bool magnified = zoom > ZOOM_EPSILON;

	bool forceRedraw = false;

	// Defensive: PostRender unsubstitutes at the end of every magnified
	// frame; clean up the rare frame where that did not happen.
	if (!magnified && g_substituted)
	{
		RestoreViewDims();
		forceRedraw = true;
	}

	// Poll the vanilla view rect and feed the reactive watch. Runs whenever
	// the view dimensions are currently vanilla — which includes the frames
	// of a magnified session, because PostRender restored the pair at the
	// end of the previous frame — and only skips a frame whose dimensions
	// are still substituted (a PostRender that did not run), so the watch
	// never sees our own scaling. Polling while magnified matters: the
	// first zoom of a session can start before any native frame observed
	// the rect (the game sits at a screen where view == window and the
	// poll rejects it), and substituting against the raw client rect
	// renders the wrong geometry while ClampCoordMap keeps the full-view
	// bound — the invisible wall at the map corner.
	if (!g_substituted)
	{
		RECT candidate = {};
		int windowW = 0;
		int windowH = 0;
		if (ReadGameViewRect(candidate, &windowW, &windowH))
		{
			ScalerConflict::ObserveViewDims(windowW, windowH,
				candidate.right - candidate.left, candidate.bottom - candidate.top,
				now);
			if (ScalerConflict::ReactiveConflict())
			{
				Disable("a world-space scaler is rescaling the view");
				return false;
			}

			if (AcceptViewRect(candidate, now))
			{
				LOG("RenderZoom: view rect (%ld,%ld)-(%ld,%ld)",
					candidate.left, candidate.top, candidate.right, candidate.bottom);
				Zoomer::SetViewRect(candidate, true);
			}
		}
	}

	if (magnified)
	{
		const RECT view = Zoomer::ViewRect();
		const int w = view.right - view.left;
		const int h = view.bottom - view.top;
		if (w > 0 && h > 0)
		{
			const POINT anchor = Zoomer::ZoomAnchor();
			const SourceRect src = ComputeSourceRect(
				w, h, anchor.x - view.left, anchor.y - view.top, zoom);

			// Substitute only for a real magnification; a degenerate source
			// rect falls back to the whole view and renders vanilla.
			if (src.W > 0 && src.H > 0 && src.W < w && src.H < h)
			{
				if (!g_substituted)
				{
					int savedW = 0;
					int savedH = 0;
					if (!ReadViewDims(savedW, savedH))
					{
						// No game data to substitute; render vanilla.
						if (g_magnifiedLastFrame) forceRedraw = true;
						g_magnifiedLastFrame = false;
						return forceRedraw;
					}
					g_savedViewW = savedW;
					g_savedViewH = savedH;
					if (!WriteViewDims(src.W, src.H))
					{
						if (g_magnifiedLastFrame) forceRedraw = true;
						g_magnifiedLastFrame = false;
						return forceRedraw;
					}
					g_substituted = true;
					// Set_View_Dimensions keeps TacticalClass::view_bound in
					// lockstep with the DSurface pair and re-runs
					// CalcViewportCells; do the same or the engine renders the
					// small rect at the full-view origin (cut off bottom /
					// right). The camera was already clamped to the source
					// size during TickFrame, so only the origin needs the
					// recompute here.
					WriteViewBoundCache(src.W, src.H);
					int posX = 0, posY = 0;
					const bool recalced = RecalcViewport(&posX, &posY);
					// The origin doubles as the runtime proof the recalc
					// really ran (recalc=false means the instance guard
					// rejected the heap TacticalClass and the origin stayed
					// full-view based).
					LOG("RenderZoom: view %dx%d -> source %dx%d -> origin (%d,%d)%s",
						savedW, savedH, src.W, src.H, posX, posY,
						recalced ? "" : " [recalc SKIPPED]");
				}
				// The engine renders the small view; ask it to repaint all
				// of it so no stale full view pixels leak in. The text
				// funnels arm for the draw pass that follows: anything
				// anchored inside the region is captured and replayed
				// after the stretch (TextDefer).
				TextDefer::BeginFrame(view, src.W, src.H);
				forceRedraw = true;
			}
		}
	}
	else
	{
		// The stretch of the previous magnified frames is still on screen:
		// one full vanilla repaint clears it.
		if (g_magnifiedLastFrame) forceRedraw = true;
	}

	g_magnifiedLastFrame = magnified && g_substituted;
	return forceRedraw;
}

void RenderZoom::PostRender()
{
	if (!g_enabled) return;

	const float zoom = Zoomer::CurrentZoom();
	const bool magnified = zoom > ZOOM_EPSILON;

	if (magnified && g_substituted)
	{
		const RECT view = Zoomer::ViewRect();
		const int fullW = view.right - view.left;
		const int fullH = view.bottom - view.top;

		const POINT anchor = Zoomer::ZoomAnchor();
		const SourceRect src = ComputeSourceRect(
			fullW, fullH, anchor.x - view.left, anchor.y - view.top, zoom);

		if (fullW > 0 && fullH > 0 && src.W > 0 && src.H > 0
			&& src.W < fullW && src.H < fullH)
		{
			// The engine rendered only the source rect, at the view origin.
			const RECT drawn = {
				view.left, view.top, view.left + src.W, view.top + src.H
			};

			void* pixels = nullptr;
			int pitch = 0;
			if (LockView(view, pixels, pitch))
			{
				const size_t bytes = (size_t)src.W * src.H * 2;
				if (g_buffer.size() < bytes) g_buffer.resize(bytes);

				if (CopyRows(static_cast<unsigned char*>(pixels), pitch, drawn,
					g_buffer.data(), src.W, src.H, true))
				{
					auto* dst = reinterpret_cast<unsigned short*>(
						static_cast<unsigned char*>(pixels)
						+ (size_t)view.top * pitch + (size_t)view.left * 2);
					Upscale(reinterpret_cast<const unsigned short*>(g_buffer.data()), src.W,
						dst, pitch / 2, fullW, fullH,
						SourceRect{ 0, 0, src.W, src.H });
				}
				UnlockSurface();
			}
		}

		// The stretch sits on the surface. The vanilla view dimensions go
		// back FIRST: the deferred HUD draws below re-invoke originals
		// that recompute their layout from those dimensions — with the
		// substituted pair still in place they would re-derive the small
		// source rect and come out magnified again.
		if (g_substituted)
			RestoreViewDims();

		// Re-invoke the deferred DrawTimer / DrawScreenText / DrawUI
		// calls: they land at native size on top of the stretch — before
		// the cursor blit and Present.
		TextDefer::ReplayDeferred();
	}

	// Safety net for a substituted frame that took a different path: the
	// coordinate paths must see the vanilla view dimensions again between
	// frames, and no record may outlive its frame.
	if (g_substituted)
		RestoreViewDims();
	TextDefer::Reset();
}

RenderZoom::SourceRect RenderZoom::ComputeSourceRect(
	int viewW, int viewH, int anchorX, int anchorY, float zoom)
{
	const SourceRect full = { 0, 0, viewW, viewH };
	if (viewW <= 0 || viewH <= 0) return { 0, 0, 0, 0 };
	if (zoom <= ZOOM_EPSILON) return full;

	const float srcW = (float)viewW / zoom;
	const float srcH = (float)viewH / zoom;

	// Rounding half up, like the game side helper this was ported from.
	SourceRect src;
	src.X = (int)floorf((float)anchorX - srcW * 0.5f + 0.5f);
	const int x1 = (int)floorf((float)anchorX + srcW * 0.5f + 0.5f);
	src.W = x1 - src.X;
	src.Y = (int)floorf((float)anchorY - srcH * 0.5f + 0.5f);
	const int y1 = (int)floorf((float)anchorY + srcH * 0.5f + 0.5f);
	src.H = y1 - src.Y;

	if (src.X < 0) src.X = 0;
	if (src.Y < 0) src.Y = 0;
	if (src.W < 1 || src.H < 1) return full;
	if (src.X + src.W > viewW) src.W = viewW - src.X;
	if (src.Y + src.H > viewH) src.H = viewH - src.Y;
	if (src.W < 1 || src.H < 1) return full;
	return src;
}

void RenderZoom::Upscale(
	const unsigned short* backup, int backupW,
	unsigned short* dst, int dstPitch,
	int dstW, int dstH, const SourceRect& src)
{
	if (!backup || !dst) return;
	if (backupW <= 0 || dstPitch <= 0 || dstW <= 0 || dstH <= 0) return;
	if (src.W <= 0 || src.H <= 0) return;

	// The source rect never leaves the backup, so clamping to it keeps every
	// read inside the buffer.
	const int lastX = src.X + src.W - 1;
	const int lastY = src.Y + src.H - 1;

	for (int y = 0; y < dstH; ++y)
	{
		int sy = src.Y + (int)(((long long)y * src.H) / dstH);
		if (sy < 0) sy = 0;
		else if (sy > lastY) sy = lastY;

		const unsigned short* srcRow = backup + (size_t)sy * backupW;
		unsigned short* dstRow = dst + (size_t)y * dstPitch;

		for (int x = 0; x < dstW; ++x)
		{
			int sx = src.X + (int)(((long long)x * src.W) / dstW);
			if (sx < 0) sx = 0;
			else if (sx > lastX) sx = lastX;
			dstRow[x] = srcRow[sx];
		}
	}
}

bool RenderZoom::CopyRows(
	unsigned char* pixels, int pitch, const RECT& view,
	unsigned char* buffer, int bufferW, int bufferH, bool toBuffer)
{
	if (!pixels || !buffer) return false;
	if (view.left < 0 || view.top < 0) return false;

	const int w = view.right - view.left;
	const int h = view.bottom - view.top;
	if (w <= 0 || h <= 0) return false;
	if (w > bufferW || h > bufferH) return false;
	// Every row starts at view.left, so the pitch has to hold the whole
	// right edge and not only the width of the copy.
	if (pitch < view.right * 2) return false;

	const size_t rowBytes = (size_t)w * 2;
	const size_t bufferStride = (size_t)bufferW * 2;

	for (int y = 0; y < h; ++y)
	{
		unsigned char* surfaceRow =
			pixels + (size_t)(view.top + y) * pitch + (size_t)view.left * 2;
		unsigned char* bufferRow = buffer + (size_t)y * bufferStride;

		if (toBuffer) memcpy(bufferRow, surfaceRow, rowBytes);
		else memcpy(surfaceRow, bufferRow, rowBytes);
	}
	return true;
}

bool RenderZoom::AcceptViewRect(const RECT& candidate, DWORD nowMs)
{
	if (candidate.left >= candidate.right || candidate.top >= candidate.bottom)
		return false;

	// Throttled: the game rect is only observed every VIEW_RECT_POLL_MS.
	if (g_viewRectValid && (nowMs - g_lastViewPoll) < VIEW_RECT_POLL_MS)
		return false;

	g_lastViewPoll = nowMs;
	if (g_viewRectValid && memcmp(&candidate, &g_viewRect, sizeof(RECT)) == 0)
		return false;

	g_viewRect = candidate;
	g_viewRectValid = true;
	return true;
}

bool RenderZoom::CachedViewRect(RECT& out)
{
	if (!g_viewRectValid) return false;
	out = g_viewRect;
	return true;
}

int RenderZoom::ClampWidth()
{
	++g_clampHookCallsW;
	return ClampViewDimension(true);
}

int RenderZoom::ClampHeight()
{
	++g_clampHookCallsH;
	return ClampViewDimension(false);
}

void RenderZoom::LogCameraClampState()
{
	// Captured before the Clamp* calls below bump them once more: a zero
	// here means the syhks hook never ran in this process.
	const unsigned long callsW = g_clampHookCallsW;
	const unsigned long callsH = g_clampHookCallsH;

	const int clampW = ClampWidth();
	const int clampH = ClampHeight();
	const float zoom = Zoomer::CurrentZoom();

	RECT view = {};
	const bool haveView = CachedViewRect(view);
	int srcW = 0;
	int srcH = 0;
	if (haveView)
	{
		const int w = view.right - view.left;
		const int h = view.bottom - view.top;
		const POINT anchor = Zoomer::ZoomAnchor();
		const SourceRect src = ComputeSourceRect(
			w, h, anchor.x - view.left, anchor.y - view.top, zoom);
		srcW = src.W;
		srcH = src.H;
	}

	int map[5] = {};
	for (int i = 0; i < 5; ++i)
	{
		const auto* p = reinterpret_cast<const void*>(g_mapAddrs[i]);
		if (IsGameReadable(p, sizeof(int)))
			map[i] = *reinterpret_cast<const int*>(p);
	}

	LOG("clamp state: zoom=%.3f hooks=%lu/%lu clampDim=(%d,%d) src=(%dx%d) viewCached=%d map=(%d,%d,%d,%d,%d)",
		zoom, callsW, callsH, clampW, clampH, srcW, srcH,
		haveView ? 1 : 0, map[0], map[1], map[2], map[3], map[4]);
}

int RenderZoom::SidebarViewWidth()
{
	// While the view is substituted the game global holds the smaller source
	// rect; the sidebar blit must still position against the full view width.
	if (g_substituted) return g_savedViewW;

	int w = 0;
	int h = 0;
	if (!ReadViewDims(w, h)) return 0;
	return w;
}

int RenderZoom::ClampDimension(int gameDim, int srcDim, bool magnified)
{
	if (magnified && srcDim > 0) return srcDim;
	return gameDim;
}
