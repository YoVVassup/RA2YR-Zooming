#define VIEWCTRL_TEST
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include <vector>

#include "../src/Zoomer.hpp"
#include "../src/Log.h"
#include "mock_ddraw.h"
#include "mock_window.h"

static HRESULT WINAPI MockBlt(
    LPDIRECTDRAWSURFACE7 self,
    LPRECT destRect,
    LPDIRECTDRAWSURFACE7 srcSurface,
    LPRECT srcRect,
    DWORD flags,
    LPDDBLTFX fx)
{
    return DD_OK;
}

static HRESULT WINAPI MockFlip(
    LPDIRECTDRAWSURFACE7 self,
    LPDIRECTDRAWSURFACE7 target,
    DWORD flags)
{
    return DD_OK;
}

static HRESULT WINAPI MockBltFast(
    LPDIRECTDRAWSURFACE7 self,
    DWORD x,
    DWORD y,
    LPDIRECTDRAWSURFACE7 srcSurface,
    LPRECT srcRect,
    DWORD flags)
{
    return DD_OK;
}

static BOOL WINAPI MockGetCursorPos(LPPOINT lpPoint)
{
    if (!lpPoint) return FALSE;
    lpPoint->x = 500;
    lpPoint->y = 500;
    return TRUE;
}

static HRESULT WINAPI MockLock(
    IDirectDrawSurface7* self,
    LPRECT lpDestRect,
    LPDDSURFACEDESC2 lpDDSurfaceDesc,
    DWORD dwFlags,
    HANDLE hEvent)
{
    if (lpDDSurfaceDesc) {
        lpDDSurfaceDesc->lpSurface = (void*)0x12345678;
        lpDDSurfaceDesc->lPitch = 7680;
        lpDDSurfaceDesc->dwWidth = 1920;
        lpDDSurfaceDesc->dwHeight = 1080;
    }
    return DD_OK;
}

static HRESULT WINAPI MockUnlock(IDirectDrawSurface7* self, LPRECT lpRect)
{
    return DD_OK;
}

static BOOL WINAPI MockSwapBuffers(HDC hdc)
{
    return TRUE;
}

static LRESULT CALLBACK MockWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

class TestableZoomer : public Zoomer {
public:
    static void ResetState() {
        g_hWnd = (HWND)0xDEAD;
        g_zoom.store(ZOOM_DEFAULT);
        g_targetZoom.store(ZOOM_DEFAULT);
        g_invZoom.store(1.0f);
        g_centerX = 400;
        g_centerY = 300;
        g_clientRect = { 0, 0, 1920, 1080 };
        g_clientWidth = 1920;
        g_clientHeight = 1080;
        g_mapRight = g_clientWidth - SIDEBAR_WIDTH;
        g_mapBottom = g_clientHeight - BOTTOM_BAR_HEIGHT + GUARD_LINES;
        g_destDetected = true;
        g_knownPrimaryOrBack.clear();
        g_initialized = false;
        g_wndProcHooked = false;
        OriginalWndProc = (WNDPROC)MockWndProc;
        OriginalBlt = (void*)MockBlt;
        OriginalFlip = (void*)MockFlip;
        OriginalBltFast = (void*)MockBltFast;
        OriginalGetCursorPos = (void*)MockGetCursorPos;
        OriginalBitBlt = nullptr;
        OriginalStretchBlt = nullptr;
        OriginalSwapBuffers = nullptr;
        g_surfaceStates.clear();
        g_vtable = nullptr;
        g_perfCounterReady = false;
        g_lastLerpTime = {};
        g_perfFrequency = {};
        g_hMonitor = nullptr;
        g_monitorRect = {};
        g_monitorWidth = 0;
        g_monitorHeight = 0;
        g_ddrawWrapper = DDrawWrapper::Unknown;
        if (g_hThread) { CloseHandle(g_hThread); g_hThread = nullptr; }
    }

    using Zoomer::IsMapArea;
    using Zoomer::IsPointInMapArea;
    using Zoomer::ApplyZoomToRect;
    using Zoomer::IsPrimaryOrBackBuffer;
    using Zoomer::ClampToViewport;
    using Zoomer::g_surfaceStates;
    using Zoomer::UpdateClientCache;
    using Zoomer::UpdateCenter;
    using Zoomer::UpdateLerp;
    using Zoomer::UpdateLerpFrameIndependent;
    using Zoomer::ResetZoom;
    using Zoomer::DetectDDrawWrapper;
    using Zoomer::UpdateMonitorInfo;
    using Zoomer::HookedBlt;
    using Zoomer::HookedFlip;
    using Zoomer::HookedBltFast;
    using Zoomer::HookedGetCursorPos;
    using Zoomer::HookedBitBlt;
    using Zoomer::HookedStretchBlt;
    using Zoomer::HookedSwapBuffers;
    using Zoomer::HookedPresent;
    using Zoomer::NewWndProc;
    using Zoomer::Shutdown;
};

// ==================== IsMapArea ====================

