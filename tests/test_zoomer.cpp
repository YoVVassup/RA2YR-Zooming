#ifndef VIEWCTRL_TEST
#define VIEWCTRL_TEST
#endif
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include <vector>
#include <cstring>
#include <string>

#include "../src/Zoomer.hpp"
#include "../src/RenderZoom.hpp"
#include "../src/GameCamera.hpp"
#include "../src/GameAddrs.hpp"
#include "../src/ScalerConflict.hpp"
#include "../src/Log.h"
#include "mock_window.h"

// Main.cpp test seam: the sidebar flag byte the PostRender hook reads.
void SetFlagByteForTest(const BYTE* p);

// DllMain lives in Main.cpp, which the test project links; call it directly
// to cover the process attach/detach paths.
extern BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved);

static UINT   g_recordedMsg = 0;
static LPARAM g_recordedLParam = 0;

static LRESULT CALLBACK MockWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    g_recordedMsg = msg;
    g_recordedLParam = lParam;
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
        GameCamera::SetCameraPositionForTest(nullptr);
        GameCamera::SetTacticalInstanceForTest(nullptr);
        SetFlagByteForTest(nullptr);
        g_clientRect = { 0, 0, 1920, 1080 };
        g_clientWidth = 1920;
        g_clientHeight = 1080;
        g_viewRect = { 0, 0, 800, 600 };
        g_viewRectFromGame = false;
        g_initialized = false;
        g_wndProcHooked = false;
        OriginalWndProc = (WNDPROC)MockWndProc;
        g_perfCounterReady = false;
        g_lastLerpTime = {};
        g_perfFrequency = {};
        g_ctrlHeld = false;
        g_haveLastPress = false;
        g_lastPressMs = 0;
        g_wheelRemainder = 0;
        g_initStarted = false;
        g_recordedMsg = 0;
        g_recordedLParam = 0;
        if (g_hThread) { CloseHandle(g_hThread); g_hThread = nullptr; }
        RenderZoom::SetTestGameRegion(nullptr, nullptr, nullptr, nullptr);
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
    using Zoomer::CtrlHeld;
    using Zoomer::ApplyWheelSteps;
    using Zoomer::RegisterDoublePress;
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
        CHECK(pt.x == 799);
    }

    SUBCASE("point above the view rect") {
        POINT pt = { 500, -100 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.y == 0);
    }

    SUBCASE("point below the view rect") {
        POINT pt = { 500, 2000 };
        TestableZoomer::ClampToViewport(&pt);
        CHECK(pt.y == 599);
    }

    SUBCASE("a clamped point is inside the view rect") {
        POINT corners[4] = { { -100, -100 }, { 2000, -100 }, { -100, 2000 }, { 2000, 2000 } };
        for (auto& pt : corners) {
            TestableZoomer::ClampToViewport(&pt);
            CHECK(TestableZoomer::IsPointInMapArea(pt) == true);
        }
    }

    SUBCASE("view rect with an offset origin") {
        TestableZoomer::SetViewRect({ 100, 50, 900, 650 }, true);

        POINT before = { -50, 10 };
        TestableZoomer::ClampToViewport(&before);
        CHECK(before.x == 100);
        CHECK(before.y == 50);

        POINT after = { 1000, 1000 };
        TestableZoomer::ClampToViewport(&after);
        CHECK(after.x == 899);
        CHECK(after.y == 649);
        CHECK(TestableZoomer::IsPointInMapArea(after) == true);
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

// ==================== UnMagnify ====================

TEST_CASE("UnMagnify") {
    TestableZoomer::ResetState();

    SUBCASE("zoom 1.0 - rejected") {
        POINT in = { 100, 100 };
        POINT out = { -1, -1 };
        CHECK(TestableZoomer::UnMagnify(in, out) == false);
    }

    SUBCASE("inside the snap epsilon - rejected") {
        TestableZoomer::g_zoom.store(1.0005f);

        POINT in = { 100, 100 };
        POINT out = { -1, -1 };
        CHECK(TestableZoomer::UnMagnify(in, out) == false);
    }

    SUBCASE("the center point stays put at 2.0x") {
        TestableZoomer::g_zoom.store(2.0f);

        POINT in = { 400, 300 };
        POINT out = { -1, -1 };
        REQUIRE(TestableZoomer::UnMagnify(in, out));
        CHECK(out.x == 400);
        CHECK(out.y == 300);
    }

    SUBCASE("the view origin maps to the source origin") {
        TestableZoomer::g_zoom.store(2.0f);

        // 800x600 at 2.0x shows the centered source rect (200,150,400,300)
        POINT in = { 0, 0 };
        POINT out = { -1, -1 };
        REQUIRE(TestableZoomer::UnMagnify(in, out));
        CHECK(out.x == 200);
        CHECK(out.y == 150);
    }

    SUBCASE("the far corner stays inside the view") {
        TestableZoomer::g_zoom.store(2.0f);

        POINT in = { 799, 599 };
        POINT out = { -1, -1 };
        REQUIRE(TestableZoomer::UnMagnify(in, out));
        CHECK(out.x == 600);
        CHECK(out.y == 450);
        CHECK(out.x >= 0);
        CHECK(out.x < 800);
        CHECK(out.y >= 0);
        CHECK(out.y < 600);
    }

    SUBCASE("half up rounding") {
        TestableZoomer::g_zoom.store(2.0f);

        POINT in = { 1, 1 };
        POINT out = { -1, -1 };
        REQUIRE(TestableZoomer::UnMagnify(in, out));
        CHECK(out.x == 201);
        CHECK(out.y == 151);
    }

    SUBCASE("points outside the view rect are rejected") {
        TestableZoomer::g_zoom.store(2.0f);

        POINT right = { 800, 300 };
        POINT left = { -1, 10 };
        POINT bottom = { 400, 600 };
        POINT out = { -1, -1 };
        CHECK(TestableZoomer::UnMagnify(right, out) == false);
        CHECK(TestableZoomer::UnMagnify(left, out) == false);
        CHECK(TestableZoomer::UnMagnify(bottom, out) == false);
    }

    SUBCASE("an offset view origin behaves like the plain one") {
        TestableZoomer::SetViewRect({ 100, 50, 900, 650 }, true);
        TestableZoomer::g_zoom.store(2.0f);

        POINT in = { 0, 0 };
        POINT out = { -1, -1 };
        REQUIRE(TestableZoomer::UnMagnify(in, out));
        CHECK(out.x == 200);
        CHECK(out.y == 150);
    }

    SUBCASE("a degenerate view rect is rejected") {
        TestableZoomer::g_zoom.store(2.0f);
        TestableZoomer::g_viewRect = { 100, 100, 100, 100 };

        POINT in = { 0, 0 };
        POINT out = { -1, -1 };
        CHECK(TestableZoomer::UnMagnify(in, out) == false);
    }
}

// ==================== ContentScale ====================

TEST_CASE("ContentScale") {
    TestableZoomer::ResetState();

    SUBCASE("zoom 1.0 - inactive") {
        float sx = 0.0f, sy = 0.0f;
        CHECK(TestableZoomer::ContentScale(sx, sy) == false);
    }

    SUBCASE("a centered 2.0x view moves content at half speed") {
        TestableZoomer::g_zoom.store(2.0f);

        float sx = 0.0f, sy = 0.0f;
        REQUIRE(TestableZoomer::ContentScale(sx, sy));
        CHECK(sx == doctest::Approx(0.5f));
        CHECK(sy == doctest::Approx(0.5f));
    }

    SUBCASE("a centered 4.0x view moves content at quarter speed") {
        TestableZoomer::g_zoom.store(4.0f);

        float sx = 0.0f, sy = 0.0f;
        REQUIRE(TestableZoomer::ContentScale(sx, sy));
        CHECK(sx == doctest::Approx(0.25f));
        CHECK(sy == doctest::Approx(0.25f));
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
        TestableZoomer::g_ctrlHeld = true;
        TestableZoomer::g_targetZoom.store(1.0f);
        LPARAM lParam = MAKELPARAM(400, 300);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), lParam);

        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(ZOOM_GEAR));
    }

    SUBCASE("WM_MOUSEWHEEL zoom out") {
        TestableZoomer::g_ctrlHeld = true;
        TestableZoomer::g_targetZoom.store(1.5f);
        LPARAM lParam = MAKELPARAM(400, 300);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -120), lParam);

        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.5f / ZOOM_GEAR));
    }

    SUBCASE("WM_MOUSEWHEEL without Ctrl - passes to the game") {
        TestableZoomer::g_ctrlHeld = false;
        TestableZoomer::g_focusValid = false;
        TestableZoomer::g_targetZoom.store(1.0f);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 120),
                                   MAKELPARAM(400, 300));

        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.0f));
        CHECK(!TestableZoomer::g_focusValid);
    }

    SUBCASE("partial wheel steps add up to a full one") {
        TestableZoomer::g_ctrlHeld = true;
        TestableZoomer::g_targetZoom.store(1.0f);
        LPARAM lParam = MAKELPARAM(400, 300);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 40), lParam);
        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.0f));
        CHECK(TestableZoomer::g_wheelRemainder == 40);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 40), lParam);
        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.0f));
        CHECK(TestableZoomer::g_wheelRemainder == 80);

        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 40), lParam);
        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(ZOOM_GEAR));
        CHECK(TestableZoomer::g_wheelRemainder == 0);
    }

    SUBCASE("negative partial wheel steps add up too") {
        TestableZoomer::g_ctrlHeld = true;
        TestableZoomer::g_targetZoom.store(1.5f);
        LPARAM lParam = MAKELPARAM(400, 300);

        for (int i = 0; i < 3; ++i)
            TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -40), lParam);

        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.5f / ZOOM_GEAR));
        CHECK(TestableZoomer::g_wheelRemainder == 0);
    }

    SUBCASE("a wheel that goes to the game drops the remainder") {
        TestableZoomer::g_ctrlHeld = true;
        TestableZoomer::g_targetZoom.store(1.0f);
        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 40),
                                   MAKELPARAM(400, 300));
        REQUIRE(TestableZoomer::g_wheelRemainder == 40);

        TestableZoomer::g_ctrlHeld = false;
        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 40),
                                   MAKELPARAM(400, 300));
        CHECK(TestableZoomer::g_wheelRemainder == 0);
        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.0f));
    }

    SUBCASE("WM_MOUSEWHEEL clamps to MAX") {
        TestableZoomer::g_ctrlHeld = true;
        TestableZoomer::g_targetZoom.store(3.95f);
        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 120),
                                   MAKELPARAM(400, 300));
        CHECK(TestableZoomer::g_targetZoom.load() <= ZOOM_MAX);
    }

    SUBCASE("WM_MOUSEWHEEL clamps to MIN") {
        TestableZoomer::g_ctrlHeld = true;
        TestableZoomer::g_targetZoom.store(1.0f);
        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -120),
                                   MAKELPARAM(400, 300));
        CHECK(TestableZoomer::g_targetZoom.load() >= ZOOM_MIN);
    }

    SUBCASE("WM_MOUSEWHEEL outside the view rect - ignored") {
        TestableZoomer::g_ctrlHeld = true;
        TestableZoomer::g_targetZoom.store(1.0f);
        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 120),
                                   MAKELPARAM(1900, 1300));

        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.0f));
    }

    SUBCASE("WM_MOUSEWHEEL latches the focus instead of moving the anchor") {
        TestableZoomer::g_ctrlHeld = true;
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

    SUBCASE("arrow keys at zoom 1.0 pass through") {
        TestableZoomer::g_zoom.store(ZOOM_DEFAULT);
        TestableZoomer::g_targetZoom.store(ZOOM_DEFAULT);
        const LONG prevX = TestableZoomer::g_centerX.load();
        g_recordedMsg = 0;

        LRESULT result = TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_RIGHT, 0);

        CHECK(result == 0);
        CHECK(g_recordedMsg == WM_KEYDOWN);
        CHECK(TestableZoomer::g_centerX.load() == prevX);
    }

    SUBCASE("arrow keys inside the snap epsilon do not pan") {
        const float barelyZoomed = ZOOM_DEFAULT + ZOOM_SNAP * 0.5f;
        TestableZoomer::g_zoom.store(barelyZoomed);
        TestableZoomer::g_targetZoom.store(barelyZoomed);
        const LONG prevX = TestableZoomer::g_centerX.load();
        g_recordedMsg = 0;

        TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_RIGHT, 0);

        CHECK(g_recordedMsg == WM_KEYDOWN);
        CHECK(TestableZoomer::g_centerX.load() == prevX);
    }

    SUBCASE("arrow keys while Ctrl is held pass through") {
        TestableZoomer::g_zoom.store(2.0f);
        TestableZoomer::g_targetZoom.store(2.0f);
        TestableZoomer::g_ctrlHeld = true;
        const LONG prevX = TestableZoomer::g_centerX.load();
        g_recordedMsg = 0;

        TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_RIGHT, 0);

        CHECK(g_recordedMsg == WM_KEYDOWN);
        CHECK(TestableZoomer::g_centerX.load() == prevX);
    }

    SUBCASE("mouse messages pass through untouched even when zoomed") {
        TestableZoomer::g_zoom.store(2.0f);
        TestableZoomer::g_targetZoom.store(2.0f);
        TestableZoomer::g_invZoom.store(0.5f);
        g_recordedMsg = 0;
        g_recordedLParam = 0;

        LPARAM lParam = MAKELPARAM(500, 400);
        TestableZoomer::NewWndProc(win.hWnd, WM_MOUSEMOVE, 0, lParam);

        CHECK(g_recordedMsg == WM_MOUSEMOVE);
        CHECK(g_recordedLParam == lParam);
    }

    SUBCASE("non-zoom message passes through") {
        LPARAM lParam = MAKELPARAM(100, 100);
        LRESULT result = TestableZoomer::NewWndProc(win.hWnd, WM_NCHITTEST, 0, lParam);
        CHECK(result == HTCLIENT);
    }
}

