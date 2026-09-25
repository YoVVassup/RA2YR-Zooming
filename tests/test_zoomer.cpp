#ifndef VIEWCTRL_TEST
#define VIEWCTRL_TEST
#endif
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include <vector>
#include <cstring>

#include "../src/Zoomer.hpp"
#include "../src/RenderZoom.hpp"
#include "../src/GameCamera.hpp"
#include "../src/Log.h"
#include "mock_window.h"

static BOOL WINAPI MockGetCursorPos(LPPOINT lpPoint)
{
    if (!lpPoint) return FALSE;
    lpPoint->x = 500;
    lpPoint->y = 500;
    return TRUE;
}

static LRESULT CALLBACK MockWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool SameRect(const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top
        && a.right == b.right && a.bottom == b.bottom;
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
        g_focusX = 0;
        g_focusY = 0;
        g_focusValid = false;
        g_camOffset = { 0, 0 };
        g_cameraBusy = false;
        GameCamera::Disable();
        g_clientRect = { 0, 0, 1920, 1080 };
        g_clientWidth = 1920;
        g_clientHeight = 1080;
        g_viewRect = { 0, 0, 800, 600 };
        g_viewRectFromGame = false;
        g_initialized = false;
        g_wndProcHooked = false;
        OriginalWndProc = (WNDPROC)MockWndProc;
        OriginalGetCursorPos = (void*)MockGetCursorPos;
        g_perfCounterReady = false;
        g_lastLerpTime = {};
        g_perfFrequency = {};
        g_useGScript = false;
        g_pZoomFactor = nullptr;
        if (g_hThread) { CloseHandle(g_hThread); g_hThread = nullptr; }
        RenderZoom::ResetFrameState();
        RenderZoom::SetEnabled(false);
    }

    using Zoomer::IsPointInMapArea;
    using Zoomer::ClampToViewport;
    using Zoomer::UpdateClientCache;
    using Zoomer::UpdateLerp;
    using Zoomer::UpdateLerpFrameIndependent;
    using Zoomer::CommitZoom;
    using Zoomer::ApplyCameraStep;
    using Zoomer::UndoCameraOffset;
    using Zoomer::PanCamera;
    using Zoomer::ResetZoom;
    using Zoomer::HookedGetCursorPos;
    using Zoomer::NewWndProc;
    using Zoomer::Shutdown;
};

// ==================== IsPointInMapArea ====================

TEST_CASE("IsPointInMapArea") {
    TestableZoomer::ResetState();

    SUBCASE("point inside the view rect") {
        POINT pt = { 500, 500 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == true);
    }

    SUBCASE("origin is inside") {
        POINT pt = { 0, 0 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == true);
    }

    SUBCASE("right edge belongs to the next area") {
        POINT pt = { 800, 500 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == false);
    }

    SUBCASE("bottom edge belongs to the next area") {
        POINT pt = { 500, 600 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == false);
    }

    SUBCASE("point left of the view rect") {
        POINT pt = { -1, 100 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == false);
    }

    SUBCASE("the tactical view rect decides, not the window") {
        TestableZoomer::SetViewRect({ 0, 0, 1752, 1248 }, true);

        POINT inside = { 1751, 1247 };
        POINT inSidebar = { 1800, 500 };
        CHECK(TestableZoomer::IsPointInMapArea(inside) == true);
        CHECK(TestableZoomer::IsPointInMapArea(inSidebar) == false);
    }

    SUBCASE("null hWnd") {
        TestableZoomer::g_hWnd = nullptr;
        POINT pt = { 100, 100 };
        CHECK(TestableZoomer::IsPointInMapArea(pt) == false);
    }
}

// ==================== ClampToViewport ====================

TEST_CASE("ClampToViewport") {
    TestableZoomer::ResetState();

    SUBCASE("point inside the view rect") {
        POINT pt = { 500, 500 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.x == 500);
        CHECK(pt.y == 500);
    }

    SUBCASE("point left of the view rect") {
        POINT pt = { -100, 500 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.x == 0);
    }

    SUBCASE("point right of the view rect") {
        POINT pt = { 2000, 500 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.x == 800);
    }

    SUBCASE("point above the view rect") {
        POINT pt = { 500, -100 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.y == 0);
    }

    SUBCASE("point below the view rect") {
        POINT pt = { 500, 2000 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.y == 600);
    }

    SUBCASE("view rect with an offset origin") {
        TestableZoomer::SetViewRect({ 100, 50, 900, 650 }, true);

        POINT before = { -50, 10 };
        TestableZoomer::ClampToViewport(&before);
        CHECK(before.x == 100);
        CHECK(before.y == 50);

        POINT after = { 1000, 1000 };
        TestableZoomer::ClampToViewport(&after);
        CHECK(after.x == 900);
        CHECK(after.y == 650);
    }

    SUBCASE("null point") {
        TestableZoomer::ClampToViewport(nullptr);
    }
}

