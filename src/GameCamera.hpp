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

	// Absolute camera placement through TacticalMapClass::SetCameraPosition
	// (GameAddr, 0x6D6000), i.e. the game clamps the point and recalculates
	// the view origin itself.
	bool WriteAbs(const POINT& point);

	// Read + add + write. Returns false if the game refused the move
	// (disabled, no instance, or the camera did not end up where asked).
	bool ShiftBy(int dx, int dy);

	// Read + write the current position back through SetCameraPosition, so
	// the game re-runs its clamp (SetCameraPosition always pipes the point
	// through ClampCoordMap). Needed when the legal camera range tightens
	// without a shift being applied — a zoom decrease with the focus at the
	// view center leaves the camera where the wider (zoomed) bound allowed
	// it, and nothing else would put it back inside the new bound.
	bool Reclamp();

#ifdef VIEWCTRL_TEST
	// Test seams: arm the wrapper without gamemd.exe (Enable() refuses outside
	// the game module), point the TacticalClass instance at a fake object
	// instead of the fixed game slot (which can collide with DLL images in
	// the test process), and redirect the SetCameraPosition call to a stub.
	void EnableForTest();
	void SetTacticalInstanceForTest(void* instance);
	void SetCameraPositionForTest(void* fn);
	// Points the Enable() prologue probe at a buffer the test owns, so both
	// the success and the unexpected-prologue paths can run outside the game.
	// Pass null to restore the real SetCameraPosition address.
	void SetCameraProbeAddressForTest(const void* address);
	// Makes the next Read() fail once (the flag clears itself): the branch
	// ApplyCameraStep takes when the camera cannot be re-read after a shift
	// is otherwise unreachable, since its own checks require every read to
	// succeed.
	void SetFailNextReadForTest(bool fail);
#endif
}