// ==================== Double Ctrl reset ====================

TEST_CASE("Double Ctrl reset") {
    TestableZoomer::ResetState();

    MockWindow win;
    REQUIRE(win.Create());
    TestableZoomer::g_hWnd = win.hWnd;
    TestableZoomer::UpdateClientCache(win.hWnd);

    SUBCASE("single Ctrl - does not reset zoom") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.8f);

        TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_CONTROL, 0);

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(1.5f));
        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.8f));
    }

    SUBCASE("Ctrl with the key repeat bit - ignored") {
        TestableZoomer::g_zoom.store(1.5f);
        TestableZoomer::g_targetZoom.store(1.8f);

        TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_CONTROL, 0);
        TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_CONTROL, 0x40000000);

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(1.5f));
        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(1.8f));
    }

    SUBCASE("double Ctrl - resets the zoom") {
        TestableZoomer::g_zoom.store(2.5f);
        TestableZoomer::g_targetZoom.store(2.8f);

        TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_CONTROL, 0);
        TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_CONTROL, 0);

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(ZOOM_DEFAULT));
        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(ZOOM_DEFAULT));
    }

    SUBCASE("system key messages do not reset - only WM_KEYDOWN counts") {
        TestableZoomer::g_zoom.store(2.5f);
        TestableZoomer::g_targetZoom.store(2.8f);

        TestableZoomer::NewWndProc(win.hWnd, WM_SYSKEYDOWN, VK_CONTROL, 0);
        TestableZoomer::NewWndProc(win.hWnd, WM_SYSKEYDOWN, VK_CONTROL, 0);

        CHECK(TestableZoomer::g_zoom.load() == doctest::Approx(2.5f));
        CHECK(TestableZoomer::g_targetZoom.load() == doctest::Approx(2.8f));
    }

    SUBCASE("double Ctrl also drops the focus latch") {
        TestableZoomer::g_zoom.store(2.5f);
        TestableZoomer::g_targetZoom.store(2.8f);
        TestableZoomer::g_focusValid = true;

        TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_CONTROL, 0);
        TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_CONTROL, 0);

        CHECK(TestableZoomer::g_focusValid == false);
    }

    SUBCASE("double press window") {
        CHECK(!TestableZoomer::RegisterDoublePress(1000));
        CHECK(TestableZoomer::RegisterDoublePress(1300));
        CHECK(!TestableZoomer::RegisterDoublePress(1400));
        CHECK(!TestableZoomer::RegisterDoublePress(1400 + DOUBLE_PRESS_MS + 1));
        CHECK(TestableZoomer::RegisterDoublePress(1400 + DOUBLE_PRESS_MS + 1 + 300));
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
        TestableZoomer::g_initStarted = true;
        TestableZoomer::g_wheelRemainder = 80;

        TestableZoomer::Shutdown();

        CHECK(TestableZoomer::g_zoom.load() == ZOOM_DEFAULT);
        CHECK(TestableZoomer::g_targetZoom.load() == ZOOM_DEFAULT);
        CHECK(TestableZoomer::g_invZoom.load() == 1.0f);
        CHECK(TestableZoomer::g_initialized == false);
        CHECK(TestableZoomer::g_initStarted == false);
        CHECK(TestableZoomer::g_wheelRemainder == 0);
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

// ==================== Init ====================

TEST_CASE("Init") {
    TestableZoomer::ResetState();

    SUBCASE("a second call does not start another thread") {
        TestableZoomer::g_initStarted = true;
        TestableZoomer::g_hThread = nullptr;

        TestableZoomer::Init();

        CHECK(TestableZoomer::g_hThread == nullptr);
        CHECK(TestableZoomer::g_initStarted == true);
    }
}

// ==================== Constants ====================

TEST_CASE("Constants") {
    CHECK(ZOOM_DEFAULT == 1.0f);
    CHECK(ZOOM_MIN == 1.0f);
    CHECK(ZOOM_MAX == 4.0f);
    CHECK(ZOOM_GEAR == doctest::Approx(1.15f));
    CHECK(ZOOM_LERP == doctest::Approx(0.15f));
    CHECK(ZOOM_SNAP == doctest::Approx(0.001f));
    CHECK(DOUBLE_PRESS_MS == 400);
    // Cross-checked against YRpp / ReSource / Encyclopedia (GameAddrs.hpp).
    CHECK(GameAddr::DSurface_ViewBounds == 0x886FA0);
    CHECK(GameAddr::DSurface_WindowBounds == 0x886FB0);
    CHECK(GameAddr::DSurface_Composite == 0x88731C);
    CHECK(GameAddr::ClampCoordMap == 0x6D8640);
    CHECK(GameAddr::TacticalMapClass_SetCameraPosition == 0x6D6000);
}

// ==================== ApplyWheelSteps ====================

TEST_CASE("ApplyWheelSteps") {
    TestableZoomer::ResetState();

    SUBCASE("one step gears the zoom") {
        CHECK(TestableZoomer::ApplyWheelSteps(1.0f, 1) == doctest::Approx(ZOOM_GEAR));
        CHECK(TestableZoomer::ApplyWheelSteps(2.0f, -1) == doctest::Approx(2.0f / ZOOM_GEAR));
        CHECK(TestableZoomer::ApplyWheelSteps(2.0f, 2) == doctest::Approx(2.0f * ZOOM_GEAR * ZOOM_GEAR));
    }

    SUBCASE("zero steps keeps the zoom") {
        CHECK(TestableZoomer::ApplyWheelSteps(1.5f, 0) == doctest::Approx(1.5f));
    }

    SUBCASE("result is clamped") {
        CHECK(TestableZoomer::ApplyWheelSteps(ZOOM_MAX, 1) == ZOOM_MAX);
        CHECK(TestableZoomer::ApplyWheelSteps(ZOOM_MIN, -1) == ZOOM_MIN);
    }
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

    SUBCASE("a pitch that cannot hold the offset view is rejected") {
        std::vector<unsigned char> surf = makeSurface();
        std::vector<unsigned char> backup(16, 0);
        // 4 pixels wide starting at column 2 need 12 bytes of every row,
        // a pitch of 10 only holds up to column 4.
        CHECK(RenderZoom::CopyRows(surf.data(), 10, view, backup.data(), 4, 2, true) == false);
        CHECK(RenderZoom::CopyRows(surf.data(), 12, view, backup.data(), 4, 2, true) == true);
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


// ==================== RenderZoom clamp sizes ====================

TEST_CASE("RenderZoom::ClampDimension") {
    SUBCASE("not magnified - the game dimension stands") {
        CHECK(RenderZoom::ClampDimension(640, 320, false) == 640);
    }

    SUBCASE("magnified - the source rect size is used") {
        CHECK(RenderZoom::ClampDimension(640, 320, true) == 320);
    }

    SUBCASE("an unusable source rect falls back to the game dimension") {
        CHECK(RenderZoom::ClampDimension(640, 0, true) == 640);
        CHECK(RenderZoom::ClampDimension(640, -1, true) == 640);
    }

    SUBCASE("outside the game the vanilla view size stands in") {
        // Unreadable globals fall back to the last value seen, seeded with
        // the 640x400 the game writes at init - not 0, which would pin
        // ClampCoordMap at the map origin.
        CHECK(RenderZoom::ClampWidth() == 640);
        CHECK(RenderZoom::ClampHeight() == 400);
    }
}

// ==================== Default view rect ====================

TEST_CASE("Zoomer::DefaultViewRect") {
    SUBCASE("outside the game the 800x600 fallback stands in") {
        CHECK(SameRect(Zoomer::DefaultViewRect(), RECT{ 0, 0, 800, 600 }));
    }

    SUBCASE("shutdown parks the view rect on the default") {
        TestableZoomer::ResetState();
        TestableZoomer::SetViewRect({ 0, 0, 1752, 1248 }, true);

        TestableZoomer::Shutdown();

        CHECK(SameRect(TestableZoomer::ViewRect(), Zoomer::DefaultViewRect()));
        CHECK(TestableZoomer::g_viewRectFromGame == false);
    }
}

// ==================== Syringe hook bodies ====================
// Main.cpp compiles into this test binary (test.vcxproj): the DEFINE_HOOK
// bodies are plain extern "C" functions driven through Syringe's REGISTERS,
// with the game globals redirected at a fake region (SetTestGameRegion).

#ifndef SYR_VER
#define SYR_VER 2
#endif
#include <Helpers/Macro.h>

extern "C" DWORD __cdecl GameInt(REGISTERS* R);
extern "C" DWORD __cdecl ViewCtrlPreRenderRestore(REGISTERS* R);
extern "C" DWORD __cdecl ViewCtrlPostRenderZoom(REGISTERS* R);
extern "C" DWORD __cdecl ViewCtrlClampWidth(REGISTERS* R);
extern "C" DWORD __cdecl ViewCtrlClampHeight(REGISTERS* R);
extern "C" DWORD __cdecl ViewCtrlProcessClickCoords(REGISTERS* R);
extern "C" DWORD __cdecl ViewCtrlDragBandStart(REGISTERS* R);
extern "C" DWORD __cdecl ViewCtrlDragBandEnd(REGISTERS* R);
extern "C" DWORD __cdecl ViewCtrlRightDragSpeed(REGISTERS* R);

#include <new>

namespace
{
    // 16 bit surface standing in for DSurface::Composite; the vtable slots are
    // the ones RenderZoom::LockView looks up (YRpp Surface.h).
    struct FakeSurface
    {
        void** vptr = nullptr;
        int width = 0;
        int height = 0;
        int pitch = 0;
        unsigned short* pixels = nullptr;
        int bpp = 2;
        int locks = 0;

        int GetBytesPerPixel() { return bpp; }
        int GetPitch() { return pitch; }
        int GetWidth() { return width; }
        int GetHeight() { return height; }
        void* Lock(int, int) { ++locks; return pixels; }
        bool Unlock() { --locks; return true; }
    };

    template<typename M>
    void* PmfToPtr(M m)
    {
        static_assert(sizeof(M) == sizeof(void*));
        void* p = nullptr;
        memcpy(&p, &m, sizeof p);
        return p;
    }

    // One VirtualAlloc'd region hosts everything IsGameReadable checks: the
    // module base override has to cover the global slots, the fake objects
    // and the pixel backing store.
    class FakeGameRegion
    {
    public:
        static constexpr SIZE_T kSize = 2 * 1024 * 1024;

        BYTE* base = nullptr;
        void** compositeSlot = nullptr;
        int* viewBounds = nullptr;
        int* windowBounds = nullptr;
        BYTE* dragObj = nullptr;
        void** vtable = nullptr;
        FakeSurface* surface = nullptr;
        unsigned short* pixels = nullptr;

        bool Create(int surfW = 800, int surfH = 600)
        {
            base = static_cast<BYTE*>(VirtualAlloc(
                nullptr, kSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!base) return false;

            compositeSlot = reinterpret_cast<void**>(base + 0x10);
            viewBounds = reinterpret_cast<int*>(base + 0x100);
            windowBounds = reinterpret_cast<int*>(base + 0x110);
            dragObj = base + 0x200;
            vtable = reinterpret_cast<void**>(base + 0x8000);
            pixels = reinterpret_cast<unsigned short*>(base + 0x10000);

            memset(vtable, 0, 33 * sizeof(void*));
            vtable[23] = PmfToPtr(&FakeSurface::Lock);
            vtable[24] = PmfToPtr(&FakeSurface::Unlock);
            vtable[28] = PmfToPtr(&FakeSurface::GetBytesPerPixel);
            vtable[29] = PmfToPtr(&FakeSurface::GetPitch);
            vtable[31] = PmfToPtr(&FakeSurface::GetWidth);
            vtable[32] = PmfToPtr(&FakeSurface::GetHeight);

            surface = new (base + 0x8100) FakeSurface;
            surface->vptr = vtable;
            surface->width = surfW;
            surface->height = surfH;
            surface->pitch = surfW * 2;
            surface->pixels = pixels;
            *compositeSlot = surface;

            SetView(0, 0, 640, 400);
            SetWindow(0, 0, surfW, surfH);

            RenderZoom::SetTestGameRegion(base, compositeSlot, viewBounds, windowBounds);
            return true;
        }

        void SetView(int x, int y, int w, int h)
        {
            viewBounds[0] = x; viewBounds[1] = y;
            viewBounds[2] = w; viewBounds[3] = h;
        }

        void SetWindow(int x, int y, int w, int h)
        {
            windowBounds[0] = x; windowBounds[1] = y;
            windowBounds[2] = w; windowBounds[3] = h;
        }

        static unsigned short PatternAt(int x, int y)
        {
            return static_cast<unsigned short>((x * 31 + y * 17) & 0xFFFF);
        }

        void FillPattern()
        {
            for (int y = 0; y < surface->height; ++y)
                for (int x = 0; x < surface->width; ++x)
                    pixels[y * surface->width + x] = PatternAt(x, y);
        }

        unsigned short At(int x, int y) const
        {
            return pixels[y * surface->width + x];
        }

        void Destroy()
        {
            RenderZoom::SetTestGameRegion(nullptr, nullptr, nullptr, nullptr);
            if (base) { VirtualFree(base, 0, MEM_RELEASE); base = nullptr; }
        }

        ~FakeGameRegion() { Destroy(); }
    };
}

TEST_CASE("Hook: ViewCtrlPreRenderRestore") {
    TestableZoomer::ResetState();
    RenderZoom::SetEnabled(false);

    SUBCASE("the stolen EAX is replayed with setne cl") {
        REGISTERS R{};
        R.EAX(0u);
        CHECK(ViewCtrlPreRenderRestore(&R) == 0x4F44B4);
        CHECK(R.EAX() == 0u);
        CHECK(R.CL() == 0u);

        R.EAX(7u);
        CHECK(ViewCtrlPreRenderRestore(&R) == 0x4F44B4);
        CHECK(R.EAX() == 7u);
        CHECK(R.CL() == 1u);
    }

    SUBCASE("a backup that cannot be restored forces EAX=2") {
        FakeGameRegion game;
        REQUIRE(game.Create(400, 300));
        game.FillPattern();
        Zoomer::SetViewRect({ 0, 0, 400, 300 }, true);
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);
        RenderZoom::SetEnabled(true);
        RenderZoom::PostRender();
        REQUIRE(game.surface->locks == 0);

        // Take the surface away: the restore fails and the frame is declared
        // dirty even though the pixels were never put back.
        RenderZoom::SetTestGameRegion(nullptr, nullptr, nullptr, nullptr);

        REGISTERS R{};
        R.EAX(5u);
        CHECK(ViewCtrlPreRenderRestore(&R) == 0x4F44B4);
        CHECK(R.EAX() == 2u);
        CHECK(R.CL() == 1u);
    }
}


TEST_CASE("Hook: ViewCtrlPostRenderZoom") {
    TestableZoomer::ResetState();
    RenderZoom::SetEnabled(false);

    // In the game the flag byte is the fixed address 0xB0B519; in the test
    // process that address can belong to a DLL image (ASLR), so the hook
    // reads through SetFlagByteForTest instead.
    BYTE flagByte = 0;
    SetFlagByteForTest(&flagByte);

    SUBCASE("the sidebar flag byte replaces AL") {
        flagByte = 0x5A;
        REGISTERS R{};
        R.EAX(0x11223300u);
        CHECK(ViewCtrlPostRenderZoom(&R) == 0x4F4520);
        CHECK(R.EAX() == 0x1122335Au);
    }

    SUBCASE("a cleared flag yields AL=0") {
        flagByte = 0x00;
        REGISTERS R{};
        R.EAX(0xFFFFFFFFu);
        CHECK(ViewCtrlPostRenderZoom(&R) == 0x4F4520);
        CHECK(R.EAX() == 0xFFFFFF00u);
    }

    SetFlagByteForTest(nullptr);
}

TEST_CASE("Hook: ViewCtrlClampWidth / ViewCtrlClampHeight") {
    TestableZoomer::ResetState();

    SUBCASE("outside the game the 640x400 seed stands in") {
        REGISTERS R{};
        CHECK(ViewCtrlClampWidth(&R) == 0x6D8654);
        CHECK(R.EBX() == 640u);
        CHECK(ViewCtrlClampHeight(&R) == 0x6D8690);
        CHECK(R.EBX() == 400u);
    }

    SUBCASE("while magnified the source rect size is clamped against") {
        FakeGameRegion game;
        REQUIRE(game.Create(800, 600));
        game.SetView(0, 0, 640, 400);
        game.SetWindow(0, 0, 800, 600);
        Zoomer::SetViewRect({ 0, 0, 640, 400 }, true);
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);
        REQUIRE(RenderZoom::AcceptViewRect({ 0, 0, 640, 400 }, GetTickCount()));

        const RenderZoom::SourceRect expect =
            RenderZoom::ComputeSourceRect(640, 400, 320, 200, 2.0f);

        REGISTERS R{};
        CHECK(ViewCtrlClampWidth(&R) == 0x6D8654);
        CHECK(R.EBX() == static_cast<DWORD>(expect.W));
        CHECK(ViewCtrlClampHeight(&R) == 0x6D8690);
        CHECK(R.EBX() == static_cast<DWORD>(expect.H));

        Zoomer::g_zoom.store(1.0f);
        Zoomer::g_targetZoom.store(1.0f);
        CHECK(ViewCtrlClampWidth(&R) == 0x6D8654);
        CHECK(R.EBX() == 640u);
    }
}

TEST_CASE("Hook: ViewCtrlProcessClickCoords") {
    TestableZoomer::ResetState();
    RenderZoom::SetEnabled(false);

    BYTE stack[0x80] = {};
    REGISTERS R{};
    R.ESP(reinterpret_cast<DWORD>(&stack[0]));

    SUBCASE("without an active zoom the point passes through") {
        POINT pt = { 100, 150 };
        *reinterpret_cast<POINT**>(&stack[0x28]) = &pt;
        R.EAX(0x1234u);
        R.EDX(0u);

        CHECK(ViewCtrlProcessClickCoords(&R) == 0x69232B);
        CHECK(R.EBP() == reinterpret_cast<DWORD>(&pt));
        CHECK(R.EAX() == 0x1234u);
    }

    SUBCASE("an active zoom un-magnifies the point") {
        Zoomer::SetViewRect({ 0, 0, 400, 300 }, true);
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);

        POINT pt = { 0, 0 }; // the view origin maps to the source origin
        *reinterpret_cast<POINT**>(&stack[0x28]) = &pt;
        R.EDX(0u);

        CHECK(ViewCtrlProcessClickCoords(&R) == 0x69232B);
        const POINT* got = reinterpret_cast<const POINT*>(R.EBP());
        CHECK(got != &pt);
        CHECK(got->x == 100);
        CHECK(got->y == 75);
    }

    SUBCASE("the optional out pointer receives EAX") {
        POINT pt = { 10, 10 };
        *reinterpret_cast<POINT**>(&stack[0x28]) = &pt;
        DWORD outValue = 0;
        R.EAX(0xABCDu);
        R.EDX(reinterpret_cast<DWORD>(&outValue));

        CHECK(ViewCtrlProcessClickCoords(&R) == 0x69232B);
        CHECK(outValue == 0xABCDu);
        CHECK(R.EBP() == reinterpret_cast<DWORD>(&pt));
    }

    SUBCASE("a null stack point yields a null EBP") {
        *reinterpret_cast<POINT**>(&stack[0x28]) = nullptr;
        CHECK(ViewCtrlProcessClickCoords(&R) == 0x69232B);
        CHECK(R.EBP() == 0u);
    }
}

TEST_CASE("Hook: ViewCtrlDragBandStart / End") {
    TestableZoomer::ResetState();
    RenderZoom::SetEnabled(false);

    BYTE stack[0x40] = {};
    BYTE obj[0x1000] = {};
    memset(obj, 0, sizeof obj);
    *reinterpret_cast<DWORD*>(obj + 0xD90) = 0x00C0FFEEu;

    REGISTERS R{};
    R.ESP(reinterpret_cast<DWORD>(&stack[0]));
    R.ECX(reinterpret_cast<DWORD>(obj));

    SUBCASE("start: an inactive zoom keeps the stack point") {
        POINT pt = { 50, 60 };
        *reinterpret_cast<POINT**>(&stack[4]) = &pt;

        CHECK(ViewCtrlDragBandStart(&R) == 0x6D9F86);
        CHECK(R.EAX() == 0x00C0FFEEu);
        CHECK(*reinterpret_cast<POINT**>(&stack[4]) == &pt);
    }

    SUBCASE("start: an active zoom rewrites the stack point") {
        Zoomer::SetViewRect({ 0, 0, 400, 300 }, true);
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);
        POINT pt = { 0, 0 };
        *reinterpret_cast<POINT**>(&stack[4]) = &pt;

        CHECK(ViewCtrlDragBandStart(&R) == 0x6D9F86);
        const POINT* got = *reinterpret_cast<POINT**>(&stack[4]);
        CHECK(got != &pt);
        CHECK(got->x == 100);
        CHECK(got->y == 75);
        CHECK(R.EAX() == 0x00C0FFEEu);
    }

    SUBCASE("start: a null this pointer reads EAX=0") {
        POINT pt = { 50, 60 };
        *reinterpret_cast<POINT**>(&stack[4]) = &pt;
        R.ECX(0u);

        CHECK(ViewCtrlDragBandStart(&R) == 0x6D9F86);
        CHECK(R.EAX() == 0u);
    }

    SUBCASE("end: an active zoom rewrites the stack point") {
        Zoomer::SetViewRect({ 0, 0, 400, 300 }, true);
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);
        POINT pt = { 0, 0 };
        *reinterpret_cast<POINT**>(&stack[4]) = &pt;

        CHECK(ViewCtrlDragBandEnd(&R) == 0x6D9FC6);
        const POINT* got = *reinterpret_cast<POINT**>(&stack[4]);
        CHECK(got != &pt);
        CHECK(got->x == 100);
        CHECK(got->y == 75);
        CHECK(R.EAX() == 0x00C0FFEEu);
    }

    SUBCASE("end: a null stack point leaves the slot alone") {
        *reinterpret_cast<POINT**>(&stack[4]) = nullptr;

        CHECK(ViewCtrlDragBandEnd(&R) == 0x6D9FC6);
        CHECK(*reinterpret_cast<POINT**>(&stack[4]) == nullptr);
        CHECK(R.EAX() == 0x00C0FFEEu);
    }
}

