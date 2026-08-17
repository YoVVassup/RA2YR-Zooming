#pragma once

#include <ddraw.h>

class MockSurface : public IDirectDrawSurface7
{
public:
    DDSURFACEDESC2 mockDesc = {};
    bool isPrimary = false;
    bool isBackBuffer = false;
    bool getSurfaceDescFail = false;

    MockSurface()
    {
        ZeroMemory(&mockDesc, sizeof(mockDesc));
        mockDesc.dwSize = sizeof(mockDesc);
        mockDesc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        mockDesc.dwWidth = 64;
        mockDesc.dwHeight = 64;
        mockDesc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
    }

    void SetAsPrimary() { isPrimary = true; mockDesc.ddsCaps.dwCaps |= DDSCAPS_PRIMARYSURFACE; }
    void SetAsBackBuffer() { isBackBuffer = true; mockDesc.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 0; }

    // IDirectDrawSurface7
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(IDirectDrawSurface7*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE AddOverlayDirtyRect(LPRECT) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Blt(LPRECT, IDirectDrawSurface7*, LPRECT, DWORD, LPDDBLTFX) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE BltBatch(LPDDBLTBATCH, DWORD, DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE BltFast(DWORD, DWORD, IDirectDrawSurface7*, LPRECT, DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD, IDirectDrawSurface7*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE EnumAttachedSurfaces(LPVOID, LPDDENUMSURFACESCALLBACK7) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK7) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Flip(IDirectDrawSurface7*, DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS2, IDirectDrawSurface7**) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS2 caps) override { if (caps) caps->dwCaps = mockDesc.ddsCaps.dwCaps; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetClipper(IDirectDrawClipper**) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD, LPDDCOLORKEY) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetDC(HDC*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetOverlayPosition(LPLONG, LPLONG) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetPalette(IDirectDrawPalette**) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT) override { return DD_OK; }

    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC2 desc) override
    {
        if (getSurfaceDescFail) return DDERR_GENERIC;
        if (!desc) return DDERR_INVALIDPARAMS;
        DWORD copySize = desc->dwSize < mockDesc.dwSize ? desc->dwSize : mockDesc.dwSize;
        memcpy(desc, &mockDesc, copySize);
        return DD_OK;
    }

    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, LPDDSURFACEDESC2) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE IsLost() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Lock(LPRECT, LPDDSURFACEDESC2, DWORD, HANDLE) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Restore() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetClipper(IDirectDrawClipper*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD, LPDDCOLORKEY) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetOverlayPosition(LONG, LONG) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetPalette(IDirectDrawPalette*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Unlock(LPRECT) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE UpdateOverlay(LPRECT, IDirectDrawSurface7*, LPRECT, DWORD, LPDDOVERLAYFX) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE UpdateOverlayDisplay(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE UpdateOverlayZOrder(DWORD, IDirectDrawSurface7*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetDDInterface(LPVOID*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE PageLock(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE PageUnlock(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetSurfaceDesc(LPDDSURFACEDESC2, DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, LPVOID, DWORD, DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, LPVOID, LPDWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetUniquenessValue(LPDWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE ChangeUniquenessValue() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetPriority(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LPDWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetLOD(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetLOD(LPDWORD) override { return DD_OK; }
};
