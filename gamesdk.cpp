#include "pch.h"
#include "gamesdk.hpp"
#include "worldread.hpp"
#include "hooks.hpp"    // SafeRead / IsPlausiblePtr / GameModulePatternScan / ReadSystemString
#include "common.hpp"   // LogToFile / NowSteadyMs / GetThisModuleDirectory / StartWorker
#include "position.hpp" // g_posX/Y/Z / g_posLocked - the display state we publish into
#include "chat.hpp"     // g_stringVT - see ReadStringIfSafe; TakePendingSend
#include "shared.hpp"   // g_gameThread
#include "hookthunk.hpp"
#include "debugshape.hpp" // DbgDrainQueue - needs the game thread, and this is it
#include "prefab.hpp"     // BuildFrameTick - the build outline, same thread
#include "selection.hpp"  // SelFrameTick - the region box, same list
#include "autofarm.hpp"   // FarmOnGameFrame - frame-exact snapshots for the auto farm
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

// =============================================================================
// State
// =============================================================================
std::atomic<uintptr_t> g_gsdkRoot{ 0 };
static std::atomic<long long> g_gsdkPublishedMs{ 0 };
std::atomic<long long> g_gsdkRootLastMs{ 0 };
std::atomic<bool>      g_gsdkHookInstalled{ false };
std::atomic<uintptr_t> g_gsdkHookRva{ 0 };
std::mutex             g_gsdkHookNameMtx;
std::string            g_gsdkHookName;

std::atomic<uintptr_t> g_gsdkAppMT{ 0 };
std::atomic<uintptr_t> g_gsdkApp{ 0 };
std::atomic<bool>      g_gsdkAppScanning{ false };
std::atomic<long long> g_gsdkAppScanMs{ 0 };

std::mutex g_gsdkPathMtx;
GsdkPath   g_gsdkPath;

std::mutex g_gsdkCandMtx;
std::vector<GsdkCandidate> g_gsdkCandidates;

std::atomic<bool>   g_gsdkDiscovering{ false };
std::atomic<int>    g_gsdkDiscoverPhase{ 0 };
std::atomic<size_t> g_gsdkDiscoverCandidates{ 0 };
std::atomic<size_t> g_gsdkDiscoverVisited{ 0 };
std::atomic<bool>   g_gsdkLastWalkTruncated{ false };
std::mutex          g_gsdkStatusMtx;
std::string         g_gsdkStatus;

void GsdkSetStatus(const std::string& s) {
    { std::lock_guard lock(g_gsdkStatusMtx); g_gsdkStatus = s; }
    LogToFile("gamesdk: " + s);
}
std::string GsdkGetStatus() {
    std::lock_guard lock(g_gsdkStatusMtx);
    return g_gsdkStatus;
}

namespace {

// How far into an object we look for pointers to other objects, and how far
// past an object's base an address still counts as "inside" it. Entity's
// position sits at ~0x284 and its last known field at ~0x498, so 0x800 covers
// the objects we care about without turning the walk into a heap scan.
constexpr uint32_t kChildWindow = 0x800;
// Fields are searched further into an object than pointers are, because an
// entity in this build turned out to be considerably larger than the published
// layout suggested and the position can sit past the pointer-dense head.
constexpr uint32_t kFieldWindow = 0x1800;
constexpr uint32_t kInsideWindow = 0x1000;
constexpr int      kMaxDepth = 3;
constexpr size_t   kMaxVisited = 120000;
constexpr size_t   kMaxAutoCandidates = 60000;
constexpr long long kRootStaleMs = 2000;   // no frame hook call for this long == not in a world

bool ReadPtr(uintptr_t at, uintptr_t& out) {
    uint64_t v = 0;
    if (!SafeRead((const void*)at, &v, 8)) return false;
    out = (uintptr_t)v;
    return true;
}

// A pointer we're willing to follow: plausible range, 8-aligned (every .NET
// object is), and its first qword (the MethodTable) is itself a plausible
// pointer. That last check throws out almost all coincidental data.
bool LooksLikeObject(uintptr_t p) {
    if (!p || !IsPlausiblePtr((const void*)p)) return false;
    if (p & 7) return false;
    uintptr_t mt = 0;
    if (!ReadPtr(p, mt)) return false;
    if (!mt || !IsPlausiblePtr((const void*)mt)) return false;
    if (mt & 7) return false;
    return true;
}

// The same world-bounds sanity position.cpp applies, reused so both finders
// agree on what a position looks like.
bool PlausiblePosition(double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    if (x == 0.0 && y == 0.0 && z == 0.0) return false;
    const double boundXZ = (double)g_posBoundXZ.load();
    if (x < -boundXZ || x > boundXZ) return false;
    if (z < -boundXZ || z > boundXZ) return false;
    if (y < (double)g_posBoundYMin.load() || y >(double)g_posBoundYMax.load()) return false;
    return true;
}

// Stricter than PlausiblePosition, and used only when *seeding* a search.
// The loose test accepts things like (0.00, 0.00, 0.05), which occur in their
// thousands - every idle velocity, scale, colour and normal in the graph - and
// swamping the candidate budget with those is what stopped the first version
// from ever reaching the real field. A player standing on a world's origin
// column would be missed here, which is a trade worth making.
bool PlausibleSeedPosition(double x, double y, double z) {
    if (!PlausiblePosition(x, y, z)) return false;
    if (std::fabs(x) < 2.0 && std::fabs(y) < 2.0 && std::fabs(z) < 2.0) return false;
    // Normalised vectors and colours cluster in [-1, 1]; a unit-length triple is
    // a direction, not a place.
    const double len = std::sqrt(x * x + y * y + z * z);
    if (len > 0.98 && len < 1.02) return false;
    return true;
}

int FloorDiv(double v, int by) {
    return (int)std::floor(v / (double)by);
}

// Wrap into (-180, 180], which is the range differences of angles want.
float NormalizeSigned(float d) {
    d = std::fmod(d + 180.0f, 360.0f);
    if (d < 0) d += 360.0f;
    return d - 180.0f;
}

// ReadSystemString adopts the vtable of the first string it successfully reads
// when none is known yet, which is exactly right for the chat hooks and exactly
// wrong here: this file hands it arbitrary pointers off the heap, and a bad
// guess would poison the vtable CreateSystemString uses to build outgoing chat.
// So we only ever read strings once chat has established the real one.
bool ReadStringIfSafe(uintptr_t p, std::string& out) {
    if (!g_stringVT.load()) return false;
    return ReadSystemString((void*)p, out);
}

// Same shape position.cpp's HUD-text reader parses, kept local so the two
// don't have to share an anonymous-namespace helper across translation units.
bool ParseFeetLine(const std::string& text, const char* marker,
    double& x, double& y, double& z) {
    const size_t at = text.find(marker);
    if (at == std::string::npos) return false;
    const char* p = text.c_str() + at + strlen(marker);
    return sscanf_s(p, "%lf , %lf , %lf", &x, &y, &z) == 3;
}

} // namespace

std::string GsdkPath::ToString() const {
    if (!found) return "(none)";
    std::string s = "root";
    char buf[32];
    for (uint32_t o : derefs) {
        snprintf(buf, sizeof(buf), "->+0x%X", o);
        s += buf;
    }
    snprintf(buf, sizeof(buf), " @+0x%X", finalOffset);
    s += buf;
    s += (type == GsdkValueType::Double) ? " (double)" : " (float)";
    return s;
}

// =============================================================================
// Frame hook
// =============================================================================
// Every candidate is a GameInstance method the client calls once per frame or
// tick, so `this` arrives in rcx. We hook whichever one we find first. The
// detour prototypes differ only so that float arguments in xmm1 survive the
// call - declaring a float parameter as an integer would let the compiler
// clobber xmm1 before the original ever sees it.
namespace {

struct FrameHookCandidate {
    const char* name;
    const char* sig;
    int         shape;   // 0: (this), 1: (this, float), 2: (this, float, int)
    // The function only adds its float argument to a running total and acts
    // once that total crosses a threshold. Skipping one call and adding its
    // time to the next call is then exactly the same as not skipping - which is
    // what lets a frame's call be given up to make one of ours instead.
    // Checked by reading the code on the build it matched, not assumed:
    //     addss xmm1, [rbx+24h] ; movss [rbx+24h], xmm1 ; ucomiss ... ; jb ret
    bool        accumulatesDt;
};

// Signatures published by the Hytale-Signatures project (Turkeii) and used by
// Hytale-Carbon. Ordered cheapest-to-verify first; any one of them is enough.
const FrameHookCandidate kFrameHooks[] = {
    { "GameInstance::TickAllModules(post)",
      "57 56 53 48 83 EC ? 33 C0 48 89 44 24 ? 0F 57 E4 0F 29 64 24 ? 0F 29 64 24 ? 0F 29 64 24 ? 48 8B D9 F3 0F 58 4B", 1, true },
    { "GameInstance::OnNewFrame",
      "57 56 55 53 48 83 EC ? 0F 29 74 24 ? 33 C0 48 89 44 24 ? 0F 57 E4 0F 29 64 24 ? 48 89 44 24 ? 48 8B D9 0F 28 F1 41 8B F0", 2, false },
    { "GameInstance::UpdateModules",
      "56 53 48 83 EC ? 0F 29 74 24 ? 48 8B D9 48 8B 8B ? ? ? ? F3 0F 10 8B", 1, false },
    { "GameInstance::PreUpdateModules",
      "56 53 48 83 EC ? F3 0F 11 4C 24 ? 48 8B D9 48 8B 8B", 1, false },
    { "GameInstance::BeginFrame",
      "57 56 53 48 83 EC ? 48 8B D9 48 8B 4B ? F2 0F 5A 51", 0, false },
    { "GameInstance::UpdateInterfaceData",
      "53 48 83 EC ? 48 8B D9 48 8B 4B ? 48 8B 49 ? 39 09 E8 ? ? ? ? 48 8B 8B", 0, false },
};

// The trampoline to the hooked function, whichever shape it is. The thunk
// passes every argument register through untouched, so one pointer will do.
std::atomic<uintptr_t> g_frameOrig{ 0 };
// Whether a frame's call may be given up for one of ours (accumulatesDt).
std::atomic<bool> g_frameCanStandIn{ false };
// The thread the frame hook runs on. Chat can only be sent from here when it
// is the same thread the game sends chat from.
std::atomic<DWORD> g_frameThread{ 0 };
// Time from calls given up, added to the next real one. Game thread only.
float g_carriedDt = 0.0f;

// Kept trivial on purpose: this runs on the render thread every frame, so it
// does two atomic stores and gets out of the way.
inline void NoteRoot(void* self) {
    if (g_shuttingDown.load(std::memory_order_relaxed)) return;
    const uintptr_t p = (uintptr_t)self;
    if (!p || (p & 7)) return;
    g_gsdkRoot.store(p, std::memory_order_relaxed);
    g_gsdkRootLastMs.store(NowSteadyMs(), std::memory_order_relaxed);
}

// The frame hook is the only code we have that reliably runs on the game's own
// thread, so anything that has to talk to the client rides along with it.
inline void FrameWork() {
    if (g_shuttingDown.load(std::memory_order_relaxed)) return;
    // First, so the auto farm's snapshot is taken as close to the game's own
    // input handling as possible. A few reads and a copy; nothing when idle.
    FarmOnGameFrame();
    DbgKnownTick();       // keeps the live shape list current, throttled internally
    DbgShapeFrameTick();  // refreshes shape lifetimes, and drives the auto-setup
    // Both of these draw into the one shape list, so they take turns rather
    // than fight: whichever is on screen is the one you are working with.
    if (g_selShow.load()) SelFrameTick();
    else                  BuildFrameTick();
    // WorldTickPlayer() and the whole world-reading path are gone: they fed on
    // hooks in the client's network stack, and those crashed the game on relog.
    DbgDrainQueue();      // runs the queued ops first, then the queued spawns
}

// Runs before every call of the hooked function, then the thunk jumps on to
// the function itself - or, on a frame where a bot message is due and the
// function can safely miss a call, to the game's SendMessage instead. The
// game's caller then makes that call for us, with nothing of ours under it,
// and the skipped call's time rides along on the next one. See hookthunk.hpp.
uintptr_t FramePre(HookRegs* r) {
    const uintptr_t orig = g_frameOrig.load(std::memory_order_relaxed);
    try {
        g_frameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        NoteRoot((void*)r->rcx);
        FrameWork();

        if (!g_frameCanStandIn.load(std::memory_order_relaxed)) {
            // Can't give a call up on this build: send the old way.
            if (GsdkFrameCarriesChatSends()) ProcessPending();
            return orig;
        }

        float dt = r->Xmm1Float() + g_carriedDt;
        if (!std::isfinite(dt)) dt = r->Xmm1Float();
        g_carriedDt = 0.0f;

        void* chat = nullptr;
        void* str = nullptr;
        if (!g_shuttingDown.load(std::memory_order_relaxed) && oSendMessage &&
            TakePendingSend(chat, str)) {
            g_carriedDt = dt;
            r->rcx = (uint64_t)(uintptr_t)chat;
            r->rdx = (uint64_t)(uintptr_t)str;
            return (uintptr_t)oSendMessage;
        }
        r->SetXmm1Float(dt);
    }
    catch (...) {}
    return orig;
}

} // namespace

