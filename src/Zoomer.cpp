#include "Zoomer.hpp"
#include "Log.h"
#include <windowsx.h>
#ifndef VIEWCTRL_TEST
#include <MinHook.h>
#endif

typedef HRESULT(WINAPI* BltFunc)(
	LPDIRECTDRAWSURFACE7 self,
	LPRECT destRect,
	LPDIRECTDRAWSURFACE7 srcSurface,
	LPRECT srcRect,
	DWORD flags,
	LPDDBLTFX fx);

typedef BOOL(WINAPI* GetCursorPosFunc)(LPPOINT lpPoint);

void Zoomer::UpdateClientCache(HWND hWnd)
{
	if (!hWnd) return;
	GetClientRect(hWnd, &g_clientRect);
	g_clientWidth = g_clientRect.right - g_clientRect.left;
	g_clientHeight = g_clientRect.bottom - g_clientRect.top;
	g_mapRight = g_clientWidth - SIDEBAR_WIDTH;
	g_mapBottom = g_clientHeight - BOTTOM_BAR_HEIGHT;
}

void Zoomer::UpdateCenter(HWND hWnd)
{
	if (!hWnd) return;
	g_centerX = (g_clientRect.right + g_clientRect.left) / 2;
	g_centerY = (g_clientRect.bottom + g_clientRect.top) / 2;
}

bool Zoomer::IsMapArea(LPRECT rect)
{
	if (!rect || !g_hWnd) return false;
	if (g_mapRight <= 0 || g_mapBottom <= 0) return false;

	if (rect->left != 0 || rect->top != 0) return false;
	if (rect->right != g_mapRight || rect->bottom != g_mapBottom) return false;

	return true;
}

bool Zoomer::IsPointInMapArea(POINT pt)
{
	if (!g_hWnd) return false;

	if (pt.x >= g_mapRight)  return false;
	if (pt.y >= g_mapBottom) return false;
	return true;
}

void Zoomer::ApplyZoomToRect(LPRECT rect)
{
	if (!rect) return;

	rect->left = g_centerX + lroundf((rect->left - g_centerX) * g_zoom);
	rect->top = g_centerY + lroundf((rect->top - g_centerY) * g_zoom);
	rect->right = g_centerX + lroundf((rect->right - g_centerX) * g_zoom);
	rect->bottom = g_centerY + lroundf((rect->bottom - g_centerY) * g_zoom);
}

bool Zoomer::IsPrimaryOrBackBuffer(IDirectDrawSurface7* surface)
{
	if (!surface) return false;

	if (g_knownPrimaryOrBack.count(surface))
		return true;

	DDSURFACEDESC2 desc;
	ZeroMemory(&desc, sizeof(desc));
	desc.dwSize = sizeof(desc);

	if (FAILED(surface->GetSurfaceDesc(&desc)))
		return false;

	if (desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE ||
		desc.ddsCaps.dwCaps & DDSCAPS_BACKBUFFER)
	{
		g_knownPrimaryOrBack.insert(surface);
		return true;
	}

	return false;
}

void Zoomer::ClampToViewport(POINT* pt)
{
	if (!pt || !g_hWnd || g_mapRight <= 0 || g_mapBottom <= 0) return;

	if (pt->x < 0)              pt->x = 0;
	if (pt->x > g_mapRight)     pt->x = g_mapRight;
	if (pt->y < 0)              pt->y = 0;
	if (pt->y > g_mapBottom)    pt->y = g_mapBottom;
}