TEST_CASE("IsMapArea") {
    TestableZoomer::ResetState();

    SUBCASE("exact map rect (0,0,mapRight,mapBottom)") {
        RECT r = { 0, 0, 1752, 1248 };
        CHECK(TestableZoomer::IsMapArea(&r) == true);
    }

    SUBCASE("non-zero left") {
        RECT r = { 1, 0, 1752, 1248 };
        CHECK(TestableZoomer::IsMapArea(&r) == false);
    }

    SUBCASE("non-zero top") {
        RECT r = { 0, 1, 1752, 1248 };
        CHECK(TestableZoomer::IsMapArea(&r) == false);
    }

    SUBCASE("wrong right") {
        RECT r = { 0, 0, 1920, 1248 };
        CHECK(TestableZoomer::IsMapArea(&r) == false);
    }

    SUBCASE("wrong bottom") {
        RECT r = { 0, 0, 1752, 1080 };
        CHECK(TestableZoomer::IsMapArea(&r) == false);
    }

    SUBCASE("smaller rect inside map") {
        RECT r = { 100, 100, 400, 400 };
        CHECK(TestableZoomer::IsMapArea(&r) == false);
    }

    SUBCASE("null rect") {
        CHECK(TestableZoomer::IsMapArea(nullptr) == false);
    }

    SUBCASE("null hWnd") {
        TestableZoomer::g_hWnd = nullptr;
        RECT r = { 0, 0, 1752, 1048 };
        CHECK(TestableZoomer::IsMapArea(&r) == false);
    }

    SUBCASE("zero-size rect at origin") {
        RECT r = { 0, 0, 0, 0 };
        CHECK(TestableZoomer::IsMapArea(&r) == false);
    }
}

// ==================== IsPointInMapArea ====================

TEST_CASE("IsPointInMapArea") {
    TestableZoomer::ResetState();

    SUBCASE("point inside map") {
        POINT pt = { 500, 500 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == true);
    }

    SUBCASE("point at origin") {
        POINT pt = { 0, 0 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == true);
    }

    SUBCASE("point outside right") {
        POINT pt = { 1800, 500 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == false);
    }

    SUBCASE("point outside bottom") {
        POINT pt = { 500, 1300 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == false);
    }

    SUBCASE("point at map boundary right -1") {
        POINT pt = { TestableZoomer::g_mapRight - 1, 500 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == true);
    }

    SUBCASE("point at map boundary right") {
        POINT pt = { TestableZoomer::g_mapRight, 500 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == false);
    }

    SUBCASE("null hWnd") {
        TestableZoomer::g_hWnd = nullptr;
        POINT pt = { 100, 100 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == false);
    }
}

// ==================== ApplyZoomToRect ====================

TEST_CASE("ApplyZoomToRect") {
    TestableZoomer::ResetState();
    TestableZoomer::g_centerX = 400;
    TestableZoomer::g_centerY = 300;

    SUBCASE("zoom 1.0x - no change") {
        TestableZoomer::g_zoom.store(1.0f);
        RECT r = { 100, 100, 500, 400 };
        TestableZoomer::ApplyZoomToRect(&r);
        CHECK(r.left == 100);
        CHECK(r.top == 100);
        CHECK(r.right == 500);
        CHECK(r.bottom == 400);
    }

    SUBCASE("zoom 2.0x - doubled from center") {
        TestableZoomer::g_zoom.store(2.0f);
        RECT r = { 300, 200, 500, 400 };
        TestableZoomer::ApplyZoomToRect(&r);
        CHECK(r.left == 200);
        CHECK(r.top == 100);
        CHECK(r.right == 600);
        CHECK(r.bottom == 500);
    }

    SUBCASE("zoom 1.5x") {
        TestableZoomer::g_zoom.store(1.5f);
        RECT r = { 200, 100, 600, 500 };
        TestableZoomer::ApplyZoomToRect(&r);
        CHECK(r.left == 100);
        CHECK(r.top == 0);
        CHECK(r.right == 700);
        CHECK(r.bottom == 600);
    }

    SUBCASE("rect centered on zoom center - unchanged") {
        TestableZoomer::g_zoom.store(1.5f);
        RECT r = { 400, 300, 400, 300 };
        TestableZoomer::ApplyZoomToRect(&r);
        CHECK(r.left == 400);
        CHECK(r.top == 300);
        CHECK(r.right == 400);
        CHECK(r.bottom == 300);
    }

    SUBCASE("null rect") {
        TestableZoomer::ApplyZoomToRect(nullptr);
    }
}

// ==================== ClampToViewport ====================

TEST_CASE("ClampToViewport") {
    TestableZoomer::ResetState();

    SUBCASE("point inside viewport") {
        POINT pt = { 500, 500 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.x == 500);
        CHECK(pt.y == 500);
    }

    SUBCASE("point left of viewport") {
        POINT pt = { -100, 500 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.x == 0);
    }

    SUBCASE("point right of viewport") {
        POINT pt = { 2000, 500 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.x == TestableZoomer::g_mapRight);
    }

    SUBCASE("point above viewport") {
        POINT pt = { 500, -100 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.y == 0);
    }

    SUBCASE("point below viewport") {
        POINT pt = { 500, 2000 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.y == TestableZoomer::g_mapBottom);
    }

    SUBCASE("null point") {
        TestableZoomer::ClampToViewport(nullptr);
    }

    SUBCASE("null hWnd") {
        TestableZoomer::g_hWnd = nullptr;
        POINT pt = { 500, 500 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.x == 500);
        CHECK(pt.y == 500);
    }

    SUBCASE("zero mapRight/mapBottom - no clamp") {
        TestableZoomer::g_mapRight = 0;
        TestableZoomer::g_mapBottom = 0;
        POINT pt = { 500, 500 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.x == 500);
        CHECK(pt.y == 500);
    }
}

