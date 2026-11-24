#include "win_app.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // Enable Per-Monitor V2 DPI awareness on Windows 10 1703+ (also declared in manifest)
    using SetDpiCtxFn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
        if (auto fn = reinterpret_cast<SetDpiCtxFn>(
                ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    HANDLE singleInstance = ::CreateMutexW(nullptr, TRUE, L"Local\\YunTypeWindowsSingleInstance");
    if (singleInstance && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(singleInstance);
        return 0;
    }

    yuntype::win::WinApp app;
    const int rc = app.run(hInstance);

    if (singleInstance) {
        ::ReleaseMutex(singleInstance);
        ::CloseHandle(singleInstance);
    }
    return rc;
}