HRESULT WINAPI Zoomer::HookedBlt(
	LPDIRECTDRAWSURFACE7 self,
	LPRECT destRect,
	LPDIRECTDRAWSURFACE7 srcSurface,
	LPRECT srcRect,
	DWORD flags,
	LPDDBLTFX fx)
{
	static bool firstCall = true;

	float curZoom = g_zoom.load();
	float tgtZoom = g_targetZoom.load();

	if (curZoom != tgtZoom)
	{
		float diff = tgtZoom - curZoom;
		if (fabsf(diff) < ZOOM_SNAP)
		{
			curZoom = tgtZoom;
		}
		else
		{
			curZoom += diff * ZOOM_LERP;
		}
		g_zoom.store(curZoom);
		g_invZoom = 1.0f / curZoom;
	}

	bool isPrimary = IsPrimaryOrBackBuffer(self);
	bool isMap = destRect ? IsMapArea(destRect) : false;

	if (firstCall && destRect)
	{
		LOG("HookedBlt FIRST: self=%p dest=(%ld,%ld,%ld,%ld) src=%p isPrimary=%d isMap=%d flags=0x%X",
			self, destRect->left, destRect->top, destRect->right, destRect->bottom,
			srcSurface, isPrimary, isMap, flags);
		firstCall = false;
	}

	if (curZoom != ZOOM_DEFAULT && isPrimary && isMap && destRect)
	{
		float invZoom = 1.0f / curZoom;
		float cx = (float)g_centerX.load();
		float cy = (float)g_centerY.load();

		float srcLeft   = cx - cx * invZoom;
		float srcTop    = cy - cy * invZoom;
		float srcRight  = srcLeft + (float)g_mapRight * invZoom;
		float srcBottom = srcTop  + (float)g_mapBottom * invZoom;

		if (srcLeft < 0) srcLeft = 0;
		if (srcTop < 0)  srcTop = 0;
		if (srcRight > g_mapRight)   srcRight = (float)g_mapRight;
		if (srcBottom > g_mapBottom) srcBottom = (float)g_mapBottom;

		RECT zoomSrc = { (LONG)srcLeft, (LONG)srcTop, (LONG)srcRight, (LONG)srcBottom };

		LOG("ZOOM: src=(%ld,%ld,%ld,%ld) dest=(%ld,%ld,%ld,%ld) zoom=%.3f center=(%d,%d)",
			zoomSrc.left, zoomSrc.top, zoomSrc.right, zoomSrc.bottom,
			destRect->left, destRect->top, destRect->right, destRect->bottom,
			curZoom, (int)cx, (int)cy);

		return ((BltFunc)OriginalBlt)(self, destRect, srcSurface, &zoomSrc, flags, fx);
	}

	if (!OriginalBlt)
	{
		LOG("HookedBlt: OriginalBlt is NULL! self=%p", self);
		return DD_OK;
	}

	return ((BltFunc)OriginalBlt)(self, destRect, srcSurface, srcRect, flags, fx);
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
			clientPt.x = g_centerX + lroundf((clientPt.x - g_centerX) * g_invZoom);
			clientPt.y = g_centerY + lroundf((clientPt.y - g_centerY) * g_invZoom);
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
			g_centerX = clientPt.x;
			g_centerY = clientPt.y;

			short delta = GET_WHEEL_DELTA_WPARAM(wParam);

			float target = g_targetZoom.load();
			if (delta > 0)
			{
				target *= ZOOM_STEP;
			}
			else
			{
				if (target > ZOOM_MIN)
					target /= ZOOM_STEP;
			}

			if (target > ZOOM_MAX) target = ZOOM_MAX;
			if (target < ZOOM_MIN) target = ZOOM_MIN;
			g_targetZoom.store(target);

			LOG("WM_MOUSEWHEEL delta=%d target=%.3f", delta, target);
		}

		return 0;
	}

	if (msg == WM_SIZE || msg == WM_MOVE)
	{
		UpdateClientCache(hWnd);
		UpdateCenter(hWnd);
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
			int clientX = GET_X_LPARAM(lParam);
			int clientY = GET_Y_LPARAM(lParam);
			POINT pt = { clientX, clientY };
			if (IsPointInMapArea(pt))
			{
				int originalX = g_centerX + lroundf((clientX - g_centerX) * g_invZoom);
				int originalY = g_centerY + lroundf((clientY - g_centerY) * g_invZoom);
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

	RECT winRect = {};
	GetWindowRect(g_hWnd, &winRect);
	LOG("Window screen rect: (%ld,%ld) to (%ld,%ld) size=%ldx%ld",
		winRect.left, winRect.top, winRect.right, winRect.bottom,
		winRect.right - winRect.left, winRect.bottom - winRect.top);

	UpdateClientCache(g_hWnd);
	UpdateCenter(g_hWnd);
	LOG("Client rect: %dx%d, center=(%d,%d)",
		g_clientWidth, g_clientHeight,
		(int)g_centerX.load(), (int)g_centerY.load());

	OriginalWndProc = (WNDPROC)SetWindowLongPtrW(
		g_hWnd, GWLP_WNDPROC, (LONG_PTR)NewWndProc
	);
	g_wndProcHooked = (OriginalWndProc != nullptr);
	LOG("WndProc hook: %s (orig=%p)", g_wndProcHooked ? "OK" : "FAILED", OriginalWndProc);

	LPDIRECTDRAW7 ddraw = nullptr;
	if (FAILED(DirectDrawCreateEx(nullptr, (void**)&ddraw, IID_IDirectDraw7, nullptr)))
	{
		LOG("ERROR: DirectDrawCreateEx failed");
		return 0;
	}
	LOG("DirectDraw7 created: %p", ddraw);

	if (FAILED(ddraw->SetCooperativeLevel(g_hWnd, DDSCL_NORMAL)))
	{
		LOG("ERROR: SetCooperativeLevel failed");
		ddraw->Release();
		return 0;
	}

	DDSURFACEDESC2 ddsd = { 0 };
	ddsd.dwSize = sizeof(ddsd);
	ddsd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
	ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
	ddsd.dwWidth = 64;
	ddsd.dwHeight = 64;

	LPDIRECTDRAWSURFACE7 surf = nullptr;
	if (FAILED(ddraw->CreateSurface(&ddsd, &surf, nullptr)))
	{
		LOG("ERROR: CreateSurface failed");
		ddraw->Release();
		return 0;
	}
	LOG("Temp surface created: %p", surf);

	g_vtable = *reinterpret_cast<void***>(surf);
	OriginalBlt = g_vtable[BLT_VTABLE_INDEX];
	LOG("VTable=%p, origBlt=%p (index %d), HookedBlt=%p", g_vtable, OriginalBlt, BLT_VTABLE_INDEX, HookedBlt);

	DWORD oldProtect = 0;
	VirtualProtect(&g_vtable[BLT_VTABLE_INDEX], sizeof(void*), PAGE_READWRITE, &oldProtect);
	g_vtable[BLT_VTABLE_INDEX] = reinterpret_cast<void*>(HookedBlt);
	VirtualProtect(&g_vtable[BLT_VTABLE_INDEX], sizeof(void*), oldProtect, &oldProtect);
	LOG("VTable patched: HookedBlt=%p", HookedBlt);

	surf->Release();
	ddraw->Release();

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

	ShowCursor(FALSE);
	g_initialized = mhOk || (g_vtable != nullptr);
	LOG("Init complete: g_initialized=%d", g_initialized);

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

	if (g_vtable && OriginalBlt)
	{
		LOG("Restoring vtable[%d]: %p -> %p", BLT_VTABLE_INDEX, HookedBlt, OriginalBlt);
		DWORD oldProtect = 0;
		VirtualProtect(&g_vtable[BLT_VTABLE_INDEX], sizeof(void*), PAGE_READWRITE, &oldProtect);
		g_vtable[BLT_VTABLE_INDEX] = OriginalBlt;
		VirtualProtect(&g_vtable[BLT_VTABLE_INDEX], sizeof(void*), oldProtect, &oldProtect);
		g_vtable = nullptr;
		OriginalBlt = nullptr;
	}

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
	g_invZoom = 1.0f;
	g_knownPrimaryOrBack.clear();
	g_mapRight = 0;
	g_mapBottom = 0;
	g_destDetected = false;
	OriginalGetCursorPos = nullptr;
	g_initialized = false;

	LOG("Shutdown complete");
}
