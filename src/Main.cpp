#include "Main.hpp"
#include "Zoomer.hpp"
#include "RenderZoom.hpp"
#include "ScalerConflict.hpp"
#include "TextDefer.hpp"
#include "Log.h"
#include "Seh.h"
#include <cmath>

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	(void)lpvReserved;
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

// 0x52CAE9: inside InitGame (GameAddr::InitGame, 0x52BA60, ReSource), right
// before the game logs "Game Init Completed.". Ares hooks this exact site
// with the same 5 stolen bytes (Encyclopedia registry: _YR_PostGameInit);
// returning 0 runs the original bytes, so both handlers compose.
DEFINE_HOOK(0x52CAE9, GameInt, 0x5)
{
	(void)R; // bootstrap hook — no registers are read or written here
	// Big mods ship their own zoom (GScript.ext, Kaleidoscope, MOExperience,
	// Telescope, Ra2.Vampire.Zoom); competing with them would transform the
	// frame twice. ScalerConflict::Present() layers a module-name list, a
	// .syhks00 hook-table fingerprint, a runtime byte sniff and a known
	// export probe; when any of them fires, stay inert: no Zoomer::Init(),
	// so no init thread, no window subclass and every hook below keeps
	// replaying the original instructions. The reactive layer (a world-space
	// scaler rescaling the view mid-game) is watched from
	// RenderZoom::PreRender.
	bool conflict = true;
	VIEWCTRL_SEH_BEGIN
	conflict = ScalerConflict::Present();
	if (!conflict)
	{
		LOG("DEFINE_HOOK(0x52CAE9) triggered — calling Zoomer::Init()");
		Zoomer::Init();
	}
	else
	{
		LOG("GameInt: conflicting scaler present — ViewCtrl stays disabled");
	}
	VIEWCTRL_SEH_END
	(void)conflict;
	return 0;
}

// 0x4F44AF: "test eax, eax; setne cl" — the 5 bytes before the frame is drawn,
// inside GScreenClass::Render (GameAddr::GScreenClass_Render, 0x4F4480, YRpp;
// ReSource: TacticalMap::Redraw). Returns the address of the next instruction
// so the replaced bytes are not executed twice; the handler replays both
// writes (EAX and CL). Neighbouring framework sites — Kratos 0x4F4497,
// Ares/Kratos/Phobos 0x4F4583, Phobos 0x4F4589 — none overlaps this window.
// EAX holds a redraw mode where 2 means "repaint everything": while the view
// is magnified RenderZoom::PreRender substitutes DSurface::ViewBounds with
// the source rect and asks for a full repaint of that smaller view, and the
// first vanilla frame after a magnified one repaints to clear the stretch.
DEFINE_HOOK(0x4F44AF, ViewCtrlPreRenderRestore, 0x5)
{
	bool forceRedraw = false;
	VIEWCTRL_SEH_BEGIN
	forceRedraw = RenderZoom::PreRender();
	VIEWCTRL_SEH_END

	const DWORD eax = forceRedraw ? 2u : R->EAX();
	R->EAX(eax);
	R->CL((BYTE)(eax != 0));
	return 0x4F44B4;
}

// 0x4F451B: "mov al, byte ptr [0xB0B519]" — the 5 bytes right after the frame
// was drawn into DSurface::Composite, also inside GScreenClass::Render.
// Frame order proven by disassembly: this stretch runs before
// wwmouse->DrawCursorBuffered(Composite) at 0x4F4583 (cursor is blitted
// unscaled on top), before the virtual RenderFrame() at 0x4F459A (vtable
// 0x7E1964, slot 0x44 — the table stored into [0x87F7E8] at 0x40D1B2), and
// before Frame::Present(Composite) at 0x4F4761 — so the magnified pixels are
// what reaches the screen. [0xB0B519] has exactly three refs in the binary:
// this read, its clear at 0x4F4542, and its set at 0x6A7481 inside
// Sidebar::FlushSidebarTooltip ("sidebar invalidated -> update cursor there").
#ifdef VIEWCTRL_TEST
// Test seam: 0xB0B519 can belong to a DLL image in the test process (ASLR),
// so the tests point the hook at memory they own. Null restores the real
// address.
static const BYTE* g_flagByte = reinterpret_cast<const BYTE*>(0x00B0B519u);

