#include "Zoomer.hpp"
#include "RenderZoom.hpp"
#include "GameCamera.hpp"
#include "GameAddrs.hpp"
#include "Log.h"
#include <windowsx.h>

void Zoomer::UpdateClientCache(HWND hWnd)
{
	if (!hWnd) return;
	GetClientRect(hWnd, &g_clientRect);
	g_clientWidth = g_clientRect.right - g_clientRect.left;
	g_clientHeight = g_clientRect.bottom - g_clientRect.top;

	// Until DSurface::ViewBounds is known the whole client rect is the view.
	if (!g_viewRectFromGame)
		SetViewRect(g_clientRect, false);
}

void Zoomer::SetViewRect(const RECT& rect, bool fromGame)
{
	if (rect.left >= rect.right || rect.top >= rect.bottom) return;

	const bool changed = rect.left != g_viewRect.left || rect.top != g_viewRect.top
		|| rect.right != g_viewRect.right || rect.bottom != g_viewRect.bottom;

	g_viewRect = rect;
	if (fromGame) g_viewRectFromGame = true;

	// The magnified view is anchored in its center, so the anchor follows a
	// rect that actually changed size or position.
	if (changed)
	{
		g_centerX = (rect.left + rect.right) / 2;
		g_centerY = (rect.top + rect.bottom) / 2;
	}
}

RECT Zoomer::ViewRect()
{
	return g_viewRect;
}

RECT Zoomer::DefaultViewRect()
{
	// DSurface::WindowBounds: the rectangle the game draws its window into.
	const RECT fallback = { 0, 0, 800, 600 };

#ifdef VIEWCTRL_TEST
	const uintptr_t windowBounds = reinterpret_cast<uintptr_t>(RenderZoom::TestWindowBoundsAddress());
#else
	const uintptr_t windowBounds = GameAddr::DSurface_WindowBounds;
#endif
	if (!RenderZoom::IsGameReadable(reinterpret_cast<const void*>(windowBounds), 4 * sizeof(int)))
		return fallback;

	// RectangleStruct {X, Y, W, H} -> RECT {left, top, right=X+W, bottom=Y+H}.
	const int left = *reinterpret_cast<const int*>(windowBounds);
	const int top = *reinterpret_cast<const int*>(windowBounds + 4);
	const int width = *reinterpret_cast<const int*>(windowBounds + 8);
	const int height = *reinterpret_cast<const int*>(windowBounds + 12);
	if (width <= 0 || height <= 0) return fallback;

	const RECT rect = { left, top, left + width, top + height };
	if (rect.left >= rect.right || rect.top >= rect.bottom) return fallback;
	return rect;
}

POINT Zoomer::ZoomAnchor()
{
	POINT p = { g_centerX.load(), g_centerY.load() };
	return p;
}

float Zoomer::CurrentZoom()
{
	return g_zoom.load();
}

bool Zoomer::ZoomActive()
{
	return fabsf(g_zoom.load() - ZOOM_DEFAULT) > ZOOM_SNAP;
}

bool Zoomer::ContentScale(float& scaleX, float& scaleY)
{
	if (!ZoomActive()) return false;

	const RECT view = g_viewRect;
	const int viewW = view.right - view.left;
	const int viewH = view.bottom - view.top;
	if (viewW <= 0 || viewH <= 0) return false;

	const POINT anchor = ZoomAnchor();
	const RenderZoom::SourceRect src = RenderZoom::ComputeSourceRect(
		viewW, viewH, anchor.x - view.left, anchor.y - view.top, g_zoom.load());
	if (src.W <= 0 || src.H <= 0) return false;

	scaleX = (float)src.W / (float)viewW;
	scaleY = (float)src.H / (float)viewH;
	return true;
}