TEST_CASE("Hook: ViewCtrlRightDragSpeed") {
    TestableZoomer::ResetState();
    RenderZoom::SetEnabled(false);

    FakeGameRegion game;
    REQUIRE(game.Create(800, 600));

    BYTE* obj = game.dragObj;
    BYTE stack[0x40] = {};
    int* speedX = reinterpret_cast<int*>(&stack[0x18]);
    int* speedY = reinterpret_cast<int*>(&stack[0x1C]);

    REGISTERS R{};
    R.ESP(reinterpret_cast<DWORD>(&stack[0]));
    R.EBX(reinterpret_cast<DWORD>(obj));

    Zoomer::SetViewRect({ 0, 0, 400, 300 }, true);

    SUBCASE("an active zoom scales both positive speeds down") {
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);
        obj[0x5558] = 1;
        *speedX = 10; *speedY = 6;

        CHECK(ViewCtrlRightDragSpeed(&R) == 0x693797);
        CHECK(*speedX == 5);
        CHECK(*speedY == 3);
        CHECK(R.ESI() == 0u);
        CHECK(*reinterpret_cast<DWORD*>(&stack[0x28]) == 0u);
    }

    SUBCASE("the drag flag off leaves the speeds alone") {
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);
        obj[0x5558] = 0;
        *speedX = 10; *speedY = 6;

        ViewCtrlRightDragSpeed(&R);
        CHECK(*speedX == 10);
        CHECK(*speedY == 6);
    }

    SUBCASE("without an active zoom the speeds stay untouched") {
        Zoomer::g_zoom.store(1.0f);
        Zoomer::g_targetZoom.store(1.0f);
        obj[0x5558] = 1;
        *speedX = 10; *speedY = 6;

        ViewCtrlRightDragSpeed(&R);
        CHECK(*speedX == 10);
        CHECK(*speedY == 6);
    }

    SUBCASE("negative speeds are not scaled") {
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);
        obj[0x5558] = 1;
        *speedX = -8; *speedY = -6;

        ViewCtrlRightDragSpeed(&R);
        CHECK(*speedX == -8);
        CHECK(*speedY == -6);
    }

    SUBCASE("a null object still zeroes ESI and the stack slot") {
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);
        obj[0x5558] = 1;
        *speedX = 10; *speedY = 6;
        R.EBX(0u);

        CHECK(ViewCtrlRightDragSpeed(&R) == 0x693797);
        CHECK(*speedX == 10);
        CHECK(R.ESI() == 0u);
    }
}