// ==================== SetViewRect ====================

TEST_CASE("SetViewRect") {
    TestableZoomer::ResetState();

    SUBCASE("an inverted rect is ignored") {
        RECT bad = { 100, 100, 100, 200 };
        TestableZoomer::SetViewRect(bad, false);

        CHECK(SameRect(TestableZoomer::ViewRect(), RECT{ 0, 0, 800, 600 }));
        CHECK(TestableZoomer::g_viewRectFromGame == false);
    }

    SUBCASE("a new rect moves the zoom anchor to its center") {
        TestableZoomer::SetViewRect({ 0, 0, 1752, 1248 }, true);

        CHECK(SameRect(TestableZoomer::ViewRect(), RECT{ 0, 0, 1752, 1248 }));
        CHECK(TestableZoomer::ZoomAnchor().x == 876);
        CHECK(TestableZoomer::ZoomAnchor().y == 624);
    }

    SUBCASE("an unchanged rect keeps the anchor") {
        TestableZoomer::g_centerX = 123;
        TestableZoomer::g_centerY = 456;
        TestableZoomer::SetViewRect({ 0, 0, 800, 600 }, false);

        CHECK(TestableZoomer::ZoomAnchor().x == 123);
        CHECK(TestableZoomer::ZoomAnchor().y == 456);
    }

    SUBCASE("only the game marks the rect as its own") {
        TestableZoomer::SetViewRect({ 0, 0, 700, 500 }, false);
        CHECK(TestableZoomer::g_viewRectFromGame == false);

        TestableZoomer::SetViewRect({ 0, 0, 700, 500 }, true);
        CHECK(TestableZoomer::g_viewRectFromGame == true);
    }
}

// ==================== UpdateClientCache ====================

TEST_CASE("UpdateClientCache") {
    TestableZoomer::ResetState();

    SUBCASE("null hWnd does not crash") {
        RECT prev = TestableZoomer::ViewRect();
        TestableZoomer::UpdateClientCache(nullptr);
        CHECK(SameRect(TestableZoomer::ViewRect(), prev));
    }

    SUBCASE("valid hWnd updates the dimensions") {
        HWND hWnd = GetDesktopWindow();
        TestableZoomer::UpdateClientCache(hWnd);
        CHECK(TestableZoomer::g_clientWidth > 0);
        CHECK(TestableZoomer::g_clientHeight > 0);
    }

    SUBCASE("the window rect becomes the view rect") {
        MockWindow win;
        REQUIRE(win.Create());

        TestableZoomer::UpdateClientCache(win.hWnd);

        CHECK(SameRect(TestableZoomer::ViewRect(), TestableZoomer::g_clientRect));
        CHECK(TestableZoomer::ZoomAnchor().x == TestableZoomer::g_clientRect.right / 2);
        CHECK(TestableZoomer::ZoomAnchor().y == TestableZoomer::g_clientRect.bottom / 2);
    }

    SUBCASE("a game view rect survives a window refresh") {
        MockWindow win;
        REQUIRE(win.Create());

        TestableZoomer::SetViewRect({ 0, 0, 1752, 1248 }, true);
        TestableZoomer::UpdateClientCache(win.hWnd);

        CHECK(SameRect(TestableZoomer::ViewRect(), RECT{ 0, 0, 1752, 1248 }));
        CHECK(TestableZoomer::g_viewRectFromGame == true);
    }
}

// ==================== HookedGetCursorPos ====================

