#pragma once

#include <windows.h>
#include <ddraw.h>
#include <d3d9.h>
#include <cmath>
#include <atomic>
#include <unordered_set>
#include <unordered_map>

constexpr float ZOOM_DEFAULT = 1.0f;
constexpr float ZOOM_MIN = 1.0f;
constexpr float ZOOM_MAX = 4.0f;
constexpr float ZOOM_STEP = 0.05f;
constexpr float ZOOM_LERP = 0.15f;
constexpr float ZOOM_SNAP = 0.001f;

constexpr size_t GSCRIPT_ZOOM_FACTOR_RVA = 0x1739B0;
constexpr BYTE   GSCRIPT_ZOOM_FACTOR_BYTES[] = { 0xB0, 0x39, 0x17, 0x10 };

constexpr int SIDEBAR_WIDTH = 168;
constexpr int BOTTOM_BAR_HEIGHT = 32;
constexpr int GUARD_LINES = 200;

constexpr int BLT_VTABLE_INDEX = 5;
constexpr int FLIP_VTABLE_INDEX = 11;
constexpr int BLTFAST_VTABLE_INDEX = 7;
constexpr int LOCK_VTABLE_INDEX = 25;
	constexpr int UNLOCK_VTABLE_INDEX = 33;

	constexpr int D3D9_PRESENT_VTABLE_INDEX = 17;
	constexpr int D3D9_RESET_VTABLE_INDEX = 16;

constexpr BYTE VK_0 = 0x30;

enum class DDrawWrapper
{
	Unknown,
	Original,
	CncDDraw,
	TsDDraw,
	DDrawCompat
};

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
	static void ApplyZoomToSurface(void* pixels, int bufWidth, int bufHeight, int pitch, int bytesPerPixel);
	static void ApplyZoomToBuffer();
	static void CALLBACK ZoomTimerProc(HWND hWnd, UINT msg, UINT_PTR idEvent, DWORD dwTime);
	static void UpdateLerp();
	static void UpdateLerpFrameIndependent();
	static void ResetZoom();
	static DDrawWrapper DetectDDrawWrapper();
	static void UpdateMonitorInfo(HWND hWnd);

	static HRESULT WINAPI HookedBlt(
		LPDIRECTDRAWSURFACE7 self,
		LPRECT destRect,
		LPDIRECTDRAWSURFACE7 srcSurface,
		LPRECT srcRect,
		DWORD flags,
		LPDDBLTFX fx);

	static HRESULT WINAPI HookedFlip(
		LPDIRECTDRAWSURFACE7 self,
		LPDIRECTDRAWSURFACE7 target,
		DWORD flags);

	static HRESULT WINAPI HookedBltFast(
		LPDIRECTDRAWSURFACE7 self,
		DWORD x,
		DWORD y,
		LPDIRECTDRAWSURFACE7 srcSurface,
		LPRECT srcRect,
		DWORD flags);

	static BOOL WINAPI HookedGetCursorPos(LPPOINT lpPoint);
	static BOOL WINAPI HookedBitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop);
	static BOOL WINAPI HookedStretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest, HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, DWORD rop);
	static BOOL WINAPI HookedSwapBuffers(HDC hdc);
	static HRESULT WINAPI HookedPresent(IDirect3DDevice9* device, const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion);
	static HRESULT WINAPI HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters);
	static LRESULT CALLBACK NewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static DWORD WINAPI InitThread(LPVOID lpParam);

	static inline HWND     g_hWnd = nullptr;
	static inline std::atomic<float> g_zoom{ ZOOM_DEFAULT };
	static inline std::atomic<float> g_targetZoom{ ZOOM_DEFAULT };
	static inline std::atomic<float> g_invZoom{ 1.0f };
	static inline std::atomic<LONG> g_centerX{ 400 };
	static inline std::atomic<LONG> g_centerY{ 300 };

	static inline WNDPROC  OriginalWndProc = nullptr;

	static inline void*    OriginalBlt = nullptr;
	static inline void*    OriginalFlip = nullptr;
	static inline void*    OriginalBltFast = nullptr;
	static inline void*    OriginalGetCursorPos = nullptr;
	static inline void*    OriginalBitBlt = nullptr;
	static inline void*    OriginalStretchBlt = nullptr;
	static inline void*    OriginalSwapBuffers = nullptr;
	static inline void*    OriginalPresent = nullptr;
	static inline void*    OriginalReset = nullptr;

	struct SurfaceState {
		void* pixels = nullptr;
		int pitch = 0;
		int width = 0;
		int height = 0;
		bool isPrimaryOrBack = false;
	};
	static inline std::unordered_map<IDirectDrawSurface7*, SurfaceState> g_surfaceStates;

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

	static inline DDrawWrapper g_ddrawWrapper = DDrawWrapper::Unknown;
	static inline LARGE_INTEGER g_perfFrequency = {};
	static inline LARGE_INTEGER g_lastLerpTime = {};
	static inline bool     g_perfCounterReady = false;

	static inline HMONITOR g_hMonitor = nullptr;
	static inline RECT     g_monitorRect = {};
	static inline int      g_monitorWidth = 0;
	static inline int      g_monitorHeight = 0;

	static inline HDC      g_hDC = nullptr;
	static inline HGLRC    g_hRC = nullptr;
	static inline bool     g_openglAvailable = false;

	static inline IDirect3DTexture9* g_pCaptureTex = nullptr;
	static inline int      g_captureTexW = 0;
	static inline int      g_captureTexH = 0;

	static inline bool     g_useGScript = false;
	static inline float*   g_pZoomFactor = nullptr;
	static inline DWORD    g_presentSkipCount = 0;
};