bool Zoomer::UnMagnify(const POINT& in, POINT& out)
{
	// Coordinate transforms are gated behind an active zoom.
	if (!ZoomActive()) return false;

	const RECT view = g_viewRect;
	const int viewW = view.right - view.left;
	const int viewH = view.bottom - view.top;
	if (viewW <= 0 || viewH <= 0) return false;

	// The game hands over view relative coordinates; bound them against the
	// view rect first.
	if (in.x < 0 || in.x >= viewW || in.y < 0 || in.y >= viewH) return false;

	const POINT anchor = ZoomAnchor();
	const RenderZoom::SourceRect src = RenderZoom::ComputeSourceRect(
		viewW, viewH, anchor.x - view.left, anchor.y - view.top, g_zoom.load());
	if (src.W <= 0 || src.H <= 0) return false;

	// Inverse of what RenderZoom::Upscale draws: the displayed view relative
	// point picks its pixel from the source rect.
	float mappedX = (float)src.X + (float)in.x * (float)src.W / (float)viewW;
	float mappedY = (float)src.Y + (float)in.y * (float)src.H / (float)viewH;

	if (mappedX < 0.0f) mappedX = 0.0f;
	if (mappedX > (float)(viewW - 1)) mappedX = (float)(viewW - 1);
	if (mappedY < 0.0f) mappedY = 0.0f;
	if (mappedY > (float)(viewH - 1)) mappedY = (float)(viewH - 1);

	out.x = (int)floorf(mappedX + 0.5f);
	out.y = (int)floorf(mappedY + 0.5f);

	if (out.x < 0) out.x = 0;
	if (out.y < 0) out.y = 0;
	if (out.x > viewW - 1) out.x = viewW - 1;
	if (out.y > viewH - 1) out.y = viewH - 1;
	return true;
}

void Zoomer::TickFrame()
{
	// Runs from the pre render hook: the zoom factor the camera moves by has
	// to be advanced before the game draws the frame.
	UpdateLerpFrameIndependent();
}

bool Zoomer::IsPointInMapArea(POINT pt)
{
	if (!g_hWnd) return false;
	return PtInRect(&g_viewRect, pt) != FALSE;
}

void Zoomer::ClampToViewport(POINT* pt)
{
	if (!pt) return;

	// The right and bottom edge belong to the next area (PtInRect, which
	// IsPointInMapArea uses, and UnMagnify both exclude it), so the clamped
	// point always ends up inside the view rect.
	if (pt->x < g_viewRect.left)         pt->x = g_viewRect.left;
	if (pt->x > g_viewRect.right - 1)    pt->x = g_viewRect.right - 1;
	if (pt->y < g_viewRect.top)          pt->y = g_viewRect.top;
	if (pt->y > g_viewRect.bottom - 1)   pt->y = g_viewRect.bottom - 1;
}

void Zoomer::UpdateLerp()
{
	if (g_cameraBusy) return;

	float curZoom = g_zoom.load();
	float tgtZoom = g_targetZoom.load();
	if (curZoom == tgtZoom) return;

	float diff = tgtZoom - curZoom;
	if (fabsf(diff) < ZOOM_SNAP)
	{
		curZoom = tgtZoom;
	}
	else
	{
		curZoom += diff * ZOOM_LERP;
	}
	CommitZoom(curZoom);
}

void Zoomer::UpdateLerpFrameIndependent()
{
	if (g_cameraBusy) return;

	if (!g_perfCounterReady)
	{
		UpdateLerp();
		return;
	}

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	float dt = (float)(now.QuadPart - g_lastLerpTime.QuadPart) / (float)g_perfFrequency.QuadPart;
	g_lastLerpTime = now;

	if (dt <= 0.0f || dt > 0.1f) dt = 0.016f;

	float curZoom = g_zoom.load();
	float tgtZoom = g_targetZoom.load();
	if (curZoom == tgtZoom) return;

	float diff = tgtZoom - curZoom;
	if (fabsf(diff) < ZOOM_SNAP)
	{
		curZoom = tgtZoom;
	}
	else
	{
		float speed = 8.0f;
		curZoom += diff * (1.0f - expf(-speed * dt));
	}
	CommitZoom(curZoom);
}

void Zoomer::CommitZoom(float newZoom)
{
	const float oldZoom = g_zoom.load();
	if (newZoom == oldZoom) return;

	g_cameraBusy = true;
	g_zoom.store(newZoom);
	g_invZoom.store(1.0f / newZoom);

	ApplyCameraStep(oldZoom, newZoom);

	// Back at the native zoom the magnified view covers the whole view again,
	// so the shift we accumulated while zoomed in is dropped here.
	if (newZoom <= ZOOM_MIN && oldZoom > ZOOM_MIN)
		UndoCameraOffset();

	g_cameraBusy = false;
}