bool GsdkFrameCarriesChatSends() {
    if (!g_gsdkHookInstalled.load() || !GsdkRootLive()) return false;
    const DWORD ft = g_frameThread.load(std::memory_order_relaxed);
    return ft && ft == g_gameThread.load();
}

bool GsdkInstallFrameHook() {
    if (g_gsdkHookInstalled.load()) return true;

    // Each attempt pattern-scans a ~50 MB image up to six times, and a
    // signature that is absent stays absent, so give the module a couple of
    // chances to finish loading and then stop burning CPU on it.
    static std::atomic<int> attempts{ 0 };
    if (attempts.fetch_add(1) >= 3) return false;

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);

    static void* const thunk = MakePreHookThunk(&FramePre);
    if (!thunk) {
        GsdkSetStatus("could not allocate the frame hook thunk");
        return false;
    }

    for (const auto& cand : kFrameHooks) {
        const uintptr_t addr = GameModulePatternScan(cand.sig);
        if (!addr) continue;

        // Set before the hook is live: the first call can arrive the instant
        // it is enabled.
        g_frameCanStandIn.store(cand.shape == 1 && cand.accumulatesDt);

        void* orig = nullptr;
        if (MH_CreateHook((void*)addr, thunk, &orig) != MH_OK) {
            LogToFile(std::string("gamesdk: MH_CreateHook failed for ") + cand.name);
            continue;
        }
        g_frameOrig.store((uintptr_t)orig);
        if (MH_EnableHook((void*)addr) != MH_OK) {
            LogToFile(std::string("gamesdk: MH_EnableHook failed for ") + cand.name);
            MH_RemoveHook((void*)addr);
            g_frameOrig.store(0);
            continue;
        }

        g_gsdkHookInstalled.store(true);
        g_gsdkHookRva.store(modBase ? (addr - modBase) : addr);
        { std::lock_guard lock(g_gsdkHookNameMtx); g_gsdkHookName = cand.name; }
        char buf[64];
        snprintf(buf, sizeof(buf), " at +0x%llX", (unsigned long long)(modBase ? addr - modBase : addr));
        GsdkSetStatus(std::string("frame hook installed: ") + cand.name + buf);
        return true;
    }

    GsdkSetStatus("no frame-hook signature matched this client build");
    return false;
}

bool GsdkRootLive() {
    if (!g_gsdkRoot.load()) return false;
    return (NowSteadyMs() - g_gsdkRootLastMs.load()) < kRootStaleMs;
}

// =============================================================================
// App fallback root
// =============================================================================
namespace {

// "MethodTable::App" from the Hytale-Signatures project. The match lands on a
// `lea rcx, [rip+disp32]` whose target is Application.App's MethodTable - the
// value NativeAOT passes to RhpNewFast when it allocates the App object.
const char* kSigAppMethodTable =
"48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8B D8 48 8B CB E8 ? ? ? ? 48 89 5D ? 48 8D 05 ? ? ? ? 48 83 78 ? ? 75";
constexpr int kSigAppMethodTableOffset = 3;   // where the disp32 starts

uintptr_t ResolveRipRelative(uintptr_t sigAddr, int dispOffset) {
    if (!sigAddr) return 0;
    int32_t disp = 0;
    if (!SafeRead((const void*)(sigAddr + dispOffset), &disp, 4)) return 0;
    return sigAddr + dispOffset + 4 + (intptr_t)disp;
}

bool RegionIsHeap(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    if (mbi.Protect & PAGE_NOACCESS) return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY;
}

bool SafeBulk(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Objects are of unknown length and some sit near the end of a committed
// region, so asking for the full window fails outright. Step down until a read
// succeeds and report how much we actually got - the first version treated a
// short object as unreadable and skipped it completely.
size_t SafeBulkBest(void* dst, const void* src, size_t maxLen) {
    for (size_t len = maxLen; len >= 64; len >>= 1) {
        if (SafeBulk(dst, src, len)) return len;
    }
    return 0;
}

// Scans committed read/write memory for an 8-aligned qword equal to `value`,
// returning the address of the slot. `accept` gets a veto so the caller can
// insist the surrounding object looks right.
uintptr_t ScanForQword(uint64_t value, bool (*accept)(uintptr_t)) {
    constexpr uintptr_t kLow = 0x10000;
    constexpr uintptr_t kHigh = 0x00007FFFFFFF0000ULL;
    constexpr size_t kChunk = 4 * 1024 * 1024;

    std::vector<uint8_t> buf(kChunk);
    uintptr_t cur = kLow;

    while (cur < kHigh && !g_shuttingDown.load()) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((const void*)cur, &mbi, sizeof(mbi))) break;
        const uintptr_t regionStart = (uintptr_t)mbi.BaseAddress;
        const uintptr_t regionEnd = regionStart + mbi.RegionSize;
        if (regionEnd <= cur) break;

        if (!RegionIsHeap(mbi)) { cur = regionEnd; continue; }

        uintptr_t at = (std::max)(cur, regionStart);
        at = (at + 7) & ~(uintptr_t)7;
        while (at + 8 <= regionEnd) {
            size_t want = (size_t)(std::min)((uintptr_t)kChunk, regionEnd - at);
            want &= ~(size_t)7;
            if (want < 8) break;
            if (!SafeBulk(buf.data(), (const void*)at, want)) break;

            for (size_t i = 0; i + 8 <= want; i += 8) {
                uint64_t v;
                memcpy(&v, buf.data() + i, 8);
                if (v != value) continue;
                const uintptr_t hit = at + i;
                if (!accept || accept(hit)) return hit;
            }
            at += want;
        }
        cur = regionEnd;
    }
    return 0;
}

// An App object we'd accept: the gameState int is one of the known enum values
// and the Interface / Core.Input / Settings slots hold plausible pointers. We
// deliberately do not require INGAME here - App exists on the main menu too.
bool AcceptApp(uintptr_t obj) {
    int32_t state = 0;
    if (!SafeRead((const void*)(obj + GsdkKnown::App_GameState), &state, 4)) return false;
    if (state < 0 || state > 6) return false;

    int plausible = 0;
    for (uint32_t off : { 0x10u, 0x30u, 0x48u, 0x100u }) {
        uintptr_t p = 0;
        if (ReadPtr(obj + off, p) && LooksLikeObject(p)) ++plausible;
    }
    return plausible >= 3;
}

} // namespace

void GsdkFindApp() {
    if (g_gsdkAppScanning.exchange(true)) return;
    WorkerGuard guard;
    const long long t0 = NowSteadyMs();

    uintptr_t mt = g_gsdkAppMT.load();
    if (!mt) {
        const uintptr_t sig = GameModulePatternScan(kSigAppMethodTable);
        if (!sig) {
            GsdkSetStatus("App MethodTable signature not found in this build");
            g_gsdkAppScanning.store(false);
            return;
        }
        mt = ResolveRipRelative(sig, kSigAppMethodTableOffset);
        if (!mt || !IsPlausiblePtr((const void*)mt)) {
            GsdkSetStatus("App MethodTable resolved to an implausible address");
            g_gsdkAppScanning.store(false);
            return;
        }
        g_gsdkAppMT.store(mt);
    }

    GsdkSetStatus("scanning for the App object...");
    const uintptr_t app = ScanForQword((uint64_t)mt, &AcceptApp);
    g_gsdkAppScanMs.store(NowSteadyMs() - t0);

    if (app) {
        g_gsdkApp.store(app);
        char buf[96];
        snprintf(buf, sizeof(buf), "App found at 0x%llX (%lld ms)",
            (unsigned long long)app, (long long)g_gsdkAppScanMs.load());
        GsdkSetStatus(buf);
    }
    else {
        GsdkSetStatus("App object not found - is the game past the launcher?");
    }
    g_gsdkAppScanning.store(false);
}

uintptr_t GsdkRootFromApp() {
    const uintptr_t app = g_gsdkApp.load();
    if (!app) return 0;

    int32_t state = 0;
    if (!SafeRead((const void*)(app + GsdkKnown::App_GameState), &state, 4)) return 0;
    if (state != GsdkKnown::App_GameState_InGame) return 0;

    uintptr_t inGame = 0, gi = 0;
    if (!ReadPtr(app + GsdkKnown::App_AppInGame, inGame) || !LooksLikeObject(inGame)) return 0;
    if (!ReadPtr(inGame + GsdkKnown::AppInGame_GameInstance, gi) || !LooksLikeObject(gi)) return 0;
    return gi;
}

uintptr_t GsdkCurrentRoot() {
    if (GsdkRootLive()) return g_gsdkRoot.load();
    return GsdkRootFromApp();
}

// =============================================================================
// Path resolution
// =============================================================================
uintptr_t GsdkResolvePath(const GsdkPath& path, uintptr_t root) {
    if (!path.found || !root) return 0;
    uintptr_t cur = root;
    for (uint32_t off : path.derefs) {
        uintptr_t next = 0;
        if (!ReadPtr(cur + off, next)) return 0;
        if (!LooksLikeObject(next)) return 0;
        cur = next;
    }
    return cur + path.finalOffset;
}

bool GsdkReadTriple(uintptr_t addr, GsdkValueType type, double& x, double& y, double& z) {
    if (!addr) return false;
    if (type == GsdkValueType::Double) {
        double v[3];
        if (!SafeRead((const void*)addr, v, sizeof(v))) return false;
        x = v[0]; y = v[1]; z = v[2];
    }
    else {
        float v[3];
        if (!SafeRead((const void*)addr, v, sizeof(v))) return false;
        x = v[0]; y = v[1]; z = v[2];
    }
    return PlausiblePosition(x, y, z);
}

// =============================================================================
// Discovery
// =============================================================================
bool GsdkTryKnownOffsets() {
    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return false;

    uintptr_t player = 0;
    if (!ReadPtr(root + GsdkKnown::GameInstance_PlayerEntity, player)) return false;
    if (!LooksLikeObject(player)) return false;

    // Two known layouts, tried in order. The published one first because it is
    // what other tools document, then the chain this build actually uses.
    //
    // Having only the published offset here is why the fallback could never
    // rescue anything: Entity+0x284 does not hold the position on this client,
    // so the check failed, so a dead learned path was never replaced.
    struct Layout { const char* what; bool viaTransform; };
    static const Layout kLayouts[] = {
        { "published Entity+0x284",            false },
        { "Entity+0x210 transform, pos +0x24", true  },
    };

    double x = 0, y = 0, z = 0;
    const Layout* using_ = nullptr;
    uintptr_t transform = 0;

    for (const Layout& L : kLayouts) {
        if (!L.viaTransform) {
            if (GsdkReadTriple(player + GsdkKnown::Entity_Position,
                GsdkValueType::Float, x, y, z)) {
                using_ = &L;
                break;
            }
            continue;
        }
        uintptr_t t = 0;
        if (!ReadPtr(player + GsdkKnown::Entity_Transform, t)) continue;
        if (!LooksLikeObject(t)) continue;
        if (!GsdkReadTriple(t + GsdkKnown::Transform_Position,
            GsdkValueType::Float, x, y, z)) continue;
        transform = t;
        using_ = &L;
        break;
    }
    if (!using_) return false;

    // A plausible triple on its own is not enough to save a path to disk: any
    // object with three floats in world range would pass, and if the offsets
    // have moved since they were published we would lock onto the wrong field
    // and show confident nonsense forever. So require the surrounding struct to
    // corroborate - these only line up when the whole layout still matches.
    int corroboration = 0;

    // GameInstance.playerNetworkId and PlayerEntity.networkId describe the same
    // entity, so if both offsets are still right the two ints are equal.
    int32_t giNetId = 0, entNetId = 0;
    if (SafeRead((const void*)(root + GsdkKnown::GameInstance_PlayerNetworkId), &giNetId, 4) &&
        SafeRead((const void*)(player + GsdkKnown::Entity_NetworkId), &entNetId, 4) &&
        giNetId > 0 && giNetId == entNetId) {
        corroboration += 2;   // decisive on its own
    }

    float fov = 0;
    if (SafeRead((const void*)(root + GsdkKnown::GameInstance_Fov), &fov, 4) &&
        std::isfinite(fov) && fov > 20.0f && fov < 179.0f) {
        ++corroboration;
    }

    float box[6];
    if (SafeRead((const void*)(player + GsdkKnown::Entity_BoundingBox), box, sizeof(box))) {
        bool ok = true;
        for (float v : box) if (!std::isfinite(v) || std::fabs(v) > 64.0f) ok = false;
        if (ok && box[3] > box[0] && box[4] > box[1] && box[5] > box[2]) ++corroboration;
    }

    if (corroboration < 2) {
        GsdkSetStatus("published offsets read a position-shaped value but the rest of "
            "the layout doesn't match - this build has moved on. Use \"Learn from the "
            "locked address\" or auto-find instead.");
        return false;
    }

    GsdkPath p;
    if (using_->viaTransform) {
        p.derefs = { GsdkKnown::GameInstance_PlayerEntity,
                     GsdkKnown::Entity_Transform };
        p.finalOffset = GsdkKnown::Transform_Position;
    }
    else {
        p.derefs = { GsdkKnown::GameInstance_PlayerEntity };
        p.finalOffset = GsdkKnown::Entity_Position;
    }
    p.type = GsdkValueType::Float;
    p.found = true;
    (void)transform;

    { std::lock_guard lock(g_gsdkPathMtx); g_gsdkPath = p; }
    GsdkPathSaveToDisk();
    GsdkSetStatus(std::string("known offsets matched this build (") + using_->what +
        ") - " + p.ToString());
    return true;
}

