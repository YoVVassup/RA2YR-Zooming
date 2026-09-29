#pragma once

#include <windows.h>
#include <cmath>
#include <atomic>

constexpr float ZOOM_DEFAULT = 1.0f;
constexpr float ZOOM_MIN = 1.0f;
constexpr float ZOOM_MAX = 4.0f;
constexpr float ZOOM_GEAR = 1.15f;
constexpr float ZOOM_LERP = 0.15f;
constexpr float ZOOM_SNAP = 0.001f;

// Maximum gap between two key presses that counts as a double press.
constexpr DWORD DOUBLE_PRESS_MS = 400;

class Zoomer
{
public:
	static void Init();
	static void Shutdown();

	// Render hooks call this once per frame: advances the zoom lerp and moves
	// the game camera so the focus point stays under the cursor.
	static void TickFrame();

	// Current (lerped) zoom factor, what the frame hooks magnify by.
	static float CurrentZoom();

	// The tactical view rect in client coordinates. While the zoom is active
	// this is DSurface::ViewBounds, otherwise the client rect.
	static RECT ViewRect();

	// View rect used until the game reported one: DSurface::WindowBounds
	// inside the game, an 800x600 rectangle everywhere else (unit tests).
	static RECT DefaultViewRect();

	// Point the zoom keeps fixed, in client coordinates.
	static POINT ZoomAnchor();

	// Publishes a view rect. fromGame marks rects observed in DSurface, which
	// take precedence over the window size.
	static void SetViewRect(const RECT& rect, bool fromGame);

	// True while the view is magnified (zoom differs from 1.0 beyond the
	// snap epsilon); the coordinate transforms stay idle until then.
	static bool ZoomActive();

	// Maps a view relative point the game computed from the physical cursor
	// onto the source rect that is actually displayed there. Returns false
	// when the zoom is inactive or the point lies outside the view rect.
	// The inverse of what RenderZoom::Upscale draws, so out is view relative.
	static bool UnMagnify(const POINT& in, POINT& out);

	// Scale between a screen delta and the content it moves (source rect over
	// view rect = 1/zoom while the anchor sits in the view center).
	static bool ContentScale(float& scaleX, float& scaleY);

#ifdef VIEWCTRL_TEST
public:
#else
private:
#endif
	static void UpdateClientCache(HWND hWnd);
	static bool IsPointInMapArea(POINT pt);
	static void ClampToViewport(POINT* pt);
	static void UpdateLerp();
	static void UpdateLerpFrameIndependent();
	static void CommitZoom(float newZoom);
	static void ApplyCameraStep(float oldZoom, float newZoom);
	static void UndoCameraOffset();
	static void PanCamera(int dx, int dy);
	static void ResetZoom();

	// True while Ctrl is physically held (tests inject the state instead of
	// pressing real keys). Ctrl doubles as the wheel modifier and, when
	// pressed twice within DOUBLE_PRESS_MS, as the reset key.
	static bool CtrlHeld();

	// Wheel zoom: multiplies the factor by ZOOM_GEAR per scroll step in the
	// given direction and clamps it into [ZOOM_MIN, ZOOM_MAX].
	static float ApplyWheelSteps(float zoom, int steps);

	// Registers a key press at nowMs and returns true when it completes a
	// double press within DOUBLE_PRESS_MS of the previous one.
	static bool RegisterDoublePress(DWORD nowMs);

	static LRESULT CALLBACK NewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static DWORD WINAPI InitThread(LPVOID lpParam);

	static inline HWND     g_hWnd = nullptr;
	static inline std::atomic<float> g_zoom{ ZOOM_DEFAULT };
	static inline std::atomic<float> g_targetZoom{ ZOOM_DEFAULT };
	static inline std::atomic<float> g_invZoom{ 1.0f };

	// The rectangle the render zoom magnifies. It starts out as the window the
	// game draws into (DSurface::WindowBounds) and is replaced by
	// DSurface::ViewBounds once the game reported one. Outside the game both
	// fall back to the hard coded 800x600.
	static inline RECT     g_viewRect = DefaultViewRect();
	static inline bool     g_viewRectFromGame = false;

	// The magnified view stays anchored in the center of the view rect.
	static inline std::atomic<LONG> g_centerX{ (g_viewRect.left + g_viewRect.right) / 2 };
	static inline std::atomic<LONG> g_centerY{ (g_viewRect.top + g_viewRect.bottom) / 2 };

	// Screen point the next zoom step has to keep in place (latched per wheel
	// event). The rendered view stays anchored at g_centerX/g_centerY.
	static inline std::atomic<LONG> g_focusX{ 0 };
	static inline std::atomic<LONG> g_focusY{ 0 };
	static inline bool     g_focusValid = false;

	// Cumulative camera shift applied by this plugin, in native screen pixels.
	// Removed again as soon as the zoom factor returns to ZOOM_MIN.
	static inline POINT    g_camOffset = { 0, 0 };

	// True while the camera is being moved, so a repaint triggered from inside
	// GameCamera::ShiftBy() cannot start another zoom step.
	static inline bool     g_cameraBusy = false;

	// Double press bookkeeping for the Ctrl-resets-the-zoom hotkey.
	static inline DWORD    g_lastPressMs = 0;
	static inline bool     g_haveLastPress = false;

	// Leftover of a wheel delta below WHEEL_DELTA. Smooth scrolling (touch
	// pads, Windows 10) delivers smaller deltas, they have to add up to a
	// whole step instead of being dropped.
	static inline int      g_wheelRemainder = 0;

	// Set by Init() before the thread starts, so a second call cannot
	// subclass the window proc a second time.
	static inline bool     g_initStarted = false;

#ifdef VIEWCTRL_TEST
	// Wheel zoom modifier injected by the unit tests.
	static inline bool     g_ctrlHeld = false;
#endif

	static inline WNDPROC  OriginalWndProc = nullptr;

	static inline HANDLE   g_hThread = nullptr;
	static inline bool     g_initialized = false;
	static inline bool     g_wndProcHooked = false;

	static inline RECT     g_clientRect = {};
	static inline int      g_clientWidth = 0;
	static inline int      g_clientHeight = 0;

	static inline LARGE_INTEGER g_perfFrequency = {};
	static inline LARGE_INTEGER g_lastLerpTime = {};
	static inline bool     g_perfCounterReady = false;
};