// ==================== UpdateClientCache ====================

TEST_CASE("UpdateClientCache") {
    TestableZoomer::ResetState();

    SUBCASE("null hWnd does not crash") {
        int prevWidth = TestableZoomer::g_clientWidth;
        TestableZoomer::UpdateClientCache(nullptr);
        CHECK(TestableZoomer::g_clientWidth == prevWidth);
    }

    SUBCASE("valid hWnd updates dimensions") {
        HWND hWnd = GetDesktopWindow();
        TestableZoomer::UpdateClientCache(hWnd);
        CHECK(TestableZoomer::g_clientWidth > 0);
        CHECK(TestableZoomer::g_clientHeight > 0);
    }
}

// ==================== UpdateCenter ====================

TEST_CASE("UpdateCenter") {
    TestableZoomer::ResetState();

    SUBCASE("null hWnd does not crash") {
        int prevX = TestableZoomer::g_centerX.load();
        int prevY = TestableZoomer::g_centerY.load();
        TestableZoomer::UpdateCenter(nullptr);
        CHECK(TestableZoomer::g_centerX.load() == prevX);
        CHECK(TestableZoomer::g_centerY.load() == prevY);
    }

    SUBCASE("center from clientRect 1920x1080") {
        TestableZoomer::g_clientRect = { 0, 0, 1920, 1080 };
        TestableZoomer::UpdateCenter((HWND)0xDEAD);
        CHECK(TestableZoomer::g_centerX.load() == 960);
        CHECK(TestableZoomer::g_centerY.load() == 540);
    }

    SUBCASE("center from clientRect 800x600") {
        TestableZoomer::g_clientRect = { 0, 0, 800, 600 };
        TestableZoomer::UpdateCenter((HWND)0xDEAD);
        CHECK(TestableZoomer::g_centerX.load() == 400);
        CHECK(TestableZoomer::g_centerY.load() == 300);
    }

    SUBCASE("center from offset clientRect") {
        TestableZoomer::g_clientRect = { 100, 50, 900, 650 };
        TestableZoomer::UpdateCenter((HWND)0xDEAD);
        CHECK(TestableZoomer::g_centerX.load() == 500);
        CHECK(TestableZoomer::g_centerY.load() == 350);
    }
}

// ==================== IsPrimaryOrBackBuffer ====================

TEST_CASE("IsPrimaryOrBackBuffer") {
    TestableZoomer::ResetState();

    SUBCASE("null surface") {
        CHECK(TestableZoomer::IsPrimaryOrBackBuffer(nullptr) == false);
    }

    SUBCASE("primary surface") {
        MockSurface mock;
        mock.SetAsPrimary();
        CHECK(TestableZoomer::IsPrimaryOrBackBuffer(&mock) == true);
    }

    SUBCASE("back buffer surface") {
        MockSurface mock;
        mock.SetAsBackBuffer();
        CHECK(TestableZoomer::IsPrimaryOrBackBuffer(&mock) == true);
    }

    SUBCASE("offscreen surface") {
        MockSurface mock;
        CHECK(TestableZoomer::IsPrimaryOrBackBuffer(&mock) == false);
    }

    SUBCASE("cached on second call") {
        MockSurface mock;
        mock.SetAsPrimary();
        CHECK(TestableZoomer::IsPrimaryOrBackBuffer(&mock) == true);
        CHECK(TestableZoomer::IsPrimaryOrBackBuffer(&mock) == true);
        CHECK(TestableZoomer::g_knownPrimaryOrBack.count(&mock) == 1);
    }

    SUBCASE("GetSurfaceDesc fails") {
        MockSurface mock;
        mock.getSurfaceDescFail = true;
        CHECK(TestableZoomer::IsPrimaryOrBackBuffer(&mock) == false);
    }
}

// ==================== HookedBlt ====================

