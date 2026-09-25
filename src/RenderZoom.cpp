#include "RenderZoom.hpp"
#include "Zoomer.hpp"
#include "Log.h"

#include <cstring>
#include <vector>

namespace
{
	// Game globals (YRpp Surface.h).
	constexpr DWORD ADDR_VIEW_BOUNDS = 0x886FA0;    // RectangleStruct {X, Y, W, H}
	constexpr DWORD ADDR_WINDOW_BOUNDS = 0x886FB0;
	constexpr DWORD ADDR_COMPOSITE = 0x88731C;       // DSurface*

	// Surface vtable slots (YRpp Surface.h).
	constexpr int VT_LOCK = 23;
	constexpr int VT_UNLOCK = 24;
	constexpr int VT_GET_BYTES_PER_PIXEL = 28;
	constexpr int VT_GET_PITCH = 29;
	constexpr int VT_GET_WIDTH = 31;
	constexpr int VT_GET_HEIGHT = 32;

	// The view rect is only looked at while the zoom factor sits at 1.0, and
	// at most four times a second (same throttle as Telescope).
	constexpr DWORD VIEW_RECT_POLL_MS = 250;

	constexpr float ZOOM_EPSILON = 1.001f;

	using SurfaceIntFn = int(__thiscall*)(void*);
	using SurfaceLockFn = void*(__thiscall*)(void*, int, int);
	using SurfaceUnlockFn = bool(__thiscall*)(void*);

	bool g_enabled = false;

	bool g_haveBackup = false;
	RECT g_backupRect = {};
	std::vector<unsigned char> g_buffer;
	int g_bufferW = 0;
	int g_bufferH = 0;

	RECT g_viewRect = {};
	bool g_viewRectValid = false;
	DWORD g_lastViewPoll = 0;

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

	// The hard coded game globals may only be dereferenced inside gamemd.exe.
	// Outside of the game (unit tests, a different host exe) they can point at
	// unrelated memory, so they are rejected there before anything is read.
	bool IsGameReadable(const void* address, size_t bytes)
	{
		const HMODULE game = GetModuleHandleA("gamemd.exe");
		if (!game) return false;
		if (!IsReadable(address, bytes)) return false;

		MEMORY_BASIC_INFORMATION mbi = {};
		if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
		return mbi.AllocationBase == game;
	}

	void* CompositeSurface()
	{
		if (!IsGameReadable(reinterpret_cast<const void*>(ADDR_COMPOSITE), sizeof(void*)))
			return nullptr;
		return *reinterpret_cast<void**>(ADDR_COMPOSITE);
	}

	void** VTable(void* object)
	{
		if (!object || !IsReadable(object, sizeof(void*))) return nullptr;

		void** vtable = *reinterpret_cast<void***>(object);
		if (!IsReadable(vtable, sizeof(void*) * (VT_GET_HEIGHT + 1))) return nullptr;
		return vtable;
	}

