#include "RenderZoom.hpp"
#include "Zoomer.hpp"
#include "ScalerConflict.hpp"
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

	// The view rect is only looked at while the zoom factor sits at 1.0, and
	// at most four times a second.
	constexpr DWORD VIEW_RECT_POLL_MS = 250;

	// How often the eight scaler-fingerprint hook sites are re-checked for a
	// foreign runtime patch (MOExperience re-patches when ScaleBar toggles).
	constexpr DWORD PATCH_SCAN_MS = 1000;

	constexpr float ZOOM_EPSILON = 1.001f;

	using SurfaceIntFn = int(__thiscall*)(void*);
	using SurfaceLockFn = void*(__thiscall*)(void*, int, int);
	using SurfaceUnlockFn = bool(__thiscall*)(void*);

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

	// Indirection over the hard coded game globals: the defaults are the
	// production addresses, SetTestGameRegion (VIEWCTRL_TEST) redirects them
	// into a region the test owns so IsGameReadable accepts it.
	HMODULE g_gameModuleOverride = nullptr;
	const void* g_compositeSlot = reinterpret_cast<const void*>(GameAddr::DSurface_Composite);
	const void* g_viewBounds = reinterpret_cast<const void*>(GameAddr::DSurface_ViewBounds);
	const void* g_windowBounds = reinterpret_cast<const void*>(GameAddr::DSurface_WindowBounds);

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
	void RestoreViewDims()
	{
		if (WriteViewDims(g_savedViewW, g_savedViewH))
			g_substituted = false;
	}

	// Everything the replaced "mov ebx, [0x886FA8]" / "[0x886FAC]" would have
	// loaded, swapped for the visible source rect while the view is magnified.
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

		// Substitute only once a view rect has been observed; before that
		// the game dimension stands.
		RECT view = {};
		if (!RenderZoom::CachedViewRect(view)) return gameDim;

		const int viewW = view.right - view.left;
		const int viewH = view.bottom - view.top;
		if (viewW <= 0 || viewH <= 0) return gameDim;

		const POINT anchor = Zoomer::ZoomAnchor();
		const RenderZoom::SourceRect src = RenderZoom::ComputeSourceRect(
			viewW, viewH, anchor.x - view.left, anchor.y - view.top, zoom);
		return RenderZoom::ClampDimension(gameDim, width ? src.W : src.H, true);
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
}

bool RenderZoom::PreRender()
{
	if (!g_enabled) return false;

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
					LOG("RenderZoom: view %dx%d -> source %dx%d",
						savedW, savedH, src.W, src.H);
				}
				// The engine renders the small view; ask it to repaint all
				// of it so no stale full view pixels leak in.
				forceRedraw = true;
			}
		}
	}
	else
	{
		// Poll the vanilla view rect and feed the reactive watch. Only runs
		// while nothing is substituted, so the watch never sees our own
		// scaling.
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
	}

	// The stretch ran (or was skipped): between frames the coordinate paths
	// must see the vanilla view dimensions again.
	if (g_substituted)
		RestoreViewDims();
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
	return ClampViewDimension(true);
}

int RenderZoom::ClampHeight()
{
	return ClampViewDimension(false);
}

int RenderZoom::ClampDimension(int gameDim, int srcDim, bool magnified)
{
	if (magnified && srcDim > 0) return srcDim;
	return gameDim;
}