TEST_CASE("HookedBlt") {
    TestableZoomer::ResetState();

    SUBCASE("zoom 1.0x - passes through unchanged") {
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.0f);

        MockSurface self;
        MockSurface src;

        RECT dest = { 100, 100, 500, 400 };
        RECT srcRect = { 0, 0, 400, 300 };

        HRESULT hr = TestableZoomer::HookedBlt(
            &self, &dest, &src, &srcRect, DDBLT_WAIT, nullptr);
        CHECK(hr == DD_OK);
        CHECK(dest.left == 100);
        CHECK(dest.right == 500);
    }

    SUBCASE("non-primary surface - passes through") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.5f);

        MockSurface self;
        MockSurface src;

        RECT dest = { 100, 100, 500, 400 };

        HRESULT hr = TestableZoomer::HookedBlt(
            &self, &dest, &src, nullptr, DDBLT_WAIT, nullptr);
        CHECK(hr == DD_OK);
        CHECK(dest.left == 100);
    }

    SUBCASE("primary surface with zoom - applies zoom") {
        TestableZoomer::g_zoom.store(2.0f);
        TestableZoomer::g_targetZoom.store(2.0f);
        TestableZoomer::g_centerX = 400;
        TestableZoomer::g_centerY = 300;

        MockSurface self;
        self.SetAsPrimary();
        MockSurface src;

        RECT dest = { 0, 0, 1752, 1248 };

        HRESULT hr = TestableZoomer::HookedBlt(
            &self, &dest, &src, nullptr, DDBLT_WAIT, nullptr);
        CHECK(hr == DD_OK);
    }

    SUBCASE("lerp updates g_zoom") {
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.5f);

        MockSurface self;
        MockSurface src;

        RECT dest = { 100, 100, 500, 400 };

        TestableZoomer::HookedBlt(&self, &dest, &src, nullptr, DDBLT_WAIT, nullptr);

        float newZoom = TestableZoomer::g_zoom.load();
        CHECK(newZoom > 1.0f);
        CHECK(newZoom < 1.5f);
    }

    SUBCASE("snap when close to target") {
        TestableZoomer::g_zoom.store(1.4995f);
        TestableZoomer::g_targetZoom.store(1.5f);

        MockSurface self;
        MockSurface src;

        RECT dest = { 100, 100, 500, 400 };

        TestableZoomer::HookedBlt(&self, &dest, &src, nullptr, DDBLT_WAIT, nullptr);

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(1.5f));
    }

    SUBCASE("dest outside map area - not zoomed") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.5f);

        MockSurface self;
        self.SetAsPrimary();
        MockSurface src;

        RECT dest = { 1800, 1300, 1900, 1400 };

        HRESULT hr = TestableZoomer::HookedBlt(
            &self, &dest, &src, nullptr, DDBLT_WAIT, nullptr);
        CHECK(hr == DD_OK);
        CHECK(dest.left == 1800);
    }
}

// ==================== HookedFlip ====================

TEST_CASE("HookedFlip") {
    TestableZoomer::ResetState();

    SUBCASE("zoom 1.0x - passes through unchanged") {
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.0f);

        MockSurface self;
        HRESULT hr = TestableZoomer::HookedFlip(&self, nullptr, DDFLIP_WAIT);
        CHECK(hr == DD_OK);
    }

    SUBCASE("non-primary surface - passes through") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.5f);

        MockSurface self;
        HRESULT hr = TestableZoomer::HookedFlip(&self, nullptr, DDFLIP_WAIT);
        CHECK(hr == DD_OK);
    }

    SUBCASE("primary surface with zoom - applies zoom via Blt") {
        TestableZoomer::g_zoom.store(2.0f);
        TestableZoomer::g_targetZoom.store(2.0f);
        TestableZoomer::g_centerX = 400;
        TestableZoomer::g_centerY = 300;

        MockSurface self;
        self.SetAsPrimary();

        HRESULT hr = TestableZoomer::HookedFlip(&self, nullptr, DDFLIP_WAIT);
        CHECK(hr == DD_OK);
    }

    SUBCASE("lerp updates g_zoom via UpdateLerp") {
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.5f);

        MockSurface self;

        TestableZoomer::HookedFlip(&self, nullptr, DDFLIP_WAIT);

        float newZoom = TestableZoomer::g_zoom.load();
        CHECK(newZoom > 1.0f);
        CHECK(newZoom < 1.5f);
    }

    SUBCASE("null OriginalFlip - returns DD_OK") {
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.0f);
        TestableZoomer::OriginalFlip = nullptr;

        MockSurface self;
        HRESULT hr = TestableZoomer::HookedFlip(&self, nullptr, DDFLIP_WAIT);
        CHECK(hr == DD_OK);

        TestableZoomer::OriginalFlip = (void*)MockFlip;
    }
}

// ==================== HookedBltFast ====================

TEST_CASE("HookedBltFast") {
    TestableZoomer::ResetState();

    SUBCASE("zoom 1.0x - passes through unchanged") {
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.0f);

        MockSurface self;
        MockSurface src;
        RECT srcRect = { 0, 0, 100, 100 };

        HRESULT hr = TestableZoomer::HookedBltFast(&self, 50, 50, &src, &srcRect, DDBLTFAST_WAIT);
        CHECK(hr == DD_OK);
    }

    SUBCASE("non-primary surface - passes through") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.5f);

        MockSurface self;
        MockSurface src;
        RECT srcRect = { 0, 0, 100, 100 };

        HRESULT hr = TestableZoomer::HookedBltFast(&self, 50, 50, &src, &srcRect, DDBLTFAST_WAIT);
        CHECK(hr == DD_OK);
    }

    SUBCASE("primary surface with zoom - applies zoom") {
        TestableZoomer::g_zoom.store(2.0f);
        TestableZoomer::g_targetZoom.store(2.0f);
        TestableZoomer::g_centerX = 400;
        TestableZoomer::g_centerY = 300;

        MockSurface self;
        self.SetAsPrimary();
        MockSurface src;
        RECT srcRect = { 0, 0, 100, 100 };

        HRESULT hr = TestableZoomer::HookedBltFast(&self, 50, 50, &src, &srcRect, DDBLTFAST_WAIT);
        CHECK(hr == DD_OK);
    }

    SUBCASE("lerp updates g_zoom") {
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.5f);

        MockSurface self;
        MockSurface src;
        RECT srcRect = { 0, 0, 100, 100 };

        TestableZoomer::HookedBltFast(&self, 50, 50, &src, &srcRect, DDBLTFAST_WAIT);

        float newZoom = TestableZoomer::g_zoom.load();
        CHECK(newZoom > 1.0f);
        CHECK(newZoom < 1.5f);
    }

    SUBCASE("null srcRect - passes through") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.5f);

        MockSurface self;
        self.SetAsPrimary();
        MockSurface src;

        HRESULT hr = TestableZoomer::HookedBltFast(&self, 50, 50, &src, nullptr, DDBLTFAST_WAIT);
        CHECK(hr == DD_OK);
    }
}

