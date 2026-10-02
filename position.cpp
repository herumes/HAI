#include "pch.h"
#include "position.hpp"
#include "hooks.hpp"   // SafeRead / IsPlausiblePtr / RegionReadable / GetGameModuleBaseSize
#include "common.hpp"  // LogToFile / NowSteadyMs / GetThisModuleDirectory / WideToUtf8
#include "chat.hpp"    // g_stringVT
#include <cmath>
#include <chrono>
#include <sstream>
#include <limits>
#include <fstream>
#include <functional>

std::mutex g_posMtx;
std::vector<PosCandidate> g_posCandidates;

std::atomic<bool>   g_posScanning{ false };
std::atomic<bool>   g_posLastScanTruncated{ false };
std::atomic<size_t> g_posLastScanRegions{ 0 };
std::atomic<size_t> g_posLastScanBytes{ 0 };
std::atomic<long long> g_posLastScanMs{ 0 };

std::atomic<bool>     g_posLocked{ false };
std::atomic<uintptr_t> g_posLockedAddr{ 0 };
std::atomic<double>   g_posX{ 0.0 };
std::atomic<double>   g_posY{ 0.0 };
std::atomic<double>   g_posZ{ 0.0 };
std::atomic<long long> g_posLastUpdateMs{ 0 };
std::atomic<long long> g_posLastGoodMs{ 0 };

std::atomic<float> g_posBoundXZ{ 30000.0f };
std::atomic<float> g_posBoundYMin{ -256.0f };
std::atomic<float> g_posBoundYMax{ 512.0f };

std::atomic<bool>   g_posUseGuess{ false };
std::atomic<double> g_posGuessX{ 0.0 };
std::atomic<double> g_posGuessY{ 0.0 };
std::atomic<double> g_posGuessZ{ 0.0 };
std::atomic<float>  g_posGuessTolerance{ 3.0f };

std::atomic<int> g_posValueType{ (int)PosValueType::Double };

std::mutex g_posPtrMtx;
std::vector<PosPtrHit> g_posPtrHits;
std::atomic<bool> g_posPtrScanning{ false };
std::atomic<uintptr_t> g_posPtrCurrentTarget{ 0 };
std::atomic<int> g_posPtrWindow{ 0x800 };

std::mutex g_posChainMtx;
std::vector<int64_t> g_posChainOffsets;
std::atomic<bool> g_posChainDone{ false };
std::wstring g_posChainModule;
uintptr_t g_posChainStaticOffset{ 0 };
std::atomic<int> g_posChainValueType{ (int)PosValueType::Double };

std::atomic<bool> g_posHudTextEnabled{ false };
std::atomic<bool> g_posHudTextScanning{ false };
std::atomic<long long> g_posHudTextLastAttemptMs{ 0 };
std::atomic<long long> g_posHudTextLastScanDurMs{ 0 };
std::atomic<long long> g_posHudTextLastFoundMs{ 0 };
std::atomic<uintptr_t> g_posHudTextAddr{ 0 };
std::mutex g_posHudTextMtx;
std::string g_posHudTextLast;
std::atomic<bool> g_posHudTextUsedFastPath{ false }; // diagnostic, shown in the UI

