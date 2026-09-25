#include "Zoomer.hpp"
#include "RenderZoom.hpp"
#include "GameCamera.hpp"
#include "Log.h"
#include <windowsx.h>
#include <psapi.h>
#ifndef VIEWCTRL_TEST
#include <MinHook.h>
#endif

typedef BOOL(WINAPI* GetCursorPosFunc)(LPPOINT lpPoint);

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
	constexpr DWORD ADDR_WINDOW_BOUNDS = 0x886FB0;
	const RECT fallback = { 0, 0, 800, 600 };

	if (!RenderZoom::IsGameReadable(reinterpret_cast<const void*>(ADDR_WINDOW_BOUNDS), 4 * sizeof(int)))
		return fallback;

	const int left = *reinterpret_cast<const int*>(ADDR_WINDOW_BOUNDS);
	const int top = *reinterpret_cast<const int*>(ADDR_WINDOW_BOUNDS + 4);
	const int width = *reinterpret_cast<const int*>(ADDR_WINDOW_BOUNDS + 8);
	const int height = *reinterpret_cast<const int*>(ADDR_WINDOW_BOUNDS + 12);
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

bool Zoomer::UsingGScript()
{
	return g_useGScript;
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

	if (pt->x < g_viewRect.left)   pt->x = g_viewRect.left;
	if (pt->x > g_viewRect.right)  pt->x = g_viewRect.right;
	if (pt->y < g_viewRect.top)    pt->y = g_viewRect.top;
	if (pt->y > g_viewRect.bottom) pt->y = g_viewRect.bottom;
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

BOOL WINAPI Zoomer::HookedGetCursorPos(LPPOINT lpPoint)
{
	if (!OriginalGetCursorPos || !lpPoint)
		return FALSE;

	BOOL result = ((GetCursorPosFunc)OriginalGetCursorPos)(lpPoint);

	if (g_zoom != ZOOM_DEFAULT && g_hWnd)
	{
		POINT clientPt = *lpPoint;
		ScreenToClient(g_hWnd, &clientPt);

		if (IsPointInMapArea(clientPt))
		{
			clientPt.x = g_centerX + lroundf((clientPt.x - g_centerX) * g_invZoom.load());
			clientPt.y = g_centerY + lroundf((clientPt.y - g_centerY) * g_invZoom.load());
			ClampToViewport(&clientPt);
			ClientToScreen(g_hWnd, &clientPt);
			*lpPoint = clientPt;
		}
	}

	return result;
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

		if (IsPointInMapArea(clientPt))
		{
			// The magnified view stays anchored at its center: the point under
			// the cursor is kept in place by moving the game camera instead.
			POINT focus = clientPt;
			ClampToViewport(&focus);
			g_focusX = focus.x;
			g_focusY = focus.y;
			g_focusValid = true;

			short delta = GET_WHEEL_DELTA_WPARAM(wParam);

			if (g_useGScript && g_pZoomFactor)
			{
				float curZoom = *g_pZoomFactor;
				if (curZoom < ZOOM_MIN) curZoom = ZOOM_MIN;
				if (curZoom > ZOOM_MAX) curZoom = ZOOM_MAX;

				if (delta > 0)
					curZoom += ZOOM_STEP;
				else
					curZoom -= ZOOM_STEP;

				if (curZoom < ZOOM_MIN) curZoom = ZOOM_MIN;
				if (curZoom > ZOOM_MAX) curZoom = ZOOM_MAX;

				*g_pZoomFactor = curZoom;
				LOG("GScript ZOOM: factor=%.3f", curZoom);
			}
			else
			{
				float target = g_targetZoom.load();
				if (delta > 0)
					target += ZOOM_STEP;
				else
					target -= ZOOM_STEP;

				if (target > ZOOM_MAX) target = ZOOM_MAX;
				if (target < ZOOM_MIN) target = ZOOM_MIN;
				g_targetZoom.store(target);

				LOG("WM_MOUSEWHEEL delta=%d target=%.3f", delta, target);
			}
		}

		return 0;
	}

	if (msg == WM_KEYDOWN)
	{
		bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		if (ctrl && wParam == VK_0)
		{
			ResetZoom();
			return 0;
		}

		if (g_zoom.load() != ZOOM_DEFAULT && !ctrl)
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

	if (g_zoom != ZOOM_DEFAULT)
	{
		switch (msg)
		{
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDBLCLK:
		{
			POINT clientPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

			// Only the magnified view remaps, the sidebar still takes raw
			// client coordinates.
			if (PtInRect(&g_viewRect, clientPt))
			{
				int originalX = g_centerX + lroundf((clientPt.x - g_centerX) * g_invZoom.load());
				int originalY = g_centerY + lroundf((clientPt.y - g_centerY) * g_invZoom.load());
				lParam = MAKELPARAM(originalX, originalY);
			}
		}
		break;
		}
	}
	return CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
}

DWORD WINAPI Zoomer::InitThread(LPVOID lpParam)
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

	bool mhOk = false;
#ifndef VIEWCTRL_TEST
	MH_STATUS mhStatus = MH_Initialize();
	LOG("MH_Initialize: %s (%d)", MH_StatusToString(mhStatus), mhStatus);

	if (mhOk = (mhStatus == MH_OK))
	{
		MH_STATUS createStatus = MH_CreateHookApi(
			L"user32.dll",
			"GetCursorPos",
			HookedGetCursorPos,
			(void**)&OriginalGetCursorPos
		);
		LOG("MH_CreateHookApi GetCursorPos: %s (%d)", MH_StatusToString(createStatus), createStatus);

		MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
		LOG("MH_EnableHook: %s (%d)", MH_StatusToString(enableStatus), enableStatus);
	}
#endif

	HMODULE hGScript = GetModuleHandleA("GScript.ext");
	if (!hGScript) hGScript = GetModuleHandleA("GScript.dll");
	if (hGScript)
	{
		MODULEINFO modInfo = {};
		if (GetModuleInformation(GetCurrentProcess(), hGScript, &modInfo, sizeof(modInfo)))
		{
			g_pZoomFactor = reinterpret_cast<float*>(
				reinterpret_cast<BYTE*>(modInfo.lpBaseOfDll) + GSCRIPT_ZOOM_FACTOR_RVA);

			DWORD oldProtect = 0;
			if (VirtualProtect(g_pZoomFactor, sizeof(float), PAGE_READWRITE, &oldProtect))
			{
				g_useGScript = true;
				LOG("GScript.ext found at %p, zoom_factor at %p (base+0x%X)",
					modInfo.lpBaseOfDll, g_pZoomFactor, GSCRIPT_ZOOM_FACTOR_RVA);
			}
			else
			{
				LOG("GScript.ext found but VirtualProtect failed for zoom_factor");
			}
		}
	}
	else
	{
		LOG("GScript.ext not found — using the in game zoom");
	}

	if (g_useGScript)
	{
		LOG("GScript mode: game camera control and render zoom stay disabled");
	}
	else
	{
		GameCamera::Enable();
	}

	RenderZoom::Init(!g_useGScript);

	ShowCursor(FALSE);
	g_initialized = mhOk || g_wndProcHooked;
	LOG("Init complete: g_initialized=%d useGScript=%d", g_initialized, g_useGScript);

	return 0;
}

void Zoomer::Init()
{
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

	if (g_initialized)
	{
		LOG("Disabling MinHook hooks");
#ifndef VIEWCTRL_TEST
		MH_DisableHook(MH_ALL_HOOKS);
		MH_Uninitialize();
#endif
		ShowCursor(TRUE);
	}

	g_hWnd = nullptr;
	g_zoom.store(ZOOM_DEFAULT);
	g_targetZoom.store(ZOOM_DEFAULT);
	g_invZoom.store(1.0f);
	OriginalGetCursorPos = nullptr;
	g_initialized = false;
	g_perfCounterReady = false;
	g_lastLerpTime = {};
	g_perfFrequency = {};
	g_viewRect = DefaultViewRect();
	g_centerX = (g_viewRect.left + g_viewRect.right) / 2;
	g_centerY = (g_viewRect.top + g_viewRect.bottom) / 2;
	g_viewRectFromGame = false;
	g_useGScript = false;
	g_pZoomFactor = nullptr;

	LOG("Shutdown complete");
}