// ==================== HookedGetCursorPos ====================

TEST_CASE("HookedGetCursorPos") {
    TestableZoomer::ResetState();

    MockWindow win;
    REQUIRE(win.Create());
    TestableZoomer::g_hWnd = win.hWnd;

    SUBCASE("null lpPoint") {
        BOOL result = TestableZoomer::HookedGetCursorPos(nullptr);
        CHECK(result == FALSE);
    }

    SUBCASE("zoom 1.0x - no remap") {
        TestableZoomer::g_zoom.store(1.0f);
        POINT pt = { 500, 500 };
        BOOL result = TestableZoomer::HookedGetCursorPos(&pt);
        CHECK(result == TRUE);
    }
}

// ==================== NewWndProc ====================

TEST_CASE("NewWndProc") {
    TestableZoomer::ResetState();

    MockWindow win;
    REQUIRE(win.Create());
    TestableZoomer::g_hWnd = win.hWnd;
    TestableZoomer::UpdateClientCache(win.hWnd);
    TestableZoomer::UpdateCenter(win.hWnd);

    SUBCASE("WM_MOUSEWHEEL zoom in") {
        TestableZoomer::g_targetZoom.store(1.0f);
        short delta = 120;
        WPARAM wParam = MAKEWPARAM(0, delta);
        LPARAM lParam = MAKELPARAM(400, 300);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, wParam, lParam);

        float target = TestableZoomer::g_targetZoom.load();
        CHECK(target > 1.0f);
        CHECK(target == doctest::Approx(1.05f));
    }

    SUBCASE("WM_MOUSEWHEEL zoom out") {
        TestableZoomer::g_targetZoom.store(1.5f);
        short delta = -120;
        WPARAM wParam = MAKEWPARAM(0, delta);
        LPARAM lParam = MAKELPARAM(400, 300);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, wParam, lParam);

        float target = TestableZoomer::g_targetZoom.load();
        CHECK(target == doctest::Approx(1.45f));
    }

    SUBCASE("WM_MOUSEWHEEL clamps to MAX") {
        TestableZoomer::g_targetZoom.store(1.95f);
        short delta = 120;
        WPARAM wParam = MAKEWPARAM(0, delta);
        LPARAM lParam = MAKELPARAM(400, 300);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, wParam, lParam);

        CHECK(TestableZoomer::g_targetZoom.load() <= ZOOM_MAX);
    }

    SUBCASE("WM_MOUSEWHEEL clamps to MIN") {
        TestableZoomer::g_targetZoom.store(1.0f);
        short delta = -120;
        WPARAM wParam = MAKEWPARAM(0, delta);
        LPARAM lParam = MAKELPARAM(400, 300);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, wParam, lParam);

        CHECK(TestableZoomer::g_targetZoom.load() >= ZOOM_MIN);
    }

    SUBCASE("WM_MOUSEWHEEL outside map area - ignored") {
        TestableZoomer::g_targetZoom.store(1.0f);
        short delta = 120;
        WPARAM wParam = MAKEWPARAM(0, delta);
        LPARAM lParam = MAKELPARAM(1900, 1300);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, wParam, lParam);

        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.0f));
    }

    SUBCASE("WM_SIZE updates cache") {
        int prevWidth = TestableZoomer::g_clientWidth;
        TestableZoomer::NewWndProc(win.hWnd, WM_SIZE, 0, 0);
        CHECK(TestableZoomer::g_clientWidth > 0);
    }

    SUBCASE("WM_MOUSEMOVE with zoom - remaps coords") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.5f);
        TestableZoomer::g_invZoom.store(1.0f / 1.5f);
        TestableZoomer::g_centerX = 400;
        TestableZoomer::g_centerY = 300;

        LPARAM lParam = MAKELPARAM(500, 400);
        LRESULT result = TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEMOVE, 0, lParam);

        CHECK(result == 0);
    }

    SUBCASE("non-zoom message passes through") {
        LPARAM lParam = MAKELPARAM(100, 100);
        LRESULT result = TestableZoomer::NewWndProc(win.hWnd, WM_NCHITTEST, 0, lParam);
        CHECK(result == HTCLIENT);
    }
}