namespace {

constexpr uintptr_t kScanLow = 0x10000;
constexpr uintptr_t kScanHigh = 0x00007FFFFFFF0000ULL; // top of x64 user address space
constexpr size_t kChunkSize = 8 * 1024 * 1024;
constexpr size_t kOverlap = 2048; // covers a straddling triple/pointer AND a ~500-char debug string
constexpr size_t kMaxCandidates = 200000;
constexpr size_t kMaxPtrHits = 20000;
constexpr long long kScanBudgetMs = 20000;
constexpr size_t kScanByteBudget = 12ULL * 1024 * 1024 * 1024; // 12 GB

bool IsFloatMode() { return g_posValueType.load() == (int)PosValueType::Float; }
size_t TripleByteSize() { return IsFloatMode() ? 12 : 24; }
size_t TripleAlign() { return IsFloatMode() ? 4 : 8; }

// Widens (float mode) or copies (double mode) a triple out of raw bytes.
void ReadTripleFromBytes(const uint8_t* p, double& x, double& y, double& z) {
    if (IsFloatMode()) {
        float fx, fy, fz;
        memcpy(&fx, p, 4);
        memcpy(&fy, p + 4, 4);
        memcpy(&fz, p + 8, 4);
        x = fx; y = fy; z = fz;
    }
    else {
        memcpy(&x, p, 8);
        memcpy(&y, p + 8, 8);
        memcpy(&z, p + 16, 8);
    }
}

// Base sanity check only - world bounds, finiteness, not-all-zero. Applied
// everywhere (seeding, filtering, the locked per-frame read).
bool InRangeTriple(double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    if (x == 0.0 && y == 0.0 && z == 0.0) return false; // extremely common, never useful
    const float boundXZ = g_posBoundXZ.load();
    const float yMin = g_posBoundYMin.load();
    const float yMax = g_posBoundYMax.load();
    if (x < -boundXZ || x > boundXZ) return false;
    if (z < -boundXZ || z > boundXZ) return false;
    if (y < yMin || y > yMax) return false;
    return true;
}

// Proximity to the user-entered "known coordinates" (e.g. read off Hytale's
// own F7 debug overlay). Only ever applied where the caller explicitly wants
// it - NOT baked into InRangeTriple, because after the player moves their
// real position is expected to be far from wherever the guess was last set.
bool MatchesGuess(double x, double y, double z) {
    if (!g_posUseGuess.load()) return true;
    const float tol = g_posGuessTolerance.load();
    const double gx = g_posGuessX.load(), gy = g_posGuessY.load(), gz = g_posGuessZ.load();
    if (std::fabs(x - gx) > tol) return false;
    if (std::fabs(y - gy) > tol) return false;
    if (std::fabs(z - gz) > tol) return false;
    return true;
}

// __try/__except can't live in a function that also needs C++ object
// unwinding (vectors, etc. with destructors) - C2712 - so this is split
// out into its own frame, same as SafeRead in hooks.cpp.
bool SafeBulkCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RegionIsScannable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    if (mbi.Protect & PAGE_NOACCESS) return false;
    const DWORD prot = mbi.Protect & 0xFF;
    // Writable, non-code pages only - this is where live game state lives.
    return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY;
}

// Our own module's image (globals, function-local statics, ImGui's own
// widget buffers) lives in the same address space we're scanning. Without
// excluding it, g_posGuessX/Y/Z - and the ui.cpp float fields you type F7's
// numbers into - are themselves a perfect, permanent "match" for their own
// guess, and get rediscovered as fake candidates on every single scan.
uintptr_t g_selfModuleBase = 0;
size_t g_selfModuleSize = 0;

void EnsureSelfModuleRange() {
    if (g_selfModuleBase) return;
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&EnsureSelfModuleRange, &hMod) || !hMod)
        return;
    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), hMod, &mi, sizeof(mi))) {
        g_selfModuleBase = (uintptr_t)mi.lpBaseOfDll;
        g_selfModuleSize = mi.SizeOfImage;
    }
}

bool OverlapsSelfModule(uintptr_t addr, size_t len) {
    if (!g_selfModuleBase) return false;
    const uintptr_t selfEnd = g_selfModuleBase + g_selfModuleSize;
    return addr < selfEnd && (addr + len) > g_selfModuleBase;
}

// Wipe a candidate list's own values before it's discarded. Freed heap
// memory is not zeroed by the OS, so a previous scan's results otherwise
// keep looking like valid new candidates - "found" over and over - forever.
void ScrubCandidates(std::vector<PosCandidate>& v) {
    for (auto& c : v) {
        c.x = c.y = c.z = std::numeric_limits<double>::quiet_NaN();
        c.addr = 0;
    }
}

