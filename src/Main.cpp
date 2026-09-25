#include "Main.hpp"
#include "Zoomer.hpp"
#include "RenderZoom.hpp"
#include "Log.h"
#include <cmath>

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	switch (fdwReason) {
	case DLL_PROCESS_ATTACH:
		LOG("DLL_PROCESS_ATTACH hModule=%p", hinstDLL);
		Debug::SetDllHandle(hinstDLL);
		DisableThreadLibraryCalls((HMODULE)hinstDLL);
		break;
	case DLL_PROCESS_DETACH:
		LOG("DLL_PROCESS_DETACH");
		Zoomer::Shutdown();
		break;
	}

	return true;
}

DEFINE_HOOK(0x52CAE9, GameInt, 0x5)
{
	LOG("DEFINE_HOOK(0x52CAE9) triggered — calling Zoomer::Init()");
	Zoomer::Init();
	return 0;
}

// 0x4F44AF: "test eax, eax; setne cl" — the 5 bytes before the frame is drawn.
// Returns the address of the next instruction so the replaced bytes are not
// executed twice. EAX holds a redraw mode where 2 means "repaint everything".
DEFINE_HOOK(0x4F44AF, ViewCtrlPreRenderRestore, 0x5)
{
	const bool forceRedraw = RenderZoom::PreRender();

	const DWORD eax = forceRedraw ? 2u : R->EAX();
	R->EAX(eax);
	R->CL((BYTE)(eax != 0));
	return 0x4F44B4;
}

// 0x4F451B: "mov al, byte ptr [0xB0B519]" — the 5 bytes right after the frame
// was drawn into DSurface::Composite.
DEFINE_HOOK(0x4F451B, ViewCtrlPostRenderZoom, 0x5)
{
	RenderZoom::PostRender();

	const DWORD eax = (R->EAX() & 0xFFFFFF00u) | (DWORD)(*(const BYTE*)0xB0B519);
	R->EAX(eax);
	return 0x4F4520;
}

// 0x6D864E: "mov ebx, dword ptr [0x886FA8]" — the view width ClampCoordMap
// clamps the viewport against. 6 bytes, execution continues at 0x6D8654.
// The 0x6D868A hook below replaces the matching view height read.
DEFINE_HOOK(0x6D864E, ViewCtrlClampWidth, 0x6)
{
	R->EBX((DWORD)RenderZoom::ClampWidth());
	return 0x6D8654;
}

// 0x6D868A: "mov ebx, dword ptr [0x886FAC]" — the view height. Runs after the
// width was already used, so only the height half of the clamp changes here.
DEFINE_HOOK(0x6D868A, ViewCtrlClampHeight, 0x6)
{
	R->EBX((DWORD)RenderZoom::ClampHeight());
	return 0x6D8690;
}

// The four hooks below replace the old window message / GetCursorPos level
// cursor remap: the game keeps the physical cursor, and only the points where
// it turns a screen position into game space are un-magnified. They mirror
// Telescope, whose FUN_1003d1b0/FUN_1003be70 pair runs at exactly these four
// call sites.

static POINT g_clickCoords = {};
static POINT g_bandStart = {};
static POINT g_bandEnd = {};

// 0x692325: "mov ebp, [esp+0x28]" (4 bytes) + "mov [edx], eax" (2 bytes) —
// 6 bytes, execution continues at 0x69232B. The tactical click pipeline
// validates the point at [esp+0x28] against the cell under the cursor: hand
// it the un-magnified point so the selection lands on what is actually
// displayed. EDX is an optional out pointer that receives the result EAX.
DEFINE_HOOK(0x692325, ViewCtrlProcessClickCoords, 0x6)
{
	static bool logged = false;

	POINT* pt = reinterpret_cast<POINT*>(R->Stack32(0x28));
	POINT* use = pt;
	if (pt)
	{
		POINT out = {};
		if (Zoomer::UnMagnify(*pt, out))
		{
			g_clickCoords = out;
			use = &g_clickCoords;
			if (!logged)
			{
				LOG("ProcessClick transform active (%ld,%ld) -> (%ld,%ld)",
					pt->x, pt->y, out.x, out.y);
				logged = true;
			}
		}
	}

	R->EBP(reinterpret_cast<DWORD>(use));
	if (R->EDX())
		*reinterpret_cast<DWORD*>(R->EDX()) = R->EAX();
	return 0x69232B;
}

