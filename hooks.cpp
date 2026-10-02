#include "pch.h"
#include "hooks.hpp"
#include <intrin.h>   // _ReturnAddress
#include "common.hpp"
#include "chat.hpp"
#include "settings.hpp"
#include "shared.hpp"
#include "extras.hpp"
#include "hookthunk.hpp"
#include "gamesdk.hpp"  // GsdkFrameCarriesChatSends

// Forward decls from chat_core
void ProcessMessage(void* message);
void ProcessPending();

// Signatures
static const char* SIG_SEND_MESSAGE =
"57 56 53 48 83 EC ? 48 8B D9 48 8B F2 48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8B F8 48 8B 4B";

static const char* SIG_ADD_MESSAGE =
"57 56 53 48 83 EC ? 48 8B D9 48 8B F2 48 8B 4B ? 39 09";

static const char* SIG_ON_RECEIVE =
"41 56 57 56 55 53 48 83 EC ? 33 C0 48 89 44 24 ? 0F 57 E4 0F 29 64 24 ? 0F 29 64 24 ? 0F 29 64 24 ? 48 8B D9 48 8B F2 48 8D 0D";


// Memory helpers (needed by hooks / ProcessMessage)
bool RegionReadable(const void* addr, size_t n) {
    if (!addr || !n) return false;
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + n;
    if (end < start) return false; // overflow
    while (start < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((void*)start, &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        const DWORD prot = mbi.Protect & 0xFF;
        if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE || prot == 0)
            return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (regionEnd <= start)
            return false;
        start = regionEnd;
    }
    return true;
}

bool IsPlausiblePtr(const void* p) {
    const uintptr_t a = (uintptr_t)p;
    if (a < 0x10000) return false;
    if (a > 0x00007FFFFFFFFFFFULL) return false;
    if (a & 0x1) return false;
    return true;
}

bool SafeRead(const void* src, void* dst, size_t n) {
    // Hot path: must stay cheap. VirtualQuery-per-read lagged the client to death.
    // IsPlausiblePtr rejects obvious junk; SEH catches real AVs without a probe storm
    // when combined with the tight walker limits / no blind arrLen 1..128 scans.
    if (!src || !dst || !n) return false;
    if (!IsPlausiblePtr(src)) return false;
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static uintptr_t GetModuleBaseSize(const wchar_t* name, size_t& outSize);

uintptr_t GetGameModuleBaseSize(size_t& outSize) {
    return GetModuleBaseSize(L"HytaleClient.exe", outSize);
}

static uintptr_t GetModuleBaseSize(const wchar_t* name, size_t& outSize) {
    HMODULE mods[1024]{};
    DWORD cb = 0;
    outSize = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &cb)) return 0;
    for (unsigned i = 0; i < cb / sizeof(HMODULE); ++i) {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameExW(GetCurrentProcess(), mods[i], path, MAX_PATH)) continue;
        if (!wcsstr(path, name)) continue;
        MODULEINFO mi{};
        if (GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) {
            outSize = mi.SizeOfImage;
            return (uintptr_t)mi.lpBaseOfDll;
        }
    }
    return 0;
}

static bool ParsePattern(const char* ida, std::vector<uint8_t>& bytes, std::vector<bool>& mask) {
    bytes.clear(); mask.clear();
    const char* p = ida;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?') {
            bytes.push_back(0); mask.push_back(false);
            ++p; if (*p == '?') ++p;
            continue;
        }
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) return false;
        auto h = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            return 10 + (char)tolower(c) - 'a';
            };
        bytes.push_back((h(p[0]) << 4) | h(p[1]));
        mask.push_back(true);
        p += 2;
    }
    return !bytes.empty();
}

static uintptr_t PatternScan(uintptr_t base, size_t size, const char* ida) {
    std::vector<uint8_t> pat;
    std::vector<bool> mask;
    if (!ParsePattern(ida, pat, mask) || size < pat.size()) return 0;
    const size_t plen = pat.size();
    uintptr_t cur = base;
    const uintptr_t endAddr = base + size;

    // Walk VirtualQuery regions so we never touch unmapped holes in SizeOfImage.
    // Within a readable region, scan with direct reads (fast). Per-byte SafeRead
    // over a multi-MB image is too slow and delayed overlay startup indefinitely.
    while (cur + plen <= endAddr) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((const void*)cur, &mbi, sizeof(mbi)))
            break;

        const uintptr_t regionStart = (uintptr_t)mbi.BaseAddress;
        const uintptr_t regionEnd = regionStart + mbi.RegionSize;
        if (regionEnd <= cur)
            break;

        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable =
            mbi.State == MEM_COMMIT &&
            !(mbi.Protect & PAGE_GUARD) &&
            (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                prot == PAGE_EXECUTE_WRITECOPY);

        if (!readable) {
            cur = regionEnd;
            continue;
        }

        const uintptr_t scanStart = (std::max)(cur, regionStart);
        const uintptr_t scanEnd = (std::min)(endAddr, regionEnd);
        if (scanEnd < scanStart + plen) {
            cur = regionEnd;
            continue;
        }

        const uint8_t* hay = (const uint8_t*)scanStart;
        const size_t limit = (size_t)(scanEnd - scanStart - plen + 1);
        for (size_t i = 0; i < limit; ++i) {
            bool ok = true;
            for (size_t j = 0; j < plen; ++j) {
                if (mask[j] && hay[i + j] != pat[j]) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                return scanStart + i;
        }
        cur = regionEnd;
    }
    return 0;
}