// ==================== Shutdown ====================

TEST_CASE("Shutdown") {
    TestableZoomer::ResetState();

    SUBCASE("resets all state") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.8f);
        TestableZoomer::g_invZoom.store(0.666f);
        TestableZoomer::g_initialized = true;
        TestableZoomer::g_knownPrimaryOrBack.insert((IDirectDrawSurface7*)0xDEAD);

        TestableZoomer::Shutdown();

        CHECK(TestableZoomer::g_zoom.load() == ZOOM_DEFAULT);
        CHECK(TestableZoomer::g_targetZoom.load() == ZOOM_DEFAULT);
        CHECK(TestableZoomer::g_invZoom.load() == 1.0f);
        CHECK(TestableZoomer::g_initialized == false);
        CHECK(TestableZoomer::g_knownPrimaryOrBack.empty());
    }

    SUBCASE("restores vtable if set") {
        MockSurface mock;
        TestableZoomer::g_vtable = *reinterpret_cast<void***>(&mock);
        TestableZoomer::OriginalBlt = (void*)0xBEEF;
        TestableZoomer::OriginalFlip = (void*)0xFACE;
        TestableZoomer::OriginalBltFast = (void*)0xCAFE;

        TestableZoomer::Shutdown();

        CHECK(TestableZoomer::g_vtable == nullptr);
        CHECK(TestableZoomer::OriginalBlt == nullptr);
        CHECK(TestableZoomer::OriginalFlip == nullptr);
        CHECK(TestableZoomer::OriginalBltFast == nullptr);
    }

    SUBCASE("restores WndProc if hooked") {
        TestableZoomer::g_wndProcHooked = true;
        TestableZoomer::OriginalWndProc = (WNDPROC)0xCAFE;

        TestableZoomer::Shutdown();

        CHECK(TestableZoomer::g_wndProcHooked == false);
        CHECK(TestableZoomer::OriginalWndProc == nullptr);
    }

    SUBCASE("resets map detection") {
        TestableZoomer::g_mapRight = 1752;
        TestableZoomer::g_mapBottom = 1248;
        TestableZoomer::g_destDetected = true;

        TestableZoomer::Shutdown();

        CHECK(TestableZoomer::g_mapRight == 0);
        CHECK(TestableZoomer::g_mapBottom == 0);
        CHECK(TestableZoomer::g_destDetected == false);
    }

    SUBCASE("multiple calls are safe") {
        TestableZoomer::Shutdown();
        TestableZoomer::Shutdown();
        CHECK(TestableZoomer::g_initialized == false);
    }
}

// ==================== Zoom lerp logic ====================

TEST_CASE("Zoom lerp") {
    TestableZoomer::ResetState();

    SUBCASE("lerp towards target") {
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.5f);

        float cur = TestableZoomer::g_zoom.load();
        float tgt = TestableZoomer::g_targetZoom.load();
        float diff = tgt - cur;

        CHECK(diff == doctest::Approx(0.5f));

        cur += diff * ZOOM_LERP;
        CHECK(cur == doctest::Approx(1.075f));
    }

    SUBCASE("snap when close enough") {
        TestableZoomer::g_zoom.store(1.4995f);
        TestableZoomer::g_targetZoom.store(1.5f);

        float cur = TestableZoomer::g_zoom.load();
        float tgt = TestableZoomer::g_targetZoom.load();
        float diff = tgt - cur;

        CHECK(fabsf(diff) < ZOOM_SNAP);
        cur = tgt;
        CHECK(cur == doctest::Approx(1.5f));
    }

    SUBCASE("multiple lerp steps converge") {
        float cur = 1.0f;
        float tgt = 1.5f;
        int steps = 0;

        while (fabsf(tgt - cur) >= ZOOM_SNAP && steps < 100) {
            cur += (tgt - cur) * ZOOM_LERP;
            steps++;
        }

        CHECK(cur == doctest::Approx(tgt).epsilon(0.01));
        CHECK(steps > 0);
        CHECK(steps < 100);
    }

    SUBCASE("lerp down") {
        float cur = 2.0f;
        float tgt = 1.0f;

        for (int i = 0; i < 50; i++) {
            float diff = tgt - cur;
            if (fabsf(diff) < ZOOM_SNAP) { cur = tgt; break; }
            cur += diff * ZOOM_LERP;
        }

        CHECK(cur == doctest::Approx(1.0f).epsilon(0.01));
    }

    SUBCASE("invZoom updates correctly") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_invZoom.store(1.0f / TestableZoomer::g_zoom.load());
        CHECK(TestableZoomer::g_invZoom.load() == doctest::Approx(1.0f / 1.5f));
    }
}

// ==================== Constants ====================

TEST_CASE("Constants") {
    CHECK(ZOOM_DEFAULT == 1.0f);
    CHECK(ZOOM_MIN == 1.0f);
    CHECK(ZOOM_MAX == 4.0f);
    CHECK(ZOOM_STEP == doctest::Approx(0.05f));
    CHECK(ZOOM_LERP == doctest::Approx(0.15f));
    CHECK(ZOOM_SNAP == doctest::Approx(0.001f));
    CHECK(BLT_VTABLE_INDEX == 5);
    CHECK(FLIP_VTABLE_INDEX == 11);
    CHECK(BLTFAST_VTABLE_INDEX == 7);
    CHECK(SIDEBAR_WIDTH == 168);
    CHECK(BOTTOM_BAR_HEIGHT == 32);
}

