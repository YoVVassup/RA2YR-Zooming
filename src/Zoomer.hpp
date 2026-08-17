#pragma once

#include <windows.h>
#include <ddraw.h>
#include <cmath>
#include <atomic>
#include <unordered_set>

constexpr float ZOOM_DEFAULT = 1.0f;
constexpr float ZOOM_MIN = 1.0f;
constexpr float ZOOM_MAX = 2.0f;
constexpr float ZOOM_STEP = 1.1f;
constexpr float ZOOM_LERP = 0.15f;
constexpr float ZOOM_SNAP = 0.001f;

constexpr int SIDEBAR_WIDTH = 168;
constexpr int BOTTOM_BAR_HEIGHT = 32;

constexpr int BLT_VTABLE_INDEX = 5;

class Zoomer
{
public:
	static void Init();
	static void Shutdown();

#ifdef VIEWCTRL_TEST
public:
#else
private:
#endif
	static void UpdateClientCache(HWND hWnd);
	static void UpdateCenter(HWND hWnd);
	static bool IsMapArea(LPRECT rect);
	static bool IsPointInMapArea(POINT pt);
	static void ApplyZoomToRect(LPRECT rect);
	static bool IsPrimaryOrBackBuffer(IDirectDrawSurface7* surface);
	static void ClampToViewport(POINT* pt);

	static HRESULT WINAPI HookedBlt(
		LPDIRECTDRAWSURFACE7 self,
		LPRECT destRect,
		LPDIRECTDRAWSURFACE7 srcSurface,
		LPRECT srcRect,
		DWORD flags,
		LPDDBLTFX fx);

	static BOOL WINAPI HookedGetCursorPos(LPPOINT lpPoint);
	static LRESULT CALLBACK NewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static DWORD WINAPI InitThread(LPVOID lpParam);

	static inline HWND     g_hWnd = nullptr;
	static inline std::atomic<float> g_zoom{ ZOOM_DEFAULT };
	static inline std::atomic<float> g_targetZoom{ ZOOM_DEFAULT };
	static inline float    g_invZoom = 1.0f;
	static inline std::atomic<LONG> g_centerX{ 400 };
	static inline std::atomic<LONG> g_centerY{ 300 };

	static inline WNDPROC  OriginalWndProc = nullptr;

	static inline void*    OriginalBlt = nullptr;
	static inline void*    OriginalGetCursorPos = nullptr;

	static inline HANDLE   g_hThread = nullptr;
	static inline bool     g_initialized = false;
	static inline bool     g_wndProcHooked = false;

	static inline std::unordered_set<IDirectDrawSurface7*> g_knownPrimaryOrBack;

	static inline RECT     g_clientRect = {};
	static inline int      g_clientWidth = 0;
	static inline int      g_clientHeight = 0;
	static inline int      g_mapRight = 0;
	static inline int      g_mapBottom = 0;
	static inline bool     g_destDetected = false;

	static inline void**   g_vtable = nullptr;
};