// ==================== Render pipeline against a fake game ====================

TEST_CASE("RenderZoom: observes the game view rect through the fake region") {
    TestableZoomer::ResetState();
    RenderZoom::SetEnabled(true);

    SUBCASE("view bounds smaller than the window are published") {
        FakeGameRegion game;
        REQUIRE(game.Create(800, 600));
        game.SetView(10, 20, 640, 400);
        game.SetWindow(0, 0, 800, 600);

        CHECK(RenderZoom::PreRender() == false);
        CHECK(SameRect(Zoomer::ViewRect(), RECT{ 10, 20, 650, 420 }));
    }

    SUBCASE("a view rect equal to the window is ignored") {
        FakeGameRegion game;
        REQUIRE(game.Create(800, 600));
        game.SetView(0, 0, 800, 600);
        game.SetWindow(0, 0, 800, 600);
        Zoomer::SetViewRect({ 0, 0, 640, 400 }, true);

        CHECK(RenderZoom::PreRender() == false);
        CHECK(SameRect(Zoomer::ViewRect(), RECT{ 0, 0, 640, 400 }));
    }
}

TEST_CASE("RenderZoom: PostRender magnifies, PreRender restores") {
    TestableZoomer::ResetState();
    RenderZoom::SetEnabled(true);

    FakeGameRegion game;
    REQUIRE(game.Create(800, 600));
    game.FillPattern();
    Zoomer::SetViewRect({ 0, 0, 400, 300 }, true);
    Zoomer::g_zoom.store(2.0f);
    Zoomer::g_targetZoom.store(2.0f);

    RenderZoom::PostRender();
    REQUIRE(game.surface->locks == 0);

    const RenderZoom::SourceRect src =
        RenderZoom::ComputeSourceRect(400, 300, 200, 150, 2.0f);

    SUBCASE("the view rect holds the magnified source pixels") {
        CHECK(game.At(0, 0) == FakeGameRegion::PatternAt(src.X, src.Y));
        CHECK(game.At(399, 299) ==
            FakeGameRegion::PatternAt(src.X + src.W - 1, src.Y + src.H - 1));
        // The anchor maps onto itself.
        CHECK(game.At(200, 150) == FakeGameRegion::PatternAt(200, 150));
    }

    SUBCASE("pixels outside the view rect stay untouched") {
        CHECK(game.At(50, 400) == FakeGameRegion::PatternAt(50, 400));
        CHECK(game.At(500, 100) == FakeGameRegion::PatternAt(500, 100));
    }

    SUBCASE("PreRender puts the original pixels back without a repaint") {
        CHECK(RenderZoom::PreRender() == false);
        CHECK(game.At(0, 0) == FakeGameRegion::PatternAt(0, 0));
        CHECK(game.At(399, 299) == FakeGameRegion::PatternAt(399, 299));
        // A second frame without a backup changes nothing.
        CHECK(RenderZoom::PreRender() == false);
    }
}

