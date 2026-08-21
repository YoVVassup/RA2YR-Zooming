#include "Zoomer.hpp"
#include "Log.h"
#include <windowsx.h>
#include <GL/gl.h>
#include <psapi.h>
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef VIEWCTRL_TEST
#include <MinHook.h>
#endif

typedef HRESULT(WINAPI* PresentFunc)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(WINAPI* ResetFunc)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

struct ZoomVertex {
	float x, y, z, rhw;
	float u, v;
};
static const DWORD D3DFVF_ZOOM = D3DFVF_XYZRHW | D3DFVF_TEX1;

typedef HRESULT(WINAPI* BltFunc)(
	LPDIRECTDRAWSURFACE7 self,
	LPRECT destRect,
	LPDIRECTDRAWSURFACE7 srcSurface,
	LPRECT srcRect,
	DWORD flags,
	LPDDBLTFX fx);

typedef HRESULT(WINAPI* FlipFunc)(
	LPDIRECTDRAWSURFACE7 self,
	LPDIRECTDRAWSURFACE7 target,
	DWORD flags);

typedef HRESULT(WINAPI* BltFastFunc)(
	LPDIRECTDRAWSURFACE7 self,
	DWORD x,
	DWORD y,
	LPDIRECTDRAWSURFACE7 srcSurface,
	LPRECT srcRect,
	DWORD flags);

typedef BOOL(WINAPI* GetCursorPosFunc)(LPPOINT lpPoint);
typedef BOOL(WINAPI* SwapBuffersFunc)(HDC hdc);

void Zoomer::UpdateClientCache(HWND hWnd)
{
	if (!hWnd) return;
	GetClientRect(hWnd, &g_clientRect);
	g_clientWidth = g_clientRect.right - g_clientRect.left;
	g_clientHeight = g_clientRect.bottom - g_clientRect.top;
	g_mapRight = g_clientWidth - SIDEBAR_WIDTH;
	g_mapBottom = g_clientHeight - BOTTOM_BAR_HEIGHT + GUARD_LINES;
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

	if (g_clientWidth > 0 && g_clientHeight > 0)
	{
		if ((int)desc.dwWidth == g_clientWidth && (int)desc.dwHeight == g_clientHeight)
		{
			g_knownPrimaryOrBack.insert(surface);
			return true;
		}
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

void Zoomer::UpdateLerp()
{
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
		g_invZoom.store(1.0f / curZoom);
	}
}

void Zoomer::UpdateLerpFrameIndependent()
{
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

	if (curZoom != tgtZoom)
	{
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
		g_zoom.store(curZoom);
		g_invZoom.store(1.0f / curZoom);
	}
}

void Zoomer::ResetZoom()
{
	g_zoom.store(ZOOM_DEFAULT);
	g_targetZoom.store(ZOOM_DEFAULT);
	g_invZoom.store(1.0f);
	LOG("Zoom reset to %.1f", ZOOM_DEFAULT);
}

DDrawWrapper Zoomer::DetectDDrawWrapper()
{
	HMODULE hDdraw = GetModuleHandleA("ddraw.dll");
	if (!hDdraw) return DDrawWrapper::Original;

	char path[MAX_PATH];
	GetModuleFileNameA(hDdraw, path, MAX_PATH);

	char sysPath[MAX_PATH];
	GetSystemDirectoryA(sysPath, MAX_PATH);

	if (_strnicmp(path, sysPath, strlen(sysPath)) == 0)
	{
		LOG("Original ddraw.dll: %s", path);
		return DDrawWrapper::Original;
	}

	LOG("Third-party ddraw wrapper: %s", path);

	if (strstr(path, "cnc-ddraw"))
	{
		LOG("WARNING: cnc-ddraw detected. ViewCtrl zoom may conflict.");
		return DDrawWrapper::CncDDraw;
	}
	if (strstr(path, "ts-ddraw"))
	{
		LOG("WARNING: ts-ddraw detected. ViewCtrl zoom may conflict.");
		return DDrawWrapper::TsDDraw;
	}
	if (strstr(path, "DDrawCompat"))
	{
		LOG("WARNING: DDrawCompat detected. VTable hooks may be overridden.");
		return DDrawWrapper::DDrawCompat;
	}

	LOG("Unknown ddraw wrapper detected.");
	return DDrawWrapper::Unknown;
}

void Zoomer::UpdateMonitorInfo(HWND hWnd)
{
	if (!hWnd) return;

	HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
	if (!hMon) return;

	MONITORINFO mi = {};
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoA(hMon, &mi))
	{
		g_hMonitor = hMon;
		g_monitorRect = mi.rcMonitor;
		g_monitorWidth = g_monitorRect.right - g_monitorRect.left;
		g_monitorHeight = g_monitorRect.bottom - g_monitorRect.top;
		LOG("Monitor: %dx%d at (%ld,%ld)",
			g_monitorWidth, g_monitorHeight,
			g_monitorRect.left, g_monitorRect.top);
	}
}