uintptr_t GameModulePatternScan(const char* idaPattern) {
    size_t size = 0;
    const uintptr_t base = GetGameModuleBaseSize(size);
    if (!base || !size) return 0;
    return PatternScan(base, size, idaPattern);
}

// =============================================================================
// System.String
// =============================================================================
bool ReadSystemString(void* obj, std::string& out, uint64_t* outVT) {
    if (!obj || !IsPlausiblePtr(obj)) return false;
    uint64_t vt = 0;
    int32_t len = 0;
    if (!SafeRead(obj, &vt, 8) || !vt || !IsPlausiblePtr((void*)vt)) return false;

    const uint64_t known = g_stringVT.load();
    if (known && vt != known) return false;

    if (!SafeRead((uint8_t*)obj + 8, &len, 4)) return false;
    if (len <= 0 || len > 2048) return false;

    wchar_t term = 1;
    if (!SafeRead((uint8_t*)obj + 0x0C + len * 2, &term, 2) || term != 0) return false;

    std::vector<wchar_t> buf(len);
    if (!SafeRead((uint8_t*)obj + 0x0C, buf.data(), len * 2)) return false;

    out = WideToUtf8(buf.data(), len);
    if (outVT) *outVT = vt;

    size_t nullPos = out.find('\0');
    if (nullPos != std::string::npos)
        out.resize(nullPos);

    if (!out.empty() && !known && vt)
        g_stringVT.store(vt);

    return !out.empty();
}

void* CreateSystemString(const std::string& utf8) {
    const uint64_t vt = g_stringVT.load();
    if (!vt) return nullptr;
    auto w = Utf8ToWide(utf8);

    if (w.size() > HYTALE_CHAT_MAX)
        w.resize(HYTALE_CHAT_MAX);

    int32_t len = (int32_t)w.size();
    if (len <= 0) return nullptr;

    size_t total = 32 + 0x0C + (size_t)(len + 1) * 2 + 16;
    auto mem = std::shared_ptr<uint8_t>(new uint8_t[total](), std::default_delete<uint8_t[]>());

    uint8_t* p = mem.get() + 32;

    memcpy(p + 0x00, &vt, 8);
    memcpy(p + 0x08, &len, 4);
    memcpy(p + 0x0C, w.data(), (size_t)len * 2);
    wchar_t z = 0;
    memcpy(p + 0x0C + (size_t)len * 2, &z, 2);

    {
        std::lock_guard lock(g_mtx);
        g_keepalive.push_back(mem);
        // Never erase: reuse/UAF of fake System.String is worse than a small leak.
        // Note it in the log each time the count doubles past the soft cap -
        // logging every insert past it used to write a line per message sent.
        const size_t n = g_keepalive.size();
        if (n > KEEPALIVE_MAX * 2 && (n & (n - 1)) == 0)
            LogToFile("keepalive oversized: " + std::to_string(n));
    }
    return p;
}


// Hook implementations
// =============================================================================
// Only accept chat instances from Send/Add hooks (same as original). Never from OnReceive.
static void NoteChatObject(void* self) {
    if (!self || !IsPlausiblePtr(self)) return;

    uint64_t vt = 0;
    if (!SafeRead(self, &vt, 8) || !vt || !IsPlausiblePtr((void*)vt))
        return;

    const uint64_t knownVT = g_chatVT.load();
    // Once we know the chat vtable, ignore foreign objects with a different VT.
    if (knownVT && vt != knownVT)
        return;

    void* prev = g_pChat.load();
    if (prev && prev != self) {
        // New instance of the same chat class (server switch).
        g_chatReady.store(false);
        {
            std::lock_guard lock(g_mtx);
            if (!g_pending.empty()) {
                LogToFile("Chat instance changed — cleared " +
                    std::to_string(g_pending.size()) + " pending send(s)");
                g_pending.clear();
            }
        }
        g_sendDisabled.store(false);
        LogToFile("Chat object replaced — waiting for you to type once in chat");
    }

    g_pChat.store(self);
    g_chatVT.store(vt);
    if (g_sendDisabled.load())
        g_sendDisabled.store(false);
}