// Scan one already-read local buffer (mirrors bytes read from [regionAddr, regionAddr+len))
// for plausible (x,y,z) triples, aligned per the current value type.
// Returns false to tell the caller to stop (candidate cap hit).
bool ScanBufferForTriples(const uint8_t* buf, size_t len, uintptr_t regionAddr,
    std::vector<PosCandidate>& out) {
    const size_t stride = TripleByteSize();
    const size_t align = TripleAlign();
    if (len < stride) return true;
    const bool useGuess = g_posUseGuess.load();
    for (size_t i = 0; i + stride <= len; i += align) {
        double x, y, z;
        ReadTripleFromBytes(buf + i, x, y, z);
        if (!InRangeTriple(x, y, z)) continue;
        if (useGuess && !MatchesGuess(x, y, z)) continue;
        out.push_back(PosCandidate{ regionAddr + i, x, y, z });
        if (out.size() >= kMaxCandidates) return false;
    }
    return true;
}

// Scan one already-read local buffer for 8-byte-aligned pointers whose value
// lands within [target-window, target]. Returns false to stop (hit cap).
bool ScanBufferForPointers(const uint8_t* buf, size_t len, uintptr_t regionAddr,
    uintptr_t target, int64_t window, std::vector<PosPtrHit>& out) {
    if (len < 8) return true;
    for (size_t i = 0; i + 8 <= len; i += 8) {
        uintptr_t v;
        memcpy(&v, buf + i, 8);
        if (!v || !IsPlausiblePtr((void*)v)) continue;
        if (v > target) continue;
        const int64_t offset = (int64_t)(target - v);
        if (offset > window) continue;
        out.push_back(PosPtrHit{ regionAddr + i, offset, false });
        if (out.size() >= kMaxPtrHits) return false;
    }
    return true;
}

// Scan one already-read local buffer for a .NET String object (vtable
// matches the already-known g_stringVT, plausible length, valid null
// terminator - same validation as hooks.cpp's ReadSystemString) whose
// decoded text contains `marker`. Near-zero false positive rate, since the
// vtable check alone eliminates almost every candidate offset instantly.
bool ScanBufferForHudText(const uint8_t* buf, size_t len, uintptr_t regionAddr,
    uint64_t knownVT, const std::string& marker,
    std::vector<std::pair<uintptr_t, std::string>>& out) {
    if (!knownVT || len < 14) return true;
    for (size_t i = 0; i + 14 <= len; i += 8) {
        uint64_t vt;
        memcpy(&vt, buf + i, 8);
        if (vt != knownVT) continue;

        int32_t strLen;
        memcpy(&strLen, buf + i + 8, 4);
        if (strLen <= 0 || strLen > 512) continue;

        const size_t charBytes = (size_t)strLen * 2;
        const size_t need = 12 + charBytes + 2; // header + chars + null terminator
        if (i + need > len) continue; // spans a chunk boundary - kOverlap usually still catches it next pass

        uint16_t term;
        memcpy(&term, buf + i + 12 + charBytes, 2);
        if (term != 0) continue;

        std::wstring w(strLen, L'\0');
        memcpy(w.data(), buf + i + 12, charBytes);
        std::string utf8 = WideToUtf8(w.c_str(), strLen);
        if (utf8.find('\0') != std::string::npos) continue;
        if (utf8.find(marker) == std::string::npos) continue;

        out.push_back({ regionAddr + i, utf8 });
        if (out.size() >= 4096) return false; // sane upper bound, not a "we found it" cap
    }
    return true;
}

// Parses "Feet (x, y, z)" (or whatever marker precedes it) out of a decoded
// debug line. Tolerant of the exact spacing Hytale uses around the commas.
bool ParseFeetLine(const std::string& text, const std::string& marker, double& x, double& y, double& z) {
    size_t p = text.find(marker);
    if (p == std::string::npos) return false;
    p += marker.size();
    return sscanf_s(text.c_str() + p, "%lf , %lf , %lf", &x, &y, &z) == 3;
}

