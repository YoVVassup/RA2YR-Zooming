#pragma once

#include <windows.h>
#include <cmath>
#include <atomic>

constexpr float ZOOM_DEFAULT = 1.0f;
constexpr float ZOOM_MIN = 1.0f;
constexpr float ZOOM_MAX = 4.0f;
constexpr float ZOOM_STEP = 0.05f;
constexpr float ZOOM_LERP = 0.15f;
constexpr float ZOOM_SNAP = 0.001f;

constexpr size_t GSCRIPT_ZOOM_FACTOR_RVA = 0x1739B0;
constexpr BYTE   GSCRIPT_ZOOM_FACTOR_BYTES[] = { 0xB0, 0x39, 0x17, 0x10 };

constexpr BYTE VK_0 = 0x30;

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

	// Point the zoom keeps fixed, in client coordinates.
	static POINT ZoomAnchor();

	// Publishes a view rect. fromGame marks rects observed in DSurface, which
	// take precedence over the window size.
	static void SetViewRect(const RECT& rect, bool fromGame);

	// True when another extension (Ares' GScript zoom) owns the zoom factor.
	static bool UsingGScript();

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

	static BOOL WINAPI HookedGetCursorPos(LPPOINT lpPoint);
	static LRESULT CALLBACK NewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static DWORD WINAPI InitThread(LPVOID lpParam);

	static inline HWND     g_hWnd = nullptr;
	static inline std::atomic<float> g_zoom{ ZOOM_DEFAULT };
	static inline std::atomic<float> g_targetZoom{ ZOOM_DEFAULT };
	static inline std::atomic<float> g_invZoom{ 1.0f };
	static inline std::atomic<LONG> g_centerX{ 400 };
	static inline std::atomic<LONG> g_centerY{ 300 };

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

	static inline WNDPROC  OriginalWndProc = nullptr;
	static inline void*    OriginalGetCursorPos = nullptr;

	static inline HANDLE   g_hThread = nullptr;
	static inline bool     g_initialized = false;
	static inline bool     g_wndProcHooked = false;

	static inline RECT     g_clientRect = {};
	static inline int      g_clientWidth = 0;
	static inline int      g_clientHeight = 0;

	// The rectangle the render zoom magnifies. Defaults to the client rect and
	// is replaced by DSurface::ViewBounds while in game.
	static inline RECT     g_viewRect = { 0, 0, 800, 600 };
	static inline bool     g_viewRectFromGame = false;

	static inline LARGE_INTEGER g_perfFrequency = {};
	static inline LARGE_INTEGER g_lastLerpTime = {};
	static inline bool     g_perfCounterReady = false;

	static inline bool     g_useGScript = false;
	static inline float*   g_pZoomFactor = nullptr;
};
