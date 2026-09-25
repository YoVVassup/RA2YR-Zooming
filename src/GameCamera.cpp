#include "GameCamera.hpp"
#include "Log.h"

// YRpp is a third-party header set: keep its unused-parameter noise out of
// our own warnings.
#pragma warning(push)
#pragma warning(disable: 4100)
#include <TacticalClass.h>
#pragma warning(pop)
#include <cmath>

namespace
{
	bool g_enabled = false;

	// TacticalClass::SetViewPos(POINT*) @ 0x6D6000 — __thiscall, one stack
	// argument. It runs the point through ClampCoordMap (0x6D8640), stores it
	// as the view center (this+0xD64/0xD68) and as the last position
	// (this+0xD74/0xD78), recalculates the view origin
	// (CalcViewportCells, 0x6D8B30 -> TacticalPos at this+0xB0) and raises the
	// redrawing flag (this+0xD7D).
	using SetViewPosFn = void(__fastcall*)(void* self, void* /*edx*/, void* point);
	constexpr uintptr_t SET_VIEW_POS = 0x6D6000;
	constexpr BYTE     SET_VIEW_POS_PROLOGUE = 0x83; // sub esp, 8

	// View center inside TacticalClass: right after visibleCells[800],
	// immediately before field_D6C (unnamed in YRpp).
	constexpr size_t VIEW_CENTER_X = 0xD64;
	constexpr size_t VIEW_CENTER_Y = 0xD68;
}

POINT GameCamera::ComputeShift(POINT focus, POINT fixedPoint, float oldZoom, float newZoom)
{
	POINT shift = { 0, 0 };

	if (!(oldZoom > 0.0f) || !(newZoom > 0.0f)) return shift;
	if (oldZoom == newZoom) return shift;

	const float delta = 1.0f / oldZoom - 1.0f / newZoom;
	shift.x = lroundf((float)(focus.x - fixedPoint.x) * delta);
	shift.y = lroundf((float)(focus.y - fixedPoint.y) * delta);
	return shift;
}

void GameCamera::Enable()
{
	if (g_enabled) return;

	HMODULE module = nullptr;
	if (!GetModuleHandleExA(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(SET_VIEW_POS), &module) || module == nullptr)
	{
		LOG("GameCamera: address 0x%IX is not inside a loaded module, disabled", SET_VIEW_POS);
		return;
	}

	if (*reinterpret_cast<const BYTE*>(SET_VIEW_POS) != SET_VIEW_POS_PROLOGUE)
	{
		LOG("GameCamera: unexpected prologue at 0x%IX, disabled", SET_VIEW_POS);
		return;
	}

	g_enabled = true;
	LOG("GameCamera: enabled (SetViewPos=%p module=%p instance=%p)",
		reinterpret_cast<void*>(SET_VIEW_POS), module, TacticalClass::Instance);
}

void GameCamera::Disable()
{
	if (!g_enabled) return;
	g_enabled = false;
	LOG("GameCamera: disabled");
}

bool GameCamera::IsEnabled()
{
	return g_enabled;
}

bool GameCamera::Read(POINT& out)
{
	if (!g_enabled) return false;

	TacticalClass* tactical = TacticalClass::Instance;
	if (!tactical) return false;

	const BYTE* base = reinterpret_cast<const BYTE*>(tactical);
	out.x = *reinterpret_cast<const int*>(base + VIEW_CENTER_X);
	out.y = *reinterpret_cast<const int*>(base + VIEW_CENTER_Y);
	return true;
}

bool GameCamera::WriteAbs(const POINT& point)
{
	if (!g_enabled) return false;

	TacticalClass* tactical = TacticalClass::Instance;
	if (!tactical) return false;

	POINT target = point;
	reinterpret_cast<SetViewPosFn>(SET_VIEW_POS)(tactical, nullptr, &target);
	return true;
}

bool GameCamera::ShiftBy(int dx, int dy)
{
	if (!g_enabled) return false;
	if (dx == 0 && dy == 0) return true;

	POINT current = {};
	if (!Read(current)) return false;

	POINT target = { current.x + dx, current.y + dy };
	if (!WriteAbs(target)) return false;

	// The game clamps the camera to the map, so the result may differ from
	// what was asked for — report failure when nothing moved at all.
	POINT after = {};
	if (Read(after))
	{
		if (after.x == current.x && after.y == current.y && (dx != 0 || dy != 0))
			return false;
	}
	return true;
}