// Re-scans a window anchored on the last hit's address, instead of the whole
// process. A full walk takes several seconds - far longer than these
// short-lived debug strings survive between GCs, so most full scans simply
// miss the window entirely. .NET's nursery is a bump-pointer allocator, so
// the next allocation lands shortly after the last one within the same
// committed segment; anchoring on the last hit (rather than its region's
// base) keeps working even if that segment is much larger than what we
// bother reading. Biased forward since the pointer only moves one way
// between GCs.
bool TryFastRegionScan(uint64_t knownVT, const std::string& marker,
    std::vector<std::pair<uintptr_t, std::string>>& out) {
    const uintptr_t anchor = g_posHudTextAddr.load();
    if (!anchor) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((const void*)anchor, &mbi, sizeof(mbi))) return false;
    if (!RegionIsScannable(mbi)) return false;

    const uintptr_t regionStart = (uintptr_t)mbi.BaseAddress;
    const uintptr_t regionEnd = regionStart + mbi.RegionSize;

    constexpr size_t kBehind = 4ULL * 1024 * 1024;
    constexpr size_t kAhead = 32ULL * 1024 * 1024;
    const uintptr_t winStart = (anchor > regionStart + kBehind) ? (anchor - kBehind) : regionStart;
    const uintptr_t winEnd = (std::min)(regionEnd, anchor + kAhead);
    if (winEnd <= winStart) return false;
    const size_t want = (size_t)(winEnd - winStart);
    if (want < 14) return false;

    std::vector<uint8_t> buf(want);
    if (!SafeBulkCopy(buf.data(), (const void*)winStart, want)) return false;

    ScanBufferForHudText(buf.data(), want, winStart, knownVT, marker, out);
    return !out.empty();
}

// Generic "walk all scannable memory, one bulk-copied chunk at a time"
// driver shared by the value scan and the pointer scan. `visit` returns
// false to abort the whole walk early (e.g. a result cap was hit).
struct WalkStats {
    size_t regions = 0;
    size_t bytes = 0;
    bool truncated = false;
};

WalkStats WalkScannableMemory(
    const std::function<bool(const uint8_t* buf, size_t len, uintptr_t regionAddr)>& visit) {
    WalkStats stats;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> chunk(kChunkSize);

    uintptr_t cur = kScanLow;
    bool stop = false;
    while (!stop && cur < kScanHigh && !g_shuttingDown.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed > kScanBudgetMs || stats.bytes > kScanByteBudget) {
            stats.truncated = true;
            break;
        }

        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((const void*)cur, &mbi, sizeof(mbi)))
            break;

        const uintptr_t regionStart = (uintptr_t)mbi.BaseAddress;
        const uintptr_t regionEnd = regionStart + mbi.RegionSize;
        if (regionEnd <= cur) break; // shouldn't happen, avoid infinite loop

        if (!RegionIsScannable(mbi) || OverlapsSelfModule(regionStart, mbi.RegionSize)) {
            cur = regionEnd;
            continue;
        }
        ++stats.regions;

        uintptr_t pos = (std::max)(cur, regionStart);
        while (pos < regionEnd && !g_shuttingDown.load()) {
            size_t want = (std::min)(kChunkSize, (size_t)(regionEnd - pos));
            if (want < 8) break;

            if (!SafeBulkCopy(chunk.data(), (const void*)pos, want))
                break; // region changed under us (freed/decommitted) - move on

            if (!visit(chunk.data(), want, pos)) {
                stats.truncated = true;
                stop = true;
                break;
            }
            stats.bytes += want;

            if (want < kChunkSize) break; // consumed the rest of the region
            pos += (kChunkSize - kOverlap);
        }

        cur = regionEnd;
    }
    return stats;
}

} // namespace

void PosFirstScan() {
    if (g_posScanning.exchange(true)) return;
    EnsureSelfModuleRange();
    auto t0 = std::chrono::steady_clock::now();

    std::vector<PosCandidate> found;
    found.reserve(4096);

    auto stats = WalkScannableMemory([&](const uint8_t* buf, size_t len, uintptr_t regionAddr) {
        return ScanBufferForTriples(buf, len, regionAddr, found);
        });

    {
        std::lock_guard lock(g_posMtx);
        ScrubCandidates(g_posCandidates); // wipe the outgoing list before it's freed
        g_posCandidates = std::move(found);
    }
    g_posLastScanRegions.store(stats.regions);
    g_posLastScanBytes.store(stats.bytes);
    g_posLastScanTruncated.store(stats.truncated);
    g_posLastScanMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());

    LogToFile("PosFirstScan: regions=" + std::to_string(stats.regions) +
        " bytes=" + std::to_string(stats.bytes) +
        " candidates=" + std::to_string(g_posCandidates.size()) +
        (stats.truncated ? " (budget/cap hit - narrow the guess/tolerance and rescan)" : ""));

    g_posScanning.store(false);
}