namespace {

struct WalkNode {
    uintptr_t addr;
    std::vector<uint32_t> path;
};

// Breadth-first walk of the object graph starting at `root`. `visit` is called
// once per distinct object with its path; returning false stops the walk.
// Deliberately bounded: this is a graph walk over a handful of game objects,
// not a heap scan, and it must never become one.
void WalkGraph(uintptr_t root, int maxDepth,
    const std::function<bool(uintptr_t, const std::vector<uint32_t>&)>& visit) {
    std::unordered_set<uintptr_t> seen;
    std::queue<WalkNode> q;
    q.push(WalkNode{ root, {} });
    seen.insert(root);
    size_t visited = 0;

    std::vector<uint8_t> buf(kChildWindow);

    while (!q.empty() && !g_shuttingDown.load()) {
        WalkNode node = q.front();
        q.pop();
        if (++visited > kMaxVisited) break;
        g_gsdkDiscoverVisited.store(visited);

        if (!visit(node.addr, node.path)) return;
        if ((int)node.path.size() >= maxDepth) continue;

        const size_t got = SafeBulkBest(buf.data(), (const void*)node.addr, kChildWindow);
        if (!got) continue;

        for (uint32_t off = 8; off + 8 <= got; off += 8) {
            uint64_t v;
            memcpy(&v, buf.data() + off, 8);
            const uintptr_t child = (uintptr_t)v;
            if (!LooksLikeObject(child) || seen.count(child)) continue;
            seen.insert(child);
            auto p = node.path; p.push_back(off);
            q.push(WalkNode{ child, std::move(p) });
        }
    }
}

} // namespace

void GsdkWalkGraph(uintptr_t root, int maxDepth,
    const std::function<bool(uintptr_t, const std::vector<uint32_t>&)>& visit) {
    WalkGraph(root, maxDepth, visit);
}

uintptr_t GsdkFollow(uintptr_t root, const std::vector<uint32_t>& derefs) {
    if (!root) return 0;
    uintptr_t cur = root;
    for (uint32_t off : derefs) {
        uintptr_t next = 0;
        if (!ReadPtr(cur + off, next) || !LooksLikeObject(next)) return 0;
        cur = next;
    }
    return cur;
}