void Zoomer::ApplyCameraStep(float oldZoom, float newZoom)
{
	if (!GameCamera::IsEnabled()) return;
	if (!(oldZoom > 0.0f) || oldZoom == newZoom) return;

	POINT focus = { g_focusX.load(), g_focusY.load() };
	if (!g_focusValid)
	{
		focus.x = g_centerX.load();
		focus.y = g_centerY.load();
	}

	const POINT fixedPoint = { g_centerX.load(), g_centerY.load() };
	const POINT shift = GameCamera::ComputeShift(focus, fixedPoint, oldZoom, newZoom);
	if (shift.x == 0 && shift.y == 0) return;

	if (GameCamera::ShiftBy(shift.x, shift.y))
	{
		g_camOffset.x += shift.x;
		g_camOffset.y += shift.y;
		LOG("camera shift (%ld,%ld) zoom %.3f -> %.3f offset (%ld,%ld) focus (%ld,%ld)",
			shift.x, shift.y, oldZoom, newZoom,
			g_camOffset.x, g_camOffset.y, focus.x, focus.y);
	}
	else
	{
		LOG("camera shift (%ld,%ld) rejected by the game", shift.x, shift.y);
	}
}

void Zoomer::UndoCameraOffset()
{
	if (g_camOffset.x == 0 && g_camOffset.y == 0) return;

	if (!GameCamera::IsEnabled())
	{
		g_camOffset = { 0, 0 };
		return;
	}

	const bool wasBusy = g_cameraBusy;
	g_cameraBusy = true;

	if (GameCamera::ShiftBy(-g_camOffset.x, -g_camOffset.y))
	{
		LOG("camera offset undone (%ld,%ld)", g_camOffset.x, g_camOffset.y);
		g_camOffset = { 0, 0 };
	}
	else
	{
		LOG("camera offset (%ld,%ld) could not be undone", g_camOffset.x, g_camOffset.y);
	}

	g_cameraBusy = wasBusy;
}

void Zoomer::PanCamera(int dx, int dy)
{
	if (dx == 0 && dy == 0) return;

	if (GameCamera::IsEnabled())
	{
		const bool wasBusy = g_cameraBusy;
		g_cameraBusy = true;
		GameCamera::ShiftBy(dx, dy);
		g_cameraBusy = wasBusy;
		return;
	}

	// No game camera (outside gamemd.exe): move the anchor of the magnified
	// view instead, keeping the source rect inside the view rect.
	const int viewW = g_viewRect.right - g_viewRect.left;
	const int viewH = g_viewRect.bottom - g_viewRect.top;
	if (viewW <= 0 || viewH <= 0) return;

	const float curZoom = g_zoom.load();
	const float halfW = (float)viewW / curZoom * 0.5f;
	const float halfH = (float)viewH / curZoom * 0.5f;

	float cx = (float)g_centerX.load() + (float)dx;
	float cy = (float)g_centerY.load() + (float)dy;

	const float minX = (float)g_viewRect.left + halfW;
	const float maxX = (float)g_viewRect.right - halfW;
	const float minY = (float)g_viewRect.top + halfH;
	const float maxY = (float)g_viewRect.bottom - halfH;

	if (cx < minX) cx = (minX < maxX) ? minX : maxX;
	if (cx > maxX) cx = (minX < maxX) ? maxX : minX;
	if (cy < minY) cy = (minY < maxY) ? minY : maxY;
	if (cy > maxY) cy = (minY < maxY) ? maxY : minY;

	g_centerX = (LONG)cx;
	g_centerY = (LONG)cy;
}

void Zoomer::ResetZoom()
{
	g_cameraBusy = true;
	g_zoom.store(ZOOM_DEFAULT);
	g_targetZoom.store(ZOOM_DEFAULT);
	g_invZoom.store(1.0f);
	g_focusValid = false;
	UndoCameraOffset();
	g_cameraBusy = false;

	LOG("Zoom reset to %.1f", ZOOM_DEFAULT);
}