void PosFilterScan() {
    if (g_posScanning.exchange(true)) return;
    auto t0 = std::chrono::steady_clock::now();

    std::vector<PosCandidate> snapshot;
    {
        std::lock_guard lock(g_posMtx);
        snapshot = g_posCandidates;
    }

    const size_t stride = TripleByteSize();
    std::vector<PosCandidate> kept;
    kept.reserve(snapshot.size());

    for (const auto& c : snapshot) {
        if (g_shuttingDown.load()) break;
        uint8_t buf[24];
        if (!SafeRead((const void*)c.addr, buf, stride)) continue;
        double nx, ny, nz;
        ReadTripleFromBytes(buf, nx, ny, nz);
        if (!InRangeTriple(nx, ny, nz)) continue;

        const double moved = std::fabs(nx - c.x) + std::fabs(ny - c.y) + std::fabs(nz - c.z);
        if (moved <= 0.0005) continue; // didn't change - not tracking us

        kept.push_back(PosCandidate{ c.addr, nx, ny, nz });
    }

    {
        std::lock_guard lock(g_posMtx);
        ScrubCandidates(g_posCandidates);
        g_posCandidates = std::move(kept);
    }
    g_posLastScanMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());

    LogToFile("PosFilterScan: kept=" + std::to_string(g_posCandidates.size()) +
        " of " + std::to_string(snapshot.size()));

    g_posScanning.store(false);
}

void PosFilterByGuess() {
    if (g_posScanning.exchange(true)) return;
    auto t0 = std::chrono::steady_clock::now();

    std::vector<PosCandidate> snapshot;
    {
        std::lock_guard lock(g_posMtx);
        snapshot = g_posCandidates;
    }

    const size_t stride = TripleByteSize();
    std::vector<PosCandidate> kept;
    kept.reserve(snapshot.size());

    for (const auto& c : snapshot) {
        if (g_shuttingDown.load()) break;
        uint8_t buf[24];
        if (!SafeRead((const void*)c.addr, buf, stride)) continue;
        double nx, ny, nz;
        ReadTripleFromBytes(buf, nx, ny, nz);
        if (!InRangeTriple(nx, ny, nz)) continue;
        if (!MatchesGuess(nx, ny, nz)) continue; // must be near the freshly-entered known coords

        kept.push_back(PosCandidate{ c.addr, nx, ny, nz });
    }

    {
        std::lock_guard lock(g_posMtx);
        ScrubCandidates(g_posCandidates);
        g_posCandidates = std::move(kept);
    }
    g_posLastScanMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());

    LogToFile("PosFilterByGuess: kept=" + std::to_string(g_posCandidates.size()) +
        " of " + std::to_string(snapshot.size()));

    g_posScanning.store(false);
}

void PosLock(uintptr_t addr) {
    g_posLockedAddr.store(addr);
    g_posLocked.store(true);
    g_posLastGoodMs.store(NowSteadyMs());
    std::ostringstream oss;
    oss << std::hex << addr;
    LogToFile("Position locked at 0x" + oss.str());
}

void PosUnlock() {
    g_posLocked.store(false);
    g_posLockedAddr.store(0);
}

void PosClearCandidates() {
    std::lock_guard lock(g_posMtx);
    ScrubCandidates(g_posCandidates);
    g_posCandidates.clear();
}