// 0x6D9F80: "mov eax, dword ptr [ecx+0xD90]" — TacticalClass::InitScrollBounds,
// 6 bytes, continues at 0x6D9F86. The band start point arrives through
// [esp+4]: replace it with the un-magnified point so the rubber band is
// dragged in content space. The EAX load is reproduced with a null check on
// ECX, the same way Telescope does.
DEFINE_HOOK(0x6D9F80, ViewCtrlDragBandStart, 0x6)
{
	static bool logged = false;

	POINT* pt = reinterpret_cast<POINT*>(R->Stack32(4));
	if (pt)
	{
		POINT out = {};
		if (Zoomer::UnMagnify(*pt, out))
		{
			g_bandStart = out;
			R->ref_Stack<DWORD>(4) = reinterpret_cast<DWORD>(&g_bandStart);
			if (!logged)
			{
				LOG("DragBandStart transform active (%ld,%ld) -> (%ld,%ld)",
					pt->x, pt->y, out.x, out.y);
				logged = true;
			}
		}
	}

	const DWORD thisPtr = R->ECX();
	R->EAX(thisPtr ? *reinterpret_cast<const DWORD*>(thisPtr + 0xD90) : 0u);
	return 0x6D9F86;
}

// 0x6D9FC0: "mov eax, dword ptr [ecx+0xD90]" — TacticalClass::UpdateScrollMaxBounds,
// 6 bytes, continues at 0x6D9FC6. The band end point at [esp+4] gets the same
// un-magnify as the band start above.
DEFINE_HOOK(0x6D9FC0, ViewCtrlDragBandEnd, 0x6)
{
	static bool logged = false;

	POINT* pt = reinterpret_cast<POINT*>(R->Stack32(4));
	if (pt)
	{
		POINT out = {};
		if (Zoomer::UnMagnify(*pt, out))
		{
			g_bandEnd = out;
			R->ref_Stack<DWORD>(4) = reinterpret_cast<DWORD>(&g_bandEnd);
			if (!logged)
			{
				LOG("DragBandEnd transform active (%ld,%ld) -> (%ld,%ld)",
					pt->x, pt->y, out.x, out.y);
				logged = true;
			}
		}
	}

	const DWORD thisPtr = R->ECX();
	R->EAX(thisPtr ? *reinterpret_cast<const DWORD*>(thisPtr + 0xD90) : 0u);
	return 0x6D9FC6;
}

// 0x693791: "xor esi, esi" (2 bytes) + "mov [esp+0x28], esi" (4 bytes) —
// 6 bytes, continues at 0x693797. The right drag scroll speeds live at
// [esp+0x18]/[esp+0x1c] and are screen deltas: while the view is magnified
// the content moves by source/view (= 1/zoom) of that.
DEFINE_HOOK(0x693791, ViewCtrlRightDragSpeed, 0x6)
{
	static bool logged = false;

	const DWORD obj = R->EBX();
	if (obj
		&& RenderZoom::IsGameReadable(reinterpret_cast<const void*>(obj), 0x5558 + 1)
		&& *reinterpret_cast<const BYTE*>(obj + 0x5558) != 0
		&& Zoomer::ZoomActive())
	{
		float scaleX = 0.0f;
		float scaleY = 0.0f;
		if (Zoomer::ContentScale(scaleX, scaleY))
		{
			int* speedX = reinterpret_cast<int*>(R->ESP() + 0x18);
			int* speedY = reinterpret_cast<int*>(R->ESP() + 0x1c);
			if (*speedX > 0)
				*speedX = (int)floorf((float)*speedX * scaleX + 0.5f);
			if (*speedY > 0)
				*speedY = (int)floorf((float)*speedY * scaleY + 0.5f);
			if (!logged)
			{
				LOG("RightDragSpeed scaled by (%.3f,%.3f)", scaleX, scaleY);
				logged = true;
			}
		}
	}

	R->ESI(0u);
	R->ref_Stack<DWORD>(0x28) = 0u;
	return 0x693797;
}