bool Zoomer::CtrlHeld()
{
#ifdef VIEWCTRL_TEST
	return g_ctrlHeld;
#else
	return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
#endif
}

float Zoomer::ApplyWheelSteps(float zoom, int steps)
{
	if (steps == 0)
		return zoom;

	const float factor = powf(ZOOM_GEAR, (float)(steps > 0 ? steps : -steps));
	if (steps > 0)
		zoom *= factor;
	else
		zoom /= factor;

	if (zoom > ZOOM_MAX) zoom = ZOOM_MAX;
	if (zoom < ZOOM_MIN) zoom = ZOOM_MIN;
	return zoom;
}

bool Zoomer::RegisterDoublePress(DWORD nowMs)
{
	if (g_haveLastPress && (nowMs - g_lastPressMs) <= DOUBLE_PRESS_MS)
	{
		g_haveLastPress = false;
		return true;
	}

	g_lastPressMs = nowMs;
	g_haveLastPress = true;
	return false;
}

LRESULT CALLBACK Zoomer::NewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_MOUSEWHEEL)
	{
		POINT screenPt;
		screenPt.x = GET_X_LPARAM(lParam);
		screenPt.y = GET_Y_LPARAM(lParam);
		POINT clientPt = screenPt;
		ScreenToClient(hWnd, &clientPt);

		LOG("MOUSEWHEEL: screen=(%ld,%ld) client=(%ld,%ld)", screenPt.x, screenPt.y, clientPt.x, clientPt.y);

		// The magnified view zooms only while Ctrl is held; otherwise the
		// game sees the wheel.
		if (CtrlHeld() && IsPointInMapArea(clientPt))
		{
			// The magnified view stays anchored at its center: the point under
			// the cursor is kept in place by moving the game camera instead.
			POINT focus = clientPt;
			ClampToViewport(&focus);
			g_focusX = focus.x;
			g_focusY = focus.y;
			g_focusValid = true;

			const short delta = GET_WHEEL_DELTA_WPARAM(wParam);

			// Smooth scrolling hands out deltas smaller than WHEEL_DELTA:
			// keep what is left over so the steps add up instead of being
			// rounded away to zero.
			g_wheelRemainder += delta;
			const int steps = g_wheelRemainder / WHEEL_DELTA;
			g_wheelRemainder %= WHEEL_DELTA;

			const float target = ApplyWheelSteps(g_targetZoom.load(), steps);
			g_targetZoom.store(target);

			LOG("WM_MOUSEWHEEL delta=%d steps=%d target=%.3f", delta, steps, target);

			return 0;
		}

		// The wheel belongs to the game: start the remainder over, a later
		// zoom gesture must not inherit what was collected here.
		g_wheelRemainder = 0;
		return CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
	}

	// Double press Ctrl resets the zoom: watch WM_KEYDOWN of VK_CONTROL,
	// ignoring the auto repeat bit. Ctrl itself is never consumed.
	if (msg == WM_KEYDOWN && wParam == VK_CONTROL && !(lParam & 0x40000000))
	{
		if (RegisterDoublePress(GetTickCount()))
			ResetZoom();
	}

	if (msg == WM_KEYDOWN)
	{
		// Panning needs a zoom that is actually away from 1.0, the same snap
		// epsilon the coordinate transforms are gated behind.
		if (ZoomActive() && !CtrlHeld())
		{
			const int viewW = g_viewRect.right - g_viewRect.left;
			const int viewH = g_viewRect.bottom - g_viewRect.top;

			if (viewW > 0 && viewH > 0)
			{
				float curZoom = g_zoom.load();
				int stepX = (int)((float)viewW / curZoom * 0.2f);
				int stepY = (int)((float)viewH / curZoom * 0.2f);
				int dx = 0;
				int dy = 0;

				switch (wParam)
				{
				case VK_LEFT:  dx = -stepX; break;
				case VK_RIGHT: dx =  stepX; break;
				case VK_UP:    dy = -stepY; break;
				case VK_DOWN:  dy =  stepY; break;
				default: break;
				}

				if (dx != 0 || dy != 0)
				{
					PanCamera(dx, dy);
					return 0;
				}
			}
		}
	}

	if (msg == WM_SIZE || msg == WM_MOVE)
	{
		UpdateClientCache(hWnd);
	}

	// Mouse messages pass through untouched: the game consumes the physical
	// position and the coordinate transforms happen at the points where it
	// turns a screen position into game space (see Main.cpp) instead of
	// remapping the message stream.
	return CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
}