void PosFrameTick() {
    if (g_posHudTextEnabled.load()) return; // PosHudTextTick owns updates while this is on
    if (!g_posLocked.load()) return;
    const uintptr_t addr = g_posLockedAddr.load();
    if (!addr) { PosUnlock(); return; }

    uint8_t buf[24];
    const size_t stride = TripleByteSize();
    const long long now = NowSteadyMs();

    double x, y, z;
    if (SafeRead((const void*)addr, buf, stride)) {
        ReadTripleFromBytes(buf, x, y, z);
        if (InRangeTriple(x, y, z)) {
            g_posX.store(x);
            g_posY.store(y);
            g_posZ.store(z);
            g_posLastUpdateMs.store(now);
            g_posLastGoodMs.store(now);
            return;
        }
    }

    // Tolerate brief implausible reads (e.g. mid-write tear) but give up on
    // a stretch of consistently bad reads - the memory was likely freed/reused.
    if (now - g_posLastGoodMs.load() > 3000) {
        LogToFile("Position lock lost (stale/implausible for 3s) - unlocking");
        PosUnlock();
    }
}

// =============================================================================
// Pointer chain finder
// =============================================================================

void PosChainBegin(uintptr_t startTarget) {
    g_posPtrCurrentTarget.store(startTarget);
    {
        std::lock_guard lock(g_posPtrMtx);
        g_posPtrHits.clear();
    }
    {
        std::lock_guard lock(g_posChainMtx);
        g_posChainOffsets.clear();
        g_posChainModule.clear();
        g_posChainStaticOffset = 0;
    }
    g_posChainDone.store(false);
    g_posChainValueType.store(g_posValueType.load());
    LogToFile("PosChainBegin: target=0x" + [&] { std::ostringstream o; o << std::hex << startTarget; return o.str(); }());
}

void PosFindPointersToCurrentTarget() {
    if (g_posPtrScanning.exchange(true)) return;
    EnsureSelfModuleRange();

    const uintptr_t target = g_posPtrCurrentTarget.load();
    const int64_t window = g_posPtrWindow.load();

    std::vector<PosPtrHit> found;
    found.reserve(1024);

    if (target) {
        WalkScannableMemory([&](const uint8_t* buf, size_t len, uintptr_t regionAddr) {
            return ScanBufferForPointers(buf, len, regionAddr, target, window, found);
            });
    }

    size_t gameBase = 0, gameSize = 0;
    uintptr_t gBase = GetGameModuleBaseSize(gameSize);
    gameBase = (size_t)gBase;
    for (auto& h : found) {
        h.inGameModule = gameBase && h.slotAddr >= gameBase && h.slotAddr < gameBase + gameSize;
    }

    {
        std::lock_guard lock(g_posPtrMtx);
        g_posPtrHits = std::move(found);
    }

    LogToFile("PosFindPointersToCurrentTarget: target=0x" +
        [&] { std::ostringstream o; o << std::hex << target; return o.str(); }() +
        " hits=" + std::to_string(g_posPtrHits.size()));

    g_posPtrScanning.store(false);
}

void PosChainDrillInto(const PosPtrHit& hit) {
    {
        std::lock_guard lock(g_posChainMtx);
        g_posChainOffsets.push_back(hit.offset);
    }
    g_posPtrCurrentTarget.store(hit.slotAddr);
    {
        std::lock_guard lock(g_posPtrMtx);
        g_posPtrHits.clear();
    }
    LogToFile("PosChainDrillInto: offset=" + std::to_string(hit.offset) +
        " newTarget=0x" + [&] { std::ostringstream o; o << std::hex << hit.slotAddr; return o.str(); }());
}