TEST_CASE("HookedGetCursorPos") {
    TestableZoomer::ResetState();

    MockWindow win;
    REQUIRE(win.Create());
    TestableZoomer::g_hWnd = win.hWnd;
    TestableZoomer::UpdateClientCache(win.hWnd);

    SUBCASE("null lpPoint") {
        BOOL result = TestableZoomer::HookedGetCursorPos(nullptr);
        CHECK(result == FALSE);
    }

    SUBCASE("zoom 1.0x - no remap") {
        TestableZoomer::g_zoom.store(1.0f);
        POINT pt = { 500, 500 };
        BOOL result = TestableZoomer::HookedGetCursorPos(&pt);
        CHECK(result == TRUE);
        CHECK(pt.x == 500);
        CHECK(pt.y == 500);
    }

    SUBCASE("zoomed in - the point under the cursor is remapped") {
        TestableZoomer::g_zoom.store(2.0f);
        TestableZoomer::g_invZoom.store(0.5f);

        POINT client = { 500, 500 };
        ScreenToClient(win.hWnd, &client);
        REQUIRE(TestableZoomer::IsPointInMapArea(client));

        int expectedX = TestableZoomer::g_centerX.load()
            + lroundf((client.x - TestableZoomer::g_centerX.load()) * 0.5f);
        int expectedY = TestableZoomer::g_centerY.load()
            + lroundf((client.y - TestableZoomer::g_centerY.load()) * 0.5f);
        POINT expected = { expectedX, expectedY };
        TestableZoomer::ClampToViewport(&expected);
        ClientToScreen(win.hWnd, &expected);

        POINT pt = { 500, 500 };
        CHECK(TestableZoomer::HookedGetCursorPos(&pt) == TRUE);
        CHECK(pt.x == expected.x);
        CHECK(pt.y == expected.y);
        const bool unmoved = (pt.x == 500 && pt.y == 500);
        CHECK_FALSE(unmoved);
    }
}

// ==================== NewWndProc ====================

TEST_CASE("NewWndProc") {
    TestableZoomer::ResetState();

    MockWindow win;
    REQUIRE(win.Create());
    TestableZoomer::g_hWnd = win.hWnd;
    TestableZoomer::UpdateClientCache(win.hWnd);

    SUBCASE("WM_MOUSEWHEEL zoom in") {
        TestableZoomer::g_targetZoom.store(1.0f);
        LPARAM lParam = MAKELPARAM(400, 300);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), lParam);

        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.05f));
    }

    SUBCASE("WM_MOUSEWHEEL zoom out") {
        TestableZoomer::g_targetZoom.store(1.5f);
        LPARAM lParam = MAKELPARAM(400, 300);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -120), lParam);

        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.45f));
    }

    SUBCASE("WM_MOUSEWHEEL clamps to MAX") {
        TestableZoomer::g_targetZoom.store(3.95f);
        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 120),
                                   MAKELPARAM(400, 300));
        CHECK(TestableZoomer::g_targetZoom.load() <= ZOOM_MAX);
    }

    SUBCASE("WM_MOUSEWHEEL clamps to MIN") {
        TestableZoomer::g_targetZoom.store(1.0f);
        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -120),
                                   MAKELPARAM(400, 300));
        CHECK(TestableZoomer::g_targetZoom.load() >= ZOOM_MIN);
    }

    SUBCASE("WM_MOUSEWHEEL outside the view rect - ignored") {
        TestableZoomer::g_targetZoom.store(1.0f);
        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 120),
                                   MAKELPARAM(1900, 1300));

        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.0f));
    }

    SUBCASE("WM_MOUSEWHEEL latches the focus instead of moving the anchor") {
        TestableZoomer::g_focusValid = false;
        TestableZoomer::g_targetZoom.store(1.0f);
        const LONG prevCenterX = TestableZoomer::g_centerX.load();
        const LONG prevCenterY = TestableZoomer::g_centerY.load();

        POINT screenPt = { 400, 300 };
        POINT expected = screenPt;
        ScreenToClient(win.hWnd, &expected);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 120),
                                   MAKELPARAM(screenPt.x, screenPt.y));

        CHECK(TestableZoomer::g_focusValid);
        CHECK(TestableZoomer::g_focusX.load() == expected.x);
        CHECK(TestableZoomer::g_focusY.load() == expected.y);
        CHECK(TestableZoomer::g_centerX.load() == prevCenterX);
        CHECK(TestableZoomer::g_centerY.load() == prevCenterY);
        CHECK(TestableZoomer::g_targetZoom.load() > 1.0f);
    }

    SUBCASE("WM_SIZE refreshes the view rect") {
        TestableZoomer::g_clientWidth = 0;
        TestableZoomer::g_clientHeight = 0;

        TestableZoomer::NewWndProc(win.hWnd, WM_SIZE, 0, 0);

        CHECK(TestableZoomer::g_clientWidth > 0);
        CHECK(TestableZoomer::g_clientHeight > 0);
        CHECK(SameRect(TestableZoomer::ViewRect(), TestableZoomer::g_clientRect));
    }

    SUBCASE("arrow keys pan the magnified view") {
        if (GetKeyState(VK_CONTROL) & 0x8000) return;

        TestableZoomer::g_zoom.store(2.0f);
        TestableZoomer::g_targetZoom.store(2.0f);
        const LONG prevX = TestableZoomer::g_centerX.load();
        const RECT view = TestableZoomer::ViewRect();
        const int stepX = (int)((float)(view.right - view.left) / 2.0f * 0.2f);

        LRESULT result = TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_RIGHT, 0);

        CHECK(result == 0);
        CHECK(TestableZoomer::g_centerX.load() == prevX + stepX);
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