DWORD WINAPI Zoomer::InitThread(LPVOID)
{
	LOG("InitThread started, PID=%lu", GetCurrentProcessId());

	if (QueryPerformanceFrequency(&g_perfFrequency))
	{
		g_perfCounterReady = true;
		QueryPerformanceCounter(&g_lastLerpTime);
		LOG("Performance counter: freq=%lld", g_perfFrequency.QuadPart);
	}

	DWORD pid = GetCurrentProcessId();
	HWND hWnd = NULL;
	while ((hWnd = FindWindowEx(NULL, hWnd, NULL, NULL)) != NULL) {
		DWORD dwPid = 0;
		GetWindowThreadProcessId(hWnd, &dwPid);
		if (dwPid == pid && IsWindowVisible(hWnd)) break;
	}
	g_hWnd = hWnd;

	if (!g_hWnd)
	{
		LOG("ERROR: window not found for PID=%lu", pid);
		return 0;
	}
	LOG("Found window HWND=%p", g_hWnd);

	UpdateClientCache(g_hWnd);
	LOG("Client rect: %dx%d, view=(%ld,%ld)-(%ld,%ld)",
		g_clientWidth, g_clientHeight,
		g_viewRect.left, g_viewRect.top, g_viewRect.right, g_viewRect.bottom);

	OriginalWndProc = (WNDPROC)SetWindowLongPtrW(
		g_hWnd, GWLP_WNDPROC, (LONG_PTR)NewWndProc
	);
	g_wndProcHooked = (OriginalWndProc != nullptr);
	LOG("WndProc hook: %s (orig=%p)", g_wndProcHooked ? "OK" : "FAILED", OriginalWndProc);

	// GScript.ext is no longer supported: ViewCtrl always owns the zoom. If
	// the module is present anyway, both extensions would magnify at once.
	if (GetModuleHandleA("GScript.ext") || GetModuleHandleA("GScript.dll"))
		LOG("WARNING: GScript.ext detected - ViewCtrl no longer delegates the zoom; disable one of the two");

	GameCamera::Enable();

	RenderZoom::Init();

	g_initialized = g_wndProcHooked;
	LOG("Init complete: g_initialized=%d", g_initialized);

	return 0;
}

void Zoomer::Init()
{
	// The Syringe startup hook may fire more than once: a second thread
	// would subclass the window proc again and Shutdown could only restore
	// our own handler.
	if (g_initStarted)
	{
		LOG("Init() ignored — already started");
		return;
	}
	g_initStarted = true;

	LOG("Init() called — creating init thread");
	g_hThread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
}

void Zoomer::Shutdown()
{
	LOG("Shutdown() called");

	UndoCameraOffset();
	GameCamera::Disable();

	if (g_hThread)
	{
		LOG("Waiting for init thread...");
		WaitForSingleObject(g_hThread, 2000);
		CloseHandle(g_hThread);
		g_hThread = nullptr;
	}

	if (g_wndProcHooked && OriginalWndProc && g_hWnd)
	{
		LOG("Restoring WndProc: %p -> %p", NewWndProc, OriginalWndProc);
		SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, (LONG_PTR)OriginalWndProc);
		OriginalWndProc = nullptr;
		g_wndProcHooked = false;
	}

	RenderZoom::Shutdown();

	g_hWnd = nullptr;
	g_zoom.store(ZOOM_DEFAULT);
	g_targetZoom.store(ZOOM_DEFAULT);
	g_invZoom.store(1.0f);
	g_initialized = false;
	g_initStarted = false;
	g_wheelRemainder = 0;
	g_perfCounterReady = false;
	g_lastLerpTime = {};
	g_perfFrequency = {};
	g_viewRect = DefaultViewRect();
	g_centerX = (g_viewRect.left + g_viewRect.right) / 2;
	g_centerY = (g_viewRect.top + g_viewRect.bottom) / 2;
	g_viewRectFromGame = false;

	LOG("Shutdown complete");
}