void SetFlagByteForTest(const BYTE* p)
{
	g_flagByte = p ? p : reinterpret_cast<const BYTE*>(0x00B0B519u);
}
#endif

DEFINE_HOOK(0x4F451B, ViewCtrlPostRenderZoom, 0x5)
{
	VIEWCTRL_SEH_BEGIN
	RenderZoom::PostRender();
	VIEWCTRL_SEH_END

#ifdef VIEWCTRL_TEST
	const DWORD eax = (R->EAX() & 0xFFFFFF00u) | (DWORD)(*g_flagByte);
#else
	const DWORD eax = (R->EAX() & 0xFFFFFF00u) | (DWORD)(*(const BYTE*)0xB0B519);
#endif
	R->EAX(eax);
	return 0x4F4520;
}

// 0x6D864E: "mov ebx, dword ptr [0x886FA8]" (DSurface::ViewBounds.W) inside
// ClampCoordMap (GameAddr::ClampCoordMap). The instruction is 6 bytes but the
// hook window is 5: the handler supplies the value and execution resumes at
// 0x6D8654, past the whole instruction. The 0x6D868A hook below replaces the
// matching view height read.
DEFINE_HOOK(0x6D864E, ViewCtrlClampWidth, 0x6)
{
	int width = 0;
	VIEWCTRL_SEH_BEGIN
	width = RenderZoom::ClampWidth();
	VIEWCTRL_SEH_END
	R->EBX((DWORD)width);
	return 0x6D8654;
}

// 0x6D868A: "mov ebx, dword ptr [0x886FAC]" — the view height. Runs after the
// width was already used, so only the height half of the clamp changes here.
DEFINE_HOOK(0x6D868A, ViewCtrlClampHeight, 0x6)
{
	int height = 0;
	VIEWCTRL_SEH_BEGIN
	height = RenderZoom::ClampHeight();
	VIEWCTRL_SEH_END
	R->EBX((DWORD)height);
	return 0x6D8690;
}

// The five hooks below replace the ViewBounds.W reads inside BlitSidebar
// (GameAddr::BlitSidebar, 0x6A70E0). BlitSidebar copies DSurface_Sidebar to
// DSurface_Primary and uses ViewBounds.W to compute the blit destination X;
// while the view is magnified the game global holds the smaller source rect,
// so the sidebar would shift into the map area. Each site returns the real
// view width instead. The instructions are 6 bytes each ("mov r32, [0x886FA8]").

// 0x6A71D0: "mov edx, dword ptr [0x886FA8]" — first dest X computation.
DEFINE_HOOK(0x6A71D0, ViewCtrlSidebarBlitX1, 0x6)
{
	int width = 0;
	VIEWCTRL_SEH_BEGIN
	width = RenderZoom::SidebarViewWidth();
	VIEWCTRL_SEH_END
	R->EDX((DWORD)width);
	return 0x6A71D6;
}

// 0x6A72B6: "mov edx, dword ptr [0x886FA8]" — second dest X computation.
DEFINE_HOOK(0x6A72B6, ViewCtrlSidebarBlitX2, 0x6)
{
	int width = 0;
	VIEWCTRL_SEH_BEGIN
	width = RenderZoom::SidebarViewWidth();
	VIEWCTRL_SEH_END
	R->EDX((DWORD)width);
	return 0x6A72BC;
}

// 0x6A732C: "mov ebp, dword ptr [0x886FA8]" — third dest X computation.
DEFINE_HOOK(0x6A732C, ViewCtrlSidebarBlitX3, 0x6)
{
	int width = 0;
	VIEWCTRL_SEH_BEGIN
	width = RenderZoom::SidebarViewWidth();
	VIEWCTRL_SEH_END
	R->EBP((DWORD)width);
	return 0x6A7332;
}

// 0x6A73C3: "mov ebp, dword ptr [0x886FA8]" — fourth dest X computation.
DEFINE_HOOK(0x6A73C3, ViewCtrlSidebarBlitX4, 0x6)
{
	int width = 0;
	VIEWCTRL_SEH_BEGIN
	width = RenderZoom::SidebarViewWidth();
	VIEWCTRL_SEH_END
	R->EBP((DWORD)width);
	return 0x6A73C9;
}

