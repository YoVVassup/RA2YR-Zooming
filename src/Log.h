#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

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

    // Release builds log only when the player opts in: set the VIEWCTRL_LOG
    // environment variable to a non-empty value other than "0" and a
    // ViewCtrl.log appears next to the game executable — no Debug rebuild
    // needed for a support request. Debug builds always log.
    // Cached after the first call so the per-frame hooks pay a load only.
#ifdef VIEWCTRL_TEST
    // Tests pin the flag so the disabled branch of Log() can run under
    // the Debug configuration (where the env-var path is compiled out).
    inline bool& LogForceDisabled()
    {
        static bool flag = false;
        return flag;
    }
#endif

    inline bool LogEnabled()
    {
#ifdef VIEWCTRL_TEST
        if (LogForceDisabled())
            return false;
#endif
#ifdef NDEBUG
        static const bool enabled = []() {
            char buf[8] = {};
            const DWORD n = GetEnvironmentVariableA("VIEWCTRL_LOG", buf, sizeof(buf));
            return n > 0 && n < sizeof(buf) && buf[0] != '0';
        }();
        return enabled;
#else
        return true;
#endif
    }

#ifdef VIEWCTRL_TEST
    inline void SetLogEnabledForTest(bool enabled)
    {
        LogForceDisabled() = !enabled;
    }
#endif

    inline void Log(const char* fmt, ...)
    {
        if (!LogEnabled())
            return;

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

// Debug builds always write ViewCtrl.log next to the game executable.
// Release builds write nothing unless VIEWCTRL_LOG is set (see
// Debug::LogEnabled); the call still costs one cached bool check.
#ifdef NDEBUG
#define LOG(fmt, ...) do { if (Debug::LogEnabled()) Debug::Log("[ViewCtrl] " fmt "\n", ##__VA_ARGS__); } while (0)
#else
#define LOG(fmt, ...) Debug::Log("[ViewCtrl] " fmt "\n", ##__VA_ARGS__)
#endif