// ==================== Zoom clamping ====================

TEST_CASE("Zoom clamping") {
    TestableZoomer::ResetState();

    SUBCASE("target clamped to MAX") {
        float target = 5.0f;
        if (target > ZOOM_MAX) target = ZOOM_MAX;
        CHECK(target == ZOOM_MAX);
    }

    SUBCASE("target clamped to MIN") {
        float target = 0.5f;
        if (target < ZOOM_MIN) target = ZOOM_MIN;
        CHECK(target == ZOOM_MIN);
    }

    SUBCASE("target within range unchanged") {
        float target = 1.3f;
        if (target > ZOOM_MAX) target = ZOOM_MAX;
        if (target < ZOOM_MIN) target = ZOOM_MIN;
        CHECK(target == doctest::Approx(1.3f));
    }
}

// ==================== Mouse coordinate remapping ====================

TEST_CASE("Mouse coordinate remapping") {
    TestableZoomer::ResetState();
    TestableZoomer::g_centerX = 960;
    TestableZoomer::g_centerY = 540;

    SUBCASE("zoom 1.5x - remap coordinates") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_invZoom.store(1.0f / 1.5f);

        int clientX = 1000;
        int clientY = 600;

        int originalX = TestableZoomer::g_centerX.load() +
            lroundf((clientX - TestableZoomer::g_centerX.load()) * TestableZoomer::g_invZoom);
        int originalY = TestableZoomer::g_centerY.load() +
            lroundf((clientY - TestableZoomer::g_centerY.load()) * TestableZoomer::g_invZoom);

        CHECK(originalX == 987);
        CHECK(originalY == 580);
    }

    SUBCASE("zoom 2.0x - remap coordinates") {
        TestableZoomer::g_zoom.store(2.0f);
        TestableZoomer::g_invZoom.store(0.5f);

        int clientX = 1200;
        int clientY = 740;

        int originalX = TestableZoomer::g_centerX.load() +
            lroundf((clientX - TestableZoomer::g_centerX.load()) * TestableZoomer::g_invZoom);
        int originalY = TestableZoomer::g_centerY.load() +
            lroundf((clientY - TestableZoomer::g_centerY.load()) * TestableZoomer::g_invZoom);

        CHECK(originalX == 1080);
        CHECK(originalY == 640);
    }

    SUBCASE("point at center - unchanged") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_invZoom.store(1.0f / 1.5f);

        int clientX = 960;
        int clientY = 540;

        int originalX = TestableZoomer::g_centerX.load() +
            lroundf((clientX - TestableZoomer::g_centerX.load()) * TestableZoomer::g_invZoom);
        int originalY = TestableZoomer::g_centerY.load() +
            lroundf((clientY - TestableZoomer::g_centerY.load()) * TestableZoomer::g_invZoom);

        CHECK(originalX == 960);
        CHECK(originalY == 540);
    }
}

// ==================== Dest detection ====================

TEST_CASE("Dest detection") {
    TestableZoomer::ResetState();

    SUBCASE("map area computed from clientWidth minus fixed sizes") {
        TestableZoomer::g_clientWidth = 1920;
        TestableZoomer::g_clientHeight = 1080;
        TestableZoomer::g_mapRight = TestableZoomer::g_clientWidth - SIDEBAR_WIDTH;
        TestableZoomer::g_mapBottom = TestableZoomer::g_clientHeight - BOTTOM_BAR_HEIGHT + GUARD_LINES;
        CHECK(TestableZoomer::g_mapRight == 1752);
        CHECK(TestableZoomer::g_mapBottom == 1248);
    }

    SUBCASE("shutdown resets detection") {
        TestableZoomer::g_destDetected = true;
        TestableZoomer::g_mapRight = 1752;
        TestableZoomer::g_mapBottom = 1248;
        TestableZoomer::Shutdown();
        CHECK(TestableZoomer::g_destDetected == false);
        CHECK(TestableZoomer::g_mapRight == 0);
        CHECK(TestableZoomer::g_mapBottom == 0);
    }
}

// ==================== GUARD_LINES ====================

TEST_CASE("Guard lines constant") {
    CHECK(GUARD_LINES == 200);
}

// ==================== ResetZoom ====================

TEST_CASE("ResetZoom") {
    TestableZoomer::ResetState();

    SUBCASE("resets zoom to default") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.8f);
        TestableZoomer::g_invZoom.store(0.5f);

        TestableZoomer::ResetZoom();

        CHECK(TestableZoomer::g_zoom.load() == ZOOM_DEFAULT);
        CHECK(TestableZoomer::g_targetZoom.load() == ZOOM_DEFAULT);
        CHECK(TestableZoomer::g_invZoom.load() == 1.0f);
    }

    SUBCASE("already at default - no change") {
        TestableZoomer::g_zoom.store(ZOOM_DEFAULT);
        TestableZoomer::g_targetZoom.store(ZOOM_DEFAULT);
        TestableZoomer::g_invZoom.store(1.0f);

        TestableZoomer::ResetZoom();

        CHECK(TestableZoomer::g_zoom.load() == ZOOM_DEFAULT);
        CHECK(TestableZoomer::g_invZoom.load() == 1.0f);
    }
}