	bool ReadGameViewRect(RECT& out)
	{
		if (!IsGameReadable(reinterpret_cast<const void*>(ADDR_VIEW_BOUNDS), 4 * sizeof(int))) return false;
		if (!IsGameReadable(reinterpret_cast<const void*>(ADDR_WINDOW_BOUNDS), 4 * sizeof(int))) return false;

		int view[4] = {};
		int window[4] = {};
		memcpy(view, reinterpret_cast<const void*>(ADDR_VIEW_BOUNDS), sizeof(view));
		memcpy(window, reinterpret_cast<const void*>(ADDR_WINDOW_BOUNDS), sizeof(window));

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

		// Only 16 bit surfaces are backed up and magnified here.
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

	bool RestoreBackup()
	{
		if (!g_haveBackup || g_buffer.empty()) return false;

		const int w = g_backupRect.right - g_backupRect.left;
		const int h = g_backupRect.bottom - g_backupRect.top;
		if (w <= 0 || h <= 0 || w > g_bufferW || h > g_bufferH) return false;

		void* pixels = nullptr;
		int pitch = 0;
		if (!LockView(g_backupRect, pixels, pitch)) return false;

		const bool ok = RenderZoom::CopyRows(
			static_cast<unsigned char*>(pixels), pitch, g_backupRect,
			g_buffer.data(), g_bufferW, g_bufferH, false);
		UnlockSurface();
		return ok;
	}
}

void RenderZoom::Init(bool zoomEnabled)
{
	ResetFrameState();

	char path[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	_strlwr(path);
	const bool isGame = (strstr(path, "gamemd.exe") != nullptr);

	g_enabled = zoomEnabled && isGame;
	LOG("RenderZoom: enabled=%d (zoomEnabled=%d, gamemd=%d)", g_enabled, zoomEnabled, isGame);
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
#endif

void RenderZoom::ResetFrameState()
{
	g_haveBackup = false;
	g_backupRect = {};
	g_buffer.clear();
	g_bufferW = 0;
	g_bufferH = 0;
	g_viewRect = {};
	g_viewRectValid = false;
	g_lastViewPoll = 0;
}

bool RenderZoom::PreRender()
{
	if (!g_enabled) return false;

	// Advance the zoom lerp and the game camera before the frame is drawn, so
	// the magnification below matches the camera move of this very frame.
	Zoomer::TickFrame();

	bool forceRedraw = false;
	if (g_haveBackup)
	{
		// Put the original pixels back first: whatever happens afterwards, the
		// game draws on a clean frame again.
		forceRedraw = !RestoreBackup();
		g_haveBackup = false;
		if (forceRedraw) LOG("RenderZoom: restore failed, asking for a full repaint");
	}

	// The view rect may only change while nothing is magnified, otherwise the
	// backup and the pixels on screen would disagree about where they came from.
	if (Zoomer::CurrentZoom() <= ZOOM_EPSILON)
	{
		RECT candidate = {};
		if (ReadGameViewRect(candidate) && AcceptViewRect(candidate, GetTickCount()))
		{
			LOG("RenderZoom: view rect (%ld,%ld)-(%ld,%ld)",
				candidate.left, candidate.top, candidate.right, candidate.bottom);
			Zoomer::SetViewRect(candidate, true);
		}
	}

	return forceRedraw;
}

void RenderZoom::PostRender()
{
	if (!g_enabled) return;

	const float zoom = Zoomer::CurrentZoom();
	if (zoom <= ZOOM_EPSILON) return;

	const RECT view = Zoomer::ViewRect();
	const int w = view.right - view.left;
	const int h = view.bottom - view.top;
	if (w <= 0 || h <= 0) return;

	void* pixels = nullptr;
	int pitch = 0;
	if (!LockView(view, pixels, pitch)) return;

	const size_t bytes = (size_t)w * h * 2;
	if (g_buffer.size() < bytes) g_buffer.resize(bytes);

	if (!CopyRows(static_cast<unsigned char*>(pixels), pitch, view,
		g_buffer.data(), w, h, true))
	{
		UnlockSurface();
		return;
	}

	g_backupRect = view;
	g_bufferW = w;
	g_bufferH = h;
	g_haveBackup = true;

	// Magnify the backup in place. The anchor is the point the whole zoom keeps
	// fixed, which is the center of the view rect as long as nothing panned it.
	const POINT anchor = Zoomer::ZoomAnchor();
	const SourceRect src = ComputeSourceRect(
		w, h, anchor.x - view.left, anchor.y - view.top, zoom);

	auto* dst = reinterpret_cast<unsigned short*>(
		static_cast<unsigned char*>(pixels) + (size_t)view.top * pitch + (size_t)view.left * 2);
	Upscale(reinterpret_cast<const unsigned short*>(g_buffer.data()), w,
		dst, pitch / 2, w, h, src);

	UnlockSurface();
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
	if (pitch < w * 2) return false;

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
