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
// The nine Syringe hook sites live in Main.cpp: DEFINE_HOOK pastes the
// address token into the export name (YRpp\Syringe.h declhook), so those
// addresses must stay as literals at the call site. Their containing
// functions are annotated there.
//
// Cross-checked 2026-09; YRDict covers unrelated (file/launch/debug) code and
// holds none of these addresses.
// ---------------------------------------------------------------------------
namespace GameAddr
{
	// --- globals, YRpp Surface.h -------------------------------------------

	// RectangleStruct {X, Y, W, H} - the origin the click/render paths use.
	// Not the view_bound rectangle at 0xB0CE28 (Encyclopedia, Selection-Mouse).
	constexpr DWORD DSurface_ViewBounds = 0x886FA0;

	// RectangleStruct {X, Y, W, H} - the rect the game draws its window into.
	constexpr DWORD DSurface_WindowBounds = 0x886FB0;

	// DSurface* - the surface both render hooks back up and magnify.
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

	// Contains hook site 0x52CAE9 (right before "Game Init Completed.").
	constexpr uintptr_t InitGame = 0x52BA60;

	// --- functions, YRpp / Encyclopedia (documentation for Main.cpp) -------

	// YRpp GScreenClass.h:49; contains hook sites 0x4F44AF and 0x4F451B.
	// ReSource calls the same address TacticalMap::Redraw.
	constexpr uintptr_t GScreenClass_Render = 0x4F4480;

	// YRpp DisplayClass.h; contains hook site 0x692325. ReSource alias:
	// Tactical::MouseOver (0x692300).
	constexpr uintptr_t DisplayClass_ProcessClickCoords = 0x692300;
}