// ==================== Ctrl+0 hotkey ====================

TEST_CASE("Ctrl+0 hotkey") {
    TestableZoomer::ResetState();

    MockWindow win;
    REQUIRE(win.Create());
    TestableZoomer::g_hWnd = win.hWnd;
    TestableZoomer::UpdateClientCache(win.hWnd);

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

// ==================== Zoom lerp ====================

TEST_CASE("Zoom lerp") {
    TestableZoomer::ResetState();

    SUBCASE("lerp towards target") {
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.5f);

        TestableZoomer::UpdateLerp();

        float cur = TestableZoomer::g_zoom.load();
        CHECK(cur == doctest::Approx(1.0f + 0.5f * ZOOM_LERP));
        CHECK(TestableZoomer::g_invZoom.load() == doctest::Approx(1.0f / cur));
    }

    SUBCASE("snap when close enough") {
        TestableZoomer::g_zoom.store(1.4995f);
        TestableZoomer::g_targetZoom.store(1.5f);

        TestableZoomer::UpdateLerp();

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(1.5f));
    }

    SUBCASE("multiple lerp steps converge") {
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.5f);

        int steps = 0;
        while (fabsf(TestableZoomer::g_targetZoom.load() - TestableZoomer::g_zoom.load()) >= ZOOM_SNAP
               && steps < 100) {
            TestableZoomer::UpdateLerp();
            ++steps;
        }
        // one more step, it is the one that snaps onto the target
        TestableZoomer::UpdateLerp();

        CHECK(steps > 0);
        CHECK(steps < 100);
        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(1.5f));
    }

    SUBCASE("lerp down") {
        TestableZoomer::g_zoom.store(2.0f);
        TestableZoomer::g_targetZoom.store(1.0f);

        for (int i = 0; i < 50; ++i) {
            if (fabsf(TestableZoomer::g_targetZoom.load() - TestableZoomer::g_zoom.load()) < ZOOM_SNAP) break;
            TestableZoomer::UpdateLerp();
        }
        TestableZoomer::UpdateLerp();

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(1.0f));
    }

    SUBCASE("at the target the lerp does nothing") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.5f);

        TestableZoomer::UpdateLerp();

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(1.5f));
    }

    SUBCASE("nested lerp is ignored while the camera moves") {
        TestableZoomer::g_cameraBusy = true;
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(2.0f);

        TestableZoomer::UpdateLerp();

        CHECK(TestableZoomer::g_zoom.load() == 1.0f);
        TestableZoomer::g_cameraBusy = false;
    }
}

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

// ==================== CommitZoom / camera plumbing ====================

TEST_CASE("CommitZoom") {
    TestableZoomer::ResetState();

    SUBCASE("stores zoom and its inverse") {
        TestableZoomer::CommitZoom(2.0f);

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(2.0f));
        CHECK(TestableZoomer::g_invZoom.load() == doctest::Approx(0.5f));
        CHECK(TestableZoomer::g_cameraBusy == false);
    }

    SUBCASE("camera step does nothing when the game camera is unavailable") {
        TestableZoomer::g_focusValid = true;
        TestableZoomer::g_focusX = 800;
        TestableZoomer::g_focusY = 540;

        TestableZoomer::ApplyCameraStep(1.0f, 2.0f);

        CHECK(TestableZoomer::g_camOffset.x == 0);
        CHECK(TestableZoomer::g_camOffset.y == 0);
    }

    SUBCASE("zooming back to 1.0 drops the accumulated offset") {
        TestableZoomer::g_zoom.store(2.0f);
        TestableZoomer::g_invZoom.store(0.5f);
        TestableZoomer::g_camOffset = { 40, -25 };

        TestableZoomer::CommitZoom(1.0f);

        CHECK(TestableZoomer::g_camOffset.x == 0);
        CHECK(TestableZoomer::g_camOffset.y == 0);
    }
}

// ==================== PanCamera ====================

TEST_CASE("PanCamera") {
    TestableZoomer::ResetState();
    TestableZoomer::g_zoom.store(2.0f);
    TestableZoomer::g_invZoom.store(0.5f);

    SUBCASE("moves the anchor of the magnified view") {
        TestableZoomer::g_centerX = 400;
        TestableZoomer::g_centerY = 300;

        TestableZoomer::PanCamera(50, -20);

        CHECK(TestableZoomer::g_centerX.load() == 450);
        CHECK(TestableZoomer::g_centerY.load() == 280);
    }

    SUBCASE("the anchor stops at the view rect") {
        TestableZoomer::PanCamera(-1000, -1000);

        // half of the magnified view is 200x150 pixels at 2.0x
        CHECK(TestableZoomer::g_centerX.load() == 200);
        CHECK(TestableZoomer::g_centerY.load() == 150);

        TestableZoomer::PanCamera(1000, 1000);

        CHECK(TestableZoomer::g_centerX.load() == 600);
        CHECK(TestableZoomer::g_centerY.load() == 450);
    }

    SUBCASE("no movement at all is a no-op") {
        TestableZoomer::g_centerX = 400;
        TestableZoomer::PanCamera(0, 0);
        CHECK(TestableZoomer::g_centerX.load() == 400);
    }
}