TEST_CASE("RenderZoom: lock rejections leave the frame alone") {
    TestableZoomer::ResetState();
    RenderZoom::SetEnabled(true);

    SUBCASE("a 24 bit surface is not backed up") {
        FakeGameRegion game;
        REQUIRE(game.Create(800, 600));
        game.FillPattern();
        game.surface->bpp = 3;
        Zoomer::SetViewRect({ 0, 0, 400, 300 }, true);
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);

        RenderZoom::PostRender();
        CHECK(game.At(0, 0) == FakeGameRegion::PatternAt(0, 0));
        CHECK(RenderZoom::PreRender() == false);
    }

    SUBCASE("a view rect wider than the surface is rejected") {
        FakeGameRegion game;
        REQUIRE(game.Create(800, 600));
        game.FillPattern();
        Zoomer::SetViewRect({ 0, 0, 900, 600 }, true);
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);

        RenderZoom::PostRender();
        CHECK(game.At(0, 0) == FakeGameRegion::PatternAt(0, 0));
        CHECK(RenderZoom::PreRender() == false);
    }

    SUBCASE("an odd pitch is rejected") {
        FakeGameRegion game;
        REQUIRE(game.Create(800, 600));
        game.FillPattern();
        game.surface->pitch = 801;
        Zoomer::SetViewRect({ 0, 0, 400, 300 }, true);
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);

        RenderZoom::PostRender();
        CHECK(game.At(0, 0) == FakeGameRegion::PatternAt(0, 0));
    }

    SUBCASE("a pitch below the view right edge fails CopyRows and unlocks") {
        FakeGameRegion game;
        REQUIRE(game.Create(800, 600));
        game.FillPattern();
        // Positive and even, so LockView accepts it, but below
        // view.right * 2, so CopyRows refuses to walk the rows.
        game.surface->pitch = 100;
        Zoomer::SetViewRect({ 0, 0, 400, 300 }, true);
        Zoomer::g_zoom.store(2.0f);
        Zoomer::g_targetZoom.store(2.0f);

        RenderZoom::PostRender();
        CHECK(game.surface->locks == 0);
        CHECK(game.At(0, 0) == FakeGameRegion::PatternAt(0, 0));
        CHECK(RenderZoom::PreRender() == false);
    }
}