// 0x6A7439: "mov edi, dword ptr [0x886FA8]" — fifth dest X computation.
DEFINE_HOOK(0x6A7439, ViewCtrlSidebarBlitX5, 0x6)
{
	int width = 0;
	VIEWCTRL_SEH_BEGIN
	width = RenderZoom::SidebarViewWidth();
	VIEWCTRL_SEH_END
	R->EDI((DWORD)width);
	return 0x6A743F;
}

// The four hooks below replace the old window message / GetCursorPos level
// cursor remap: the game keeps the physical cursor, and only the points where
// it turns a screen position into game space are un-magnified — exactly these
// four call sites.

static POINT g_clickCoords = {};
static POINT g_bandStart = {};
static POINT g_bandEnd = {};

// 0x692325: "mov ebp, [esp+0x28]" (4 bytes) + "mov [edx], eax" (2 bytes) —
// 6 bytes, execution continues at 0x69232B. Inside DisplayClass::
// ProcessClickCoords (GameAddr, 0x692300; YRpp/Encyclopedia; ReSource alias
// "Tactical::MouseOver"). The tactical click pipeline validates the point at
// [esp+0x28] against the cell under the cursor: hand it the un-magnified
// point so the selection lands on what is actually displayed. The click
// origin is DSurface::ViewBounds (0x886FA0), not the view_bound rectangle at
// 0xB0CE28 (Encyclopedia, Selection-Mouse). EDX is an optional out pointer
// that receives the result EAX.
DEFINE_HOOK(0x692325, ViewCtrlProcessClickCoords, 0x6)
{
	static bool logged = false;

	POINT* pt = reinterpret_cast<POINT*>(R->Stack32(0x28));
	POINT* use = pt;
	VIEWCTRL_SEH_BEGIN
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
	VIEWCTRL_SEH_END

	R->EBP(reinterpret_cast<DWORD>(use));
	if (R->EDX())
		*reinterpret_cast<DWORD*>(R->EDX()) = R->EAX();
	return 0x69232B;
}

// 0x6D9F80: "mov eax, dword ptr [ecx+0xD90]" — TacticalClass::InitScrollBounds,
// 6 bytes, continues at 0x6D9F86. The band start point arrives through
// [esp+4]: replace it with the un-magnified point so the rubber band is
// dragged in content space. The EAX load is reproduced with a null check on
// ECX: a null this sets EAX=0 instead of faulting (the original would
// dereference [ecx+0xD90] unguarded). This is an intentional, test-locked
// semantic difference — defensive against a dangling TacticalClass.
DEFINE_HOOK(0x6D9F80, ViewCtrlDragBandStart, 0x6)
{
	static bool logged = false;

	POINT* pt = reinterpret_cast<POINT*>(R->Stack32(4));
	VIEWCTRL_SEH_BEGIN
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
	VIEWCTRL_SEH_END
	return 0x6D9F86;
}

// 0x6D9FC0: "mov eax, dword ptr [ecx+0xD90]" — TacticalClass::UpdateScrollMaxBounds,
// 6 bytes, continues at 0x6D9FC6. The band end point at [esp+4] gets the same
// un-magnify as the band start above. Same null-guard drift as the start
// hook: EAX=0 on a null this instead of the original's unguarded load.
DEFINE_HOOK(0x6D9FC0, ViewCtrlDragBandEnd, 0x6)
{
	static bool logged = false;

	POINT* pt = reinterpret_cast<POINT*>(R->Stack32(4));
	VIEWCTRL_SEH_BEGIN
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
	VIEWCTRL_SEH_END
	return 0x6D9FC6;
}