// All three chat hooks run through hookthunk.hpp: our work happens first, then
// the thunk JUMPS to the game's function. They used to call the original and
// carry on afterwards, which left a C++ frame under the game's code - and every
// crash dump of the game fail-fasting (0xC0000602) had hkOnReceive's return
// address as the frame the garbage collector could not walk past.
//
// Bot messages are no longer sent from here as a rule. When the frame hook is
// running on the chat thread it sends them in place of one frame's call (see
// gamesdk.cpp), which leaves nothing of ours under the game's code at all. The
// chat hooks only send as a fallback, on builds where that is not possible.
static void FlushPendingFallback() {
    if (!GsdkFrameCarriesChatSends())
        ProcessPending();
}

static uintptr_t SendMessagePre(HookRegs* r) {
    HookGuard guard;
    void* const self = (void*)r->rcx;
    void* const message = (void*)r->rdx;
    bool swallow = false;
    try {
        if (!g_shuttingDown.load()) {
            NoteChatObject(self);
            g_gameThread.store(GetCurrentThreadId());

            std::string text;
            uint64_t vt = 0;
            if (ReadSystemString(message, text, &vt) && vt) {
                g_stringVT.store(vt);
                g_chatReady.store(true);

                if (HandleOwnerCommand(text, false))
                    swallow = true; // owner commands never reach the server
            }
            // Before the player's own message rather than after it, now that
            // nothing may run once the game's function has been entered.
            if (g_chatReady.load())
                FlushPendingFallback();
        }
    }
    catch (...) { LogToFile("hkSendMessage exception"); }

    if (swallow) return 0;
    return (uintptr_t)oSendMessage;
}

static uintptr_t AddMessagePre(HookRegs* r) {
    HookGuard guard;
    void* const self = (void*)r->rcx;
    void* const message = (void*)r->rdx;
    try {
        if (!g_shuttingDown.load()) {
            NoteChatObject(self);
            if (!g_gameThread.load())
                g_gameThread.store(GetCurrentThreadId());

            // Until the player types once on this chat instance, do nothing else.
            if (g_chatReady.load()) {
                // Reading the message before the game shows it rather than
                // after: it is the same object either way, and after is no
                // longer somewhere we can be.
                if (message && IsPlausiblePtr(message))
                    ProcessMessage(message);
                FlushPendingFallback();
            }
        }
    }
    catch (...) { LogToFile("hkAddMessage exception"); }
    return (uintptr_t)oAddMessage;
}

std::atomic<uintptr_t> g_recvCallerRva{ 0 };
std::atomic<uintptr_t> g_recvFuncRva{ 0 };

static uintptr_t OnReceivePre(HookRegs* r) {
    HookGuard guard;
    void* const message = (void*)r->rdx;

    // Where the client called us FROM. The inbound packet signature does not
    // match this build, so there is no dispatcher to hook by pattern - but chat
    // arrives over the network like everything else, so whoever calls this is
    // standing on the inbound path. A return address is the one way to find a
    // function when its bytes cannot be recognised.
    if (!g_recvCallerRva.load()) {
        const uintptr_t ret = (uintptr_t)r->ret;
        size_t modSize = 0;
        const uintptr_t base = GetGameModuleBaseSize(modSize);
        if (base && ret > base && ret < base + modSize) {
            g_recvCallerRva.store(ret - base);

            // Walk back to the start of whatever function that is. Functions
            // are aligned and preceded by padding, so the first run of int3 or
            // nop above it marks the boundary.
            uintptr_t at = ret;
            for (size_t back = 0; back < 0x1000 && at > base + 16; ++back, --at) {
                uint8_t b[2] = { 0, 0 };
                if (!SafeRead((const void*)(at - 1), b, 2)) break;
                if (b[0] != 0xCC && b[0] != 0x90) continue;
                // Skip the padding itself and take what follows.
                uintptr_t f = at;
                while (f < ret) {
                    uint8_t p = 0;
                    if (!SafeRead((const void*)f, &p, 1)) break;
                    if (p != 0xCC && p != 0x90) break;
                    ++f;
                }
                if (f < ret) g_recvFuncRva.store(f - base);
                break;
            }
        }
    }

    try {
        // Never treat OnReceive `self` as the chat object (different type).
        // Network/join traffic must not drive our walker until the player has typed.
        const DWORD gt = g_gameThread.load();
        if (!g_shuttingDown.load() && g_chatReady.load() && gt && GetCurrentThreadId() == gt) {
            if (message && IsPlausiblePtr(message))
                ProcessMessage(message);
            FlushPendingFallback();
        }
    }
    catch (...) { LogToFile("hkOnReceive exception"); }
    return (uintptr_t)oOnReceive;
}

