#pragma once

#include <windows.h>

// Frame by frame zoom of the tactical view, world-space style.
//
// Two Syringe hooks inside GScreenClass::Render (0x4F4480, YRpp) drive it:
//   0x4F44AF (pre)  - substitute DSurface::ViewBounds.W/H with the source
//                     rect, so the game itself renders only the magnified
//                     window (fewer cells, each still 60x30 native pixels),
//                     and advance the zoom lerp / game camera.
//   0x4F451B (post) - stretch that freshly rendered corner region over the
//                     full view area inside DSurface::Composite, then put
//                     the vanilla view dimensions back.
//
// The substitution mirrors the game's own Set_View_Dimensions (0x4A8960):
// both the DSurface pair and TacticalClass::view_bound (0xB0CE28) are
// written, and CalcViewportCells (0x6D8B30) is re-run so TacticalPos
// follows the smaller rect. Skipping the second half leaves the engine
// rendering the source rect at the full-view origin — the map cut off at
// the bottom and the right once the camera sits near a map edge.
//
// The engine owning the zoomed render is what Kaleidoscope does: overlays,
// smudge and selection markers drawn inside the view come out consistent by
// construction, the backup buffer shrinks to the source rect, and the game
// skips the cells outside it. The final nearest-neighbour stretch is
// unavoidable - gamemd has no scaled cell blitter - but it runs over a
// frame the engine already rendered small.
//
// The view rect comes from DSurface::ViewBounds, polled at most every 250 ms
// on every frame whose dimensions are currently vanilla — including frames
// of a magnified session, so the first zoom of a session does not render
// against the raw client rect. While the dimensions are substituted the
// ScalerConflict L4 watch is fed the vanilla pair only, so the plugin never
// trips over its own scaling.
class RenderZoom
{
public:
	// Source rectangle of the rendered view: the sub rect of the full view
	// that is displayed, magnified, at the anchor point.
	struct SourceRect
	{
		int X = 0;
		int Y = 0;
		int W = 0;
		int H = 0;
	};

	// Arms the render zoom; only meaningful inside gamemd.exe.
	static void Init();
	static void Shutdown();

	// True when a game global may be dereferenced. Only inside gamemd.exe do
	// the hard coded addresses point at the game's own data; everywhere else
	// (unit tests, a different host exe) they are rejected.
	static bool IsGameReadable(const void* address, size_t bytes);

	// Runs before the game draws. Substitutes the view dimensions with the
	// source rect while the zoom is active and returns true when the frame
	// has to be repainted entirely (emulated by writing 2 into the hooked
	// EAX). Also feeds the ScalerConflict L4 watch and disables the module
	// when a foreign world-space scaler is detected.
	static bool PreRender();

	// Runs after the game drew. Stretches the source rect the engine
	// rendered over the full view area, then restores the vanilla view
	// dimensions so the coordinate paths between frames see the original
	// pair again.
	static void PostRender();

	// --- pure helpers, also covered by the unit tests ---

	static SourceRect ComputeSourceRect(
		int viewW, int viewH, int anchorX, int anchorY, float zoom);

	// Nearest-neighbour stretch of the src sub rect over the dstW x dstH
	// destination (the scratch buffer feeds it the rendered source rect).
	static void Upscale(
		const unsigned short* backup, int backupW,
		unsigned short* dst, int dstPitch,
		int dstW, int dstH, const SourceRect& src);

	// Copies a view rect between the surface and the scratch buffer.
	static bool CopyRows(
		unsigned char* pixels, int pitch, const RECT& view,
		unsigned char* buffer, int bufferW, int bufferH, bool toBuffer);

	// Debounced acceptance of a newly observed view rect.
	// Returns true when the cached rect changed and the caller has to publish it.
	static bool AcceptViewRect(const RECT& candidate, DWORD nowMs);

	static bool CachedViewRect(RECT& out);

	// Sizes ClampCoordMap (GameAddr, 0x6D8640) clamps the viewport against,
	// reached from 0x6D864E (width) and 0x6D868A (height). Both instructions
	// read DSurface::ViewBounds; while the view is magnified the visible world
	// area is the source rect instead, so the game clamps against that.
	static int ClampWidth();
	static int ClampHeight();

	// Rule behind both: a usable source rect size stands in for the game
	// dimension only while the view is magnified.
	static int ClampDimension(int gameDim, int srcDim, bool magnified);

	// One diagnostic line behind a rejected camera focus shift: the zoom,
	// the values the ClampCoordMap hooks feed the game right now, their
	// entry counters (0 = the hook never ran) and the map globals
	// ClampCoordMap builds its bounds from. Guards the game addresses
	// with IsGameReadable, so tests stay safe.
	static void LogCameraClampState();

	// The real (unsubstituted) DSurface::ViewBounds.W, read through the
	// five BlitSidebar hook sites. While the view is magnified the game
	// global holds the smaller source rect; the sidebar blit must still
	// position against the full view width so it does not shift into the
	// map area.
	static int SidebarViewWidth();

	// Drops the backup buffer and the debounced view rect (tests / shutdown).
	static void ResetFrameState();

#ifdef VIEWCTRL_TEST
	// The frame hooks are no-ops until the module is enabled; tests flip this
	// to reach the code that runs inside the game.
	static void SetEnabled(bool enabled);
	static bool IsEnabled();

	// Redirects the hard coded game globals (DSurface::Composite slot,
	// ViewBounds, WindowBounds) into a region the test owns; moduleBase is
	// what IsGameReadable accepts as "the game module" afterwards. A null
	// moduleBase restores the real addresses.
	static void SetTestGameRegion(void* moduleBase, const void* compositeSlot,
		const void* viewBounds, const void* windowBounds);

	// Redirects the TacticalClass view_bound cache (GameAddr 0xB0CE28), the
	// TacticalClass instance slot and the CalcViewportCells entry the same
	// way; PreRender/PostRender keep the cache in sync with ViewBounds and
	// recompute TacticalPos through this triple. Nulls restore the real
	// addresses (and make the recompute a no-op outside the game).
	// tacticalInstanceSlot points at a void* slot (not at the instance).
	static void SetViewBoundForTest(const void* viewBound,
		void* tacticalInstanceSlot, void* calcViewportCells);

	// Address the WindowBounds reads go through: the game global by default,
	// the test region after SetTestGameRegion (Zoomer::DefaultViewRect).
	static const void* TestWindowBoundsAddress();

	// Pins the L3 patch-scan timestamp so the next PreRender always scans
	// (or forces the scan to look stale).
	static void SetLastPatchScanForTest(DWORD ms);

	// Redirects the five map cell-extent globals LogCameraClampState reads
	// (ClampCoordMap's bound inputs) into readable memory; null restores
	// the real addresses.
	static void SetMapBoundsForTest(const void* addrs[5]);
#endif
};
