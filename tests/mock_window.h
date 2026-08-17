#pragma once

#include <windows.h>

class MockWindow
{
public:
    HWND hWnd = NULL;
    WNDCLASSEXW wc = {};
    bool registered = false;

    bool Create()
    {
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"ViewCtrlTestWnd";
        if (!RegisterClassExW(&wc)) return false;
        registered = true;

        hWnd = CreateWindowExW(
            0, wc.lpszClassName, L"Test",
            WS_OVERLAPPEDWINDOW,
            0, 0, 800, 600,
            NULL, NULL, wc.hInstance, NULL
        );
        return hWnd != NULL;
    }

    void Destroy()
    {
        if (hWnd) { DestroyWindow(hWnd); hWnd = NULL; }
        if (registered) { UnregisterClassW(wc.lpszClassName, wc.hInstance); registered = false; }
    }

    ~MockWindow() { Destroy(); }
};
