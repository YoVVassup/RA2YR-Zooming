#include "Main.hpp"
#include "Zoomer.hpp"
#include "RenderZoom.hpp"
#include "Log.h"

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