TEST_CASE("RenderZoom: Init disables the module outside the game") {
    TestableZoomer::ResetState();
    RenderZoom::SetEnabled(true);

    RenderZoom::Init();
    CHECK(RenderZoom::IsEnabled() == false);
}

TEST_CASE("GameCamera: Enable refuses outside the game") {
    TestableZoomer::ResetState();

    GameCamera::Enable();
    CHECK(GameCamera::IsEnabled() == false);

    POINT p = { 0, 0 };
    CHECK(GameCamera::Read(p) == false);
    CHECK(GameCamera::WriteAbs(p) == false);
    CHECK(GameCamera::ShiftBy(1, 1) == false);

    GameCamera::Disable(); // already off, nothing to do
    CHECK(GameCamera::IsEnabled() == false);
}

TEST_CASE("Hook: GameInt") {
    TestableZoomer::ResetState();

    REGISTERS R{};
    CHECK(GameInt(&R) == 0);

    // Let the init thread wind down before the next case inspects state.
    if (TestableZoomer::g_hThread)
        WaitForSingleObject(TestableZoomer::g_hThread, 2000);
}

// ==================== GameCamera against a fake TacticalClass ====================

namespace
{
    // Fake TacticalClass instance: only the view center at +0xD64/+0xD68 is
    // touched (by GameCamera through its SetTacticalInstanceForTest seam, so
    // the fixed game slot at 0x887324 stays untouched as well).
    constexpr size_t VIEW_CENTER_X_OFF = 0xD64;
    constexpr size_t VIEW_CENTER_Y_OFF = 0xD68;

    struct FakeTactical
    {
        BYTE obj[0xD64 + 8] = {};

        void SetViewCenter(int x, int y)
        {
            *reinterpret_cast<int*>(obj + VIEW_CENTER_X_OFF) = x;
            *reinterpret_cast<int*>(obj + VIEW_CENTER_Y_OFF) = y;
        }

        POINT ViewCenter() const
        {
            POINT p = {
                *reinterpret_cast<const int*>(obj + VIEW_CENTER_X_OFF),
                *reinterpret_cast<const int*>(obj + VIEW_CENTER_Y_OFF)
            };
            return p;
        }
    };

    // WriteAbs target: records the requested position as the new view center
    // (g_stubAccepts simulates the game clamping the move to a no-op).
    static bool g_stubAccepts = true;

    static void __fastcall StubSetCameraPosition(void* self, void* /*edx*/, void* point)
    {
        if (!g_stubAccepts) return;
        const POINT* p = static_cast<const POINT*>(point);
        BYTE* base = static_cast<BYTE*>(self);
        *reinterpret_cast<int*>(base + VIEW_CENTER_X_OFF) = p->x;
        *reinterpret_cast<int*>(base + VIEW_CENTER_Y_OFF) = p->y;
    }

    static void AttachFakeCamera(FakeTactical& tac)
    {
        g_stubAccepts = true;
        GameCamera::SetCameraPositionForTest(reinterpret_cast<void*>(&StubSetCameraPosition));
        GameCamera::EnableForTest();
        GameCamera::SetTacticalInstanceForTest(tac.obj);
    }

    static void DetachFakeCamera()
    {
        g_stubAccepts = true;
        GameCamera::SetCameraPositionForTest(nullptr);
        GameCamera::SetTacticalInstanceForTest(nullptr);
        GameCamera::Disable();
    }
}

TEST_CASE("GameCamera: read, write and shift through a fake tactical instance") {
    TestableZoomer::ResetState();
    FakeTactical tac;

    GameCamera::SetCameraPositionForTest(reinterpret_cast<void*>(&StubSetCameraPosition));

    POINT p = { 7, 7 };

    // Disabled wrapper: every entry refuses before touching game memory.
    CHECK(GameCamera::Read(p) == false);

    GameCamera::EnableForTest();

    // No instance in the slot yet.
    CHECK(GameCamera::Read(p) == false);
    CHECK(GameCamera::WriteAbs(p) == false);
    CHECK(GameCamera::ShiftBy(0, 0) == true);
    CHECK(GameCamera::ShiftBy(10, 0) == false);

    // Instance present: read the view center.
    GameCamera::SetTacticalInstanceForTest(tac.obj);
    tac.SetViewCenter(123, 45);
    CHECK(GameCamera::Read(p) == true);
    CHECK(p.x == 123);
    CHECK(p.y == 45);

    // Absolute placement runs through the (stubbed) SetCameraPosition.
    CHECK(GameCamera::WriteAbs({ 200, 150 }) == true);
    POINT center = tac.ViewCenter();
    CHECK(center.x == 200);
    CHECK(center.y == 150);

    // Read + move + verify the camera actually ended up elsewhere.
    CHECK(GameCamera::ShiftBy(50, 25) == true);
    center = tac.ViewCenter();
    CHECK(center.x == 250);
    CHECK(center.y == 175);

    // The game refused the move: the center did not change -> failure.
    g_stubAccepts = false;
    CHECK(GameCamera::ShiftBy(10, 0) == false);

    GameCamera::Disable();
    CHECK(GameCamera::IsEnabled() == false);

    DetachFakeCamera();
}