HRESULT WINAPI Zoomer::HookedBlt(
	LPDIRECTDRAWSURFACE7 self,
	LPRECT destRect,
	LPDIRECTDRAWSURFACE7 srcSurface,
	LPRECT srcRect,
	DWORD flags,
	LPDDBLTFX fx)
{
	UpdateLerpFrameIndependent();
	float curZoom = g_zoom.load();

	bool isPrimary = IsPrimaryOrBackBuffer(self);
	bool isMap = destRect ? IsMapArea(destRect) : false;

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

HRESULT WINAPI Zoomer::HookedFlip(
	LPDIRECTDRAWSURFACE7 self,
	LPDIRECTDRAWSURFACE7 target,
	DWORD flags)
{
	UpdateLerpFrameIndependent();
	float curZoom = g_zoom.load();

	bool isPrimary = IsPrimaryOrBackBuffer(self);

	if (curZoom != ZOOM_DEFAULT && isPrimary)
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
		RECT zoomDest = { 0, 0, g_mapRight, g_mapBottom };

		LOG("HookedFlip ZOOM: src=(%ld,%ld,%ld,%ld) dest=(%ld,%ld,%ld,%ld) zoom=%.3f",
			zoomSrc.left, zoomSrc.top, zoomSrc.right, zoomSrc.bottom,
			zoomDest.left, zoomDest.top, zoomDest.right, zoomDest.bottom,
			curZoom);

		if (OriginalBlt)
			return ((BltFunc)OriginalBlt)(self, &zoomDest, nullptr, &zoomSrc, DDBLT_WAIT, nullptr);
	}

	if (!OriginalFlip)
	{
		LOG("HookedFlip: OriginalFlip is NULL! self=%p", self);
		return DD_OK;
	}

	return ((FlipFunc)OriginalFlip)(self, target, flags);
}

HRESULT WINAPI Zoomer::HookedBltFast(
	LPDIRECTDRAWSURFACE7 self,
	DWORD x,
	DWORD y,
	LPDIRECTDRAWSURFACE7 srcSurface,
	LPRECT srcRect,
	DWORD flags)
{
	UpdateLerpFrameIndependent();
	float curZoom = g_zoom.load();

	bool isPrimary = IsPrimaryOrBackBuffer(self);

	if (curZoom != ZOOM_DEFAULT && isPrimary && srcRect)
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

		LOG("HookedBltFast ZOOM: src=(%ld,%ld,%ld,%ld) pos=(%lu,%lu) zoom=%.3f",
			zoomSrc.left, zoomSrc.top, zoomSrc.right, zoomSrc.bottom,
			x, y, curZoom);

		if (OriginalBlt)
			return ((BltFunc)OriginalBlt)(self, nullptr, srcSurface, &zoomSrc, DDBLT_WAIT, nullptr);
	}

	if (!OriginalBltFast)
	{
		LOG("HookedBltFast: OriginalBltFast is NULL! self=%p", self);
		return DD_OK;
	}

	return ((BltFastFunc)OriginalBltFast)(self, x, y, srcSurface, srcRect, flags);
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

typedef BOOL(WINAPI* BitBltFunc)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef BOOL(WINAPI* StretchBltFunc)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);