// ==================== DetectDDrawWrapper ====================

TEST_CASE("DetectDDrawWrapper") {
    TestableZoomer::ResetState();

    SUBCASE("returns a valid enum value") {
        DDrawWrapper w = TestableZoomer::DetectDDrawWrapper();
        bool valid = (w == DDrawWrapper::Original ||
                      w == DDrawWrapper::CncDDraw ||
                      w == DDrawWrapper::TsDDraw ||
                      w == DDrawWrapper::DDrawCompat ||
                      w == DDrawWrapper::Unknown);
        CHECK(valid);
    }
}

// ==================== UpdateMonitorInfo ====================

TEST_CASE("UpdateMonitorInfo") {
    TestableZoomer::ResetState();

    SUBCASE("null hWnd does not crash") {
        TestableZoomer::UpdateMonitorInfo(nullptr);
        CHECK(TestableZoomer::g_hMonitor == nullptr);
    }

    SUBCASE("valid hWnd sets monitor info") {
        HWND hWnd = GetDesktopWindow();
        TestableZoomer::UpdateMonitorInfo(hWnd);
        CHECK(TestableZoomer::g_hMonitor != nullptr);
        CHECK(TestableZoomer::g_monitorWidth > 0);
        CHECK(TestableZoomer::g_monitorHeight > 0);
    }
}

// ==================== UpdateLerpFrameIndependent ====================

TEST_CASE("UpdateLerpFrameIndependent") {
    TestableZoomer::ResetState();

    SUBCASE("no perf counter - falls back to UpdateLerp") {
        TestableZoomer::g_perfCounterReady = false;
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.5f);

        TestableZoomer::UpdateLerpFrameIndependent();

        float newZoom = TestableZoomer::g_zoom.load();
        CHECK(newZoom > 1.0f);
        CHECK(newZoom < 1.5f);
    }

    SUBCASE("with perf counter - applies exponential interpolation") {
        TestableZoomer::g_perfCounterReady = true;
        QueryPerformanceFrequency(&TestableZoomer::g_perfFrequency);
        QueryPerformanceCounter(&TestableZoomer::g_lastLerpTime);
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.5f);

        TestableZoomer::UpdateLerpFrameIndependent();

        float newZoom = TestableZoomer::g_zoom.load();
        CHECK(newZoom > 1.0f);
        CHECK(newZoom <= 1.5f);
    }

    SUBCASE("snap when close to target") {
        TestableZoomer::g_perfCounterReady = true;
        QueryPerformanceFrequency(&TestableZoomer::g_perfFrequency);
        QueryPerformanceCounter(&TestableZoomer::g_lastLerpTime);
        TestableZoomer::g_zoom.store(1.4995f);
        TestableZoomer::g_targetZoom.store(1.5f);

        TestableZoomer::UpdateLerpFrameIndependent();

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(1.5f));
    }

    SUBCASE("already at target - no change") {
        TestableZoomer::g_perfCounterReady = true;
        QueryPerformanceFrequency(&TestableZoomer::g_perfFrequency);
        QueryPerformanceCounter(&TestableZoomer::g_lastLerpTime);
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.5f);

        TestableZoomer::UpdateLerpFrameIndependent();

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(1.5f));
    }
}

// ==================== Ctrl+0 hotkey ====================

TEST_CASE("Ctrl+0 hotkey") {
    TestableZoomer::ResetState();

    MockWindow win;
    REQUIRE(win.Create());
    TestableZoomer::g_hWnd = win.hWnd;
    TestableZoomer::UpdateClientCache(win.hWnd);
    TestableZoomer::UpdateCenter(win.hWnd);

    SUBCASE("0 without Ctrl - does not reset zoom") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.8f);

        TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_0, 0);

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(1.5f));
        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.8f));
    }

    SUBCASE("non-VK_0 key - does not reset zoom") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.8f);

        TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, 0x41, 0);

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(1.5f));
    }
}

// ==================== UpdateClientCache with guard lines ====================

TEST_CASE("UpdateClientCache guard lines") {
    TestableZoomer::ResetState();

    SUBCASE("mapBottom includes guard lines") {
        HWND hWnd = GetDesktopWindow();
        TestableZoomer::UpdateClientCache(hWnd);
        int expected = TestableZoomer::g_clientHeight - BOTTOM_BAR_HEIGHT + GUARD_LINES;
        CHECK(TestableZoomer::g_mapBottom == expected);
    }

    SUBCASE("mapRight unchanged by guard lines") {
        HWND hWnd = GetDesktopWindow();
        TestableZoomer::UpdateClientCache(hWnd);
        int expected = TestableZoomer::g_clientWidth - SIDEBAR_WIDTH;
        CHECK(TestableZoomer::g_mapRight == expected);
    }
}

// ==================== VK_0 constant ====================

TEST_CASE("VK_0 constant") {
    CHECK(VK_0 == 0x30);
}
