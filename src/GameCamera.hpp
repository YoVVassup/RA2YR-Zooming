#pragma once

#include <windows.h>

// Control of the game's own camera ("tactical view").
//
// Everything here is useless outside gamemd.exe: Enable() refuses to arm the
// wrapper unless it runs inside the game module and the hooked entry point
// still looks like the function we expect.
namespace GameCamera
{
	// Shift of the rendered frame (native, zoom-1 screen pixels) that keeps
	// `focus` at the same *screen* point while the zoom factor changes
	// oldZoom -> newZoom with the crop anchored at `fixedPoint`.
	//
	//   screenOffset = (worldPoint - fixedPoint) * zoom
	//   screenOffset must not change  =>  shift = (focus - fixedPoint) * (1/oldZoom - 1/newZoom)
	//
	// The camera moves by `shift`, so the rendered content moves by -shift.
	POINT ComputeShift(POINT focus, POINT fixedPoint, float oldZoom, float newZoom);

	void Enable();
	void Disable();
	bool IsEnabled();

	// Current view center of TacticalClass (this+0xD64/0xD68).
	bool Read(POINT& out);

	// Absolute camera placement through TacticalClass::SetViewPos (0x6D6000),
	// i.e. the game clamps the point and recalculates the view origin itself.
	bool WriteAbs(const POINT& point);

	// Read + add + write. Returns false if the game refused the move
	// (disabled, no instance, or the camera did not end up where asked).
	bool ShiftBy(int dx, int dy);
}