TEST_CASE("Zoomer: camera steps and the offset drive the fake camera") {
    TestableZoomer::ResetState();
    FakeTactical tac;
    AttachFakeCamera(tac);

    TestableZoomer::g_centerX = 400;
    TestableZoomer::g_centerY = 300;
    tac.SetViewCenter(400, 300);

    // Invalid zoom pairs return before anything is computed.
    TestableZoomer::ApplyCameraStep(0.0f, 2.0f);
    TestableZoomer::ApplyCameraStep(2.0f, 2.0f);

    // No focus latch: the view center doubles as focus == fixed point -> no shift.
    TestableZoomer::g_focusValid = false;
    TestableZoomer::ApplyCameraStep(1.0f, 2.0f);
    CHECK(TestableZoomer::g_camOffset.x == 0);
    CHECK(TestableZoomer::g_camOffset.y == 0);

    // Focus left of the center: the camera moves left, the offset records it.
    TestableZoomer::g_focusValid = true;
    TestableZoomer::g_focusX = 300;
    TestableZoomer::g_focusY = 300;
    TestableZoomer::ApplyCameraStep(1.0f, 2.0f);
    CHECK(TestableZoomer::g_camOffset.x == -50);
    CHECK(TestableZoomer::g_camOffset.y == 0);
    POINT center = tac.ViewCenter();
    CHECK(center.x == 350);
    CHECK(center.y == 300);

    // The game refuses the move: the offset stays as it was.
    g_stubAccepts = false;
    TestableZoomer::ApplyCameraStep(2.0f, 1.0f);
    CHECK(TestableZoomer::g_camOffset.x == -50);

    // Undo while the game refuses: the offset survives.
    TestableZoomer::UndoCameraOffset();
    CHECK(TestableZoomer::g_camOffset.x == -50);

    // Undo while the game accepts: the offset is dropped.
    g_stubAccepts = true;
    TestableZoomer::UndoCameraOffset();
    CHECK(TestableZoomer::g_camOffset.x == 0);
    CHECK(TestableZoomer::g_camOffset.y == 0);
    center = tac.ViewCenter();
    CHECK(center.x == 400);
    CHECK(center.y == 300);

    // Panning through the enabled wrapper (the return value is ignored here).
    TestableZoomer::PanCamera(10, 5);
    center = tac.ViewCenter();
    CHECK(center.x == 410);
    CHECK(center.y == 305);

    DetachFakeCamera();
}

TEST_CASE("DefaultViewRect follows the redirected WindowBounds") {
    TestableZoomer::ResetState();
    FakeGameRegion game;
    REQUIRE(game.Create(800, 600));

    // Fixture defaults to {0, 0, 800, 600}.
    RECT r = Zoomer::DefaultViewRect();
    CHECK(SameRect(r, RECT{ 0, 0, 800, 600 }));

    game.SetWindow(100, 50, 640, 480);
    r = Zoomer::DefaultViewRect();
    CHECK(SameRect(r, RECT{ 100, 50, 740, 530 }));

    // Non-positive extent falls back to the default rect.
    game.SetWindow(10, 10, 0, 400);
    CHECK(SameRect(Zoomer::DefaultViewRect(), RECT{ 0, 0, 800, 600 }));
}

TEST_CASE("NewWndProc: arrow keys pan while zoomed") {
    TestableZoomer::ResetState();

    MockWindow win;
    REQUIRE(win.Create());
    TestableZoomer::g_hWnd = win.hWnd;
    TestableZoomer::g_zoom.store(2.0f);
    TestableZoomer::g_targetZoom.store(2.0f);

    const int stepX = (int)(800.0f / 2.0f * 0.2f);
    const int stepY = (int)(600.0f / 2.0f * 0.2f);
    const LONG cx0 = TestableZoomer::g_centerX.load();
    const LONG cy0 = TestableZoomer::g_centerY.load();

    g_recordedMsg = 0;

    CHECK(TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_LEFT, 0) == 0);
    CHECK(TestableZoomer::g_centerX.load() == cx0 - stepX);

    CHECK(TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_RIGHT, 0) == 0);
    CHECK(TestableZoomer::g_centerX.load() == cx0);

    CHECK(TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_UP, 0) == 0);
    CHECK(TestableZoomer::g_centerY.load() == cy0 - stepY);

    CHECK(TestableZoomer::NewWndProc(win.hWnd, WM_KEYDOWN, VK_DOWN, 0) == 0);
    CHECK(TestableZoomer::g_centerY.load() == cy0);

    // The keys were consumed, not passed on to the game.
    CHECK(g_recordedMsg == 0);
}

TEST_CASE("Shutdown joins a pending init thread") {
    TestableZoomer::ResetState();

    Zoomer::g_hThread = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(Zoomer::g_hThread != nullptr);

    TestableZoomer::Shutdown();
    CHECK(Zoomer::g_hThread == nullptr);
}

TEST_CASE("DllMain wires the log module on attach and detaches cleanly") {
    TestableZoomer::ResetState();

    HMODULE exe = GetModuleHandleW(nullptr);

    CHECK(DllMain(exe, DLL_PROCESS_ATTACH, nullptr) == TRUE);

    // With a module handle set the log path resolves and the write lands
    // in a real file next to the test executable.
    LOG("log coverage probe");
    const char* logPath = Debug::GetLogPath();
    CHECK(logPath[0] != '\0');
    CHECK(GetFileAttributesA(logPath) != INVALID_FILE_ATTRIBUTES);

    CHECK(DllMain(exe, DLL_PROCESS_DETACH, nullptr) == TRUE);

    DeleteFileA(logPath);
    Debug::SetDllHandle(nullptr);
}

// ==================== ScalerConflict ====================

namespace
{
    // Copies our own build output to %TEMP%\ViewCtrlScalerTest\<fileName> so
    // a module with that file name can be mapped without running its code.
    // Returns the full path, empty when no build output is around.
    std::wstring MakeFakeScalerFile(const wchar_t* fileName)
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path(exe);
        // <repo>\tests\bin\ViewCtrlTests.exe -> <repo>
        size_t cut = path.find_last_of(L'\\');
        size_t cut2 = cut ? path.find_last_of(L'\\', cut - 1) : std::wstring::npos;
        size_t cut3 = cut2 ? path.find_last_of(L'\\', cut2 - 1) : std::wstring::npos;
        if (cut3 == std::wstring::npos || cut3 == 0)
            return {};
        const std::wstring root = path.substr(0, cut3);

        std::wstring src = root + L"\\Release\\ViewCtrl.dll";
        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES)
            src = root + L"\\Debug\\ViewCtrl.dll";
        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES)
            return {};

        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring dir = std::wstring(temp) + L"ViewCtrlScalerTest";
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring dst = dir + L"\\" + fileName;
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
            return {};
        return dst;
    }

    void RemoveFakeScalerFile(const std::wstring& path)
    {
        if (path.empty())
            return;
        DeleteFileW(path.c_str());
        const size_t slash = path.find_last_of(L'\\');
        if (slash != std::wstring::npos)
            RemoveDirectoryW(path.substr(0, slash).c_str());
    }

    // A mapped fake scaler that cleans up after itself even when a REQUIRE
    // aborts the test case.
    struct FakeScaler
    {
        std::wstring path;
        HMODULE mod = nullptr;

        explicit FakeScaler(std::wstring p) : path(std::move(p)) {}
        ~FakeScaler()
        {
            Unmap();
            RemoveFakeScalerFile(path);
        }

        bool Map()
        {
            if (path.empty())
                return false;
            mod = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
            return mod != nullptr;
        }

        void Unmap()
        {
            if (mod)
            {
                FreeLibrary(mod);
                mod = nullptr;
            }
        }
    };

    // Minimal mapped-style PE image whose .syhks00 holds hookdecl entries
    // ({addr, size, name ptr, pad} = 16 bytes on x86). `wrongSection` puts
    // the table behind a differently named section instead.
    struct FakeSyringeImage
    {
        std::vector<unsigned char> bytes;

        explicit FakeSyringeImage(const std::vector<unsigned int>& hookAddrs,
            bool withSection = true, bool validPe = true, bool wrongSection = false)
        {
            constexpr size_t IMG = 0x1000;
            constexpr size_t SEC_RVA = 0x600;
            bytes.assign(IMG, 0);
            if (!validPe)
            {
                bytes[0] = 'X';
                return;
            }

            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
            dos->e_magic = IMAGE_DOS_SIGNATURE;
            dos->e_lfanew = 0x80;

            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(bytes.data() + 0x80);
            nt->Signature = IMAGE_NT_SIGNATURE;
            nt->FileHeader.NumberOfSections = withSection ? 1 : 0;
            nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER);

            if (!withSection)
                return;
            auto* sec = IMAGE_FIRST_SECTION(nt);
            std::memcpy(sec->Name, wrongSection ? ".other00" : ".syhks00", 8);
            sec->Misc.VirtualSize = static_cast<DWORD>(hookAddrs.size() * 16);
            sec->VirtualAddress = static_cast<DWORD>(SEC_RVA);
            size_t off = 0;
            for (unsigned int addr : hookAddrs)
            {
                std::memcpy(bytes.data() + SEC_RVA + off, &addr, sizeof(addr));
                off += 16;
            }
        }
    };
}