void PosChainFinish(const PosPtrHit& hit) {
    size_t gameSize = 0;
    uintptr_t gameBase = GetGameModuleBaseSize(gameSize);
    if (!gameBase || hit.slotAddr < gameBase || hit.slotAddr >= gameBase + gameSize) {
        LogToFile("PosChainFinish: hit is not actually inside the game module - refusing");
        return;
    }

    std::vector<int64_t> discoveryOrder;
    {
        std::lock_guard lock(g_posChainMtx);
        discoveryOrder = g_posChainOffsets;
    }
    discoveryOrder.push_back(hit.offset); // this level's offset (applied last discovered, first resolved)

    // Resolve order is the reverse of discovery order (see position.hpp comment).
    std::vector<int64_t> resolveOrder(discoveryOrder.rbegin(), discoveryOrder.rend());

    {
        std::lock_guard lock(g_posChainMtx);
        g_posChainOffsets = std::move(resolveOrder);
        g_posChainModule = L"HytaleClient.exe";
        g_posChainStaticOffset = hit.slotAddr - gameBase;
    }
    g_posChainDone.store(true);

    LogToFile("PosChainFinish: staticOffset=0x" +
        [&] { std::ostringstream o; o << std::hex << (hit.slotAddr - gameBase); return o.str(); }() +
        " levels=" + std::to_string(discoveryOrder.size()));

    PosChainSaveToDisk();
}

bool PosChainSaveToDisk() {
    if (!g_posChainDone.load()) return false;
    std::wofstream f(GetThisModuleDirectory() + L"position_chain.txt", std::ios::trunc);
    if (!f.is_open()) return false;

    std::lock_guard lock(g_posChainMtx);
    f << L"module=" << g_posChainModule << L"\n";
    f << L"staticOffset=" << g_posChainStaticOffset << L"\n";
    f << L"valueType=" << g_posChainValueType.load() << L"\n";
    f << L"offsets=";
    for (size_t i = 0; i < g_posChainOffsets.size(); ++i) {
        if (i) f << L",";
        f << g_posChainOffsets[i];
    }
    f << L"\n";
    return true;
}

bool PosChainLoadFromDisk() {
    std::wifstream f(GetThisModuleDirectory() + L"position_chain.txt");
    if (!f.is_open()) return false;

    std::wstring line;
    std::wstring module;
    uintptr_t staticOffset = 0;
    int valueType = (int)PosValueType::Double;
    std::vector<int64_t> offsets;
    bool haveModule = false, haveOffset = false, haveOffsets = false;

    while (std::getline(f, line)) {
        auto eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq);
        std::wstring val = line.substr(eq + 1);
        if (key == L"module") { module = val; haveModule = true; }
        else if (key == L"staticOffset") {
            try { staticOffset = (uintptr_t)std::stoull(val); haveOffset = true; }
            catch (...) {}
        }
        else if (key == L"valueType") {
            try { valueType = std::stoi(val); }
            catch (...) {}
        }
        else if (key == L"offsets") {
            std::wstringstream ss(val);
            std::wstring tok;
            while (std::getline(ss, tok, L',')) {
                if (tok.empty()) continue;
                try { offsets.push_back(std::stoll(tok)); haveOffsets = true; }
                catch (...) {}
            }
        }
    }

    if (!haveModule || !haveOffset || !haveOffsets) return false;

    std::lock_guard lock(g_posChainMtx);
    g_posChainModule = module;
    g_posChainStaticOffset = staticOffset;
    g_posChainOffsets = std::move(offsets);
    g_posChainValueType.store(valueType);
    g_posChainDone.store(true);
    return true;
}

bool PosChainTryAutoLock() {
    if (!g_posChainDone.load()) {
        if (!PosChainLoadFromDisk()) return false;
    }

    std::wstring module;
    uintptr_t staticOffset;
    std::vector<int64_t> offsets;
    {
        std::lock_guard lock(g_posChainMtx);
        module = g_posChainModule;
        staticOffset = g_posChainStaticOffset;
        offsets = g_posChainOffsets;
    }
    if (module.empty() || offsets.empty()) return false;

    size_t modSize = 0;
    uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!modBase || staticOffset >= modSize) return false;

    uintptr_t addr = modBase + staticOffset;
    for (int64_t off : offsets) {
        uintptr_t next = 0;
        if (!SafeRead((const void*)addr, &next, 8) || !next) return false;
        if (off >= 0) addr = next + (uintptr_t)off;
        else addr = next - (uintptr_t)(-off);
        if (!IsPlausiblePtr((void*)addr)) return false;
    }

    g_posValueType.store(g_posChainValueType.load());

    uint8_t buf[24];
    const size_t stride = TripleByteSize();
    double x, y, z;
    if (!SafeRead((const void*)addr, buf, stride)) return false;
    ReadTripleFromBytes(buf, x, y, z);
    if (!InRangeTriple(x, y, z)) return false;

    PosLock(addr);
    LogToFile("PosChainTryAutoLock: resolved and locked automatically");
    return true;
}

