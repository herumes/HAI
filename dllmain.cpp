#include "pch.h"
#include "common.hpp"
#include "hooks.hpp"
#include "gamesdk.hpp"
#include "settings.hpp"
#include "autofarm.hpp"

// From chat_core / ui
extern HWND g_hWnd;
extern WNDPROC g_oWndProc;
extern const UINT JOSE_TIMER_ID;
extern std::atomic<bool> g_chatHooked;
extern HANDLE g_installerThread;
void OverlayThread();  // ui.cpp

// Whether this load is the real one, inside the game (see DLL_PROCESS_ATTACH).
static bool s_active = false;

// =============================================================================
// Entry
// =============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // Loaded into anything but the game (the injector loads it too),
        // stay inert. Starting the installer thread there meant it woke up
        // after the injector had unloaded the DLL again and crashed the
        // injector with an access violation in "HytaleChatLogger.dll_unloaded".
        if (!GetModuleHandleW(L"HytaleClient.exe")) return TRUE;
        s_active = true;
        if (MH_Initialize() != MH_OK) return FALSE;

        CreateDirectoryW(L"C:\\temp", nullptr);

        g_installerThread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            Sleep(2500);

            bool overlayStarted = false;

            bool sdkHookTried = false;

            for (int i = 0; i < 30 && !g_shuttingDown.load(); ++i) {
                InstallChatHooks(); // also rebinds hwnd if the game recreated it

                // The per-frame GameInstance detour that makes live coordinates
                // work without any scanning (see gamesdk.hpp). Independent of
                // the chat hooks: it is worth having even if chat never binds.
                if (!GsdkInstallFrameHook() && !sdkHookTried && i >= 2) {
                    // No frame signature matched this build. Fall back to
                    // locating Application.App by MethodTable instead - one
                    // background scan, then the same object graph works.
                    sdkHookTried = true;
                    StartWorker([]() { GsdkFindApp(); });
                }

                // Start the menu as soon as the game window exists — do not wait on hooks.
                if (!overlayStarted && g_hWnd) {
                    LogToFile("=== HyAI v3.2 DLL Injected - External Window Mode ===");
                    StartWorker([]() { OverlayThread(); });
                    overlayStarted = true;
                }

                // Do not stop looping until the game-object hook has had its
                // attempts too - chat often binds on the first pass, and leaving
                // early used to mean the position hook never got a second try.
                if (g_chatHooked.load() && overlayStarted &&
                    (g_gsdkHookInstalled.load() || sdkHookTried))
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
        if (!s_active) return TRUE;
        g_shuttingDown = true;
        // A key the farm is holding would otherwise stay held in the game.
        FarmReleaseAllNow();
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