static LRESULT CALLBACK hkGameWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_shuttingDown.load()) {
        if (g_oWndProc)
            return CallWindowProcW(g_oWndProc, h, m, w, l);
        return DefWindowProcW(h, m, w, l);
    }

    if (m == WM_TIMER && w == JOSE_TIMER_ID) {
        // Only queues. This used to send the bot queue too, which called the
        // game's managed SendMessage from inside a window message - where the
        // thread is outside the runtime (the game is waiting in a native
        // message pump), so a collection could run straight through our call
        // and move objects out from under it. Sends happen from the hooks now.
        if (IsWindow(h) && g_chatReady.load()) {
            try {
                Extras::TickReminders([](const std::string& text) {
                    QueueBotReply(text);
                    });
            }
            catch (...) {
                LogToFile("WM_TIMER reminder exception");
                InvalidateChatSession("timer exception");
            }
        }
        return 0;
    }

    if (m == WM_NCDESTROY || m == WM_DESTROY) {
        LRESULT r = g_oWndProc ? CallWindowProcW(g_oWndProc, h, m, w, l) : DefWindowProcW(h, m, w, l);
        if (h == g_hWnd) {
            KillTimer(h, JOSE_TIMER_ID);
            g_oWndProc = nullptr;
            g_hWnd = nullptr;
            InvalidateChatSession("game window destroy");
            LogToFile("Game window destroyed — session cleared");
        }
        return r;
    }

    if (g_oWndProc)
        return CallWindowProcW(g_oWndProc, h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

static void EnsureGameWindowHooked() {
    HWND hwnd = FindWindowW(nullptr, L"Hytale");
    if (!hwnd) hwnd = FindWindowW(nullptr, L"HytaleClient");
    if (!hwnd)
        return;

    // Only bind once while hwnd is alive. Do NOT re-subclass from a background
    // thread during server switches — that races the game and can hard-crash.
    if (g_hWnd && IsWindow(g_hWnd) && g_oWndProc)
        return;

    g_hWnd = hwnd;
    if (!g_oWndProc) {
        g_oWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)hkGameWndProc);
        SetTimer(hwnd, JOSE_TIMER_ID, 150, nullptr);
        LogToFile("Game WndProc hooked + 150ms timer for pending sends");
    }
}


bool InstallChatHooks() {
    EnsureGameWindowHooked();

    if (g_chatHooked.load()) return true;

    size_t size = 0;
    uintptr_t base = GetModuleBaseSize(L"HytaleClient.exe", size);
    if (!base) {
        LogToFile("InstallChatHooks: HytaleClient.exe base not found");
        return false;
    }

    uintptr_t send = PatternScan(base, size, SIG_SEND_MESSAGE);
    uintptr_t add = PatternScan(base, size, SIG_ADD_MESSAGE);
    uintptr_t recv = PatternScan(base, size, SIG_ON_RECEIVE);

    if (!send) {
        LogToFile("InstallChatHooks: SIG_SEND_MESSAGE not found (base=" +
            std::to_string(base) + " size=" + std::to_string(size) + ")");
        return false;
    }

    static void* const sendThunk = MakePreHookThunk(&SendMessagePre);
    static void* const addThunk = MakePreHookThunk(&AddMessagePre);
    static void* const recvThunk = MakePreHookThunk(&OnReceivePre);
    if (!sendThunk || !addThunk || !recvThunk) {
        LogToFile("InstallChatHooks: could not allocate hook thunks");
        return false;
    }

    if (MH_CreateHook((void*)send, sendThunk, (void**)&oSendMessage) != MH_OK ||
        MH_EnableHook((void*)send) != MH_OK)
        return false;

    if (add) {
        MH_CreateHook((void*)add, addThunk, (void**)&oAddMessage);
        MH_EnableHook((void*)add);
    }
    if (recv) {
        MH_CreateHook((void*)recv, recvThunk, (void**)&oOnReceive);
        MH_EnableHook((void*)recv);
    }
    g_chatHooked = true;
    LogToFile("Chat hooks installed");
    return true;
}



void UninstallChatHooks() {
    // Hooks are torn down via MH_DisableHook(MH_ALL_HOOKS) in DllMain detach.
}