// 0x693791: inside ScrollMapEdge (GameAddr::ScrollMapEdge, 0x693440, ReSource):
// "xor esi, esi" (2 bytes) + "mov [esp+0x28], esi" (4 bytes) —
// 6 bytes, continues at 0x693797. The right drag scroll speeds live at
// [esp+0x18]/[esp+0x1c] and are screen deltas: while the view is magnified
// the content moves by source/view (= 1/zoom) of that.
DEFINE_HOOK(0x693791, ViewCtrlRightDragSpeed, 0x6)
{
	static bool logged = false;

	VIEWCTRL_SEH_BEGIN
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
			*speedX = (int)roundf((float)*speedX * scaleX);
			*speedY = (int)roundf((float)*speedY * scaleY);
			if (!logged)
			{
				LOG("RightDragSpeed scaled by (%.3f,%.3f)", scaleX, scaleY);
				logged = true;
			}
		}
	}
	VIEWCTRL_SEH_END

	R->ESI(0u);
	R->ref_Stack<DWORD>(0x28) = 0u;
	return 0x693797;
}

// The three hooks below capture HUD draws while the frame is substituted
// (TextDefer, the Kaleidoscope v1.1 pattern): DrawTimer and DrawScreenText
// are skipped to their untouched `ret N` tails at function entry — the
// prologue never runs, so no stack slot is unbalanced — and the
// GScreenClass::Render DrawUI virtual call is skipped as a whole window.
// ReplayDeferred re-invokes the originals after RenderZoom restored the
// view dimensions, so they recompute their layout at native size. Appended
// at the end of this file so the .syhks00 declaration order the tests read
// stays stable.

// 0x6D4B50: TacticalClass::DrawTimer — fastcall (index=ECX, scheme=EDX),
// 4 stack args (Time, Text, xy1, xy2), `ret 0x10` at 0x6D4DAC. EAX on the
// skip: Kaleidoscope leaves the index there (`mov eax,ecx`); the call sites
// overwrite or ignore the return (site 0x6D4A6B stores its own value, the
// other two ignore it).
DEFINE_HOOK(0x6D4B50, ViewCtrlDrawTimerDefer, 0x6)
{
	bool defer = false;
	VIEWCTRL_SEH_BEGIN
	defer = TextDefer::TryDeferTimer(static_cast<int>(R->ECX()),
		reinterpret_cast<void*>(R->EDX()),
		static_cast<int>(R->Stack32(4)),
		reinterpret_cast<const wchar_t*>(R->Stack32(8)),
		reinterpret_cast<void*>(R->Stack32(0xC)),
		reinterpret_cast<void*>(R->Stack32(0x10)));
	VIEWCTRL_SEH_END

	if (defer)
	{
		R->EAX(R->ECX()); // the skipped body's first output (Kaleidoscope)
		return 0x6D4DAC;  // `ret 0x10`
	}
	return 0;
}

// 0x6D4E20: TacticalClass::DrawScreenText — thiscall (this=ECX), 1 stack
// arg (a Point2D* member of the TacticalClass), `ret 4` at 0x6D501F. The
// 5-byte prologue (`mov al,[0xa8ed6b]; sub esp,0x2c`) never runs on a
// skip, so the tail pops the pristine entry stack; EAX is not inspected
// by any of the three call sites.
DEFINE_HOOK(0x6D4E20, ViewCtrlDrawScreenTextDefer, 0x5)
{
	bool defer = false;
	VIEWCTRL_SEH_BEGIN
	defer = TextDefer::TryDeferScreenText(reinterpret_cast<void*>(R->ECX()),
		reinterpret_cast<void*>(R->Stack32(4)));
	VIEWCTRL_SEH_END

	if (defer)
		return 0x6D501F; // `ret 4`
	return 0;
}

// 0x4F44F9: GScreenClass::Render — 12-byte window over the DrawUI virtual
// call (`mov eax,[esp+0x10]; mov edx,[esi]; push eax; mov ecx,esi;
// call [edx+0x40]`). Skipping the whole window defers the entire UI pass
// with a single record; the `push eax` inside it is skipped as well, so
// the stack stays balanced. EAX is left as the preceding blit returned it
// (nothing between here and the resume inspects it).
DEFINE_HOOK(0x4F44F9, ViewCtrlDrawUIDefer, 0xC)
{
	bool defer = false;
	VIEWCTRL_SEH_BEGIN
	defer = TextDefer::TryDeferUI(reinterpret_cast<void*>(R->ESI()),
		R->Stack32(0x10));
	VIEWCTRL_SEH_END

	if (defer)
		return 0x4F4505;
	return 0;
}