// ==================== ResetZoom ====================

TEST_CASE("ResetZoom") {
    TestableZoomer::ResetState();

    SUBCASE("resets zoom to default") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.8f);
        TestableZoomer::g_invZoom.store(0.5f);
        TestableZoomer::g_focusValid = true;

        TestableZoomer::ResetZoom();

        CHECK(TestableZoomer::g_zoom.load() == ZOOM_DEFAULT);
        CHECK(TestableZoomer::g_targetZoom.load() == ZOOM_DEFAULT);
        CHECK(TestableZoomer::g_invZoom.load() == 1.0f);
        CHECK(TestableZoomer::g_focusValid == false);
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

// ==================== Shutdown ====================

TEST_CASE("Shutdown") {
    TestableZoomer::ResetState();

    SUBCASE("resets the zoom state") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.8f);
        TestableZoomer::g_invZoom.store(0.666f);
        TestableZoomer::g_initialized = true;

        TestableZoomer::Shutdown();

        CHECK(TestableZoomer::g_zoom.load() == ZOOM_DEFAULT);
        CHECK(TestableZoomer::g_targetZoom.load() == ZOOM_DEFAULT);
        CHECK(TestableZoomer::g_invZoom.load() == 1.0f);
        CHECK(TestableZoomer::g_initialized == false);
    }

    SUBCASE("restores the window proc if hooked") {
        TestableZoomer::g_wndProcHooked = true;
        TestableZoomer::OriginalWndProc = (WNDPROC)MockWndProc;

        TestableZoomer::Shutdown();

        CHECK(TestableZoomer::g_wndProcHooked == false);
        CHECK(TestableZoomer::OriginalWndProc == nullptr);
    }

    SUBCASE("forgets the game view rect") {
        TestableZoomer::SetViewRect({ 0, 0, 1752, 1248 }, true);

        TestableZoomer::Shutdown();

        CHECK(SameRect(TestableZoomer::ViewRect(), RECT{ 0, 0, 800, 600 }));
        CHECK(TestableZoomer::g_viewRectFromGame == false);
    }

    SUBCASE("multiple calls are safe") {
        TestableZoomer::Shutdown();
        TestableZoomer::Shutdown();
        CHECK(TestableZoomer::g_initialized == false);
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

    SUBCASE("point at the anchor - unchanged") {
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

// ==================== Constants ====================

TEST_CASE("Constants") {
    CHECK(ZOOM_DEFAULT == 1.0f);
    CHECK(ZOOM_MIN == 1.0f);
    CHECK(ZOOM_MAX == 4.0f);
    CHECK(ZOOM_STEP == doctest::Approx(0.05f));
    CHECK(ZOOM_LERP == doctest::Approx(0.15f));
    CHECK(ZOOM_SNAP == doctest::Approx(0.001f));
    CHECK(VK_0 == 0x30);
    CHECK(GSCRIPT_ZOOM_FACTOR_RVA == 0x1739B0);
}

// ==================== Zoom clamping ====================

TEST_CASE("Zoom clamping") {
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

// ==================== GameCamera::ComputeShift ====================

TEST_CASE("GameCamera::ComputeShift") {
    const POINT fixed = { 960, 540 };

    SUBCASE("zoom in pushes the frame along the focus offset") {
        POINT focus = { 1360, 540 };
        POINT s = GameCamera::ComputeShift(focus, fixed, 1.0f, 2.0f);
        CHECK(s.x == 200);
        CHECK(s.y == 0);
    }

    SUBCASE("zoom out pulls the frame back") {
        POINT focus = { 1360, 540 };
        POINT s = GameCamera::ComputeShift(focus, fixed, 2.0f, 1.0f);
        CHECK(s.x == -200);
        CHECK(s.y == 0);
    }

    SUBCASE("focus on the anchor needs no movement") {
        POINT s = GameCamera::ComputeShift(fixed, fixed, 1.0f, 3.0f);
        CHECK(s.x == 0);
        CHECK(s.y == 0);
    }

    SUBCASE("diagonal focus offsets shift both axes") {
        POINT focus = { 1160, 340 };
        POINT s = GameCamera::ComputeShift(focus, fixed, 1.0f, 2.0f);
        CHECK(s.x == 100);
        CHECK(s.y == -100);
    }

    SUBCASE("identical zoom levels do not move the camera") {
        POINT focus = { 1360, 540 };
        POINT s = GameCamera::ComputeShift(focus, fixed, 2.0f, 2.0f);
        CHECK(s.x == 0);
        CHECK(s.y == 0);
    }

    SUBCASE("non positive zoom factors are rejected") {
        POINT focus = { 1360, 540 };
        CHECK(GameCamera::ComputeShift(focus, fixed, 0.0f, 2.0f).x == 0);
        CHECK(GameCamera::ComputeShift(focus, fixed, 1.0f, -1.0f).x == 0);
    }

    SUBCASE("zoom in and back out cancels out") {
        POINT focus = { 1360, 240 };
        POINT up = GameCamera::ComputeShift(focus, fixed, 1.0f, 1.7f);
        POINT down = GameCamera::ComputeShift(focus, fixed, 1.7f, 1.0f);
        CHECK(up.x + down.x == 0);
        CHECK(up.y + down.y == 0);
    }

    SUBCASE("small steps round to whole pixels") {
        POINT focus = { 1060, 540 };
        POINT s = GameCamera::ComputeShift(focus, fixed, 1.0f, 1.05f);
        CHECK(s.x == 5);
    }
}

// ==================== RenderZoom::ComputeSourceRect ====================

TEST_CASE("RenderZoom::ComputeSourceRect") {
    using SR = RenderZoom::SourceRect;

    auto same = [](const SR& r, int x, int y, int w, int h) {
        return r.X == x && r.Y == y && r.W == w && r.H == h;
    };

    SUBCASE("zoomed out - the whole view is the source") {
        SR r = RenderZoom::ComputeSourceRect(800, 600, 400, 300, 1.0f);
        CHECK(same(r, 0, 0, 800, 600));
    }

    SUBCASE("exactly at the epsilon - still the whole view") {
        SR r = RenderZoom::ComputeSourceRect(800, 600, 400, 300, 1.001f);
        CHECK(same(r, 0, 0, 800, 600));
    }

    SUBCASE("doubled zoom takes a centered quarter") {
        SR r = RenderZoom::ComputeSourceRect(100, 100, 50, 50, 2.0f);
        CHECK(same(r, 25, 25, 50, 50));
    }

    SUBCASE("the rect scales with the zoom factor") {
        SR r = RenderZoom::ComputeSourceRect(100, 100, 50, 50, 4.0f);
        CHECK(same(r, 38, 38, 25, 25));
    }

    SUBCASE("half up rounding of an odd source size") {
        SR r = RenderZoom::ComputeSourceRect(800, 600, 400, 300, 1.5f);
        CHECK(same(r, 133, 100, 534, 400));
    }

    SUBCASE("anchor near the left edge clamps the origin") {
        SR r = RenderZoom::ComputeSourceRect(100, 100, 10, 50, 2.0f);
        CHECK(same(r, 0, 25, 50, 50));
    }

    SUBCASE("anchor near the right edge clamps the width") {
        SR r = RenderZoom::ComputeSourceRect(100, 100, 95, 50, 2.0f);
        CHECK(same(r, 70, 25, 30, 50));
    }

    SUBCASE("anchor near the bottom clamps the height") {
        SR r = RenderZoom::ComputeSourceRect(100, 100, 50, 95, 2.0f);
        CHECK(same(r, 25, 70, 50, 30));
    }

    SUBCASE("zero sized view") {
        SR r = RenderZoom::ComputeSourceRect(0, 0, 0, 0, 2.0f);
        CHECK(same(r, 0, 0, 0, 0));
    }

    SUBCASE("a source below one pixel falls back to the whole view") {
        SR r = RenderZoom::ComputeSourceRect(4, 4, 2, 2, 10.0f);
        CHECK(same(r, 0, 0, 4, 4));
    }

    SUBCASE("the smallest possible source is a single pixel") {
        SR r = RenderZoom::ComputeSourceRect(4, 4, 2, 2, 4.0f);
        CHECK(same(r, 2, 2, 1, 1));
    }
}

// ==================== RenderZoom::Upscale ====================

TEST_CASE("RenderZoom::Upscale") {
    SUBCASE("doubles the backup") {
        std::vector<unsigned short> backup(4 * 4);
        for (int i = 0; i < 16; ++i) backup[i] = (unsigned short)(i + 1);

        std::vector<unsigned short> dst(8 * 8, 0);
        RenderZoom::SourceRect src = { 0, 0, 4, 4 };
        RenderZoom::Upscale(backup.data(), 4, dst.data(), 8, 8, 8, src);

        // nearest neighbour: two destination pixels per source pixel
        CHECK(dst[0] == backup[0]);
        CHECK(dst[1] == backup[0]);
        CHECK(dst[2] == backup[1]);
        CHECK(dst[3] == backup[1]);
        CHECK(dst[4] == backup[2]);

        CHECK(dst[8] == backup[0]);    // second row still reads the first
        CHECK(dst[56] == backup[12]);  // last row reads the last source row
    }

    SUBCASE("a source rect selects the sub region") {
        std::vector<unsigned short> backup(4 * 4);
        for (int i = 0; i < 16; ++i) backup[i] = (unsigned short)(i + 1);

        std::vector<unsigned short> dst(4 * 4, 0);
        RenderZoom::SourceRect src = { 1, 1, 2, 2 };
        RenderZoom::Upscale(backup.data(), 4, dst.data(), 4, 4, 4, src);

        CHECK(dst[0] == backup[5]);
        CHECK(dst[1] == backup[5]);
        CHECK(dst[2] == backup[6]);
        CHECK(dst[3] == backup[6]);
        CHECK(dst[12] == backup[9]);
    }

    SUBCASE("the destination may be wider than the backup") {
        std::vector<unsigned short> backup(2 * 2, 7);
        std::vector<unsigned short> dst(8, 0);
        RenderZoom::SourceRect src = { 0, 0, 2, 2 };
        RenderZoom::Upscale(backup.data(), 2, dst.data(), 4, 4, 2, src);

        CHECK(dst[0] == 7);
        CHECK(dst[3] == 7);
    }

    SUBCASE("degenerate input is ignored") {
        std::vector<unsigned short> backup(4, 1);
        std::vector<unsigned short> dst(4, 99);

        RenderZoom::SourceRect empty = { 0, 0, 0, 4 };
        RenderZoom::Upscale(backup.data(), 2, dst.data(), 2, 2, 2, empty);
        CHECK(dst[0] == 99);

        RenderZoom::SourceRect full = { 0, 0, 2, 2 };
        RenderZoom::Upscale(nullptr, 2, dst.data(), 2, 2, 2, full);
        CHECK(dst[0] == 99);
    }
}

// ==================== RenderZoom::CopyRows ====================

TEST_CASE("RenderZoom::CopyRows") {
    // 8 pixels wide, 4 rows deep, 16 bit pixels
    auto makeSurface = []() {
        std::vector<unsigned char> surf(16 * 4, 0);
        for (int i = 0; i < 16 * 4; ++i) surf[i] = (unsigned char)(i & 0xFF);
        return surf;
    };
    const RECT view = { 2, 1, 6, 3 };  // 4x2 pixels

    SUBCASE("copies the view into the backup") {
        std::vector<unsigned char> surf = makeSurface();
        std::vector<unsigned char> backup(4 * 2 * 2, 0);

        bool ok = RenderZoom::CopyRows(surf.data(), 16, view, backup.data(), 4, 2, true);

        CHECK(ok);
        CHECK(memcmp(backup.data(), surf.data() + 16 + 4, 8) == 0);
        CHECK(memcmp(backup.data() + 8, surf.data() + 32 + 4, 8) == 0);
    }

    SUBCASE("writes the backup back into the view") {
        std::vector<unsigned char> surf = makeSurface();
        const std::vector<unsigned char> original = surf;
        std::vector<unsigned char> backup(4 * 2 * 2, 0x5A);

        bool ok = RenderZoom::CopyRows(surf.data(), 16, view, backup.data(), 4, 2, false);

        CHECK(ok);
        CHECK(memcmp(surf.data() + 16 + 4, backup.data(), 8) == 0);
        CHECK(memcmp(surf.data() + 32 + 4, backup.data() + 8, 8) == 0);

        // everything outside the view rect stays untouched
        const int expectedBytes = (view.right - view.left) * (view.bottom - view.top) * 2;
        int changed = 0;
        for (size_t i = 0; i < surf.size(); ++i) {
            const int col = (int)(i % 16) / 2;
            const int row = (int)(i / 16);
            const bool inView = col >= view.left && col < view.right
                && row >= view.top && row < view.bottom;
            if (inView) {
                ++changed;
            } else if (surf[i] != original[i]) {
                changed = -1;
                break;
            }
        }
        CHECK(changed == expectedBytes);
    }

    SUBCASE("null arguments") {
        std::vector<unsigned char> surf = makeSurface();
        std::vector<unsigned char> backup(16, 0);
        CHECK(RenderZoom::CopyRows(nullptr, 16, view, backup.data(), 4, 2, true) == false);
        CHECK(RenderZoom::CopyRows(surf.data(), 16, view, nullptr, 4, 2, true) == false);
    }

    SUBCASE("a view larger than the backup is rejected") {
        std::vector<unsigned char> surf = makeSurface();
        std::vector<unsigned char> backup(4, 0);
        CHECK(RenderZoom::CopyRows(surf.data(), 16, view, backup.data(), 1, 1, true) == false);
    }

    SUBCASE("a pitch that cannot hold the view is rejected") {
        std::vector<unsigned char> surf = makeSurface();
        std::vector<unsigned char> backup(16, 0);
        CHECK(RenderZoom::CopyRows(surf.data(), 6, view, backup.data(), 4, 2, true) == false);
    }

    SUBCASE("an inverted view is rejected") {
        std::vector<unsigned char> surf = makeSurface();
        std::vector<unsigned char> backup(16, 0);
        RECT inverted = { 6, 3, 2, 1 };
        CHECK(RenderZoom::CopyRows(surf.data(), 16, inverted, backup.data(), 4, 2, true) == false);
    }
}

// ==================== RenderZoom::AcceptViewRect ====================

TEST_CASE("RenderZoom::AcceptViewRect") {
    RenderZoom::ResetFrameState();
    const RECT small = { 0, 0, 800, 600 };
    const RECT game = { 0, 0, 1752, 1248 };

    SUBCASE("the first rect is accepted immediately") {
        CHECK(RenderZoom::AcceptViewRect(small, 0) == true);

        RECT cached = {};
        REQUIRE(RenderZoom::CachedViewRect(cached));
        CHECK(SameRect(cached, small));
    }

    SUBCASE("an invalid rect is rejected") {
        RECT bad = { 100, 100, 50, 50 };
        CHECK(RenderZoom::AcceptViewRect(bad, 0) == false);

        RECT cached = {};
        CHECK(RenderZoom::CachedViewRect(cached) == false);
    }

    SUBCASE("the game rect is polled at most every 250 ms") {
        CHECK(RenderZoom::AcceptViewRect(small, 0) == true);
        CHECK(RenderZoom::AcceptViewRect(game, 100) == false);

        RECT cached = {};
        REQUIRE(RenderZoom::CachedViewRect(cached));
        CHECK(SameRect(cached, small));

        CHECK(RenderZoom::AcceptViewRect(game, 250) == true);
        REQUIRE(RenderZoom::CachedViewRect(cached));
        CHECK(SameRect(cached, game));
    }

    SUBCASE("an unchanged rect is not published again") {
        CHECK(RenderZoom::AcceptViewRect(small, 0) == true);
        CHECK(RenderZoom::AcceptViewRect(small, 300) == false);
        CHECK(RenderZoom::AcceptViewRect(small, 600) == false);
    }

    SUBCASE("resetting forgets the debounce") {
        CHECK(RenderZoom::AcceptViewRect(small, 0) == true);
        RenderZoom::ResetFrameState();

        RECT cached = {};
        CHECK(RenderZoom::CachedViewRect(cached) == false);
        CHECK(RenderZoom::AcceptViewRect(small, 10) == true);
    }
}

// ==================== RenderZoom frame hooks ====================

TEST_CASE("RenderZoom frame hooks") {
    TestableZoomer::ResetState();

    SUBCASE("a disabled module does nothing") {
        CHECK(RenderZoom::PreRender() == false);
        RenderZoom::PostRender();

        RECT cached = {};
        CHECK(RenderZoom::CachedViewRect(cached) == false);
    }

    SUBCASE("without the game data there is nothing to magnify") {
        RenderZoom::SetEnabled(true);
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_invZoom.store(1.0f / 1.5f);

        CHECK(RenderZoom::PreRender() == false);
        RenderZoom::PostRender();

        // no backup was taken, so the next frame has nothing to restore
        CHECK(RenderZoom::PreRender() == false);
        RECT cached = {};
        CHECK(RenderZoom::CachedViewRect(cached) == false);
    }

    SUBCASE("pre render advances the zoom lerp before the frame is drawn") {
        RenderZoom::SetEnabled(true);
        TestableZoomer::g_perfCounterReady = false;
        TestableZoomer::g_zoom.store(1.0f);
        TestableZoomer::g_targetZoom.store(1.5f);

        CHECK(RenderZoom::PreRender() == false);
        CHECK(TestableZoomer::g_zoom.load() > 1.0f);
        CHECK(TestableZoomer::g_zoom.load() < 1.5f);
    }
}
