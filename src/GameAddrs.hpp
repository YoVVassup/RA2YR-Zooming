#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// Canonical addresses used by the plugin, each verified against the public
// reference projects:
//
//   YRpp  - Phobos-developers/YRpp          (authoritative for globals/structs)
//   ReSrc - Ritanlisa/RA2YR_ReSource        (IDA names, signals.json)
//   Enc   - SethGekco/YR-Hook-Encyclopedia  (framework hook-site registry)
//
// The seventeen Syringe hook sites live in Main.cpp: DEFINE_HOOK pastes
// the address token into the export name (YRpp\Syringe.h declhook), so
// those addresses must stay as literals at the call site. Their
// containing functions are annotated there.
//
// Cross-checked 2026-09; YRDict covers unrelated (file/launch/debug) code and
// holds none of these addresses.
// ---------------------------------------------------------------------------
namespace GameAddr
{
	// --- globals, YRpp Surface.h -------------------------------------------

	// RectangleStruct {X, Y, W, H} - the origin the click/render paths use.
	// Not the TacticalClass view_bound cache below (Encyclopedia,
	// Selection-Mouse); Set_View_Dimensions copies this rect into that cache.
	constexpr DWORD DSurface_ViewBounds = 0x886FA0;

	// RectangleStruct {X, Y, W, H} - TacticalClass's own copy of the view
	// bounds, written by Set_View_Dimensions (0x4A8960) via the cache writer
	// at 0x6D5F60 right after the DSurface globals. CalcViewportCells
	// (0x6D8B30) builds TacticalPos (this+0xB0) from the camera center and
	// the W/H half of this rect, and CoordsToClient culls against its
	// Width/Height (YRpp TacticalClass.h view_bound, 0xB0CE28).
	constexpr DWORD TacticalClass_viewBound = 0xB0CE28;

	// TacticalClass* instance slot (YRpp TacticalClass.h). Needed to invoke
	// CalcViewportCells after the view bounds are substituted: the engine
	// computes the viewport origin once per camera/dim change, so a plugin
	// that shrinks the view without going through Set_View_Dimensions has to
	// re-run the recompute itself or the small render keeps the full-view
	// origin (map cut off at the bottom and right).
	constexpr DWORD TacticalClass_Instance = 0x887324;

	// RectangleStruct {X, Y, W, H} - the rect the game draws its window into.
	constexpr DWORD DSurface_WindowBounds = 0x886FB0;

	// DSurface* - the surface the render hooks stretch the magnified view
	// over (world-space: the game renders the source rect into it, the post
	// hook scales that up to the full view area).
	constexpr DWORD DSurface_Composite = 0x88731C;

	// --- functions, ReSource -----------------------------------------------

	// stdcall, returns char; reads ViewBounds.W/H (replaced by the two
	// clamp hooks at 0x6D864E / 0x6D868A).
	constexpr uintptr_t ClampCoordMap = 0x6D8640;

	// thiscall, one stack argument. ReSource name; NOT SetTacticalPosition
	// (0x6D6070 - a different function, hooked by Phobos).
	constexpr uintptr_t TacticalMapClass_SetCameraPosition = 0x6D6000;

	// thiscall; computes TacticalPos at this+0xB0.
	constexpr uintptr_t TacticalClass_CalcViewportCells = 0x6D8B30;

	// Contains hook site 0x693791 (right-drag scroll speeds).
	constexpr uintptr_t ScrollMapEdge = 0x693440;

	// BlitSidebar — copies DSurface_Sidebar to DSurface_Primary. Reads
	// DSurface::ViewBounds.W to compute the blit destination X; while the
	// view is substituted that value is the smaller source rect, so the
	// sidebar would land in the map area. The five read sites are hooked
	// to return the real view width instead (Main.cpp).
	constexpr uintptr_t BlitSidebar = 0x6A70E0;

	// Contains hook site 0x52CAE9 (right before "Game Init Completed.").
	constexpr uintptr_t InitGame = 0x52BA60;

	// --- HUD draw functions, disassembly (Main.cpp hooks, TextDefer
	// replay — the Kaleidoscope v1.1 defer points) -------------------------

	// TacticalClass::DrawTimer — fastcall (index=ECX, scheme=EDX), 4 stack
	// args (Time, Text, xy1, xy2), `ret 0x10`. Hook site 0x6D4B50 (6-byte
	// prologue `sub esp,0x634; push ebp`).
	constexpr uintptr_t TacticalClass_DrawTimer = 0x6D4B50;

	// The untouched `ret 0x10` tail (at function end, 0x6D4DAC) the timer
	// hook jumps to when the draw is deferred: the prologue never ran, no
	// register was touched, the tail pops the pristine arg block.
	constexpr uintptr_t TacticalClass_DrawTimer_RetSkip = 0x6D4DAC;

	// TacticalClass::DrawScreenText — thiscall (this=ECX), 1 stack arg (a
	// Point2D* member), `ret 4`. Hook site 0x6D4E20, 5-byte prologue.
	constexpr uintptr_t TacticalClass_DrawScreenText = 0x6D4E20;

	// The untouched `ret 4` tail (0x6D501F) the DrawScreenText hook jumps
	// to when the draw is deferred.
	constexpr uintptr_t TacticalClass_DrawScreenText_RetSkip = 0x6D501F;

	// GScreenClass::Render: the 12-byte window over the DrawUI virtual
	// call (`mov eax,[esp+0x10]; mov edx,[esi]; push eax; mov ecx,esi;
	// call [edx+0x40]`). Skipping it defers the whole UI pass; the replay
	// re-runs the same vcall after the stretch with the view dimensions
	// restored.
	constexpr uintptr_t GScreenClass_Render_DrawUI = 0x4F44F9;

	// Resume right after the skipped window (0x4F4505: `mov ecx,
	// [0x88731c]` — the composite blit that follows the UI pass). The
	// skipped vcall's `push eax` never ran, so the stack is balanced.
	constexpr uintptr_t GScreenClass_Render_DrawUI_Resume = 0x4F4505;

	// --- functions, YRpp / Encyclopedia (documentation for Main.cpp) -------

	// YRpp GScreenClass.h:49; contains hook sites 0x4F44AF and 0x4F451B.
	// ReSource calls the same address TacticalMap::Redraw.
	constexpr uintptr_t GScreenClass_Render = 0x4F4480;

	// YRpp DisplayClass.h; contains hook site 0x692325. ReSource alias:
	// Tactical::MouseOver (0x692300).
	constexpr uintptr_t DisplayClass_ProcessClickCoords = 0x692300;
}