BOOL WINAPI Zoomer::HookedBitBlt(
	HDC hdc, int x, int y, int cx, int cy,
	HDC hdcSrc, int x1, int y1, DWORD rop)
{
	UpdateLerpFrameIndependent();
	float curZoom = g_zoom.load();

	if (curZoom != ZOOM_DEFAULT && g_hWnd && hdc)
	{
		HWND destWnd = WindowFromDC(hdc);
		if (destWnd == g_hWnd)
		{
			float invZoom = 1.0f / curZoom;
			float cx_f = (float)g_centerX.load();
			float cy_f = (float)g_centerY.load();

			int srcLeft = x1 + lroundf(((float)x - cx_f) * (1.0f - invZoom));
			int srcTop  = y1 + lroundf(((float)y - cy_f) * (1.0f - invZoom));
			int srcWidth  = lroundf((float)cx * invZoom);
			int srcHeight = lroundf((float)cy * invZoom);

			if (srcLeft < 0) srcLeft = 0;
			if (srcTop < 0) srcTop = 0;

			LOG("HookedBitBlt ZOOM: src=(%d,%d,%dx%d) dest=(%d,%d,%dx%d) zoom=%.3f",
				srcLeft, srcTop, srcWidth, srcHeight, x, y, cx, cy, curZoom);

			return ((StretchBltFunc)OriginalStretchBlt)(
				hdc, x, y, cx, cy, hdcSrc, srcLeft, srcTop, srcWidth, srcHeight, rop);
		}
	}

	if (!OriginalBitBlt) return FALSE;
	return ((BitBltFunc)OriginalBitBlt)(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
}

BOOL WINAPI Zoomer::HookedStretchBlt(
	HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
	HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, DWORD rop)
{
	UpdateLerpFrameIndependent();
	float curZoom = g_zoom.load();

	if (curZoom != ZOOM_DEFAULT && g_hWnd && hdcDest)
	{
		HWND destWnd = WindowFromDC(hdcDest);
		if (destWnd == g_hWnd)
		{
			float invZoom = 1.0f / curZoom;
			float cx_f = (float)g_centerX.load();
			float cy_f = (float)g_centerY.load();

			int newSrcLeft = xSrc + lroundf(((float)xDest - cx_f) * (1.0f - invZoom));
			int newSrcTop  = ySrc + lroundf(((float)yDest - cy_f) * (1.0f - invZoom));
			int newSrcWidth  = lroundf((float)wSrc * invZoom);
			int newSrcHeight = lroundf((float)hSrc * invZoom);

			if (newSrcLeft < 0) newSrcLeft = 0;
			if (newSrcTop < 0) newSrcTop = 0;

			LOG("HookedStretchBlt ZOOM: src=(%d,%d,%dx%d) dest=(%d,%d,%dx%d) zoom=%.3f",
				newSrcLeft, newSrcTop, newSrcWidth, newSrcHeight, xDest, yDest, wDest, hDest, curZoom);

			return ((StretchBltFunc)OriginalStretchBlt)(
				hdcDest, xDest, yDest, wDest, hDest, hdcSrc,
				newSrcLeft, newSrcTop, newSrcWidth, newSrcHeight, rop);
		}
	}

	if (!OriginalStretchBlt) return FALSE;
	return ((StretchBltFunc)OriginalStretchBlt)(
		hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
}

BOOL WINAPI Zoomer::HookedSwapBuffers(HDC hdc)
{
	UpdateLerpFrameIndependent();
	float curZoom = g_zoom.load();

	if (curZoom != ZOOM_DEFAULT && g_openglAvailable)
	{
		GLint viewport[4] = {};
		glGetIntegerv(GL_VIEWPORT, viewport);
		int fbW = viewport[2];
		int fbH = viewport[3];
		if (fbW <= 0 || fbH <= 0)
			goto passthrough;

		float invZoom = 1.0f / curZoom;
		float cx = (float)g_centerX.load();
		float cy = (float)g_centerY.load();

		float srcLeft   = cx - cx * invZoom;
		float srcTop    = cy - cy * invZoom;
		float srcRight  = srcLeft + (float)fbW * invZoom;
		float srcBottom = srcTop  + (float)fbH * invZoom;

		if (srcLeft < 0) srcLeft = 0;
		if (srcTop < 0)  srcTop = 0;
		if (srcRight > (float)fbW) srcRight = (float)fbW;
		if (srcBottom > (float)fbH) srcBottom = (float)fbH;

		int srcW = (int)(srcRight - srcLeft);
		int srcH = (int)(srcBottom - srcTop);
		if (srcW <= 0 || srcH <= 0)
			goto passthrough;

		static bool logged = false;
		if (!logged) {
			LOG("SWAPBUFFERS ZOOM: viewport=%dx%d src=(%.0f,%.0f,%.0f,%.0f) zoom=%.3f center=(%d,%d)",
				fbW, fbH, srcLeft, srcTop, srcRight, srcBottom, curZoom, (int)cx, (int)cy);
			logged = true;
		}

		static GLuint texName = 0;
		if (!texName)
			glGenTextures(1, &texName);

		glPushAttrib(GL_ENABLE_BIT | GL_TRANSFORM_BIT | GL_VIEWPORT_BIT | GL_TEXTURE_BIT);

		glViewport(0, 0, fbW, fbH);

		glBindTexture(GL_TEXTURE_2D, texName);
		glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, fbW, fbH, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
		glLoadIdentity();
		glOrtho(0, fbW, fbH, 0, -1, 1);

		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		glLoadIdentity();

		glDisable(GL_DEPTH_TEST);
		glDisable(GL_BLEND);
		glDisable(GL_LIGHTING);
		glEnable(GL_TEXTURE_2D);

		float u0 = srcLeft / (float)fbW;
		float v0 = srcBottom / (float)fbH;
		float u1 = srcRight / (float)fbW;
		float v1 = srcTop / (float)fbH;

		glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

		glBegin(GL_QUADS);
		glTexCoord2f(u0, v0); glVertex2f(0, 0);
		glTexCoord2f(u1, v0); glVertex2f((GLfloat)fbW, 0);
		glTexCoord2f(u1, v1); glVertex2f((GLfloat)fbW, (GLfloat)fbH);
		glTexCoord2f(u0, v1); glVertex2f(0, (GLfloat)fbH);
		glEnd();

		glPopMatrix();
		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
		glMatrixMode(GL_MODELVIEW);

		glPopAttrib();
	}

passthrough:
	if (!OriginalSwapBuffers)
	{
		LOG("HookedSwapBuffers: OriginalSwapBuffers is NULL! hdc=%p", hdc);
		return FALSE;
	}

	return ((SwapBuffersFunc)OriginalSwapBuffers)(hdc);
}

HRESULT WINAPI Zoomer::HookedPresent(
	IDirect3DDevice9* device,
	const RECT* pSourceRect,
	const RECT* pDestRect,
	HWND hDestWindowOverride,
	const RGNDATA* pDirtyRegion)
{
	if (g_useGScript || !device)
		goto pass_through;

	{
		UpdateLerpFrameIndependent();
		float curZoom = g_zoom.load();

		if (curZoom == ZOOM_DEFAULT)
			goto pass_through;

		IDirect3DSurface9* pBB = nullptr;
		if (FAILED(device->GetRenderTarget(0, &pBB)) || !pBB)
			goto pass_through;

		D3DSURFACE_DESC bbDesc = {};
		pBB->GetDesc(&bbDesc);

		int bbW = (int)bbDesc.Width;
		int bbH = (int)bbDesc.Height;
		int mapW = bbW - SIDEBAR_WIDTH;
		int mapH = bbH - BOTTOM_BAR_HEIGHT;
		if (mapW <= 0 || mapH <= 0) {
			pBB->Release();
			goto pass_through;
		}

		if (!g_pCaptureTex || g_captureTexW != bbW || g_captureTexH != bbH) {
			if (g_pCaptureTex) { g_pCaptureTex->Release(); g_pCaptureTex = nullptr; }
			if (FAILED(device->CreateTexture(bbW, bbH, 1, D3DUSAGE_RENDERTARGET,
				bbDesc.Format, D3DPOOL_DEFAULT, &g_pCaptureTex, nullptr))) {
				pBB->Release();
				goto pass_through;
			}
			g_captureTexW = bbW;
			g_captureTexH = bbH;
			LOG("Capture texture created: %dx%d fmt=%d", bbW, bbH, bbDesc.Format);
		}

		IDirect3DSurface9* pCapSurf = nullptr;
		g_pCaptureTex->GetSurfaceLevel(0, &pCapSurf);

		if (FAILED(device->StretchRect(pBB, nullptr, pCapSurf, nullptr, D3DTEXF_NONE))) {
			pCapSurf->Release();
			pBB->Release();
			goto pass_through;
		}

		IDirect3DStateBlock9* pSB = nullptr;
		device->CreateStateBlock(D3DSBT_ALL, &pSB);
		if (pSB) pSB->Capture();

		device->SetPixelShader(nullptr);
		device->SetVertexShader(nullptr);
		device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		device->SetTexture(0, g_pCaptureTex);
		device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		device->SetRenderState(D3DRS_ZENABLE, FALSE);
		device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		device->SetRenderState(D3DRS_LIGHTING, FALSE);
		device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		device->SetStreamSource(0, nullptr, 0, 0);

		ZoomVertex fullBB[4] = {
			{ 0.0f,        0.0f,        0.0f, 1.0f, 0.0f, 0.0f },
			{ (float)bbW,  0.0f,        0.0f, 1.0f, 1.0f, 0.0f },
			{ (float)bbW,  (float)bbH,  0.0f, 1.0f, 1.0f, 1.0f },
			{ 0.0f,        (float)bbH,  0.0f, 1.0f, 0.0f, 1.0f },
		};
		device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, fullBB, sizeof(ZoomVertex));

		device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

		float invZoom = 1.0f / curZoom;

		float cursorX = (float)g_centerX.load();
		float cursorY = (float)g_centerY.load();
		if (cursorX < 0.0f) cursorX = 0.0f;
		if (cursorX > (float)mapW) cursorX = (float)mapW;
		if (cursorY < 0.0f) cursorY = 0.0f;
		if (cursorY > (float)mapH) cursorY = (float)mapH;

		float halfW = (float)mapW * 0.5f * invZoom;
		float halfH = (float)mapH * 0.5f * invZoom;

		float srcLeft = cursorX - halfW;
		float srcTop = cursorY - halfH;
		float srcRight = cursorX + halfW;
		float srcBottom = cursorY + halfH;

		if (srcLeft < 0.0f) { srcLeft = 0.0f; srcRight =2.0f * halfW; }
		if (srcTop < 0.0f) { srcTop = 0.0f; srcBottom =2.0f * halfH; }
		if (srcRight > (float)mapW) { srcRight = (float)mapW; srcLeft = srcRight -2.0f * halfW; }
		if (srcBottom > (float)mapH) { srcBottom = (float)mapH; srcTop = srcBottom -2.0f * halfH; }
		if (srcLeft < 0.0f) srcLeft = 0.0f;
		if (srcTop < 0.0f) srcTop = 0.0f;

		float u0 = srcLeft / (float)bbW;
		float v0 = srcTop / (float)bbH;
		float u1 = srcRight / (float)bbW;
		float v1 = srcBottom / (float)bbH;

		ZoomVertex mapQuad[4] = {
			{ 0.0f,           0.0f,           0.0f, 1.0f, u0, v0 },
			{ (float)mapW,    0.0f,           0.0f, 1.0f, u1, v0 },
			{ (float)mapW,    (float)mapH,    0.0f, 1.0f, u1, v1 },
			{ 0.0f,           (float)mapH,    0.0f, 1.0f, u0, v1 },
		};
		device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, mapQuad, sizeof(ZoomVertex));

		device->SetTexture(0, nullptr);

		if (pSB) {
			pSB->Apply();
			pSB->Release();
		}

		pCapSurf->Release();
		pBB->Release();

		static int logCounter = 0;
		if (logCounter++ % 60 == 0) {
			LOG("CAPTURE ZOOM: zoom=%.3f cursor=(%.0f,%.0f) src=(%.0f,%.0f,%.0f,%.0f) map=%dx%d",
				curZoom, cursorX, cursorY, srcLeft, srcTop, srcRight, srcBottom, mapW, mapH);
		}

		return ((PresentFunc)OriginalPresent)(device, nullptr, nullptr, hDestWindowOverride, nullptr);
	}

pass_through:
	if (!OriginalPresent)
	{
		LOG("HookedPresent: OriginalPresent is NULL!");
		return D3DERR_INVALIDCALL;
	}

	return ((PresentFunc)OriginalPresent)(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

HRESULT WINAPI Zoomer::HookedReset(
	IDirect3DDevice9* device,
	D3DPRESENT_PARAMETERS* pPresentationParameters)
{
	LOG("D3D9 Reset called");

	if (g_pCaptureTex) { g_pCaptureTex->Release(); g_pCaptureTex = nullptr; }
	g_captureTexW = 0;
	g_captureTexH = 0;

	if (!OriginalReset)
	{
		LOG("HookedReset: OriginalReset is NULL!");
		return D3DERR_INVALIDCALL;
	}

	return ((ResetFunc)OriginalReset)(device, pPresentationParameters);
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
			int mapW = g_clientWidth - SIDEBAR_WIDTH;
			int mapH = g_clientHeight - BOTTOM_BAR_HEIGHT;

			if (mapW > 0 && mapH > 0)
			{
				float curZoom = g_zoom.load();
				float visW = (float)mapW / curZoom;
				float visH = (float)mapH / curZoom;
				float stepX = visW * 0.2f;
				float stepY = visH * 0.2f;

				float cx = (float)g_centerX.load();
				float cy = (float)g_centerY.load();
				bool handled = true;

				switch (wParam)
				{
				case VK_LEFT:  cx -= stepX; break;
				case VK_RIGHT: cx += stepX; break;
				case VK_UP:    cy -= stepY; break;
				case VK_DOWN:  cy += stepY; break;
				default: handled = false; break;
				}

				if (handled)
				{
					float halfW = visW * 0.5f;
					float halfH = visH * 0.5f;
					if (cx < halfW) cx = halfW;
					if (cy < halfH) cy = halfH;
					if (cx > (float)mapW - halfW) cx = (float)mapW - halfW;
					if (cy > (float)mapH - halfH) cy = (float)mapH - halfH;

					g_centerX = (LONG)cx;
					g_centerY = (LONG)cy;
					return 0;
				}
			}
		}
	}

	if (msg == WM_SIZE || msg == WM_MOVE)
	{
		UpdateClientCache(hWnd);
		UpdateCenter(hWnd);
		UpdateMonitorInfo(hWnd);
		g_knownPrimaryOrBack.clear();
	}

	if (g_zoom != ZOOM_DEFAULT)
	{
		int visualMapRight = g_clientWidth - SIDEBAR_WIDTH;
		int visualMapBottom = g_clientHeight - BOTTOM_BAR_HEIGHT;

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

			if (clientX < visualMapRight && clientY < visualMapBottom)
			{
				int originalX = g_centerX + lroundf((clientX - g_centerX) * g_invZoom.load());
				int originalY = g_centerY + lroundf((clientY - g_centerY) * g_invZoom.load());
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

	g_ddrawWrapper = DetectDDrawWrapper();

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

	RECT winRect = {};
	GetWindowRect(g_hWnd, &winRect);
	LOG("Window screen rect: (%ld,%ld) to (%ld,%ld) size=%ldx%ld",
		winRect.left, winRect.top, winRect.right, winRect.bottom,
		winRect.right - winRect.left, winRect.bottom - winRect.top);

	UpdateClientCache(g_hWnd);
	UpdateCenter(g_hWnd);
	UpdateMonitorInfo(g_hWnd);
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
	LOG("VTable[%d] patched: HookedBlt=%p", BLT_VTABLE_INDEX, HookedBlt);

	OriginalFlip = g_vtable[FLIP_VTABLE_INDEX];
	VirtualProtect(&g_vtable[FLIP_VTABLE_INDEX], sizeof(void*), PAGE_READWRITE, &oldProtect);
	g_vtable[FLIP_VTABLE_INDEX] = reinterpret_cast<void*>(HookedFlip);
	VirtualProtect(&g_vtable[FLIP_VTABLE_INDEX], sizeof(void*), oldProtect, &oldProtect);
	LOG("VTable[%d] patched: HookedFlip=%p", FLIP_VTABLE_INDEX, HookedFlip);

	OriginalBltFast = g_vtable[BLTFAST_VTABLE_INDEX];
	VirtualProtect(&g_vtable[BLTFAST_VTABLE_INDEX], sizeof(void*), PAGE_READWRITE, &oldProtect);
	g_vtable[BLTFAST_VTABLE_INDEX] = reinterpret_cast<void*>(HookedBltFast);
	VirtualProtect(&g_vtable[BLTFAST_VTABLE_INDEX], sizeof(void*), oldProtect, &oldProtect);
	LOG("VTable[%d] patched: HookedBltFast=%p", BLTFAST_VTABLE_INDEX, HookedBltFast);

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

		MH_STATUS createBitBlt = MH_CreateHookApi(
			L"gdi32.dll",
			"BitBlt",
			HookedBitBlt,
			(void**)&OriginalBitBlt
		);
		LOG("MH_CreateHookApi BitBlt: %s (%d)", MH_StatusToString(createBitBlt), createBitBlt);

		MH_STATUS createStretchBlt = MH_CreateHookApi(
			L"gdi32.dll",
			"StretchBlt",
			HookedStretchBlt,
			(void**)&OriginalStretchBlt
		);
		LOG("MH_CreateHookApi StretchBlt: %s (%d)", MH_StatusToString(createStretchBlt), createStretchBlt);

		MH_STATUS createSwapBuffers = MH_CreateHookApi(
			L"opengl32.dll",
			"wglSwapBuffers",
			HookedSwapBuffers,
			(void**)&OriginalSwapBuffers
		);
		LOG("MH_CreateHookApi wglSwapBuffers: %s (%d)", MH_StatusToString(createSwapBuffers), createSwapBuffers);

		HMODULE hD3D9 = LoadLibraryA("d3d9.dll");
		LOG("d3d9.dll loaded: %s", hD3D9 ? "YES" : "NO");

		if (hD3D9)
		{
			typedef IDirect3D9* (WINAPI* Direct3DCreate9Func)(UINT);
			Direct3DCreate9Func pDirect3DCreate9 = (Direct3DCreate9Func)GetProcAddress(hD3D9, "Direct3DCreate9");

			if (pDirect3DCreate9)
			{
				IDirect3D9* d3d = pDirect3DCreate9(D3D_SDK_VERSION);
				if (d3d)
				{
					WNDCLASSEX wcDummy = { sizeof(wcDummy) };
					wcDummy.lpfnWndProc = DefWindowProcA;
					wcDummy.hInstance = GetModuleHandleA(nullptr);
					wcDummy.lpszClassName = "ViewCtrlD3D9Dummy";
					RegisterClassExA(&wcDummy);
					HWND hDummyWnd = CreateWindowExA(0, "ViewCtrlD3D9Dummy", "", WS_OVERLAPPEDWINDOW,
						0, 0, 1, 1, nullptr, nullptr, wcDummy.hInstance, nullptr);

					D3DPRESENT_PARAMETERS d3dpp = {};
					d3dpp.Windowed = TRUE;
					d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
					d3dpp.hDeviceWindow = hDummyWnd;
					d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
					d3dpp.BackBufferWidth = 2;
					d3dpp.BackBufferHeight = 2;
					d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

					IDirect3DDevice9* dummyDevice = nullptr;
					HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hDummyWnd,
						D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &dummyDevice);

					if (SUCCEEDED(hr) && dummyDevice)
					{
						void** vtable = *reinterpret_cast<void***>(dummyDevice);
						void* pPresentFunc = vtable[D3D9_PRESENT_VTABLE_INDEX];
						LOG("D3D9 device vtable=%p, Present=%p (index %d)", vtable, pPresentFunc, D3D9_PRESENT_VTABLE_INDEX);

						MH_STATUS createPresent = MH_CreateHook(
							pPresentFunc,
							reinterpret_cast<LPVOID>(HookedPresent),
							reinterpret_cast<LPVOID*>(&OriginalPresent)
						);
						LOG("MH_CreateHook D3D9 Present: %s (%d)", MH_StatusToString(createPresent), createPresent);

						void* pResetFunc = vtable[D3D9_RESET_VTABLE_INDEX];
						MH_STATUS createReset = MH_CreateHook(
							pResetFunc,
							reinterpret_cast<LPVOID>(HookedReset),
							reinterpret_cast<LPVOID*>(&OriginalReset)
						);
						LOG("MH_CreateHook D3D9 Reset: %s (%d)", MH_StatusToString(createReset), createReset);

						dummyDevice->Release();
					}
					else
					{
						LOG("D3D9 CreateDevice failed: 0x%08lX", hr);
					}

					if (hDummyWnd) DestroyWindow(hDummyWnd);
					UnregisterClassA("ViewCtrlD3D9Dummy", wcDummy.hInstance);
					d3d->Release();
				}
			}
		}

		MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
		LOG("MH_EnableHook: %s (%d)", MH_StatusToString(enableStatus), enableStatus);
	}
#endif

	g_openglAvailable = (GetModuleHandleA("opengl32.dll") != nullptr);
	LOG("opengl32.dll loaded: %s", g_openglAvailable ? "YES" : "NO");

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
		LOG("GScript.ext not found — using D3D9 fallback mode");
	}

	ShowCursor(FALSE);
	g_initialized = mhOk || (g_vtable != nullptr);
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

		if (OriginalFlip)
		{
			LOG("Restoring vtable[%d]: %p -> %p", FLIP_VTABLE_INDEX, HookedFlip, OriginalFlip);
			VirtualProtect(&g_vtable[FLIP_VTABLE_INDEX], sizeof(void*), PAGE_READWRITE, &oldProtect);
			g_vtable[FLIP_VTABLE_INDEX] = OriginalFlip;
			VirtualProtect(&g_vtable[FLIP_VTABLE_INDEX], sizeof(void*), oldProtect, &oldProtect);
		}

		if (OriginalBltFast)
		{
			LOG("Restoring vtable[%d]: %p -> %p", BLTFAST_VTABLE_INDEX, HookedBltFast, OriginalBltFast);
			VirtualProtect(&g_vtable[BLTFAST_VTABLE_INDEX], sizeof(void*), PAGE_READWRITE, &oldProtect);
			g_vtable[BLTFAST_VTABLE_INDEX] = OriginalBltFast;
			VirtualProtect(&g_vtable[BLTFAST_VTABLE_INDEX], sizeof(void*), oldProtect, &oldProtect);
		}

		g_vtable = nullptr;
		OriginalBlt = nullptr;
		OriginalFlip = nullptr;
		OriginalBltFast = nullptr;
		g_surfaceStates.clear();
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
	g_invZoom.store(1.0f);
	g_knownPrimaryOrBack.clear();
	g_mapRight = 0;
	g_mapBottom = 0;
	g_destDetected = false;
	OriginalGetCursorPos = nullptr;
	g_initialized = false;
	g_perfCounterReady = false;
	g_lastLerpTime = {};
	g_perfFrequency = {};
	g_hMonitor = nullptr;
	g_monitorRect = {};
	g_monitorWidth = 0;
	g_monitorHeight = 0;
	g_ddrawWrapper = DDrawWrapper::Unknown;
	g_openglAvailable = false;

	if (g_pCaptureTex) { g_pCaptureTex->Release(); g_pCaptureTex = nullptr; }
	g_captureTexW = 0;
	g_captureTexH = 0;
	OriginalPresent = nullptr;

	LOG("Shutdown complete");
}
