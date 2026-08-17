#include "Main.hpp"
#include "Zoomer.hpp"
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
