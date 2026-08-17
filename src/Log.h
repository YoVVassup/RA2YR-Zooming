#pragma once

#include <windows.h>
#include <cstdio>

namespace Debug
{
    inline HMODULE& GetDllModule()
    {
        static HMODULE hMod = NULL;
        return hMod;
    }

    inline void SetDllHandle(HMODULE h)
    {
        GetDllModule() = h;
    }

    inline const char* GetLogPath()
    {
        static char path[MAX_PATH] = {};
        if (path[0] == '\0' && GetDllModule())
        {
            GetModuleFileNameA(GetDllModule(), path, MAX_PATH);
            char* lastSlash = strrchr(path, '\\');
            if (lastSlash) *(lastSlash + 1) = '\0';
            strcat_s(path, "ViewCtrl.log");
        }
        return path;
    }

    inline void Log(const char* fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        SYSTEMTIME st;
        GetLocalTime(&st);

        FILE* f = nullptr;
        fopen_s(&f, GetLogPath(), "a");
        if (f)
        {
            fprintf(f, "[%02d:%02d:%02d.%03d] %s",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
            fclose(f);
        }
    }
}

#define LOG(fmt, ...) Debug::Log("[ViewCtrl] " fmt "\n", ##__VA_ARGS__)
