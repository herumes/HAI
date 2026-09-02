#include "pch.h"
#include "common.hpp"
#include "hooks.hpp"
#include "settings.hpp"

// From chat_core / ui
extern HWND g_hWnd;
extern WNDPROC g_oWndProc;
extern const UINT JOSE_TIMER_ID;
extern std::atomic<bool> g_chatHooked;
extern HANDLE g_installerThread;
void OverlayThread();  // ui.cpp

// =============================================================================
// Entry
// =============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        if (MH_Initialize() != MH_OK) return FALSE;

        CreateDirectoryW(L"C:\\temp", nullptr);

        g_installerThread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            Sleep(2500);

            bool overlayStarted = false;

            for (int i = 0; i < 30 && !g_shuttingDown.load(); ++i) {
                InstallChatHooks(); // also rebinds hwnd if the game recreated it

                // Start the menu as soon as the game window exists — do not wait on hooks.
                if (!overlayStarted && g_hWnd) {
                    LogToFile("=== Josebot v3.2 DLL Injected - External Window Mode ===");
                    StartWorker([]() { OverlayThread(); });
                    overlayStarted = true;
                }

                if (g_chatHooked.load() && overlayStarted)
                    break;

                Sleep(2000);
            }

            if (!overlayStarted && !g_shuttingDown.load()) {
                LogToFile("Installer: game hwnd never found; starting overlay anyway");
                StartWorker([]() { OverlayThread(); });
            }
            return 0;
            }, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        g_shuttingDown = true;
        AbortHttp();

        // Process exit: do nothing further under loader lock.
        if (lpReserved != nullptr)
            return TRUE;

        // FreeLibrary path only:
        if (g_hWnd) {
            KillTimer(g_hWnd, JOSE_TIMER_ID);
            if (g_oWndProc)
                SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, (LONG_PTR)g_oWndProc);
            g_oWndProc = nullptr;
        }

        MH_DisableHook(MH_ALL_HOOKS);
        DetachAllWorkers();

        // No Sleep, no join, no file I/O, no MH_Uninitialize under loader lock.
        // Settings are saved from the overlay when dirty.
    }
    return TRUE;
}
