// Tiny DLL loaded by the unit tests so Present()'s L5 export probe and
// ModuleExportsScaler have a real foreign module to match. Not shipped.
#include <windows.h>

extern "C" __declspec(dllexport) float exfunc_get_zoom_factor() { return 1.0f; }

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
