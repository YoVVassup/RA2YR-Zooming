#pragma once

#include <windows.h>

// Frame by frame zoom of the tactical view, ported from Telescope.dll.
//
// Two Syringe hooks around the tactical render drive it:
//   0x4F44AF (pre)  - put the original pixels back so the game draws on the
//                     clean frame, then advance the zoom lerp / game camera.
//   0x4F451B (post) - back the freshly rendered view rect up and magnify it
//                     in place inside DSurface::Composite.
//
// The view rect comes from DSurface::ViewBounds, polled at most every 250 ms
// and only while the zoom factor is back at 1.0 (Telescope does the same).
class RenderZoom
{
public:
	// Source rectangle of the backup that gets scaled up to fill the view.
	struct SourceRect
	{
		int X = 0;
		int Y = 0;
		int W = 0;
		int H = 0;
	};

	// zoomEnabled: false while another extension (GScript) owns the zoom.
	static void Init(bool zoomEnabled);
	static void Shutdown();

	// True when a game global may be dereferenced. Only inside gamemd.exe do
	// the hard coded addresses point at the game's own data; everywhere else
	// (unit tests, a different host exe) they are rejected.
	static bool IsGameReadable(const void* address, size_t bytes);

	// Runs before the game draws. Returns true when the frame the game is
	// about to draw on still holds magnified pixels and it has to be told to
	// repaint everything (emulated by writing 2 into the hooked EAX).
	static bool PreRender();

	// Runs after the game drew. Backs the view rect up and magnifies it.
	static void PostRender();

	// --- pure helpers, also covered by the unit tests ---

	static SourceRect ComputeSourceRect(
		int viewW, int viewH, int anchorX, int anchorY, float zoom);

	static void Upscale(
		const unsigned short* backup, int backupW,
		unsigned short* dst, int dstPitch,
		int dstW, int dstH, const SourceRect& src);

	// Copies the view rect between the surface and the backup buffer.
	static bool CopyRows(
		unsigned char* pixels, int pitch, const RECT& view,
		unsigned char* buffer, int bufferW, int bufferH, bool toBuffer);

	// Debounced acceptance of a newly observed view rect.
	// Returns true when the cached rect changed and the caller has to publish it.
	static bool AcceptViewRect(const RECT& candidate, DWORD nowMs);

	static bool CachedViewRect(RECT& out);

	// Sizes ClampCoordMap clamps the viewport against, reached from
	// 0x6D864E (width) and 0x6D868A (height). Both instructions read
	// DSurface::ViewBounds; while the view is magnified the visible world
	// area is the source rect instead, so the game clamps against that.
	static int ClampWidth();
	static int ClampHeight();

	// Rule behind both: a usable source rect size stands in for the game
	// dimension only while the view is magnified.
	static int ClampDimension(int gameDim, int srcDim, bool magnified);

	// Drops the backup buffer and the debounced view rect (tests / shutdown).
	static void ResetFrameState();

#ifdef VIEWCTRL_TEST
	// The frame hooks are no-ops until the module is enabled; tests flip this
	// to reach the code that runs inside the game.
	static void SetEnabled(bool enabled);
#endif
};