void GsdkLearnFromAddress(uintptr_t lockedAddr) {
    if (g_gsdkDiscovering.exchange(true)) return;
    StartWorker([lockedAddr]() {
        WorkerGuard guard;
        g_gsdkDiscoverPhase.store(1);
        g_gsdkDiscoverVisited.store(0);

        const uintptr_t root = GsdkCurrentRoot();
        if (!root) {
            GsdkSetStatus("no live game root - join a world first");
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        // Collect every object that contains the address, then take the one
        // with the shortest path; among equals, the tightest final offset,
        // which is almost always the object the field actually belongs to.
        std::vector<GsdkPath> hits;
        WalkGraph(root, kMaxDepth, [&](uintptr_t obj, const std::vector<uint32_t>& path) {
            if (lockedAddr >= obj && (lockedAddr - obj) < kInsideWindow) {
                GsdkPath p;
                p.derefs = path;
                p.finalOffset = (uint32_t)(lockedAddr - obj);
                p.type = (g_posValueType.load() == (int)PosValueType::Double)
                    ? GsdkValueType::Double : GsdkValueType::Float;
                p.found = true;
                hits.push_back(std::move(p));
            }
            return true;
            });

        if (hits.empty()) {
            GsdkSetStatus("that address isn't reachable from the game root - "
                "it may belong to an object the walk doesn't cover");
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        std::sort(hits.begin(), hits.end(), [](const GsdkPath& a, const GsdkPath& b) {
            if (a.derefs.size() != b.derefs.size()) return a.derefs.size() < b.derefs.size();
            return a.finalOffset < b.finalOffset;
            });

        const GsdkPath& best = hits.front();

        // Prove it before adopting it: re-resolve from the root and check a
        // plausible position reads back through the chain.
        double x, y, z;
        const uintptr_t resolved = GsdkResolvePath(best, GsdkCurrentRoot());
        if (!resolved || !GsdkReadTriple(resolved, best.type, x, y, z)) {
            GsdkSetStatus("found a path but it didn't read back a valid position");
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        { std::lock_guard lock(g_gsdkPathMtx); g_gsdkPath = best; }
        GsdkPathSaveToDisk();
        GsdkSetStatus("learned " + best.ToString() + " (" +
            std::to_string(hits.size()) + " candidate object(s), shortest chosen)");
        g_gsdkDiscoverPhase.store(0);
        g_gsdkDiscovering.store(false);
        });
}

void GsdkAutoFind() {
    if (g_gsdkDiscovering.exchange(true)) return;
    StartWorker([]() {
        WorkerGuard guard;
        g_gsdkDiscoverPhase.store(1);
        g_gsdkDiscoverVisited.store(0);
        g_gsdkDiscoverCandidates.store(0);
        { std::lock_guard lock(g_gsdkCandMtx); g_gsdkCandidates.clear(); }

        const uintptr_t root = GsdkCurrentRoot();
        if (!root) {
            GsdkSetStatus("no live game root - join a world first");
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        GsdkSetStatus("walking the object graph...");

        // Phase 1 - every float and double triple in the graph that could be a
        // world position. Seeding uses the strict test: the loose one matches so
        // much idle vector noise that the budget runs out long before the walk
        // reaches anything interesting.
        std::vector<GsdkPath> cands;
        std::vector<uint8_t> buf(kFieldWindow);
        g_gsdkLastWalkTruncated.store(false);

        WalkGraph(root, kMaxDepth, [&](uintptr_t obj, const std::vector<uint32_t>& path) {
            // Over budget: keep walking so the visit count stays honest, but
            // stop collecting. Aborting here is what truncated the search.
            if (cands.size() >= kMaxAutoCandidates) {
                g_gsdkLastWalkTruncated.store(true);
                return true;
            }
            const size_t got = SafeBulkBest(buf.data(), (const void*)obj, kFieldWindow);
            if (!got) return true;

            for (uint32_t off = 8; off + 12 <= got; off += 4) {
                float f[3];
                memcpy(f, buf.data() + off, 12);
                if (PlausibleSeedPosition(f[0], f[1], f[2])) {
                    GsdkPath p;
                    p.derefs = path; p.finalOffset = off;
                    p.type = GsdkValueType::Float; p.found = true;
                    cands.push_back(std::move(p));
                }
                if ((off & 7) == 0 && off + 24 <= got) {
                    double d[3];
                    memcpy(d, buf.data() + off, 24);
                    if (PlausibleSeedPosition(d[0], d[1], d[2])) {
                        GsdkPath p;
                        p.derefs = path; p.finalOffset = off;
                        p.type = GsdkValueType::Double; p.found = true;
                        cands.push_back(std::move(p));
                    }
                }
                if (cands.size() >= kMaxAutoCandidates) break;
            }
            return true;
            });

        g_gsdkDiscoverCandidates.store(cands.size());
        if (cands.empty()) {
            GsdkSetStatus("no position-shaped values anywhere in the object graph");
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        // Phase 2 - sample while the player moves. A real position changes
        // continuously; velocity and scratch vectors either sit still, jump
        // around, or stop being readable.
        g_gsdkDiscoverPhase.store(2);
        GsdkSetStatus("walk around for ~8 seconds while I watch " +
            std::to_string(cands.size()) + " candidates...");

        struct Sample { double x, y, z, movement; int changes; bool alive; };
        std::vector<Sample> state(cands.size());
        for (size_t i = 0; i < cands.size(); ++i) {
            const uintptr_t a = GsdkResolvePath(cands[i], GsdkCurrentRoot());
            double x, y, z;
            state[i].alive = a && GsdkReadTriple(a, cands[i].type, x, y, z);
            state[i].x = x; state[i].y = y; state[i].z = z;
            state[i].movement = 0; state[i].changes = 0;
        }

        for (int pass = 0; pass < 32 && !g_shuttingDown.load(); ++pass) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const uintptr_t liveRoot = GsdkCurrentRoot();
            for (size_t i = 0; i < cands.size(); ++i) {
                if (!state[i].alive) continue;
                const uintptr_t a = GsdkResolvePath(cands[i], liveRoot);
                double x, y, z;
                if (!a || !GsdkReadTriple(a, cands[i].type, x, y, z)) {
                    state[i].alive = false;
                    continue;
                }
                const double dx = x - state[i].x, dy = y - state[i].y, dz = z - state[i].z;
                const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                // A teleport-sized jump in a quarter second is not the player
                // walking; it is some other vector reusing the slot.
                if (d > 60.0) { state[i].alive = false; continue; }
                if (d > 0.001) { state[i].changes++; state[i].movement += d; }
                state[i].x = x; state[i].y = y; state[i].z = z;
            }
        }

        std::vector<GsdkCandidate> survivors;
        for (size_t i = 0; i < cands.size(); ++i) {
            if (!state[i].alive) continue;
            if (state[i].changes < 4) continue;       // never moved with you
            if (state[i].movement < 1.0) continue;    // jitter, not walking
            GsdkCandidate c;
            c.path = cands[i];
            c.x = state[i].x; c.y = state[i].y; c.z = state[i].z;
            c.movement = state[i].movement;
            c.changes = state[i].changes;
            survivors.push_back(std::move(c));
        }

        std::sort(survivors.begin(), survivors.end(),
            [](const GsdkCandidate& a, const GsdkCandidate& b) {
                if (a.path.derefs.size() != b.path.derefs.size())
                    return a.path.derefs.size() < b.path.derefs.size();
                return a.movement > b.movement;
            });
        if (survivors.size() > 64) survivors.resize(64);

        { std::lock_guard lock(g_gsdkCandMtx); g_gsdkCandidates = survivors; }
        g_gsdkDiscoverCandidates.store(survivors.size());

        if (survivors.empty()) {
            GsdkSetStatus(g_gsdkLastWalkTruncated.load()
                ? "nothing tracked your movement, and the walk ran out of budget before "
                  "it covered everything - use \"Find from F7 coords\" instead"
                : "nothing tracked your movement - try again and keep walking "
                  "the whole time");
        }
        else if (survivors.size() == 1) {
            { std::lock_guard lock(g_gsdkPathMtx); g_gsdkPath = survivors[0].path; }
            GsdkPathSaveToDisk();
            GsdkSetStatus("auto-find locked on: " + survivors[0].path.ToString());
        }
        else {
            GsdkSetStatus(std::to_string(survivors.size()) +
                " candidates tracked your movement - pick the one whose numbers "
                "match your in-game position");
        }

        g_gsdkDiscoverPhase.store(0);
        g_gsdkDiscovering.store(false);
        });
}

// The exact search. Hytale's own F7 overlay prints your feet position, so the
// user can hand us the answer and we only have to find where it lives. Three
// independent floats matching to within a couple of units pins the field down
// on its own - no sampling pass, no walking around, no ambiguity.
void GsdkFindFromCoords(double wantX, double wantY, double wantZ, double tol) {
    if (g_gsdkDiscovering.exchange(true)) return;
    StartWorker([wantX, wantY, wantZ, tol]() {
        WorkerGuard guard;
        g_gsdkDiscoverPhase.store(1);
        g_gsdkDiscoverVisited.store(0);
        g_gsdkDiscoverCandidates.store(0);
        g_gsdkLastWalkTruncated.store(false);
        { std::lock_guard lock(g_gsdkCandMtx); g_gsdkCandidates.clear(); }

        const uintptr_t root = GsdkCurrentRoot();
        if (!root) {
            GsdkSetStatus("no live game root - join a world first");
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        GsdkSetStatus("looking for that exact position in the object graph...");

        // NB: `near` is a legacy Windows macro, hence the name.
        auto isNear = [tol](double a, double b) { return std::fabs(a - b) <= tol; };

        std::vector<GsdkCandidate> hits;
        std::vector<uint8_t> buf(kFieldWindow);
        WalkGraph(root, kMaxDepth, [&](uintptr_t obj, const std::vector<uint32_t>& path) {
            const size_t got = SafeBulkBest(buf.data(), (const void*)obj, kFieldWindow);
            if (!got) return true;

            for (uint32_t off = 8; off + 12 <= got; off += 4) {
                float f[3];
                memcpy(f, buf.data() + off, 12);
                if (isNear(f[0], wantX) && isNear(f[1], wantY) && isNear(f[2], wantZ)) {
                    GsdkCandidate c;
                    c.path.derefs = path; c.path.finalOffset = off;
                    c.path.type = GsdkValueType::Float; c.path.found = true;
                    c.x = f[0]; c.y = f[1]; c.z = f[2];
                    hits.push_back(std::move(c));
                }
                if ((off & 7) == 0 && off + 24 <= got) {
                    double d[3];
                    memcpy(d, buf.data() + off, 24);
                    if (isNear(d[0], wantX) && isNear(d[1], wantY) && isNear(d[2], wantZ)) {
                        GsdkCandidate c;
                        c.path.derefs = path; c.path.finalOffset = off;
                        c.path.type = GsdkValueType::Double; c.path.found = true;
                        c.x = d[0]; c.y = d[1]; c.z = d[2];
                        hits.push_back(std::move(c));
                    }
                }
            }
            if (hits.size() > 200) return false;
            return true;
            });

        if (hits.empty()) {
            GsdkSetStatus("no value anywhere in the graph matches those coordinates - "
                "re-read F7 without moving, widen the tolerance, and try again");
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        // Several hits usually means the same field seen through more than one
        // route to the object, plus any mirror the client keeps (render vs.
        // simulation position). Shortest path wins; the rest stay on the list.
        std::sort(hits.begin(), hits.end(),
            [](const GsdkCandidate& a, const GsdkCandidate& b) {
                if (a.path.derefs.size() != b.path.derefs.size())
                    return a.path.derefs.size() < b.path.derefs.size();
                return a.path.finalOffset < b.path.finalOffset;
            });
        if (hits.size() > 64) hits.resize(64);

        { std::lock_guard lock(g_gsdkPathMtx); g_gsdkPath = hits.front().path; }
        GsdkPathSaveToDisk();
        { std::lock_guard lock(g_gsdkCandMtx); g_gsdkCandidates = hits; }
        g_gsdkDiscoverCandidates.store(hits.size());

        GsdkSetStatus("found it: " + hits.front().path.ToString() +
            (hits.size() > 1
                ? "  (" + std::to_string(hits.size()) + " matches; shortest adopted - "
                  "if the numbers drift from F7, pick another below)"
                : ""));
        g_gsdkDiscoverPhase.store(0);
        g_gsdkDiscovering.store(false);
        });
}

void GsdkAdoptCandidate(const GsdkPath& path) {
    { std::lock_guard lock(g_gsdkPathMtx); g_gsdkPath = path; }
    GsdkPathSaveToDisk();
    GsdkSetStatus("using " + path.ToString());
}

void GsdkForget() {
    { std::lock_guard lock(g_gsdkPathMtx); g_gsdkPath = GsdkPath{}; }
    { std::lock_guard lock(g_gsdkCandMtx); g_gsdkCandidates.clear(); }
    GsdkSetStatus("path cleared");
}

// =============================================================================
// Per-frame publish
// =============================================================================
void GsdkFrameTick() {
    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return;

    // Refresh the candidate list first and unconditionally: the moment it is
    // most needed is when the adopted path is the wrong one, and that is
    // exactly when the publish below bails out.
    {
        std::lock_guard lock(g_gsdkCandMtx);
        for (auto& c : g_gsdkCandidates) {
            const uintptr_t a = GsdkResolvePath(c.path, root);
            double cx, cy, cz;
            if (a && GsdkReadTriple(a, c.path.type, cx, cy, cz)) {
                c.x = cx; c.y = cy; c.z = cz;
            }
        }
    }

    GsdkPath path;
    { std::lock_guard lock(g_gsdkPathMtx); path = g_gsdkPath; }
    if (!path.found) return;

    const uintptr_t addr = GsdkResolvePath(path, root);
    double x, y, z;
    if (!addr || !GsdkReadTriple(addr, path.type, x, y, z))
        return;

    g_posX.store(x);
    g_posY.store(y);
    g_posZ.store(z);
    g_posLockedAddr.store(addr);
    g_posLocked.store(true);
    // Keep position.cpp's idea of the value type in step, so that if the user
    // ever unlocks us and falls back to the manual reader it does not try to
    // read our float triple as doubles.
    g_posValueType.store(path.type == GsdkValueType::Double
        ? (int)PosValueType::Double : (int)PosValueType::Float);
    const long long now = NowSteadyMs();
    g_posLastUpdateMs.store(now);
    g_posLastGoodMs.store(now);
    g_gsdkPublishedMs.store(now);
}

bool GsdkOwnsPosition() {
    const long long last = g_gsdkPublishedMs.load();
    return last != 0 && (NowSteadyMs() - last) < 2000;
}

// =============================================================================
// Persistence
// =============================================================================
namespace {
std::wstring PathFilePath() {
    return GetThisModuleDirectory() + L"position_path.txt";
}
}

bool GsdkPathSaveToDisk() {
    GsdkPath p;
    { std::lock_guard lock(g_gsdkPathMtx); p = g_gsdkPath; }
    if (!p.found) return false;

    GsdkFacingPath fp;
    { std::lock_guard lock(g_gsdkFacingMtx); fp = g_gsdkFacing; }

    std::ofstream f(PathFilePath(), std::ios::trunc);
    if (!f) return false;
    f << "v2\n";
    f << "derefs=";
    for (size_t i = 0; i < p.derefs.size(); ++i) {
        if (i) f << ",";
        f << p.derefs[i];
    }
    f << "\n";
    f << "final=" << p.finalOffset << "\n";
    f << "type=" << (int)p.type << "\n";
    f << "autosetup=" << (g_gsdkAutoSetupEnabled.load() ? 1 : 0) << "\n";
    if (fp.found) {
        f << "fderefs=";
        for (size_t i = 0; i < fp.derefs.size(); ++i) {
            if (i) f << ",";
            f << fp.derefs[i];
        }
        f << "\n";
        f << "fyaw=" << fp.yawOffset << "\n";
        f << "fpitch=" << fp.pitchOffset << "\n";
        f << "fradians=" << (fp.radians ? 1 : 0) << "\n";
        f << "fnorth=" << fp.northOffsetDeg << "\n";
        f << "fsign=" << fp.yawSign << "\n";
        f << "feast=" << (fp.eastCalibrated ? 1 : 0) << "\n";
    }
    GsdkRidePath rp;
    { std::lock_guard lock(g_gsdkRideMtx); rp = g_gsdkRide; }
    if (rp.found) {
        auto list = [&](const std::vector<uint32_t>& v) {
            std::string o;
            for (size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + std::to_string(v[i]);
            return o;
        };
        f << "ranchor=" << rp.anchor << "\n";
        f << "rderefs=" << list(rp.derefs) << "\n";
        f << "roff=" << rp.offset << "\n";
        f << "rradians=" << (rp.radians ? 1 : 0) << "\n";
        f << "rentity=" << list(rp.entityDerefs) << "\n";
    }
    return true;
}

bool GsdkPathLoadFromDisk() {
    std::ifstream f(PathFilePath());
    if (!f) return false;

    auto parseList = [](const std::string& val, std::vector<uint32_t>& out) {
        size_t start = 0;
        while (start < val.size()) {
            const size_t comma = val.find(',', start);
            const std::string tok = val.substr(start, comma == std::string::npos
                ? std::string::npos : comma - start);
            if (!tok.empty()) out.push_back((uint32_t)strtoul(tok.c_str(), nullptr, 10));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        };

    GsdkPath p;
    GsdkFacingPath fp;
    GsdkRidePath rp;
    std::string line;
    bool sawVersion = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "v1" || line == "v2") { sawVersion = true; continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "derefs")        parseList(val, p.derefs);
        else if (key == "final")    p.finalOffset = (uint32_t)strtoul(val.c_str(), nullptr, 10);
        else if (key == "type")     p.type = (atoi(val.c_str()) == 1) ? GsdkValueType::Double
                                                                     : GsdkValueType::Float;
        else if (key == "autosetup") g_gsdkAutoSetupEnabled.store(atoi(val.c_str()) != 0);
        else if (key == "fderefs")  parseList(val, fp.derefs);
        else if (key == "fyaw")   { fp.yawOffset = (uint32_t)strtoul(val.c_str(), nullptr, 10);
                                    fp.found = true; }
        else if (key == "fpitch")   fp.pitchOffset = (uint32_t)strtoul(val.c_str(), nullptr, 10);
        else if (key == "fradians") fp.radians = atoi(val.c_str()) != 0;
        else if (key == "fnorth")   fp.northOffsetDeg = (float)atof(val.c_str());
        else if (key == "fsign")    fp.yawSign = (atoi(val.c_str()) < 0) ? -1 : 1;
        else if (key == "feast")    fp.eastCalibrated = atoi(val.c_str()) != 0;
        else if (key == "ranchor") { rp.anchor = atoi(val.c_str()); rp.found = true; }
        else if (key == "rderefs")  parseList(val, rp.derefs);
        else if (key == "roff")     rp.offset = (uint32_t)strtoul(val.c_str(), nullptr, 10);
        else if (key == "rradians") rp.radians = atoi(val.c_str()) != 0;
        else if (key == "rentity")  parseList(val, rp.entityDerefs);
    }
    if (!sawVersion) return false;
    p.found = true;

    { std::lock_guard lock(g_gsdkPathMtx); g_gsdkPath = p; }
    if (fp.found) { std::lock_guard lock(g_gsdkFacingMtx); g_gsdkFacing = fp; }
    if (rp.found) GsdkSetRidePath(rp);
    GsdkSetStatus("loaded saved path " + p.ToString() +
        (fp.found ? ", facing " + fp.ToString() : ""));
    return true;
}

// =============================================================================
// System.String, discovered by shape
// =============================================================================
// chat.hpp's g_stringVT is normally learned from a chat message, which means
// nothing that needs strings works until you have typed. A .NET string is
// recognisable without that: [MethodTable][int32 length][chars][NUL]. If a
// pointer's target passes that test, and several distinct objects share the
// same MethodTable, that MethodTable is System.String's.
uint64_t GsdkFindStringVT() {
    const uint64_t known = g_stringVT.load();
    if (known) return known;

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return 0;

    std::unordered_map<uint64_t, int> votes;
    uint64_t winner = 0;

    std::vector<uint8_t> buf(kChildWindow);
    WalkGraph(root, 2, [&](uintptr_t obj, const std::vector<uint32_t>&) {
        const size_t got = SafeBulkBest(buf.data(), (const void*)obj, kChildWindow);
        if (!got) return true;

        for (uint32_t off = 8; off + 8 <= got; off += 8) {
            uint64_t v;
            memcpy(&v, buf.data() + off, 8);
            const uintptr_t p = (uintptr_t)v;
            if (!LooksLikeObject(p)) continue;

            uint8_t head[16];
            if (!SafeRead((const void*)p, head, sizeof(head))) continue;
            uint64_t mt;
            int32_t len;
            memcpy(&mt, head, 8);
            memcpy(&len, head + 8, 4);
            if (len < 2 || len > 512) continue;

            uint16_t term = 1;
            if (!SafeRead((const void*)(p + 12 + (size_t)len * 2), &term, 2)) continue;
            if (term != 0) continue;

            // Characters must look like text, not like a coincidence.
            std::vector<wchar_t> chars((size_t)len);
            if (!SafeRead((const void*)(p + 12), chars.data(), (size_t)len * 2)) continue;
            int printable = 0;
            for (wchar_t c : chars) if (c >= 0x20 && c < 0x7F) ++printable;
            if (printable < (len * 3) / 4) continue;

            if (++votes[mt] >= 12) { winner = mt; return false; }
        }
        return true;
        });

    if (!winner) {
        // Nothing reached the confidence bar; take the clear leader if there is
        // one, rather than failing outright.
        int best = 0;
        for (const auto& kv : votes) {
            if (kv.second > best) { best = kv.second; winner = kv.first; }
        }
        if (best < 4) return 0;
    }

    g_stringVT.store(winner);
    char msg[80];
    snprintf(msg, sizeof(msg), "System.String vtable identified as %llX",
        (unsigned long long)winner);
    GsdkSetStatus(msg);
    return winner;
}

// =============================================================================
// Hytale's own F7 debug line
// =============================================================================
bool GsdkReadDebugLine(std::string& out) {
    const uint64_t vt = GsdkFindStringVT();
    if (!vt) return false;

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return false;

    bool found = false;
    std::vector<uint8_t> buf(kChildWindow);

    WalkGraph(root, kMaxDepth, [&](uintptr_t obj, const std::vector<uint32_t>&) {
        const size_t got = SafeBulkBest(buf.data(), (const void*)obj, kChildWindow);
        if (!got) return true;

        for (uint32_t off = 8; off + 8 <= got; off += 8) {
            uint64_t v;
            memcpy(&v, buf.data() + off, 8);
            const uintptr_t p = (uintptr_t)v;
            if (!LooksLikeObject(p)) continue;

            uint64_t thisVt = 0;
            if (!SafeRead((const void*)p, &thisVt, 8) || thisVt != vt) continue;

            std::string s;
            if (!ReadStringIfSafe(p, s)) continue;
            if (s.find("Feet (") == std::string::npos) continue;

            out = s;
            found = true;
            return false;
        }
        return true;
        });

    return found;
}

std::atomic<long long> g_gsdkAutoSetupLastTryMs{ 0 };
std::atomic<bool>      g_gsdkAutoSetupEnabled{ true };

void GsdkAutoSetupFromDebugLine() {
    if (g_gsdkDiscovering.exchange(true)) return;
    StartWorker([]() {
        WorkerGuard guard;
        g_gsdkDiscoverPhase.store(1);

        std::string line;
        if (!GsdkReadDebugLine(line)) {
            GsdkSetStatus("couldn't find Hytale's debug line - press F7 in-game so the "
                "overlay with \"Feet (x, y, z)\" is on screen, then try again");
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        double x = 0, y = 0, z = 0;
        if (!ParseFeetLine(line, "Feet (", x, y, z)) {
            GsdkSetStatus("found the debug line but couldn't parse it: " + line);
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        char msg[160];
        snprintf(msg, sizeof(msg), "read %.2f, %.2f, %.2f off the F7 overlay - searching...",
            x, y, z);
        GsdkSetStatus(msg);

        // Hand back the discovery flag; GsdkFindFromCoords takes it itself.
        g_gsdkDiscoverPhase.store(0);
        g_gsdkDiscovering.store(false);
        GsdkFindFromCoords(x, y, z, 1.5);
        });
}

void GsdkAutoSetupSetEnabled(bool enabled) {
    g_gsdkAutoSetupEnabled.store(enabled);
    GsdkPathSaveToDisk();
}

void GsdkAutoSetupTick() {
    if (!g_gsdkAutoSetupEnabled.load()) return;
    if (g_gsdkDiscovering.load()) return;

    // Already working? Nothing to do.
    if (GsdkOwnsPosition()) return;

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return;

    // A saved path that still reads a sane position just hasn't been published
    // yet this frame - leave it alone rather than re-running discovery over it.
    {
        GsdkPath p;
        { std::lock_guard lock(g_gsdkPathMtx); p = g_gsdkPath; }
        if (p.found) {
            const uintptr_t addr = GsdkResolvePath(p, root);
            double x, y, z;
            if (addr && GsdkReadTriple(addr, p.type, x, y, z)) return;
        }
    }

    const long long now = NowSteadyMs();
    // Slow retry: a graph walk is cheap but not free, and the usual reason for
    // failing is that F7's overlay is off, which only the user can fix.
    if (now - g_gsdkAutoSetupLastTryMs.load() < 8000) return;
    g_gsdkAutoSetupLastTryMs.store(now);

    GsdkAutoSetupFromDebugLine();
}

// =============================================================================
// Facing
// =============================================================================
std::mutex g_gsdkFacingMtx;
GsdkFacingPath g_gsdkFacing;

std::string GsdkFacingPath::ToString() const {
    if (!found) return "(none)";
    std::string s = "root";
    char buf[48];
    for (uint32_t o : derefs) {
        snprintf(buf, sizeof(buf), "->+0x%X", o);
        s += buf;
    }
    snprintf(buf, sizeof(buf), " yaw@+0x%X pitch@+0x%X%s",
        yawOffset, pitchOffset, radians ? " (radians)" : "");
    s += buf;
    return s;
}

namespace {

// The objects worth searching for rotation: the ones the position path already
// goes through, plus one level of their children. Rotation lives beside
// position in practice, and this keeps the sampling set small enough to read
// in full every 200ms.
std::vector<std::pair<uintptr_t, std::vector<uint32_t>>> FacingSearchObjects(uintptr_t root) {
    std::vector<std::pair<uintptr_t, std::vector<uint32_t>>> objs;
    std::unordered_set<uintptr_t> seen;

    auto add = [&](uintptr_t a, std::vector<uint32_t> p) {
        if (!a || seen.count(a)) return;
        seen.insert(a);
        objs.emplace_back(a, std::move(p));
    };

    GsdkPath path;
    { std::lock_guard lock(g_gsdkPathMtx); path = g_gsdkPath; }

    add(root, {});
    std::vector<uint32_t> sofar;
    uintptr_t cur = root;
    for (uint32_t off : path.derefs) {
        uintptr_t next = 0;
        if (!ReadPtr(cur + off, next) || !LooksLikeObject(next)) break;
        sofar.push_back(off);
        add(next, sofar);
        cur = next;
    }

    // One level of children off each of those.
    const size_t seeds = objs.size();
    std::vector<uint8_t> buf(kChildWindow);
    for (size_t i = 0; i < seeds; ++i) {
        const uintptr_t obj = objs[i].first;
        const std::vector<uint32_t> base = objs[i].second;
        const size_t got = SafeBulkBest(buf.data(), (const void*)obj, kChildWindow);
        if (!got) continue;
        for (uint32_t off = 8; off + 8 <= got; off += 8) {
            uint64_t v;
            memcpy(&v, buf.data() + off, 8);
            const uintptr_t child = (uintptr_t)v;
            if (!LooksLikeObject(child)) continue;
            auto p = base; p.push_back(off);
            add(child, std::move(p));
            if (objs.size() > 400) return objs;
        }
    }
    return objs;
}

} // namespace

std::mutex g_gsdkFacingCandMtx;
std::vector<GsdkFacingCandidate> g_gsdkFacingCandidates;
std::mutex g_gsdkFacingPromptMtx;
std::string g_gsdkFacingPrompt;

static void SetFacingPrompt(const std::string& s) {
    std::lock_guard lock(g_gsdkFacingPromptMtx);
    g_gsdkFacingPrompt = s;
}
std::string GsdkGetFacingPrompt() {
    std::lock_guard lock(g_gsdkFacingPromptMtx);
    return g_gsdkFacingPrompt;
}

namespace {

// Per-candidate float slot, tracked across the whole run.
struct AngleStat {
    float first = 0;
    float minV = 0, maxV = 0;
    float last = 0;
    float maxAbs = 0;
    int   changes = 0;
    float maxStep = 0;      // biggest jump between consecutive samples
    bool  alive = true;
    bool  movedWhileStill = false;
};

constexpr float kAngleSane = 400.0f;      // degrees; anything beyond is not an angle
constexpr float kStillEps = 0.0005f;      // what counts as "did not move"

// Samples every float in the search set once and folds it into `stats`.
// `seeding` creates entries; later passes only update existing ones, and a
// single bad read kills a candidate outright - that is the fix for the first
// version, which skipped out-of-range samples and so let noise slots survive.
void FacingSample(const std::vector<std::pair<uintptr_t, std::vector<uint32_t>>>& objs,
    std::unordered_map<uint64_t, AngleStat>& stats,
    std::vector<uint8_t>& buf,
    bool seeding, bool mustHoldStill) {
    for (size_t oi = 0; oi < objs.size(); ++oi) {
        const size_t got = SafeBulkBest(buf.data(), (const void*)objs[oi].first, kFieldWindow);
        if (!got) continue;
        for (uint32_t off = 8; off + 4 <= got; off += 4) {
            const uint64_t key = ((uint64_t)oi << 32) | off;
            float v;
            memcpy(&v, buf.data() + off, 4);

            auto it = stats.find(key);
            if (it == stats.end()) {
                if (!seeding) continue;
                if (!std::isfinite(v) || std::fabs(v) > kAngleSane) continue;
                AngleStat st;
                st.first = st.minV = st.maxV = st.last = v;
                st.maxAbs = std::fabs(v);
                stats.emplace(key, st);
                continue;
            }

            AngleStat& st = it->second;
            if (!st.alive) continue;

            // Out of range even once means this was never an angle.
            if (!std::isfinite(v) || std::fabs(v) > kAngleSane) {
                st.alive = false;
                continue;
            }

            const float step = std::fabs(v - st.last);
            if (mustHoldStill) {
                // The player is not touching the mouse, so a real angle is
                // frozen. Anything drifting here is a counter, a timer or
                // reused memory - exactly the "spazzing" values.
                if (step > kStillEps) {
                    st.movedWhileStill = true;
                    st.alive = false;
                    continue;
                }
            }
            else {
                if (step > st.maxStep) st.maxStep = step;
                if (step > kStillEps) st.changes++;
            }

            if (v < st.minV) st.minV = v;
            if (v > st.maxV) st.maxV = v;
            if (std::fabs(v) > st.maxAbs) st.maxAbs = std::fabs(v);
            st.last = v;
        }
    }
}

} // namespace

void GsdkFindFacing() {
    if (g_gsdkDiscovering.exchange(true)) return;
    StartWorker([]() {
        WorkerGuard guard;
        g_gsdkDiscoverPhase.store(2);
        { std::lock_guard lock(g_gsdkFacingCandMtx); g_gsdkFacingCandidates.clear(); }

        const uintptr_t root = GsdkCurrentRoot();
        if (!root) {
            GsdkSetStatus("no live game root - join a world first");
            SetFacingPrompt("");
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        const auto objs = FacingSearchObjects(root);
        if (objs.empty()) {
            GsdkSetStatus("nothing to search - set up the position path first");
            SetFacingPrompt("");
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        std::unordered_map<uint64_t, AngleStat> stats;
        std::vector<uint8_t> buf(kFieldWindow);

        auto stillPass = [&](const char* label, int passes, bool seeding) {
            SetFacingPrompt(label);
            GsdkSetStatus(label);
            FacingSample(objs, stats, buf, seeding, false);   // prime `last`
            for (int i = 0; i < passes && !g_shuttingDown.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                FacingSample(objs, stats, buf, false, true);
            }
            };

        // --- phase 1: hands off the mouse. Everything that moves now is noise.
        stillPass("HOLD STILL - don't touch the mouse (3s)", 15, true);

        // --- phase 2: look around. Everything that does NOT move now is not an angle.
        SetFacingPrompt("NOW LOOK AROUND - spin a full circle, look right up and right down (8s)");
        GsdkSetStatus("now look around - spin a full circle and look up and down");
        for (int i = 0; i < 40 && !g_shuttingDown.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            FacingSample(objs, stats, buf, false, false);
        }

        // --- phase 3: hands off again. Catches slots that are merely busy.
        stillPass("HOLD STILL again (2s)", 10, false);
        SetFacingPrompt("");

        // ---- classify ----
        struct Surv { size_t obj; uint32_t off; const AngleStat* st; float range; bool radians; };
        std::vector<Surv> surv;
        for (const auto& kv : stats) {
            const AngleStat& st = kv.second;
            if (!st.alive || st.movedWhileStill) continue;
            if (st.changes < 8) continue;               // never followed your view
            const float range = st.maxV - st.minV;
            const bool radians = st.maxAbs <= 6.6f;
            if (range < (radians ? 0.6f : 25.0f)) continue;
            surv.push_back(Surv{ (size_t)(kv.first >> 32), (uint32_t)(kv.first & 0xFFFFFFFF),
                                 &st, range, radians });
        }

        if (surv.empty()) {
            GsdkSetStatus("nothing behaved like an angle: it has to sit perfectly still "
                "while you hold the mouse, then move while you look around. Run it again "
                "and make sure you really do keep still for the first and last few seconds.");
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        // Pair them up. Pitch is the one clamped to +/-90 (or +/-pi/2); yaw is
        // the wider swing. They live in the same struct in every engine I have
        // seen, so pairs in the same object and close together score highest.
        std::vector<GsdkFacingCandidate> cands;
        for (const Surv& p : surv) {
            const float pitchLimit = p.radians ? 1.60f : 91.0f;
            if (p.st->maxAbs > pitchLimit) continue;               // not a pitch
            if (p.range < (p.radians ? 0.5f : 25.0f)) continue;

            for (const Surv& y : surv) {
                if (y.obj != p.obj || y.off == p.off) continue;
                if (y.radians != p.radians) continue;
                if (y.range < (y.radians ? 1.2f : 70.0f)) continue; // yaw must swing wide
                const uint32_t gap = (y.off > p.off) ? (y.off - p.off) : (p.off - y.off);
                if (gap > 64) continue;                             // same little struct

                GsdkFacingCandidate c;
                c.path.derefs = objs[p.obj].second;
                c.path.yawOffset = y.off;
                c.path.pitchOffset = p.off;
                c.path.radians = p.radians;
                c.path.found = true;
                c.yawRange = y.range;
                c.pitchRange = p.range;
                c.gap = gap;
                cands.push_back(std::move(c));
            }
        }

        if (cands.empty()) {
            // No tidy pair; fall back to the two best singles so the user at
            // least gets a live yaw, and say so rather than pretending.
            const Surv* bestYaw = nullptr;
            const Surv* bestPitch = nullptr;
            for (const Surv& s : surv) {
                const float pitchLimit = s.radians ? 1.60f : 91.0f;
                if (s.st->maxAbs <= pitchLimit && (!bestPitch || s.range > bestPitch->range))
                    bestPitch = &s;
                if (!bestYaw || s.range > bestYaw->range) bestYaw = &s;
            }
            if (bestYaw && bestPitch && bestYaw->off != bestPitch->off) {
                GsdkFacingCandidate c;
                c.path.derefs = objs[bestPitch->obj].second;
                c.path.yawOffset = bestYaw->off;
                c.path.pitchOffset = bestPitch->off;
                c.path.radians = bestPitch->radians;
                c.path.found = true;
                c.yawRange = bestYaw->range;
                c.pitchRange = bestPitch->range;
                c.gap = 0xFFFF;
                cands.push_back(std::move(c));
            }
        }

        if (cands.empty()) {
            GsdkSetStatus("found values that follow your view but couldn't tell yaw from "
                "pitch - try again and be sure to look all the way up and all the way down");
            g_gsdkDiscoverPhase.store(0);
            g_gsdkDiscovering.store(false);
            return;
        }

        std::sort(cands.begin(), cands.end(),
            [](const GsdkFacingCandidate& a, const GsdkFacingCandidate& b) {
                if (a.gap != b.gap) return a.gap < b.gap;                 // adjacent wins
                if (a.path.derefs.size() != b.path.derefs.size())
                    return a.path.derefs.size() < b.path.derefs.size();
                return a.yawRange > b.yawRange;
            });
        if (cands.size() > 24) cands.resize(24);

        {
            std::lock_guard lock(g_gsdkFacingMtx);
            // A re-find replaces the offsets but the calibration describes the
            // client's convention, not the fields, so it is worth keeping.
            const float keepNorth = g_gsdkFacing.northOffsetDeg;
            const int keepSign = g_gsdkFacing.yawSign;
            const bool keepEast = g_gsdkFacing.eastCalibrated;
            g_gsdkFacing = cands.front().path;
            g_gsdkFacing.northOffsetDeg = keepNorth;
            g_gsdkFacing.yawSign = keepSign;
            g_gsdkFacing.eastCalibrated = keepEast;
        }
        { std::lock_guard lock(g_gsdkFacingCandMtx); g_gsdkFacingCandidates = cands; }
        GsdkPathSaveToDisk();

        GsdkSetStatus("facing found: " + cands.front().path.ToString() +
            (cands.size() > 1
                ? "  (" + std::to_string(cands.size()) + " candidates; pick another below "
                  "if this one doesn't track you)"
                : "") +
            "  - now face north in-game and hit \"Calibrate north\"");

        g_gsdkDiscoverPhase.store(0);
        g_gsdkDiscovering.store(false);
        });
}

void GsdkAdoptFacing(const GsdkFacingPath& path) {
    {
        std::lock_guard lock(g_gsdkFacingMtx);
        const float keepNorth = g_gsdkFacing.northOffsetDeg;
        const int keepSign = g_gsdkFacing.yawSign;
        const bool keepEast = g_gsdkFacing.eastCalibrated;
        g_gsdkFacing = path;
        g_gsdkFacing.northOffsetDeg = keepNorth;
        g_gsdkFacing.yawSign = keepSign;
        g_gsdkFacing.eastCalibrated = keepEast;
    }
    GsdkPathSaveToDisk();
    GsdkSetStatus("using " + path.ToString());
}
bool GsdkReadFacing(float& yawDeg, float& pitchDeg) {
    GsdkFacingPath fp;
    { std::lock_guard lock(g_gsdkFacingMtx); fp = g_gsdkFacing; }
    return GsdkReadFacingPath(fp, yawDeg, pitchDeg);
}

bool GsdkReadFacingPath(const GsdkFacingPath& fp, float& yawDeg, float& pitchDeg) {
    if (!fp.found) return false;

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return false;

    uintptr_t obj = root;
    for (uint32_t off : fp.derefs) {
        uintptr_t next = 0;
        if (!ReadPtr(obj + off, next) || !LooksLikeObject(next)) return false;
        obj = next;
    }

    float rawYaw = 0, rawPitch = 0;
    if (!SafeRead((const void*)(obj + fp.yawOffset), &rawYaw, 4)) return false;
    if (!SafeRead((const void*)(obj + fp.pitchOffset), &rawPitch, 4)) return false;
    if (!std::isfinite(rawYaw) || !std::isfinite(rawPitch)) return false;

    if (fp.radians) {
        rawYaw *= 57.2957795f;
        rawPitch *= 57.2957795f;
    }
    // A field that has stopped being an angle (freed object, wrong guess, a
    // patch moving things) reads as a huge float. Refuse it here so the rest
    // of the UI simply says "not found" instead of printing 1e33 degrees.
    if (std::fabs(rawYaw) > 720.0f || std::fabs(rawPitch) > 720.0f) return false;

    yawDeg = rawYaw;
    pitchDeg = rawPitch;
    return true;
}

float GsdkYawToCompass(float rawYawDeg) {
    GsdkFacingPath fp;
    { std::lock_guard lock(g_gsdkFacingMtx); fp = g_gsdkFacing; }
    float c = (rawYawDeg + fp.northOffsetDeg) * (float)fp.yawSign;
    c = std::fmod(c, 360.0f);
    if (c < 0) c += 360.0f;
    return c;
}

void GsdkCalibrateNorth() {
    float yaw = 0, pitch = 0;
    if (!GsdkReadFacing(yaw, pitch)) {
        GsdkSetStatus("can't calibrate - facing isn't resolving right now");
        return;
    }
    {
        std::lock_guard lock(g_gsdkFacingMtx);
        // The bearing is (raw yaw + this offset) * sign, so negating the
        // current yaw makes "facing right now" read as 0, which is north.
        g_gsdkFacing.northOffsetDeg = -yaw;
    }
    GsdkPathSaveToDisk();
    char msg[160];
    snprintf(msg, sizeof(msg),
        "north set (raw yaw %.1f is now 0). Now face EAST and hit \"Set east\" so it "
        "knows which way the numbers run.", yaw);
    GsdkSetStatus(msg);
}

// Facing east must read +90 on a clockwise compass. If it comes out negative,
// this client counts yaw the other way round, and every bearing needs flipping
// - which is the whole of the "north reads as south" family of bugs.
void GsdkCalibrateEast() {
    float yaw = 0, pitch = 0;
    if (!GsdkReadFacing(yaw, pitch)) {
        GsdkSetStatus("can't calibrate - facing isn't resolving right now");
        return;
    }

    float delta;
    {
        std::lock_guard lock(g_gsdkFacingMtx);
        delta = NormalizeSigned(yaw + g_gsdkFacing.northOffsetDeg);
        g_gsdkFacing.yawSign = (delta >= 0.0f) ? 1 : -1;
        g_gsdkFacing.eastCalibrated = true;
    }
    GsdkPathSaveToDisk();

    char msg[160];
    snprintf(msg, sizeof(msg),
        "east set: that direction is %+.0f from north, so yaw counts %s. Compass is "
        "now calibrated.", delta, (delta >= 0.0f) ? "clockwise" : "anti-clockwise");
    GsdkSetStatus(msg);
    if (std::fabs(std::fabs(delta) - 90.0f) > 35.0f) {
        GsdkSetStatus(std::string(msg) +
            "  (that is a long way from 90 though - were you really facing east?)");
    }
}

void GsdkFlipYawDirection() {
    int sign;
    {
        std::lock_guard lock(g_gsdkFacingMtx);
        g_gsdkFacing.yawSign = -g_gsdkFacing.yawSign;
        // Seeing it and correcting it is as good a calibration as standing in a
        // known direction, so stop nagging about the second point.
        g_gsdkFacing.eastCalibrated = true;
        sign = g_gsdkFacing.yawSign;
    }
    GsdkPathSaveToDisk();
    GsdkSetStatus(std::string("yaw now counts ") +
        (sign > 0 ? "clockwise" : "anti-clockwise") + " - east and west are swapped over");
}

void GsdkForgetFacing() {
    { std::lock_guard lock(g_gsdkFacingMtx); g_gsdkFacing = GsdkFacingPath{}; }
    GsdkPathSaveToDisk();
    GsdkSetStatus("facing cleared");
}

// =============================================================================
// Player details
// =============================================================================
namespace {

// Takes an already-calibrated bearing, where 0 is north and the numbers run
// clockwise. The axis annotations the first version carried ("+Z", "-X") were
// guesses about Hytale's world axes that nothing here verifies, so they are
// gone rather than wrong.
const char* FacingName(float compassDeg) {
    float y = std::fmod(compassDeg, 360.0f);
    if (y < 0) y += 360.0f;
    static const char* names[] = {
        "North", "North-East", "East", "South-East",
        "South", "South-West", "West", "North-West"
    };
    const int idx = (int)std::floor((y + 22.5f) / 45.0f) & 7;
    return names[idx];
}


} // namespace

GsdkPlayerInfo GsdkGetPlayerInfo() {
    GsdkPlayerInfo info;

    GsdkPath path;
    { std::lock_guard lock(g_gsdkPathMtx); path = g_gsdkPath; }

    const uintptr_t root = GsdkCurrentRoot();
    if (!root || !path.found) return info;
    info.gameInstance = root;

    // The object the position lives in is the last link of the chain; walk to
    // it so the other entity fields can be read relative to the same base.
    uintptr_t obj = root;
    for (uint32_t off : path.derefs) {
        uintptr_t next = 0;
        if (!ReadPtr(obj + off, next) || !LooksLikeObject(next)) return info;
        obj = next;
    }
    info.entity = obj;

    if (!GsdkReadTriple(obj + path.finalOffset, path.type, info.x, info.y, info.z))
        return info;
    info.valid = true;

    info.chunkX = FloorDiv(info.x, 16);
    info.chunkY = FloorDiv(info.y, 16);
    info.chunkZ = FloorDiv(info.z, 16);

    // The remaining fields are best-effort: they use the published offsets and
    // are simply omitted when they don't read back as something sensible, so a
    // client patch degrades this to "coordinates only" instead of garbage.
    float rawYaw = 0, rawPitch = 0;
    if (GsdkReadFacing(rawYaw, rawPitch)) {
        float northOffset = 0.0f;
        { std::lock_guard lock(g_gsdkFacingMtx); northOffset = g_gsdkFacing.northOffsetDeg; }
        (void)northOffset;
        info.haveRotation = true;
        info.rawYaw = rawYaw;
        info.pitch = rawPitch;
        info.yaw = GsdkYawToCompass(rawYaw);
        info.facing = FacingName(info.yaw);
    }

    int32_t netId = 0;
    if (SafeRead((const void*)(obj + GsdkKnown::Entity_NetworkId), &netId, 4)) {
        if (netId > 0 && netId < 1000000) {
            info.haveNetworkId = true;
            info.networkId = netId;
        }
    }

    float fov = 0;
    if (SafeRead((const void*)(root + GsdkKnown::GameInstance_Fov), &fov, 4)) {
        if (std::isfinite(fov) && fov > 20.0f && fov < 179.0f) {
            info.haveFov = true;
            info.fov = fov;
        }
    }

    float box[6];
    if (SafeRead((const void*)(obj + GsdkKnown::Entity_BoundingBox), box, sizeof(box))) {
        bool ok = true;
        for (float v : box) if (!std::isfinite(v) || std::fabs(v) > 64.0f) ok = false;
        if (ok && (box[3] > box[0] || box[4] > box[1] || box[5] > box[2])) {
            info.haveBox = true;
            info.boxMinX = box[0]; info.boxMinY = box[1]; info.boxMinZ = box[2];
            info.boxMaxX = box[3]; info.boxMaxY = box[4]; info.boxMaxZ = box[5];
        }
    }

    uintptr_t nameObj = 0;
    if (ReadPtr(obj + GsdkKnown::Entity_PlayerName, nameObj) && LooksLikeObject(nameObj)) {
        std::string s;
        if (ReadStringIfSafe(nameObj, s) && s.size() < 64)
            info.playerName = s;
    }

    return info;
}

// =============================================================================
// Raw object inspector
// =============================================================================
std::vector<GsdkSlot> GsdkDumpObject(uintptr_t obj, uint32_t startOffset, uint32_t count) {
    std::vector<GsdkSlot> out;
    if (!obj || !count) return out;
    if (count > 4096) count = 4096;
    out.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        GsdkSlot slot;
        slot.offset = startOffset + i * 8;

        uint64_t raw = 0;
        if (!SafeRead((const void*)(obj + slot.offset), &raw, 8)) {
            out.push_back(slot);
            continue;
        }
        slot.raw = raw;
        memcpy(&slot.f0, (const uint8_t*)&raw + 0, 4);
        memcpy(&slot.f1, (const uint8_t*)&raw + 4, 4);
        memcpy(&slot.i0, (const uint8_t*)&raw + 0, 4);
        memcpy(&slot.i1, (const uint8_t*)&raw + 4, 4);
        memcpy(&slot.d, &raw, 8);

        const uintptr_t p = (uintptr_t)raw;
        if (LooksLikeObject(p)) {
            slot.isPointer = true;
            uintptr_t mt = 0;
            ReadPtr(p, mt);
            slot.targetMT = mt;
            std::string s;
            if (ReadStringIfSafe(p, s) && s.size() < 200)
                slot.text = s;
        }
        out.push_back(slot);
    }
    return out;
}

// =============================================================================
// Per-frame readers
// =============================================================================
bool GsdkReadPositionFast(float& x, float& y, float& z) {
    thread_local GsdkPath path;
    thread_local long long pathAt = -1000000;
    const long long now = NowSteadyMs();
    if (now - pathAt > 500) {
        std::lock_guard lock(g_gsdkPathMtx);
        path = g_gsdkPath;
        pathAt = now;
    }
    if (!path.found) return false;
    const uintptr_t addr = GsdkResolvePath(path, GsdkCurrentRoot());
    double dx, dy, dz;
    if (!addr || !GsdkReadTriple(addr, path.type, dx, dy, dz)) return false;
    x = (float)dx; y = (float)dy; z = (float)dz;
    return true;
}

bool GsdkReadCompassFast(float& compassDeg) {
    thread_local GsdkFacingPath fp;
    thread_local long long fpAt = -1000000;
    const long long now = NowSteadyMs();
    if (now - fpAt > 500) {
        std::lock_guard lock(g_gsdkFacingMtx);
        fp = g_gsdkFacing;
        fpAt = now;
    }
    float yaw = 0, pitch = 0;
    if (!GsdkReadFacingPath(fp, yaw, pitch)) return false;
    float c = (yaw + fp.northOffsetDeg) * (float)fp.yawSign;
    c = std::fmod(c, 360.0f);
    if (c < 0) c += 360.0f;
    compassDeg = c;
    return true;
}

// =============================================================================
// The tractor's own facing
// =============================================================================
std::mutex g_gsdkRideMtx;
GsdkRidePath g_gsdkRide;
std::mutex g_gsdkRideCandMtx;
std::vector<GsdkRideCandidate> g_gsdkRideCandidates;
std::atomic<bool> g_gsdkRideFinding{ false };
std::atomic<int> g_gsdkRideStep{ 0 };
std::atomic<long long> g_gsdkRideStepEndMs{ 0 };

namespace {

std::mutex g_rideStatusMtx;
std::string g_rideStatus;
std::atomic<bool> g_rideCancel{ false };
std::atomic<uintptr_t> g_rideEntity{ 0 };       // the entity being ridden (anchor 1)
std::atomic<uintptr_t> g_rideEntityMT{ 0 };     // its class, i.e. the player's
std::atomic<bool> g_rideScanning{ false };
std::atomic<long long> g_rideLastScanMs{ 0 };
std::atomic<unsigned> g_rideVersion{ 1 };       // bumped whenever g_gsdkRide changes

void SetRideStatus(const std::string& s) {
    { std::lock_guard lock(g_rideStatusMtx); g_rideStatus = s; }
    LogToFile("ride facing: " + s);
}

// The player entity is the first link of the position path; any entity of
// the same class keeps its position down the rest of that path.
uintptr_t PlayerEntity(uintptr_t root, const GsdkPath& path) {
    if (!root || !path.found || path.derefs.empty()) return 0;
    uintptr_t e = 0;
    if (!ReadPtr(root + path.derefs[0], e) || !LooksLikeObject(e)) return 0;
    return e;
}

bool EntityPosition(uintptr_t e, const GsdkPath& path, float& x, float& y, float& z) {
    if (!e || path.derefs.empty()) return false;
    uintptr_t cur = e;
    for (size_t i = 1; i < path.derefs.size(); ++i) {
        uintptr_t n = 0;
        if (!ReadPtr(cur + path.derefs[i], n) || !LooksLikeObject(n)) return false;
        cur = n;
    }
    double dx, dy, dz;
    if (!GsdkReadTriple(cur + path.finalOffset, path.type, dx, dy, dz)) return false;
    x = (float)dx; y = (float)dy; z = (float)dz;
    return true;
}

// Every object of the player's class within `radius` of the player, nearest
// first, the player left out. One pass over the heap - seconds, not frames.
std::vector<std::pair<float, uintptr_t>> NearbyEntities(float radius, size_t maxOut) {
    std::vector<std::pair<float, uintptr_t>> out;
    GsdkPath path;
    { std::lock_guard lock(g_gsdkPathMtx); path = g_gsdkPath; }
    const uintptr_t player = PlayerEntity(GsdkCurrentRoot(), path);
    uintptr_t mt = 0;
    float px, py, pz;
    if (!player || !ReadPtr(player, mt) || !mt || !EntityPosition(player, path, px, py, pz)) return out;
    g_rideEntityMT.store(mt);

    constexpr uintptr_t kLow = 0x10000;
    constexpr uintptr_t kHigh = 0x00007FFFFFFF0000ULL;
    constexpr size_t kChunk = 4 * 1024 * 1024;
    std::vector<uint8_t> buf(kChunk);
    uintptr_t cur = kLow;
    while (cur < kHigh && !g_shuttingDown.load()) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((const void*)cur, &mbi, sizeof(mbi))) break;
        const uintptr_t regionStart = (uintptr_t)mbi.BaseAddress;
        const uintptr_t regionEnd = regionStart + mbi.RegionSize;
        if (regionEnd <= cur) break;
        if (!RegionIsHeap(mbi)) { cur = regionEnd; continue; }
        uintptr_t at = ((std::max)(cur, regionStart) + 7) & ~(uintptr_t)7;
        while (at + 8 <= regionEnd) {
            size_t want = (size_t)(std::min)((uintptr_t)kChunk, regionEnd - at) & ~(size_t)7;
            if (want < 8 || !SafeBulk(buf.data(), (const void*)at, want)) break;
            for (size_t i = 0; i + 8 <= want; i += 8) {
                uint64_t v;
                memcpy(&v, buf.data() + i, 8);
                if (v != mt) continue;
                const uintptr_t obj = at + i;
                if (obj == player) continue;
                float x, y, z;
                if (!EntityPosition(obj, path, x, y, z)) continue;
                const float d = std::sqrt((x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz));
                if (d <= radius) out.emplace_back(d, obj);
            }
            at += want;
        }
        cur = regionEnd;
    }
    std::sort(out.begin(), out.end());
    if (out.size() > maxOut) out.resize(maxOut);
    return out;
}

// Is `e` still the thing under you?
bool RideEntityValid(uintptr_t e) {
    if (!e) return false;
    uintptr_t mt = 0;
    if (!ReadPtr(e, mt) || !mt || mt != g_rideEntityMT.load()) return false;
    GsdkPath path;
    { std::lock_guard lock(g_gsdkPathMtx); path = g_gsdkPath; }
    const uintptr_t player = PlayerEntity(GsdkCurrentRoot(), path);
    if (!player || e == player) return false;
    float px, py, pz, x, y, z;
    if (!EntityPosition(player, path, px, py, pz) || !EntityPosition(e, path, x, y, z)) return false;
    return (x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz) < 6.0f * 6.0f;
}

bool ReadAngle(uintptr_t base, const std::vector<uint32_t>& derefs, uint32_t offset, bool radians, float& deg) {
    if (!base) return false;
    uintptr_t obj = base;
    for (uint32_t off : derefs) {
        uintptr_t next = 0;
        if (!ReadPtr(obj + off, next) || !LooksLikeObject(next)) return false;
        obj = next;
    }
    float v = 0;
    if (!SafeRead((const void*)(obj + offset), &v, 4) || !std::isfinite(v)) return false;
    if (radians) v *= 57.2957795f;
    if (std::fabs(v) > 1440.0f) return false;
    deg = v;
    return true;
}

// One float slot of the search, followed through the whole run.
struct RideSlot {
    float first = 0, minV = 0, maxV = 0, last = 0, maxAbs = 0;
    uint16_t changes = 0;
    bool alive = true;
    // How well the value points the way you drove, both ways round and read
    // as degrees or radians - which of those it is isn't known until the end.
    float c[4] = {}, s[4] = {};
    uint16_t n = 0;
};

struct RideObj {
    uintptr_t addr = 0;
    int anchor = 0;
    uintptr_t anchorObj = 0;
    std::vector<uint32_t> path;         // from the anchor
    uint32_t window = 0;
    size_t firstSlot = 0;
};

} // namespace

std::string GsdkRidePath::ToString() const {
    if (!found) return "(none)";
    std::string s = anchor == 0 ? "root" : "ridden entity";
    char buf[48];
    for (uint32_t o : derefs) { snprintf(buf, sizeof(buf), "->+0x%X", o); s += buf; }
    snprintf(buf, sizeof(buf), " @+0x%X%s", offset, radians ? " (radians)" : "");
    return s + buf;
}

std::string GsdkRideStatus() {
    std::lock_guard lock(g_rideStatusMtx);
    return g_rideStatus;
}

bool GsdkRideEntityKnown() { return g_rideEntity.load() != 0; }
bool GsdkRideSearching() { return g_rideScanning.load(); }

void GsdkCancelRideFacing() { g_rideCancel.store(true); }

bool GsdkReadRideYaw(float& deg) {
    thread_local GsdkRidePath cache;
    thread_local unsigned ver = 0;
    const unsigned v = g_rideVersion.load(std::memory_order_relaxed);
    if (ver != v) {
        std::lock_guard lock(g_gsdkRideMtx);
        cache = g_gsdkRide;
        ver = v;
    }
    if (!cache.found) return false;
    uintptr_t base = 0;
    if (cache.anchor == 0) base = GsdkCurrentRoot();
    else {
        base = g_rideEntity.load(std::memory_order_relaxed);
        uintptr_t mt = 0;
        if (!base || !ReadPtr(base, mt) || mt != g_rideEntityMT.load(std::memory_order_relaxed)) return false;
    }
    return ReadAngle(base, cache.derefs, cache.offset, cache.radians, deg);
}

bool GsdkReadRideCandidate(const GsdkRideCandidate& c, float& deg) {
    const uintptr_t base = c.path.anchor == 0 ? GsdkCurrentRoot() : c.anchorObj;
    return ReadAngle(base, c.path.derefs, c.path.offset, c.path.radians, deg);
}

void GsdkAdoptRide(const GsdkRideCandidate& c) {
    {
        std::lock_guard lock(g_gsdkRideMtx);
        g_gsdkRide = c.path;
    }
    if (c.path.anchor == 1) g_rideEntity.store(c.anchorObj);
    g_rideVersion.fetch_add(1);
    GsdkPathSaveToDisk();
}

void GsdkSetRidePath(const GsdkRidePath& p) {
    { std::lock_guard lock(g_gsdkRideMtx); g_gsdkRide = p; }
    g_rideVersion.fetch_add(1);
}

void GsdkForgetRide() {
    { std::lock_guard lock(g_gsdkRideMtx); g_gsdkRide = GsdkRidePath{}; }
    { std::lock_guard lock(g_gsdkRideCandMtx); g_gsdkRideCandidates.clear(); }
    g_rideEntity.store(0);
    g_rideVersion.fetch_add(1);
    GsdkPathSaveToDisk();
    SetRideStatus("forgotten");
}

void GsdkRideTick() {
    GsdkRidePath rp;
    { std::lock_guard lock(g_gsdkRideMtx); rp = g_gsdkRide; }
    if (!rp.found || rp.anchor != 1) return;
    if (RideEntityValid(g_rideEntity.load())) return;
    g_rideEntity.store(0);
    // Straight from the player to it, if that way was found.
    if (!rp.entityDerefs.empty()) {
        const uintptr_t e = GsdkFollow(GsdkCurrentRoot(), rp.entityDerefs);
        if (RideEntityValid(e)) { g_rideEntity.store(e); return; }
    }
    const long long now = NowSteadyMs();
    if (g_rideScanning.load() || now - g_rideLastScanMs.load() < 5000) return;
    g_rideScanning.store(true);
    g_rideLastScanMs.store(now);
    StartWorker([]() {
        const auto close = NearbyEntities(4.0f, 1);
        if (!close.empty()) g_rideEntity.store(close.front().second);
        g_rideLastScanMs.store(NowSteadyMs());
        g_rideScanning.store(false);
    });
}

void GsdkFindRideFacing() {
    if (g_gsdkRideFinding.exchange(true)) return;
    g_rideCancel.store(false);
    StartWorker([]() {
        auto done = [](const std::string& why) {
            SetRideStatus(why);
            g_gsdkRideStep.store(0);
            g_gsdkRideFinding.store(false);
        };
        auto cancelled = []() { return g_rideCancel.load() || g_shuttingDown.load(); };
        auto step = [](int s, int ms, const std::string& text, int beeps) {
            g_gsdkRideStepEndMs.store(NowSteadyMs() + ms);
            g_gsdkRideStep.store(s);
            SetRideStatus(text);
            for (int i = 0; i < beeps; ++i) Beep(988, 90);
        };

        GsdkPath path;
        { std::lock_guard lock(g_gsdkPathMtx); path = g_gsdkPath; }
        if (!path.found || path.derefs.empty()) { done("Your position has to be found first (Player tab)."); return; }

        // ---- 1. get ready; meanwhile find what you're sitting on ----------
        step(1, 5000, "Get on the tractor and click into Hytale. Starting in a moment...", 1);
        const auto nearby = NearbyEntities(6.0f, 6);
        while (NowSteadyMs() < g_gsdkRideStepEndMs.load()) {
            if (cancelled()) { done("Cancelled."); return; }
            Sleep(50);
        }

        const uintptr_t root = GsdkCurrentRoot();
        const uintptr_t player = PlayerEntity(root, path);
        if (!player) { done("Couldn't read your player - are you in a world?"); return; }

        // ---- what to watch ---------------------------------------------------
        std::vector<RideObj> objs;
        std::unordered_set<uintptr_t> seen;
        auto add = [&](uintptr_t a, int anchor, uintptr_t anchorObj, std::vector<uint32_t> p, uint32_t window) {
            if (!a || seen.count(a)) return;
            seen.insert(a);
            RideObj o;
            o.addr = a; o.anchor = anchor; o.anchorObj = anchorObj; o.path = std::move(p); o.window = window;
            objs.push_back(std::move(o));
        };
        // Your own objects: riding, your body may well turn with the tractor.
        add(root, 0, 0, {}, 0x800);
        {
            std::vector<uint32_t> sofar;
            uintptr_t cur = root;
            for (uint32_t off : path.derefs) {
                uintptr_t next = 0;
                if (!ReadPtr(cur + off, next) || !LooksLikeObject(next)) break;
                sofar.push_back(off);
                add(next, 0, 0, sofar, 0x1800);
                cur = next;
            }
        }
        {
            size_t count = 0;
            WalkGraph(player, 2, [&](uintptr_t obj, const std::vector<uint32_t>& p) {
                std::vector<uint32_t> full{ path.derefs[0] };
                full.insert(full.end(), p.begin(), p.end());
                add(obj, 0, 0, full, p.size() <= 1 ? 0x800 : 0x400);
                return ++count < 1000;
            });
        }
        // The things near you - one of them is the tractor.
        for (const auto& [d, e] : nearby) {
            size_t count = 0;
            WalkGraph(e, 1, [&](uintptr_t obj, const std::vector<uint32_t>& p) {
                add(obj, 1, e, p, p.empty() ? 0x1800 : 0x400);
                return ++count < 120;
            });
        }
        size_t total = 0;
        for (RideObj& o : objs) { o.firstSlot = total; total += o.window / 4; }
        std::vector<RideSlot> slots(total);
        std::vector<uint8_t> buf(0x1800);

        float lastX = 0, lastY = 0, lastZ = 0;
        bool haveLast = false;
        int phase = 0;
        // One look at every slot. Phases decide what disqualifies.
        auto sample = [&](bool seed) {
            float px = 0, py = 0, pz = 0;
            const bool havePos = EntityPosition(player, path, px, py, pz);
            double heading = 0;
            bool moved = false;
            if (havePos && haveLast) {
                const float dx = px - lastX, dz = pz - lastZ;
                if (dx * dx + dz * dz > 0.3f * 0.3f) { heading = std::atan2((double)dz, (double)dx); moved = true; }
            }
            if (havePos && (!haveLast || moved)) { lastX = px; lastY = py; lastZ = pz; haveLast = true; }

            for (const RideObj& o : objs) {
                const size_t got = SafeBulkBest(buf.data(), (const void*)o.addr, o.window);
                const size_t n = got / 4;
                for (size_t i = 0; i < o.window / 4; ++i) {
                    RideSlot& s = slots[o.firstSlot + i];
                    if (!s.alive) continue;
                    if (i < 2 || i >= n) { s.alive = false; continue; }   // the class pointer, or unreadable
                    float v;
                    memcpy(&v, buf.data() + i * 4, 4);
                    if (!std::isfinite(v) || std::fabs(v) > 400.0f) { s.alive = false; continue; }
                    if (seed) { s.first = s.minV = s.maxV = s.last = v; s.maxAbs = std::fabs(v); continue; }
                    const float step = std::fabs(v - s.last);
                    const bool radianish = s.maxAbs <= 6.4f && std::fabs(v) <= 6.4f;
                    if (phase == 2 || phase == 5) {
                        if (step > 1e-4f) { s.alive = false; continue; }
                    }
                    else if (phase == 4) {
                        // The tractor is still and only the camera moves.
                        if (step > (radianish ? 0.005f : 0.3f)) { s.alive = false; continue; }
                    }
                    else if (phase == 3) {
                        if (step > 1e-4f) s.changes++;
                        if (moved && s.changes > 0) {
                            const double asDeg = v * 0.017453292519943295, asRad = v;
                            const double a[4] = { asDeg, -asDeg, asRad, -asRad };
                            for (int k = 0; k < 4; ++k) {
                                const double d = 2.0 * (a[k] - heading);
                                s.c[k] += (float)std::cos(d);
                                s.s[k] += (float)std::sin(d);
                            }
                            s.n++;
                        }
                    }
                    s.minV = (std::min)(s.minV, v);
                    s.maxV = (std::max)(s.maxV, v);
                    s.maxAbs = (std::max)(s.maxAbs, std::fabs(v));
                    s.last = v;
                }
            }
        };
        auto run = [&](int ms) {
            const long long end = NowSteadyMs() + ms;
            while (NowSteadyMs() < end) {
                if (cancelled()) return false;
                Sleep(100);
                sample(false);
            }
            return true;
        };

        // ---- 2..5: the instructions ------------------------------------------
        phase = 2;
        step(2, 3000, "HOLD STILL - hands off the keys and the mouse.", 1);
        sample(true);
        if (!run(3000)) { done("Cancelled."); return; }
        phase = 3;
        step(3, 10000, "DRIVE IN CIRCLES - turn the tractor left and right, keep moving. Don't touch the mouse.", 2);
        if (!run(10000)) { done("Cancelled."); return; }
        phase = 4;
        step(4, 6000, "STOP. Keep the tractor still and move ONLY the mouse - look all around.", 3);
        if (!run(6000)) { done("Cancelled."); return; }
        phase = 5;
        step(5, 2000, "HOLD STILL again.", 1);
        if (!run(2000)) { done("Cancelled."); return; }
        step(6, 1000, "Checking...", 0);

        // ---- what survived -------------------------------------------------------
        std::vector<GsdkRideCandidate> cands;
        for (const RideObj& o : objs) {
            for (size_t i = 0; i < o.window / 4; ++i) {
                const RideSlot& s = slots[o.firstSlot + i];
                if (!s.alive || s.changes < 5) continue;
                const bool radians = s.maxAbs <= 6.4f;
                const float range = (s.maxV - s.minV) * (radians ? 57.2957795f : 1.0f);
                if (range < 30.0f) continue;
                GsdkRideCandidate c;
                c.path.found = true;
                c.path.anchor = o.anchor;
                c.path.derefs = o.path;
                c.path.offset = (uint32_t)(i * 4);
                c.path.radians = radians;
                c.anchorObj = o.anchorObj;
                c.range = range;
                if (s.n >= 8) {
                    float best = 0;
                    for (int k = radians ? 2 : 0; k < (radians ? 4 : 2); ++k) {
                        const float r = std::sqrt(s.c[k] * s.c[k] + s.s[k] * s.s[k]) / s.n;
                        best = (std::max)(best, r);
                    }
                    c.agree = best;
                }
                cands.push_back(std::move(c));
            }
        }
        if (cands.empty()) {
            done("Nothing turned with only the tractor. If moving the mouse steers your tractor, you don't need "
                 "this - the camera direction does the job. Otherwise try again, keeping the mouse still while "
                 "you drive and the tractor still while you look around.");
            return;
        }
        // Best: points the way you drove; then in the thing you ride rather
        // than yourself; then the bigger swing.
        std::sort(cands.begin(), cands.end(), [](const GsdkRideCandidate& a, const GsdkRideCandidate& b) {
            if ((a.agree >= 0.6f) != (b.agree >= 0.6f)) return a.agree >= 0.6f;
            if (std::fabs(a.agree - b.agree) > 0.05f) return a.agree > b.agree;
            if (a.path.anchor != b.path.anchor) return a.path.anchor > b.path.anchor;
            return a.range > b.range;
        });
        if (cands.size() > 16) cands.resize(16);

        // The tractor itself: can it be reached straight from you? Then a
        // relog doesn't need the heap search to find it again.
        if (cands.front().path.anchor == 1) {
            const uintptr_t target = cands.front().anchorObj;
            std::vector<uint32_t> found;
            WalkGraph(player, 3, [&](uintptr_t obj, const std::vector<uint32_t>& p) {
                if (obj != target) return true;
                found = p;
                return false;
            });
            if (!found.empty()) {
                std::vector<uint32_t> full{ path.derefs[0] };
                full.insert(full.end(), found.begin(), found.end());
                for (auto& c : cands) if (c.anchorObj == target) c.path.entityDerefs = full;
            }
        }

        { std::lock_guard lock(g_gsdkRideCandMtx); g_gsdkRideCandidates = cands; }
        GsdkAdoptRide(cands.front());
        const GsdkRideCandidate& top = cands.front();
        char msg[320];
        if (top.agree >= 0.6f)
            snprintf(msg, sizeof(msg), "Found it - it pointed the way you drove %.0f%% of the time. Turn the tractor "
                "and watch the number follow.%s", top.agree * 100.0f,
                cands.size() > 1 ? " Other possibilities are listed if it doesn't." : "");
        else
            snprintf(msg, sizeof(msg), "Found %zu possibilities but couldn't confirm one against the way you drove. "
                "The likeliest is in use - turn the tractor and check it follows; pick another below if not.",
                cands.size());
        done(msg);
    });
}