TEST_CASE("InitThread: hooks a visible window with GScript present") {
    TestableZoomer::ResetState();

    MockWindow win;
    REQUIRE(win.Create());
    ShowWindow(win.hWnd, SW_SHOW);
    UpdateWindow(win.hWnd);

    // A stand-in GScript.ext: our own build output mapped under that file
    // name, its code never running - only GetModuleHandleA("GScript.ext")
    // inside InitThread must succeed so the warning branch is taken.
    FakeScaler gscript(MakeFakeScalerFile(L"GScript.ext"));
    REQUIRE(!gscript.path.empty());
    REQUIRE(gscript.Map());

    // GameInt refuses to run while GScript is loaded - it is the mod's own
    // scaler - so drive the init thread directly; the gate itself is
    // covered by the ScalerConflict cases below.
    Zoomer::Init();
    REQUIRE(TestableZoomer::g_hThread != nullptr);
    WaitForSingleObject(TestableZoomer::g_hThread, 2000);

    CHECK(TestableZoomer::g_hWnd != nullptr);
    CHECK(TestableZoomer::g_wndProcHooked == true);
    CHECK(TestableZoomer::OriginalWndProc != nullptr);
    CHECK(TestableZoomer::g_initialized == true);

    // Restores the window procedure before the mock window is destroyed.
    TestableZoomer::Shutdown();
    CHECK(TestableZoomer::g_wndProcHooked == false);
    CHECK(TestableZoomer::OriginalWndProc == nullptr);

    gscript.Unmap();
}

TEST_CASE("ScalerConflict: known scaler module names") {
    CHECK(ScalerConflict::IsScalerModuleName(L"GScript.ext"));
    CHECK(ScalerConflict::IsScalerModuleName(L"gscript.ext"));
    CHECK(ScalerConflict::IsScalerModuleName(L"GSCRIPT.EXT"));
    CHECK(ScalerConflict::IsScalerModuleName(L"Telescope.dll"));
    CHECK(ScalerConflict::IsScalerModuleName(L"telescope.DLL"));

    CHECK(!ScalerConflict::IsScalerModuleName(L"Phobos.ext"));
    CHECK(!ScalerConflict::IsScalerModuleName(L"Ares.dll"));
    CHECK(!ScalerConflict::IsScalerModuleName(L"ViewCtrl.dll"));
    CHECK(!ScalerConflict::IsScalerModuleName(L""));
    CHECK(!ScalerConflict::IsScalerModuleName(nullptr));
}

TEST_CASE("ScalerConflict: own .syhks00 declares the nine hook sites") {
    unsigned int sites[16] = {};
    CHECK(ScalerConflict::OwnHookSites(sites, 16) == 9);
    CHECK(sites[0] == 0x52CAE9u);
    CHECK(sites[8] == 0x693791u);

    CHECK(ScalerConflict::OwnHookSites(nullptr, 4) == 0);
    CHECK(ScalerConflict::OwnHookSites(sites, 0) == 0);

    unsigned int tiny[2] = {};
    CHECK(ScalerConflict::OwnHookSites(tiny, 2) == 2);
    CHECK(tiny[0] == 0x52CAE9u);
}

TEST_CASE("ScalerConflict: .syhks00 share separates co-hookers from scalers") {
    unsigned int sites[16] = {};
    const unsigned int n = ScalerConflict::OwnHookSites(sites, 16);
    REQUIRE(n == 9);

    // One or two shared sites: an ordinary framework plugin, not a scaler.
    FakeSyringeImage oneSite({ 0x1234u, sites[0] });
    CHECK(!ScalerConflict::ImageClaimsScalerShare(oneSite.bytes.data(), sites, n));
    FakeSyringeImage twoSites({ sites[0], sites[1] });
    CHECK(!ScalerConflict::ImageClaimsScalerShare(twoSites.bytes.data(), sites, n));

    // Three or more: another copy of the zoom pipeline (Telescope has all).
    FakeSyringeImage threeSites({ sites[0], sites[1], sites[2] });
    CHECK(ScalerConflict::ImageClaimsScalerShare(threeSites.bytes.data(), sites, n));
    const std::vector<unsigned int> all(sites, sites + n);
    FakeSyringeImage nineSites(all);
    CHECK(ScalerConflict::ImageClaimsScalerShare(nineSites.bytes.data(), sites, n));

    // Foreign hooks only, no .syhks00 at all, not a PE image.
    FakeSyringeImage foreign({ 0x1234u, 0x5555u });
    CHECK(!ScalerConflict::ImageClaimsScalerShare(foreign.bytes.data(), sites, n));
    FakeSyringeImage noSection({}, false);
    CHECK(!ScalerConflict::ImageClaimsScalerShare(noSection.bytes.data(), sites, n));
    FakeSyringeImage wrongSection({ sites[0], sites[1], sites[2] },
        true, true, true);
    CHECK(!ScalerConflict::ImageClaimsScalerShare(wrongSection.bytes.data(), sites, n));
    FakeSyringeImage junk({}, true, false);
    CHECK(!ScalerConflict::ImageClaimsScalerShare(junk.bytes.data(), sites, n));

    // Corrupt DOS/NT headers.
    FakeSyringeImage badLfanew({ sites[0] });
    reinterpret_cast<IMAGE_DOS_HEADER*>(badLfanew.bytes.data())->e_lfanew = 0x7FFFFFFF;
    CHECK(!ScalerConflict::ImageClaimsScalerShare(badLfanew.bytes.data(), sites, n));

    FakeSyringeImage badSig({ sites[0] });
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(badSig.bytes.data());
    reinterpret_cast<IMAGE_NT_HEADERS*>(badSig.bytes.data() + dos->e_lfanew)->Signature = 0;
    CHECK(!ScalerConflict::ImageClaimsScalerShare(badSig.bytes.data(), sites, n));

    // Degenerate inputs.
    CHECK(!ScalerConflict::ImageClaimsScalerShare(nullptr, sites, n));
    CHECK(!ScalerConflict::ImageClaimsScalerShare(threeSites.bytes.data(), sites, 0));
    CHECK(!ScalerConflict::ImageClaimsScalerShare(threeSites.bytes.data(), nullptr, n));
}

TEST_CASE("ScalerConflict: a clean process reports no conflict") {
    CHECK(ScalerConflict::Present() == false);
}

TEST_CASE("ScalerConflict: Present() detects a loaded scaler") {
    FakeScaler byName(MakeFakeScalerFile(L"GScript.ext"));
    REQUIRE(!byName.path.empty());
    FakeScaler byClaim(MakeFakeScalerFile(L"ScalerClaim.dll"));
    REQUIRE(!byClaim.path.empty());

    // Name match: the copy is a full fork, but GScript.ext is the flag.
    REQUIRE(byName.Map());
    CHECK(ScalerConflict::Present());
    byName.Unmap();
    CHECK(!ScalerConflict::Present());

    // Hook-claim match under an unknown file name: our sites are a full share.
    REQUIRE(byClaim.Map());
    CHECK(ScalerConflict::Present());
    byClaim.Unmap();
    CHECK(!ScalerConflict::Present());
}

TEST_CASE("GameInt: stays inert while a conflicting scaler is loaded") {
    Zoomer::Shutdown(); // clean slate: g_initStarted / g_hThread reset
    TestableZoomer::ResetState();

    FakeScaler scaler(MakeFakeScalerFile(L"GScript.ext"));
    REQUIRE(!scaler.path.empty());
    REQUIRE(scaler.Map());

    REGISTERS R{};
    CHECK(GameInt(&R) == 0);
    CHECK(TestableZoomer::g_hThread == nullptr);
    CHECK(TestableZoomer::g_initialized == false);

    // The gate lifts again once the scaler is gone.
    scaler.Unmap();
    REGISTERS R2{};
    CHECK(GameInt(&R2) == 0);
    CHECK(TestableZoomer::g_hThread != nullptr);
    WaitForSingleObject(TestableZoomer::g_hThread, 2000);
    TestableZoomer::Shutdown();
}