// =============================================================================
// HUD-text tracking
// =============================================================================

void PosHudTextScanOnce() {
    if (g_posHudTextScanning.exchange(true)) return;
    EnsureSelfModuleRange();
    auto t0 = std::chrono::steady_clock::now();

    const uint64_t knownVT = g_stringVT.load();
    std::vector<std::pair<uintptr_t, std::string>> hits;
    bool usedFastPath = false;

    if (knownVT) {
        usedFastPath = TryFastRegionScan(knownVT, "Feet (", hits);
        if (!usedFastPath) {
            hits.clear();
            WalkScannableMemory([&](const uint8_t* buf, size_t len, uintptr_t regionAddr) {
                return ScanBufferForHudText(buf, len, regionAddr, knownVT, "Feet (", hits);
                });
        }
    }
    g_posHudTextUsedFastPath.store(usedFastPath);

    g_posHudTextLastScanDurMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());

    if (!hits.empty()) {
        // .NET strings are immutable, so if Hytale allocates a fresh one each
        // frame (typical of string interpolation), several recent frames'
        // worth can be simultaneously alive until the next GC - and a
        // bump-pointer nursery means the newest one lands at the highest
        // address. Taking the lowest/first match kept re-finding an old,
        // no-longer-updating copy and looked "frozen". Prefer whichever hit
        // actually differs from what we last saw; fall back to the newest
        // address if everything still matches (player genuinely hasn't moved).
        std::sort(hits.begin(), hits.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

        std::string prevText;
        { std::lock_guard lock(g_posHudTextMtx); prevText = g_posHudTextLast; }

        const auto* chosen = &hits.front();
        for (const auto& h : hits) {
            if (h.second != prevText) { chosen = &h; break; }
        }

        const auto& [addr, text] = *chosen;
        double x, y, z;
        if (ParseFeetLine(text, "Feet (", x, y, z) && InRangeTriple(x, y, z)) {
            const long long now = NowSteadyMs();
            g_posX.store(x);
            g_posY.store(y);
            g_posZ.store(z);
            g_posLockedAddr.store(addr); // informational only - a fresh string each scan
            g_posLocked.store(true);
            g_posLastUpdateMs.store(now);
            g_posLastGoodMs.store(now);
            g_posHudTextAddr.store(addr);
            g_posHudTextLastFoundMs.store(now);
            {
                std::lock_guard lock(g_posHudTextMtx);
                g_posHudTextLast = text;
            }
            // g_posHudTextAddr (set above) is what TryFastRegionScan anchors
            // on next tick - no separate region bookkeeping needed.
        }
    }

    g_posHudTextScanning.store(false);
}

void PosHudTextTick() {
    if (!g_posHudTextEnabled.load()) return;
    if (g_posHudTextScanning.load()) return;
    const long long now = NowSteadyMs();
    if (now - g_posHudTextLastAttemptMs.load() < 500) return; // throttle
    g_posHudTextLastAttemptMs.store(now);
    StartWorker([]() { PosHudTextScanOnce(); });
}

void PosHudTextSetEnabled(bool enabled) {
    g_posHudTextEnabled.store(enabled);
    if (!enabled) {
        PosUnlock();
    }
    std::wofstream f(GetThisModuleDirectory() + L"position_hudtext.txt", std::ios::trunc);
    if (f.is_open())
        f << L"enabled=" << (enabled ? 1 : 0) << L"\n";
}

void PosHudTextLoadFromDisk() {
    std::wifstream f(GetThisModuleDirectory() + L"position_hudtext.txt");
    if (!f.is_open()) return;
    std::wstring line;
    while (std::getline(f, line)) {
        auto eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (line.substr(0, eq) == L"enabled")
            g_posHudTextEnabled.store(line.substr(eq + 1) == L"1");
    }
}
