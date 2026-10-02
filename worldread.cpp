#include "pch.h"
#include "worldread.hpp"
#include "common.hpp"
#include "hooks.hpp"
#include "prefab.hpp"
#include "selection.hpp"
#include "position.hpp"
#include "chat.hpp"
#include "gamesdk.hpp"
#include "packets.hpp"
#include "MinHook.h"
#include <ws2tcpip.h>
#define SECURITY_WIN32
#include <sspi.h>
#include <winsock2.h>
#include <psapi.h>   // the inbound hook that was there all along   // the game root, and the chunk we are standing in

#include <algorithm>
#include <cstdio>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

std::atomic<bool> g_worldScanning{ false };

namespace {

std::mutex g_wrMtx;
ChunkLayout g_layout;
std::string g_statusText = "not found yet";
std::string g_reportText;

void SetStatus(const std::string& s) {
    std::lock_guard lock(g_wrMtx);
    g_statusText = s;
}

// Same walker the name search uses: committed, readable, private memory only.
void ScanCommitted(size_t chunkBytes,
    const std::function<bool(uintptr_t base, const uint8_t* buf, size_t len)>& fn) {

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t at = (uintptr_t)si.lpMinimumApplicationAddress;
    const uintptr_t end = (uintptr_t)si.lpMaximumApplicationAddress;
    std::vector<uint8_t> buf(chunkBytes);

    while (at < end) {
        if (g_shuttingDown.load()) return;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((LPCVOID)at, &mbi, sizeof(mbi))) break;

        const uintptr_t regionBase = (uintptr_t)mbi.BaseAddress;
        const size_t regionSize = mbi.RegionSize;
        const bool usable =
            mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));

        if (usable && regionSize) {
            for (size_t off = 0; off < regionSize; off += chunkBytes) {
                size_t want = regionSize - off;
                if (want > chunkBytes) want = chunkBytes;
                if (!SafeRead((const void*)(regionBase + off), buf.data(), want)) continue;
                if (!fn(regionBase + off, buf.data(), want)) return;
            }
        }
        if (regionSize == 0) break;
        at = regionBase + regionSize;
    }
}

uint32_t ReadAt(uintptr_t addr, uint32_t width) {
    uint32_t v = 0;
    if (width == 1) { uint8_t b = 0;  if (SafeRead((const void*)addr, &b, 1)) v = b; }
    else if (width == 2) { uint16_t b = 0; if (SafeRead((const void*)addr, &b, 2)) v = b; }
    else { if (!SafeRead((const void*)addr, &v, 4)) v = 0; }
    return v;
}

} // namespace

std::string ChunkLayout::ToString() const {
    if (!found) return "not found";
    char buf[260];
    snprintf(buf, sizeof(buf),
        "%u bytes a block, steps of %lld / %lld / %lld elements for x / y / z, "
        "%d-block chunk at %d, %d, %d",
        width, (long long)sx, (long long)sy, (long long)sz, edge,
        originX, originY, originZ);
    return buf;
}

std::string WorldStatus() { std::lock_guard lock(g_wrMtx); return g_statusText; }
std::string WorldReport() { std::lock_guard lock(g_wrMtx); return g_reportText; }
bool WorldFound() { std::lock_guard lock(g_wrMtx); return g_layout.found; }
ChunkLayout WorldGetLayout() { std::lock_guard lock(g_wrMtx); return g_layout; }

namespace {

// Where in memory a coordinate lands, for a given layout. Only meaningful
// inside the chunk the layout was found in, which is what the mask check is.
bool AddressOf(const ChunkLayout& L, int x, int y, int z, uintptr_t& out) {
    // Bounds come from the extents rather than a power-of-two mask. The array
    // this was fitted against is 137065 long, which is not a cube and not a
    // power of two, so masking would silently wrap and hand back a block from
    // somewhere else entirely.
    const int dx = x - L.originX, dy = y - L.originY, dz = z - L.originZ;
    if (dx < 0 || (L.ex && dx >= L.ex)) return false;
    if (dy < 0 || (L.ey && dy >= L.ey)) return false;
    if (dz < 0 || (L.ez && dz >= L.ez)) return false;

    const int64_t d = (int64_t)dx * L.sx + (int64_t)dy * L.sy + (int64_t)dz * L.sz;
    if (d < 0 || (L.count && d >= L.count)) return false;
    out = L.base + (intptr_t)(d * (int64_t)L.width);
    return true;
}

} // namespace

bool WorldReadBlock(int x, int y, int z, uint32_t& outId) {
    ChunkLayout L = WorldGetLayout();
    if (!L.found) return false;
    uintptr_t addr = 0;
    if (!AddressOf(L, x, y, z, addr)) return false;
    outId = ReadAt(addr, L.width);
    return true;
}

bool WorldCanReadBox(int minX, int minY, int minZ, int maxX, int maxY, int maxZ,
    long long& readable, long long& total) {
    readable = 0;
    total = 0;
    ChunkLayout L = WorldGetLayout();
    for (int y = minY; y <= maxY; ++y)
        for (int z = minZ; z <= maxZ; ++z)
            for (int x = minX; x <= maxX; ++x) {
                ++total;
                uintptr_t addr = 0;
                if (L.found && AddressOf(L, x, y, z, addr)) ++readable;
            }
    return L.found && readable == total;
}

void WorldFindChunks() {
    if (g_worldScanning.exchange(true)) return;

    StartWorker([]() {
        WorkerGuard guard;

        // What the server source says the storage actually is, which is why the
        // first version of this search could never have worked:
        //
        //   A chunk is 32x32 across and 320 tall, cut into ten sections of 32.
        //   Each section holds 32x32x32 blocks with PALETTE COMPRESSION - a
        //   short list of the distinct blocks present, plus a grid of small
        //   indices into that list.
        //
        // So block ids are not laid out across the world in memory. Searching
        // for "1238 next to 1530" was looking for something that does not exist
        // anywhere. What DOES exist is a palette: a short run holding the
        // distinct ids of whatever is nearby. That is what this looks for.
        const std::vector<BlockProbe> probes = BuildGetProbes();

        std::vector<uint32_t> ids;
        for (const BlockProbe& p : probes) {
            if (!p.id) continue;                       // Empty is in every palette
            bool seen = false;
            for (uint32_t v : ids) if (v == p.id) { seen = true; break; }
            if (!seen) ids.push_back(p.id);
        }

        std::string report;
        char line[320];
        snprintf(line, sizeof(line),
            "%zu known block(s), %zu different kinds\n", probes.size(), ids.size());
        report = line;

        if (ids.size() < 3) {
            {
                std::lock_guard lock(g_wrMtx);
                g_reportText = report;
            }
            SetStatus("needs at least three DIFFERENT kinds of block in one spot. "
                "Place three or four different blocks next to each other - they "
                "all land in the same section's palette, and that is what this "
                "goes looking for.");
            g_worldScanning.store(false);
            return;
        }

        snprintf(line, sizeof(line), "looking for a palette holding: ");
        report += line;
        for (uint32_t v : ids) {
            snprintf(line, sizeof(line), "%u ", v);
            report += line;
        }
        report += "\n\n";

        SetStatus("looking for a palette holding those blocks...");

        // The first version of this found forty-eight candidates and every one
        // of them was OUR OWN probe list - the DLL shares the process, so the
        // scan read its own bookkeeping back. Useful, as it happens: in our
        // records the ids sit sixteen bytes apart with coordinates between
        // them, whereas in a palette they are ENTRIES IN A LIST, side by side.
        //
        // So a hit now needs two known ids in adjacent slots. That is what a
        // palette looks like and what a table of {x, y, z, id} never does.
        struct Hit {
            uintptr_t addr; uint32_t width; int found; int adjacent;
            uintptr_t arr; int64_t arrLen;
        };
        std::vector<Hit> hits;
        constexpr size_t kWindow = 512;

        for (uint32_t width : { (uint32_t)2, (uint32_t)4 }) {
            ScanCommitted(1u << 20, [&](uintptr_t base, const uint8_t* buf, size_t len) {
                for (size_t i = 0; i + kWindow <= len; i += kWindow / 2) {
                    // Where each known id turns up inside this window.
                    std::vector<size_t> at;
                    for (size_t k = i; k + width <= i + kWindow && k + width <= len;
                         k += width) {
                        uint32_t got = 0;
                        memcpy(&got, buf + k, width);
                        for (uint32_t v : ids) if (got == v) { at.push_back(k); break; }
                    }
                    if (at.size() < 3) continue;

                    int adjacent = 0;
                    size_t runStart = 0;
                    bool haveRun = false;
                    for (size_t k = 1; k < at.size(); ++k) {
                        if (at[k] - at[k - 1] != width) continue;
                        ++adjacent;
                        if (!haveRun) { runStart = at[k - 1]; haveRun = true; }
                    }
                    if (adjacent < 2) continue;      // the whole point of the filter

                    // A managed array puts its MethodTable and length just
                    // before the elements. Finding one is not required - the
                    // palette could be a plain buffer - but when it is there it
                    // says how long the list is, which is the next thing needed.
                    uintptr_t arr = 0;
                    int64_t arrLen = 0;
                    for (size_t back = 0x10; back <= 0x40 && back <= runStart - i; back += 8) {
                        const size_t h = runStart - back;
                        if (h < i) break;
                        uint64_t mt = 0;
                        memcpy(&mt, buf + h, 8);
                        if (!mt || (mt & 7) || !IsPlausiblePtr((const void*)mt)) continue;
                        int64_t n = 0;
                        memcpy(&n, buf + h + 8, 8);
                        if (n <= 0 || n > 65536) continue;
                        arr = base + h;
                        arrLen = n;
                        break;
                    }

                    hits.push_back(Hit{ base + runStart, width,
                        (int)at.size(), adjacent, arr, arrLen });
                    if (hits.size() >= 48) return false;
                }
                return true;
                });
            if (!hits.empty()) break;
        }

        // A hit sitting inside a managed array is far more likely to be the
        // real thing, so those come first rather than being buried.
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
            if ((a.arr != 0) != (b.arr != 0)) return a.arr != 0;
            return a.adjacent > b.adjacent;
            });

        snprintf(line, sizeof(line), "%zu candidate(s) with ids side by side\n\n",
            hits.size());
        report += line;

        for (size_t h = 0; h < hits.size() && h < 12; ++h) {
            snprintf(line, sizeof(line),
                "  0x%llX  %u-byte  %d ids, %d side by side%s",
                (unsigned long long)hits[h].addr, hits[h].width,
                hits[h].found, hits[h].adjacent, "");
            report += line;
            if (hits[h].arr) {
                snprintf(line, sizeof(line), "  in an array of %lld at 0x%llX",
                    (long long)hits[h].arrLen, (unsigned long long)hits[h].arr);
                report += line;
            }
            report += "\n    ";

            uint8_t win[128];
            if (SafeRead((const void*)(hits[h].addr - 32), win, sizeof(win))) {
                for (uint32_t k = 0; k + hits[h].width <= sizeof(win); k += hits[h].width) {
                    uint32_t v = 0;
                    memcpy(&v, win + k, hits[h].width);
                    snprintf(line, sizeof(line), "%u ", v);
                    report += line;
                }
            }
            report += "\n";
        }

        {
            std::lock_guard lock(g_wrMtx);
            g_layout = ChunkLayout{};
            g_reportText = report;
        }

        if (hits.empty()) {
            SetStatus("no palette found holding those blocks. Place three or four "
                "DIFFERENT blocks right next to each other first - they have to "
                "share one section for their ids to share a palette.");
        }
        else {
            snprintf(line, sizeof(line),
                "found %zu possible palette(s). That is the block list; the grid "
                "of indices beside it is the next piece, and the numbers in the "
                "report are what identify it.", hits.size());
            SetStatus(line);
        }

        g_worldScanning.store(false);
        });
}

// =============================================================================
// Doing it without being typed at
// =============================================================================
// /block get answers the one question we cannot answer ourselves, and both
// halves of chat are already wired up - the DLL can send a line and it can read
// what comes back. Asking a person to type two dozen commands and paste the
// replies is work a machine should be doing.
std::atomic<bool> g_worldProbing{ false };

int WorldScrapeChat() {
    std::string all;
    {
        std::lock_guard lock(g_mtx);
        for (const ChatLine& l : g_log) {
            if (l.plain.find("Block at (") == std::string::npos) continue;
            all += l.plain;
            all += '\n';
        }
    }
    if (all.empty()) return 0;
    BuildLearnFromBlockGet(all);
    return (int)BuildGetProbes().size();
}

void WorldAutoProbe() {
    if (g_worldProbing.exchange(true)) return;

    StartWorker([]() {
        WorkerGuard guard;

        // Probe through the middle of the selection, since that is the part
        // that has to be readable. With no selection, through wherever you are.
        int cx, cy, cz;
        int a, b, c, d, e, f;
        if (SelBounds(a, b, c, d, e, f)) {
            cx = (a + d) / 2; cy = (b + e) / 2; cz = (c + f) / 2;
        }
        else {
            cx = (int)std::floor(g_posX.load());
            cy = (int)std::floor(g_posY.load());
            cz = (int)std::floor(g_posZ.load());
        }

        // Two rows along x to find the storage, then a line each along y and z
        // to pin the other two directions. Two rows rather than one because a
        // single row through a building can easily be all one material, and a
        // run of identical ids is a signature that matches everywhere.
        std::vector<std::array<int, 3>> want;
        for (int i = -3; i <= 4; ++i) want.push_back({ cx + i, cy,     cz });
        for (int i = -3; i <= 2; ++i) want.push_back({ cx + i, cy + 1, cz });
        for (int i = -2; i <= 2; ++i) if (i) want.push_back({ cx, cy, cz + i });
        for (int i = -2; i <= 2; ++i) if (i) want.push_back({ cx, cy + i, cz });

        char msg[160];
        snprintf(msg, sizeof(msg), "asking the game about %zu blocks around "
            "%d, %d, %d...", want.size(), cx, cy, cz);
        SetStatus(msg);

        for (const auto& w : want) {
            if (g_shuttingDown.load()) break;
            PendingSend ps;
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "/block get %d %d %d", w[0], w[1], w[2]);
            ps.gameText = cmd;
            ps.silent = true;
            QueueSend(ps);
        }

        // The replies arrive in chat at whatever pace the send queue manages,
        // so this watches the count rather than guessing at a duration. It
        // stops early once the answers stop arriving.
        const size_t before = BuildGetProbes().size();
        size_t last = before;
        int quiet = 0;
        for (int tick = 0; tick < 120 && !g_shuttingDown.load(); ++tick) {
            Sleep(250);
            const int now = WorldScrapeChat();
            if ((size_t)now != last) { last = (size_t)now; quiet = 0; }
            else if (++quiet >= 12) break;          // three seconds of nothing

            snprintf(msg, sizeof(msg), "%zu of %zu answered...",
                last - before, want.size());
            SetStatus(msg);
            if (last - before >= want.size()) break;
        }

        const size_t got = BuildGetProbes().size() - before;
        if (got < 4) {
            snprintf(msg, sizeof(msg),
                "only %zu blocks came back. /block get needs command permission - "
                "it works in your own world, or anywhere you are op.", got);
            SetStatus(msg);
            g_worldProbing.store(false);
            return;
        }

        g_worldProbing.store(false);
        WorldFindChunks();
        });
}

// =============================================================================
// Watching a block change
// =============================================================================
// Take a copy of memory, place one block, and see what is no longer what it
// was. It assumes nothing about layout, palettes or which id space anything is
// counted in, which is why it is here after three searches that each had to be
// right about one of those in advance and each was wrong.
//
// The first version of this could not converge, and it is worth saying why,
// because it looked like the method failing and it was not. It reported every
// slot that changed and then intersected those sets across placements. But
// about a hundred thousand slots change in any 400ms window no matter what you
// do - the Windows shared clock page at 0x7FFE0000, thread stacks, GC
// bookkeeping, frame counters - and something that changes every time survives
// every intersection. Eight placements got nowhere and could never have.
//
// So two filters now, both of which should have been here first:
//
//   1. Learn the noise. Take a copy, wait, place NOTHING, and blacklist
//      whatever moved on its own. That is the clock and the stacks, gone.
//   2. Match the value. We know which block was placed. A slot that changed to
//      exactly that number is a different class of evidence from a slot that
//      merely changed, and one placement is usually enough to find it.
//
// Filter 2 is the one that does the work, so it is tried first and reported
// separately. It can come up empty - if the block was already in that section's
// palette only a small index changes, and no raw id is written anywhere - which
// is why placing something unusual works better than placing more stone.

namespace {

struct WatchRegion {
    uintptr_t base = 0;
    std::vector<uint8_t> bytes;
};

struct WatchCand {
    uintptr_t addr = 0;
    uint16_t before = 0;
    uint16_t after = 0;
};

std::vector<WatchRegion> g_watchSnap;
std::vector<WatchCand>   g_watchCands;
std::vector<uintptr_t>   g_watchNoise;     // sorted; changes on its own

// The narrowing above finds slots that change when a block is placed, which is
// not the same as slots that HOLD the block. Placing one block re-meshes the
// whole chunk section, so the render buffers change too, and there is nothing
// in a single placement to tell the two apart.
//
// This is what tells them apart: place the next block ONE STEP over. Real block
// storage is an array, so the slot for the neighbouring coordinate sits a fixed
// few bytes from the last one. A mesh buffer just gets rewritten end to end and
// lands nowhere in particular. So the next placement keeps the old hits as
// anchors and reports what changed BY HOW FAR IT LANDED FROM ONE - and a hit at
// +2 or +4 is both the answer and the element width.
std::vector<uintptr_t>   g_watchAnchors;
bool g_watchStrideMode = false;

// =============================================================================
// Two placements of the same block, one step apart
// =============================================================================
// Diffing memory was the wrong lever and it took three rounds to admit it.
// Comparing before and after finds everything that moved, and in a running
// game that is well over a million slots a second - clocks, stacks, GC, and
// the chunk's render mesh being rebuilt, which happens every time a block is
// placed and looks exactly like the thing we want. Filtering by distance to a
// previous hit did not help either: with half a million changes in play,
// something lands two bytes from an anchor by chance alone.
//
// What finally pointed the way was one line in that noise: an address holding
// 1983, the id of the block that had just been placed. Raw block ids ARE in
// memory. So stop looking for change and look for the VALUE.
//
// Place a block, and note every slot holding its id. Place the same block one
// step east, and note them again. The storage for the new coordinate is a slot
// that was not holding that id a moment ago and now sits a few bytes from one
// that was. A particular block id is rare; ticking counters are not. There is
// nothing to subtract and no blacklist to maintain.
bool g_watchPairMode = false;
int  g_watchPairStep = 0;                // 0 waiting for the first, 1 the second
uint32_t g_watchPairId = 0;
int  g_watchPairAt[3] = { 0, 0, 0 };
std::vector<uintptr_t> g_watchPairFirst; // sorted; slots holding that id

// Every slot holding this value, at any alignment. Unaligned on purpose: a
// packed grid has no reason to line up with anything.
void ScanForValue(uint32_t id, std::vector<uintptr_t>& out) {
    const uint16_t a = (uint16_t)id;
    const uint16_t b = (uint16_t)(id >= 10 ? id - 10 : id);  // the other id space
    out.clear();
    ScanCommitted(1ull << 20, [&](uintptr_t base, const uint8_t* buf, size_t len) {
        for (size_t k = 0; k + 2 <= len; ++k) {
            uint16_t v = 0;
            memcpy(&v, buf + k, 2);
            if (v != a && v != b) continue;
            out.push_back(base + k);
            if (out.size() >= 400000) return false;
        }
        return true;
        });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

int  g_watchRound = 0;
int  g_watchCoord[3] = { 0, 0, 0 };
bool g_watchValueLocked = false;           // candidates matched the placed id

// How much memory to keep a copy of. The client's heap is bigger than this, so
// a miss is possible; the report says how much was actually covered, so that a
// miss reads as a miss rather than as the method failing.
constexpr size_t kWatchBudget = 384ull << 20;
constexpr size_t kWatchMaxCands = 3000000;
constexpr DWORD  kNoiseWaitMs = 2500;

void TakeSnapshot(size_t& total) {
    std::vector<WatchRegion> snap;
    total = 0;
    ScanCommitted(1ull << 20, [&](uintptr_t base, const uint8_t* buf, size_t len) {
        if (total + len > kWatchBudget) return false;
        WatchRegion r;
        r.base = base;
        r.bytes.assign(buf, buf + len);
        total += len;
        snap.push_back(std::move(r));
        return true;
        });
    std::lock_guard lock(g_wrMtx);
    g_watchSnap = std::move(snap);
}

// Compares the copy against memory as it is now. Caller holds g_wrMtx.
void DiffSnapshot(std::vector<WatchCand>& out, size_t& covered) {
    std::vector<uint8_t> now;
    covered = 0;
    for (const WatchRegion& r : g_watchSnap) {
        const size_t len = r.bytes.size();
        covered += len;
        now.resize(len);
        if (!SafeRead((const void*)r.base, now.data(), len)) continue;
        for (size_t k = 0; k + 2 <= len; k += 2) {
            uint16_t a = 0, b = 0;
            memcpy(&a, r.bytes.data() + k, 2);
            memcpy(&b, now.data() + k, 2);
            if (a == b) continue;
            out.push_back(WatchCand{ r.base + k, a, b });
            if (out.size() >= kWatchMaxCands) return;
        }
    }
}

void DropSnapshot() {
    g_watchSnap.clear();
    g_watchSnap.shrink_to_fit();
}

bool IsNoise(uintptr_t addr) {
    return std::binary_search(g_watchNoise.begin(), g_watchNoise.end(), addr);
}

// The world could be storing either id, since the placement packet counts ten
// higher than the name registry does and it was never established which one
// reaches the chunk. Both are accepted rather than guessed at.
bool ValueLooksPlaced(uint16_t after, uint32_t placed) {
    return (uint32_t)after == placed ||
        (uint32_t)after + 10 == placed ||
        (uint32_t)after == placed + 10;
}

} // namespace

std::atomic<bool> g_worldWatchArmed{ false };
std::atomic<bool> g_worldWatchBusy{ false };

std::string WorldWatchStatus() {
    if (g_worldWatchBusy.load()) return "working...";
    std::lock_guard lock(g_wrMtx);
    char buf[220];
    if (g_worldWatchArmed.load()) {
        snprintf(buf, sizeof(buf),
            "armed (round %d) - go place ONE block now", g_watchRound + 1);
        return buf;
    }
    if (g_watchRound == 0) {
        if (g_watchNoise.empty()) return "start with step 1";
        snprintf(buf, sizeof(buf), "%zu noisy slot(s) known - ready for step 2",
            g_watchNoise.size());
        return buf;
    }
    snprintf(buf, sizeof(buf), "%zu slot(s) left after %d placement(s)%s",
        g_watchCands.size(), g_watchRound,
        g_watchValueLocked ? ", holding the block you placed" : "");
    return buf;
}

size_t WorldWatchNoiseCount() {
    std::lock_guard lock(g_wrMtx);
    return g_watchNoise.size();
}

void WorldWatchCancel() {
    g_worldWatchArmed.store(false);
    std::lock_guard lock(g_wrMtx);
    DropSnapshot();
    g_watchCands.clear();
    g_watchRound = 0;
    g_watchValueLocked = false;
    g_statusText = "watch reset - the noise list is kept";
    g_reportText.clear();
}

void WorldWatchLearnNoise() {
    if (g_worldWatchBusy.exchange(true)) return;
    g_worldWatchArmed.store(false);

    StartWorker([]() {
        WorkerGuard guard;

        SetStatus("taking a copy of memory...");
        size_t total = 0;
        TakeSnapshot(total);

        SetStatus("watching what moves on its own - do not place anything...");
        Sleep(kNoiseWaitMs);

        std::vector<WatchCand> changed;
        size_t covered = 0;
        std::vector<uintptr_t> noise;
        {
            std::lock_guard lock(g_wrMtx);
            DiffSnapshot(changed, covered);
            DropSnapshot();

            noise.reserve(changed.size() + g_watchNoise.size());
            for (const WatchCand& c : changed) noise.push_back(c.addr);
            // Runs add up rather than replace: a slot that ticked during an
            // earlier run is noisy even if it happened to sit still this time.
            noise.insert(noise.end(), g_watchNoise.begin(), g_watchNoise.end());
            std::sort(noise.begin(), noise.end());
            noise.erase(std::unique(noise.begin(), noise.end()), noise.end());
            g_watchNoise = std::move(noise);
            g_watchRound = 0;
            g_watchCands.clear();
            g_watchValueLocked = false;
        }

        char msg[240];
        {
            std::lock_guard lock(g_wrMtx);
            snprintf(msg, sizeof(msg),
                "%zu slot(s) change on their own and will be ignored from now on "
                "- clocks, stacks, frame counters. Now do step 2.",
                g_watchNoise.size());
        }
        SetStatus(msg);
        g_worldWatchBusy.store(false);
        });
}

void WorldWatchArm() {
    if (g_worldWatchBusy.exchange(true)) return;

    StartWorker([]() {
        WorkerGuard guard;

        int round = 0;
        { std::lock_guard lock(g_wrMtx); round = g_watchRound; }

        if (round == 0) {
            SetStatus("taking a copy of memory...");
            size_t total = 0;
            TakeSnapshot(total);

            char msg[240];
            snprintf(msg, sizeof(msg),
                "armed - copied %zu MB. Place ONE block now, ideally a kind that "
                "is not already around you.", total >> 20);
            SetStatus(msg);
        }
        else {
            // Later passes only need the handful of addresses still in play.
            std::lock_guard lock(g_wrMtx);
            for (WatchCand& c : g_watchCands) {
                uint16_t v = 0;
                SafeRead((const void*)c.addr, &v, 2);
                c.before = v;
            }
            char msg[240];
            snprintf(msg, sizeof(msg),
                "armed - watching %zu slot(s). Place another block at the SAME "
                "spot.", g_watchCands.size());
            g_statusText = msg;
        }

        g_worldWatchArmed.store(true);
        g_worldWatchBusy.store(false);
        });
}

void WorldWatchStride() {
    if (g_worldWatchBusy.exchange(true)) return;

    StartWorker([]() {
        WorkerGuard guard;

        size_t anchors = 0;
        {
            std::lock_guard lock(g_wrMtx);
            g_watchAnchors.clear();
            for (const WatchCand& c : g_watchCands) g_watchAnchors.push_back(c.addr);
            std::sort(g_watchAnchors.begin(), g_watchAnchors.end());
            anchors = g_watchAnchors.size();
            g_watchStrideMode = true;
        }

        SetStatus("taking a copy of memory...");
        size_t total = 0;
        TakeSnapshot(total);

        char msg[260];
        snprintf(msg, sizeof(msg),
            "armed against %zu anchor(s), copied %zu MB. Now place ONE block "
            "one step EAST of the last one.", anchors, total >> 20);
        SetStatus(msg);

        g_worldWatchArmed.store(true);
        g_worldWatchBusy.store(false);
        });
}

namespace {

// The stride round: a full diff again, but reported by how far each change
// landed from one of the previous round's hits. Ordering by that distance is
// the whole point - an array puts the neighbour a couple of bytes away, and
// nothing else has any reason to.
void ReportStrideRound(uint32_t placed) {
    std::vector<WatchCand> raw;
    size_t covered = 0;
    {
        std::lock_guard lock(g_wrMtx);
        DiffSnapshot(raw, covered);
        DropSnapshot();
    }

    struct Near { WatchCand c; uintptr_t anchor; long long delta; };
    std::vector<Near> hits;
    size_t noisy = 0, wayOff = 0;
    {
        std::lock_guard lock(g_wrMtx);
        for (const WatchCand& c : raw) {
            if (IsNoise(c.addr)) { ++noisy; continue; }
            if (g_watchAnchors.empty()) continue;

            // Nearest anchor either side.
            auto it = std::lower_bound(g_watchAnchors.begin(), g_watchAnchors.end(),
                c.addr);
            uintptr_t best = 0;
            long long bestD = 0;
            bool have = false;
            for (int pass = 0; pass < 2; ++pass) {
                auto p = it;
                if (pass == 1) {
                    if (p == g_watchAnchors.begin()) continue;
                    --p;
                }
                else if (p == g_watchAnchors.end()) continue;
                const long long d = (long long)c.addr - (long long)*p;
                if (!have || std::llabs(d) < std::llabs(bestD)) {
                    best = *p; bestD = d; have = true;
                }
            }
            if (!have) continue;
            if (std::llabs(bestD) > 8192) { ++wayOff; continue; }
            hits.push_back(Near{ c, best, bestD });
        }
    }

    std::stable_sort(hits.begin(), hits.end(), [](const Near& a, const Near& b) {
        return std::llabs(a.delta) < std::llabs(b.delta);
        });

    std::string report;
    char line[300];
    {
        std::lock_guard lock(g_wrMtx);
        snprintf(line, sizeof(line), "stride round: placed %u at %d, %d, %d\n",
            placed, g_watchCoord[0], g_watchCoord[1], g_watchCoord[2]);
        report = line;
        snprintf(line, sizeof(line), "%zu anchor(s) from the last round\n",
            g_watchAnchors.size());
        report += line;
    }
    snprintf(line, sizeof(line),
        "%zu changed, %zu noise, %zu nowhere hits an anchor, %zu close by\n\n",
        raw.size(), noisy, wayOff, hits.size());
    report += line;

    for (size_t i = 0; i < hits.size() && i < 40; ++i) {
        const long long d = hits[i].delta;
        // Deliberately not calling anything here an array. With half a million
        // changes in play, landing two bytes from an anchor proves nothing.
        const char* mark = "";
        snprintf(line, sizeof(line), "  0x%llX  %u -> %u   %+lld from 0x%llX%s\n",
            (unsigned long long)hits[i].c.addr,
            hits[i].c.before, hits[i].c.after, d,
            (unsigned long long)hits[i].anchor, mark);
        report += line;
    }
    if (hits.size() > 40) {
        snprintf(line, sizeof(line), "  ... and %zu more\n", hits.size() - 40);
        report += line;
    }

    { std::lock_guard lock(g_wrMtx); g_reportText = report; }

    int tight = 0;
    for (const Near& n : hits) if (std::llabs(n.delta) > 0 && std::llabs(n.delta) <= 8) ++tight;

    if (tight) {
        snprintf(line, sizeof(line),
            "%d change(s) landed within eight bytes of a previous hit. That is "
            "an array of blocks, and the offset is how wide one entry is.", tight);
        SetStatus(line);
    }
    else if (hits.empty()) {
        SetStatus("nothing changed anywhere hits the previous hits, so those were "
            "mesh buffers rather than block storage. Start over and place a kind "
            "of block that is not already around you.");
    }
    else {
        SetStatus("changes landed hits the previous hits but not adjacent to "
            "them, so that was most likely the chunk mesh being rebuilt. Start "
            "over somewhere with more variety around you.");
    }
}

} // namespace

void WorldWatchPairStart() {
    std::lock_guard lock(g_wrMtx);
    g_watchPairMode = true;
    g_watchPairStep = 0;
    g_watchPairId = 0;
    g_watchPairFirst.clear();
    g_watchStrideMode = false;
    g_statusText = "armed - place ONE block, and remember exactly where.";
    g_reportText.clear();
    g_worldWatchArmed.store(true);
}

namespace {

void HandlePairPlacement(int x, int y, int z, uint32_t placed) {
    int step = 0;
    {
        std::lock_guard lock(g_wrMtx);
        step = g_watchPairStep;
    }

    Sleep(400);   // the client applies the change after the packet goes out

    if (step == 0) {
        SetStatus("looking for every slot holding that block...");
        std::vector<uintptr_t> found;
        ScanForValue(placed, found);

        char line[300];
        {
            std::lock_guard lock(g_wrMtx);
            g_watchPairFirst = std::move(found);
            g_watchPairId = placed;
            g_watchPairAt[0] = x; g_watchPairAt[1] = y; g_watchPairAt[2] = z;
            g_watchPairStep = 1;

            snprintf(line, sizeof(line),
                "first block: %u at %d, %d, %d\n%zu slot(s) in memory hold that "
                "id right now\n\nNow place the SAME kind of block one step east, "
                "at %d, %d, %d.\n",
                placed, x, y, z, g_watchPairFirst.size(), x + 1, y, z);
            g_reportText = line;

            snprintf(line, sizeof(line),
                "%zu slot(s) hold %u. Now place the SAME block one step east, at "
                "%d, %d, %d.", g_watchPairFirst.size(), placed, x + 1, y, z);
            g_statusText = line;
        }
        g_worldWatchArmed.store(true);   // straight on to the second placement
        return;
    }

    // Second placement. Anything holding the id now but not a moment ago is a
    // new arrival; the one that matters landed beside an old one.
    SetStatus("comparing against the first block...");
    std::vector<uintptr_t> found;
    ScanForValue(placed, found);

    struct Pair { uintptr_t addr, from; long long delta; };
    std::vector<Pair> pairs;
    size_t fresh = 0;
    uint32_t firstId = 0;
    int ax = 0, ay = 0, az = 0;
    size_t beforeCount = 0;

    {
        std::lock_guard lock(g_wrMtx);
        firstId = g_watchPairId;
        ax = g_watchPairAt[0]; ay = g_watchPairAt[1]; az = g_watchPairAt[2];
        beforeCount = g_watchPairFirst.size();

        for (uintptr_t addr : found) {
            if (std::binary_search(g_watchPairFirst.begin(), g_watchPairFirst.end(),
                addr)) continue;
            ++fresh;

            auto it = std::lower_bound(g_watchPairFirst.begin(),
                g_watchPairFirst.end(), addr);
            for (int pass = 0; pass < 2; ++pass) {
                auto q = it;
                if (pass == 1) {
                    if (q == g_watchPairFirst.begin()) continue;
                    --q;
                }
                else if (q == g_watchPairFirst.end()) continue;
                const long long d = (long long)addr - (long long)*q;
                if (std::llabs(d) <= 64) pairs.push_back(Pair{ addr, *q, d });
            }
        }
    }

    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
        return std::llabs(a.delta) < std::llabs(b.delta);
        });
    pairs.erase(std::unique(pairs.begin(), pairs.end(),
        [](const Pair& a, const Pair& b) { return a.addr == b.addr; }), pairs.end());

    std::string report;
    char line[320];
    snprintf(line, sizeof(line),
        "first  %u at %d, %d, %d  -> %zu slot(s)\n"
        "second %u at %d, %d, %d  -> %zu slot(s)\n"
        "%zu newly hold it, %zu of those within 64 bytes of an old one\n\n",
        firstId, ax, ay, az, beforeCount,
        placed, x, y, z, found.size(), fresh, pairs.size());
    report = line;

    if (placed != firstId) {
        snprintf(line, sizeof(line),
            "NOTE: that was block %u, not %u. It has to be the same kind both "
            "times or there is nothing to compare.\n\n", placed, firstId);
        report += line;
    }
    if (ay != y || az != z || x - ax != 1) {
        snprintf(line, sizeof(line),
            "NOTE: expected %d, %d, %d and got %d, %d, %d. The two have to be "
            "neighbours for the gap between them to mean anything.\n\n",
            ax + 1, ay, az, x, y, z);
        report += line;
    }

    for (size_t i = 0; i < pairs.size() && i < 40; ++i) {
        snprintf(line, sizeof(line), "  0x%llX  %+lld from 0x%llX\n",
            (unsigned long long)pairs[i].addr, pairs[i].delta,
            (unsigned long long)pairs[i].from);
        report += line;
    }
    if (pairs.size() > 40) {
        snprintf(line, sizeof(line), "  ... and %zu more\n", pairs.size() - 40);
        report += line;
    }

    {
        std::lock_guard lock(g_wrMtx);
        g_reportText = report;
        g_watchPairMode = false;
        g_watchPairStep = 0;
    }

    if (pairs.empty()) {
        SetStatus("nothing new landed near the first block, so the world is not "
            "keeping raw ids where they can be reached this way. That is the end "
            "of this route.");
    }
    else {
        snprintf(line, sizeof(line),
            "%zu candidate(s), closest first. A gap of 2 or 4 is one block, and "
            "that gap is the element width.", pairs.size());
        SetStatus(line);
    }
}

} // namespace

void WorldWatchOnPlacement(int x, int y, int z, uint32_t packetId) {
    if (!g_worldWatchArmed.exchange(false)) return;
    if (g_worldWatchBusy.exchange(true)) return;

    {
        std::lock_guard lock(g_wrMtx);
        g_watchCoord[0] = x; g_watchCoord[1] = y; g_watchCoord[2] = z;
    }
    const uint32_t placed = packetId;
    const int gx = x, gy = y, gz = z;

    StartWorker([placed, gx, gy, gz]() {
        WorkerGuard guard;

        bool pair = false, stride = false;
        {
            std::lock_guard lock(g_wrMtx);
            pair = g_watchPairMode;
            stride = g_watchStrideMode;
        }
        if (pair) {
            HandlePairPlacement(gx, gy, gz, placed);
            g_worldWatchBusy.store(false);
            return;
        }
        {
            // Nothing was snapshotted, so there is nothing to say. Without this
            // it reports "0 changed" over the top of whatever else was running.
            std::lock_guard lock(g_wrMtx);
            if (g_watchRound == 0 && g_watchSnap.empty() && !g_watchPairMode) {
                g_worldWatchBusy.store(false);
                return;
            }
        }

        if (stride) {
            SetStatus("looking for what changed...");
            Sleep(400);
            ReportStrideRound(placed);
            { std::lock_guard lock(g_wrMtx); g_watchStrideMode = false; }
            g_worldWatchBusy.store(false);
            return;
        }

        // The packet goes out before the client applies the change, so give it
        // a moment. Comparing too early would find nothing, and that would look
        // like a failure of the method rather than of the timing.
        SetStatus("looking for what changed...");
        Sleep(400);

        int round = 0;
        { std::lock_guard lock(g_wrMtx); round = g_watchRound; }

        std::vector<WatchCand> raw;
        size_t covered = 0;

        {
            std::lock_guard lock(g_wrMtx);
            if (round == 0) {
                DiffSnapshot(raw, covered);
                DropSnapshot();
            }
            else {
                for (const WatchCand& c : g_watchCands) {
                    uint16_t v = 0;
                    if (!SafeRead((const void*)c.addr, &v, 2)) continue;
                    if (v == c.before) continue;
                    raw.push_back(WatchCand{ c.addr, c.before, v });
                }
            }
        }

        // Filter 1: throw out everything already known to move on its own.
        std::vector<WatchCand> quiet;
        size_t noisy = 0;
        {
            std::lock_guard lock(g_wrMtx);
            quiet.reserve(raw.size());
            for (const WatchCand& c : raw) {
                if (IsNoise(c.addr)) { ++noisy; continue; }
                quiet.push_back(c);
            }
        }

        // Filter 2: of those, the ones now holding the number that was placed.
        // This is the filter that actually decides it.
        std::vector<WatchCand> exact;
        for (const WatchCand& c : quiet) {
            if (ValueLooksPlaced(c.after, placed)) exact.push_back(c);
        }

        const bool locked = !exact.empty();
        std::vector<WatchCand> keep = locked ? exact : quiet;

        std::string report;
        char line[300];

        {
            std::lock_guard lock(g_wrMtx);
            g_watchCands = keep;
            g_watchRound = round + 1;
            g_watchValueLocked = locked;

            snprintf(line, sizeof(line), "round %d: placed %u at %d, %d, %d\n",
                g_watchRound, placed,
                g_watchCoord[0], g_watchCoord[1], g_watchCoord[2]);
            report = line;
        }

        if (round == 0) {
            snprintf(line, sizeof(line), "compared %zu MB\n", covered >> 20);
            report += line;
        }
        snprintf(line, sizeof(line),
            "%zu changed, %zu of them known noise, %zu left\n",
            raw.size(), noisy, quiet.size());
        report += line;
        snprintf(line, sizeof(line), "%zu of those now hold %u, %u or %u\n\n",
            exact.size(), placed, placed >= 10 ? placed - 10 : 0, placed + 10);
        report += line;

        for (size_t i = 0; i < keep.size() && i < 32; ++i) {
            const char* mark = "";
            if (locked) {
                mark = ((uint32_t)keep[i].after == placed) ? "  <== the packet number"
                    : ((uint32_t)keep[i].after + 10 == placed) ? "  <== ten under it"
                    : "  <== ten over it";
            }
            snprintf(line, sizeof(line), "  0x%llX  %u -> %u%s\n",
                (unsigned long long)keep[i].addr,
                keep[i].before, keep[i].after, mark);
            report += line;
        }
        if (keep.size() > 32) {
            snprintf(line, sizeof(line), "  ... and %zu more\n", keep.size() - 32);
            report += line;
        }

        { std::lock_guard lock(g_wrMtx); g_reportText = report; }

        if (keep.empty()) {
            SetStatus("nothing left. Either the block landed outside the copy, or "
                "it was already in that section and only an index moved. Start "
                "over and place a kind of block that is not around you.");
            std::lock_guard lock(g_wrMtx);
            g_watchRound = 0;
        }
        else if (locked && keep.size() <= 16) {
            snprintf(line, sizeof(line),
                "%zu slot(s) hold the block you just placed. That is the world "
                "storage. Arm once more and place the SAME kind of block ONE "
                "STEP east - whichever address moves tells us the stride.",
                keep.size());
            SetStatus(line);
        }
        else if (locked) {
            snprintf(line, sizeof(line),
                "%zu slot(s) hold what you placed - more than expected, so arm "
                "again and place a different kind at the same spot to cut it "
                "down.", keep.size());
            SetStatus(line);
        }
        else {
            snprintf(line, sizeof(line),
                "nothing held the number itself, so %zu slot(s) are still in "
                "play. Arm again and place a different block at the SAME spot.",
                keep.size());
            SetStatus(line);
        }

        g_worldWatchBusy.store(false);
        });
}

// =============================================================================
// Finding the chunks through the object graph
// =============================================================================
// Four searches through raw memory, and none of them found a block. Worth
// saying why before the fifth, because the pattern is embarrassing in hindsight:
// every single thing in this project that was ever found - the frame hook, the
// player position, the shape list, the five thousand block names - was found by
// walking the object graph from the game root looking for a value we already
// knew. Not one was found by scanning memory for a shape.
//
// The last attempt scanned for a block id and got 2,698 hits in 384 MB, which
// is what pure chance gives you for any 16-bit value at that size. The gaps
// between them were all multiples of eight and every hit sat at the same offset
// inside an 8-byte-aligned object, because what it had actually found was the
// byte pattern sitting inside thousands of .NET object headers. There was never
// a signal in there.
//
// So: back to what works. The value we already know is the chunk the player is
// standing in. The client cannot draw the world without a map from chunk
// coordinate to chunk, so somewhere in the graph below the game instance there
// are two integers holding exactly that, and they change when you walk into the
// next chunk. That is a known, changing, checkable value - the only kind of
// lever that has ever worked here.
//
// Nothing here is guessed at: a candidate has to hold the right pair now AND
// the right pair after you have walked somewhere else, or it is dropped.

namespace {

struct ChunkCand {
    uintptr_t obj = 0;
    uint32_t offX = 0, offZ = 0;
    int div = 32;
};

std::vector<ChunkCand> g_chunkCands;
int g_chunkPass = 0;
int g_chunkAtX = 0, g_chunkAtZ = 0;     // chunk the last pass was taken in

constexpr size_t kGraphMaxObjects = 300000;
constexpr uint32_t kObjWindow = 512;
constexpr int kGraphDepth = 7;

int FloorDivInt(int v, int by) {
    return (v >= 0) ? (v / by) : -(((-v) + by - 1) / by);
}

// Walks out from the game instance, breadth first, collecting object addresses.
// Depth-limited rather than exhaustive: the world is near the root, and going
// deeper mostly finds the same leaves by longer paths.
void WalkGraph(uintptr_t root, std::vector<uintptr_t>& objects) {
    std::vector<uintptr_t> frontier{ root };
    std::vector<uintptr_t> seen{ root };
    std::sort(seen.begin(), seen.end());
    objects.push_back(root);

    std::vector<uint8_t> buf(kObjWindow);

    for (int depth = 0; depth < kGraphDepth && !frontier.empty(); ++depth) {
        std::vector<uintptr_t> next;
        for (uintptr_t obj : frontier) {
            if (g_shuttingDown.load()) return;
            if (objects.size() >= kGraphMaxObjects) return;

            size_t got = 0;
            for (size_t len = kObjWindow; len >= 32; len >>= 1) {
                if (SafeRead((const void*)obj, buf.data(), len)) { got = len; break; }
            }
            if (!got) continue;

            for (size_t k = 8; k + 8 <= got; k += 8) {
                uint64_t p = 0;
                memcpy(&p, buf.data() + k, 8);
                if (!p || (p & 7) || !IsPlausiblePtr((const void*)p)) continue;

                const uintptr_t child = (uintptr_t)p;
                auto it = std::lower_bound(seen.begin(), seen.end(), child);
                if (it != seen.end() && *it == child) continue;
                seen.insert(it, child);
                objects.push_back(child);
                next.push_back(child);
                if (objects.size() >= kGraphMaxObjects) return;
            }
        }
        frontier.swap(next);
    }
}

// Every place in this object where the two chunk coordinates sit close enough
// together to plausibly be one pair.
void PairsIn(const uint8_t* buf, size_t got, uintptr_t obj, int cx, int cz, int div,
    std::vector<ChunkCand>& out) {

    for (size_t a = 0; a + 4 <= got; a += 4) {
        int32_t va = 0;
        memcpy(&va, buf + a, 4);
        if (va != cx) continue;
        for (size_t b = 0; b + 4 <= got; b += 4) {
            if (b == a) continue;
            if (b > a + 64 || (a > 64 && b + 64 < a)) continue;
            int32_t vb = 0;
            memcpy(&vb, buf + b, 4);
            if (vb != cz) continue;
            out.push_back(ChunkCand{ obj, (uint32_t)a, (uint32_t)b, div });
            if (out.size() >= 20000) return;
        }
    }
}

} // namespace

void WorldChunkHunt() {
    if (g_worldScanning.exchange(true)) return;

    StartWorker([]() {
        WorkerGuard guard;

        const GsdkPlayerInfo me = GsdkGetPlayerInfo();
        if (!me.valid || !me.gameInstance) {
            SetStatus("the game instance is not resolved yet - let the Position "
                "tab settle first, then try again.");
            g_worldScanning.store(false);
            return;
        }

        const int px = (int)std::floor(me.x);
        const int pz = (int)std::floor(me.z);

        int pass = 0;
        std::vector<ChunkCand> prev;
        int wasX = 0, wasZ = 0;
        {
            std::lock_guard lock(g_wrMtx);
            pass = g_chunkPass;
            prev = g_chunkCands;
            wasX = g_chunkAtX; wasZ = g_chunkAtZ;
        }

        // Second pass and later: no walking, just check whether the candidates
        // followed us. A field that held the old chunk and now holds the new one
        // is the chunk coordinate; anything else was a coincidence.
        if (pass > 0 && !prev.empty()) {
            if (FloorDivInt(px, 32) == FloorDivInt(wasX, 32) &&
                FloorDivInt(pz, 32) == FloorDivInt(wasZ, 32)) {
                SetStatus("still in the same chunk - walk at least 32 blocks and "
                    "press it again, or there is nothing new to tell candidates "
                    "apart by.");
                g_worldScanning.store(false);
                return;
            }

            std::vector<ChunkCand> keep;
            for (const ChunkCand& c : prev) {
                const int cx = FloorDivInt(px, c.div);
                const int cz = FloorDivInt(pz, c.div);
                int32_t a = 0, b = 0;
                if (!SafeRead((const void*)(c.obj + c.offX), &a, 4)) continue;
                if (!SafeRead((const void*)(c.obj + c.offZ), &b, 4)) continue;
                if (a != cx || b != cz) continue;
                keep.push_back(c);
            }

            std::string report;
            char line[300];
            snprintf(line, sizeof(line),
                "pass %d: now at %d, %d - chunk %d, %d (by 32) / %d, %d (by 16)\n"
                "%zu candidate(s) followed, %zu dropped\n\n",
                pass + 1, px, pz,
                FloorDivInt(px, 32), FloorDivInt(pz, 32),
                FloorDivInt(px, 16), FloorDivInt(pz, 16),
                keep.size(), prev.size() - keep.size());
            report = line;

            for (size_t i = 0; i < keep.size() && i < 30; ++i) {
                snprintf(line, sizeof(line),
                    "  0x%llX  x at +0x%X, z at +0x%X, chunks of %d\n",
                    (unsigned long long)keep[i].obj, keep[i].offX, keep[i].offZ,
                    keep[i].div);
                report += line;
            }
            if (keep.size() > 30) {
                snprintf(line, sizeof(line), "  ... and %zu more\n", keep.size() - 30);
                report += line;
            }

            {
                std::lock_guard lock(g_wrMtx);
                g_chunkCands = keep;
                g_chunkPass = pass + 1;
                g_chunkAtX = px; g_chunkAtZ = pz;
                g_reportText = report;
            }

            if (keep.empty()) {
                SetStatus("nothing followed you into the new chunk. Start over - "
                    "and if it happens twice, the client is not keeping chunk "
                    "coordinates as plain integers where this can see them.");
                std::lock_guard lock(g_wrMtx);
                g_chunkPass = 0;
            }
            else if (keep.size() <= 40) {
                snprintf(line, sizeof(line),
                    "%zu object(s) tracked your chunk across %d moves. Those are "
                    "the chunk holders - walk to one more chunk to be sure, then "
                    "we go looking for the blocks inside them.",
                    keep.size(), pass + 1);
                SetStatus(line);
            }
            else {
                snprintf(line, sizeof(line),
                    "%zu left. Walk into another chunk and press it again.",
                    keep.size());
                SetStatus(line);
            }
            g_worldScanning.store(false);
            return;
        }

        // First pass: walk the graph and collect everything holding the pair.
        SetStatus("walking the object graph from the game instance...");
        std::vector<uintptr_t> objects;
        WalkGraph(me.gameInstance, objects);

        SetStatus("looking for your chunk coordinate...");
        std::vector<ChunkCand> found;
        std::vector<uint8_t> buf(kObjWindow);

        for (uintptr_t obj : objects) {
            if (g_shuttingDown.load()) break;
            size_t got = 0;
            for (size_t len = kObjWindow; len >= 32; len >>= 1) {
                if (SafeRead((const void*)obj, buf.data(), len)) { got = len; break; }
            }
            if (!got) continue;

            // Sections are 32 and the player info divides by 16, and it has
            // never been established which the client itself uses. Both, then,
            // and the passes that follow will say which survives.
            for (int div : { 32, 16 }) {
                PairsIn(buf.data(), got, obj, FloorDivInt(px, div),
                    FloorDivInt(pz, div), div, found);
            }
            if (found.size() >= 20000) break;
        }

        std::string report;
        char line[300];
        snprintf(line, sizeof(line),
            "pass 1: at %d, %d - chunk %d, %d (by 32) / %d, %d (by 16)\n"
            "%zu object(s) reachable from the game instance\n"
            "%zu hold that pair somewhere\n\n"
            "Now WALK into a different chunk - at least 32 blocks - and press it "
            "again. Whatever follows you is the real thing.\n",
            px, pz, FloorDivInt(px, 32), FloorDivInt(pz, 32),
            FloorDivInt(px, 16), FloorDivInt(pz, 16),
            objects.size(), found.size());
        report = line;

        {
            std::lock_guard lock(g_wrMtx);
            g_chunkCands = found;
            g_chunkPass = 1;
            g_chunkAtX = px; g_chunkAtZ = pz;
            g_reportText = report;
        }

        if (found.empty()) {
            SetStatus("nothing below the game instance holds your chunk "
                "coordinate as a pair of integers. Either it is stored some "
                "other way, or the world hangs off something this walk does not "
                "reach.");
            std::lock_guard lock(g_wrMtx);
            g_chunkPass = 0;
        }
        else {
            snprintf(line, sizeof(line),
                "%zu candidate(s) from %zu objects. Now walk at least 32 blocks "
                "into another chunk and press it again.",
                found.size(), objects.size());
            SetStatus(line);
        }

        g_worldScanning.store(false);
        });
}

void WorldChunkForget() {
    std::lock_guard lock(g_wrMtx);
    g_chunkCands.clear();
    g_chunkPass = 0;
    g_statusText = "chunk hunt reset";
    g_reportText.clear();
}

int WorldChunkPass() { std::lock_guard lock(g_wrMtx); return g_chunkPass; }
size_t WorldChunkCandidates() { std::lock_guard lock(g_wrMtx); return g_chunkCands.size(); }

// =============================================================================
// What is next to the chunk coordinate
// =============================================================================
// The walk found objects whose fields track the chunk the player is standing
// in. Most of the candidates it reports are the same few addresses seen through
// different object bases - the graph walk treats every pointer target as an
// object and reads a window from it, so one field gets found from half a dozen
// starts at different offsets. Adding the offsets up collapses forty-one
// candidates into about three real places.
//
// From a chunk object the blocks are a field or two away, and unlike everything
// scanned for so far they have a shape that can be recognised without guessing:
// a managed array, whose length is written down right next to its MethodTable.
// A 32x32x32 section is 32768 blocks, so an array of 32768, or 16384, or 4096
// if the indices are packed smaller, is the thing. Nothing here has to know how
// wide an entry is - it only has to find an array of about the right size and
// say so.

namespace {

struct ArrayHit {
    uintptr_t via = 0;        // the field pointing at it
    uint32_t  offset = 0;     // where that field sits in the object
    uintptr_t arr = 0;
    int64_t   len = 0;
    int       depth = 0;

    int namedPct = 0;         // of the non-empty entries, how many are real blocks
    int emptyPct = 0;
    int distinct = 0;
    int sampledSolid = 0;
};

// Does this array read like world? We have the whole block name registry, so
// this is answerable rather than arguable: take a sample, throw out the zeros,
// and see how many of the rest are ids the registry knows. World comes back at
// nearly a hundred percent. The hash table that the old length-based ranking
// picked came back at one.
void ScoreAsBlocks(ArrayHit& a) {
    const size_t want = (size_t)(std::min<int64_t>(a.len, 4096));
    if (want < 16) return;

    std::vector<uint16_t> sample(want, 0);
    if (!SafeRead((const void*)(a.arr + 0x10), sample.data(), want * 2)) return;

    int solid = 0, named = 0;
    std::vector<uint16_t> seen;
    seen.reserve(want);
    for (uint16_t v : sample) {
        if (!v) continue;                       // empty is empty in any world
        ++solid;
        if (!BuildGetBlockName(v).empty()) ++named;
        auto it = std::lower_bound(seen.begin(), seen.end(), v);
        if (it == seen.end() || *it != v) seen.insert(it, v);
    }

    a.sampledSolid = solid;
    a.distinct = (int)seen.size();
    a.emptyPct = (int)(((want - (size_t)solid) * 100) / want);
    a.namedPct = solid ? (named * 100 / solid) : 0;
}

// A managed array is a MethodTable pointer, then an 8-byte length, then the
// elements. Checking the length is what makes this reliable: random pointers
// almost never have a sane count sitting behind them.
bool LooksLikeArray(uintptr_t p, int64_t& lenOut) {
    uint64_t mt = 0;
    if (!SafeRead((const void*)p, &mt, 8)) return false;
    if (!mt || (mt & 7) || !IsPlausiblePtr((const void*)mt)) return false;
    int64_t n = 0;
    if (!SafeRead((const void*)(p + 8), &n, 8)) return false;
    if (n < 16 || n > (1 << 22)) return false;
    lenOut = n;
    return true;
}

void CollectArrays(uintptr_t obj, int depth, int maxDepth,
    std::vector<ArrayHit>& out, std::vector<uintptr_t>& seen) {

    if (depth > maxDepth) return;
    if (out.size() >= 256) return;

    auto it = std::lower_bound(seen.begin(), seen.end(), obj);
    if (it != seen.end() && *it == obj) return;
    seen.insert(it, obj);

    uint8_t buf[512];
    size_t got = 0;
    for (size_t len = sizeof(buf); len >= 32; len >>= 1) {
        if (SafeRead((const void*)obj, buf, len)) { got = len; break; }
    }
    if (!got) return;

    for (size_t k = 8; k + 8 <= got; k += 8) {
        uint64_t p = 0;
        memcpy(&p, buf + k, 8);
        if (!p || (p & 7) || !IsPlausiblePtr((const void*)p)) continue;

        int64_t n = 0;
        if (LooksLikeArray((uintptr_t)p, n)) {
            out.push_back(ArrayHit{ obj, (uint32_t)k, (uintptr_t)p, n, depth });
            if (out.size() >= 256) return;
        }
        if (depth < maxDepth) CollectArrays((uintptr_t)p, depth + 1, maxDepth, out, seen);
    }
}

} // namespace

void WorldSectionNote(uintptr_t arr, int64_t len);

void WorldChunkInspect() {
    if (g_worldScanning.exchange(true)) return;

    StartWorker([]() {
        WorkerGuard guard;

        std::vector<ChunkCand> cands;
        { std::lock_guard lock(g_wrMtx); cands = g_chunkCands; }

        if (cands.empty()) {
            SetStatus("nothing to look at yet - do the walking first.");
            g_worldScanning.store(false);
            return;
        }

        // Collapse to distinct places. Two candidates that resolve to the same
        // absolute address are one finding seen twice, and the smallest offset
        // is the closest thing to the real object base we have.
        struct Site { uintptr_t addrX, addrZ, obj; uint32_t off; int div; };
        std::vector<Site> sites;
        for (const ChunkCand& c : cands) {
            const uintptr_t ax = c.obj + c.offX;
            const uintptr_t az = c.obj + c.offZ;
            bool merged = false;
            for (Site& s : sites) {
                if (s.addrX != ax || s.addrZ != az) continue;
                if (c.offX < s.off) { s.obj = c.obj; s.off = c.offX; }
                merged = true;
                break;
            }
            if (!merged) sites.push_back(Site{ ax, az, c.obj, c.offX, c.div });
        }

        SetStatus("looking for block arrays next to the chunk coordinate...");

        std::string report;
        char line[320];
        snprintf(line, sizeof(line),
            "%zu candidate(s) collapse to %zu distinct place(s)\n\n",
            cands.size(), sites.size());
        report = line;

        for (size_t i = 0; i < sites.size() && i < 12; ++i) {
            const Site& s = sites[i];
            int32_t vx = 0, vz = 0;
            SafeRead((const void*)s.addrX, &vx, 4);
            SafeRead((const void*)s.addrZ, &vz, 4);

            snprintf(line, sizeof(line),
                "place %zu: chunk x at 0x%llX (=%d), z at 0x%llX (=%d), by %d\n"
                "  nearest object base 0x%llX, field at +0x%X\n",
                i + 1, (unsigned long long)s.addrX, vx,
                (unsigned long long)s.addrZ, vz, s.div,
                (unsigned long long)s.obj, s.off);
            report += line;

            std::vector<ArrayHit> arrays;
            std::vector<uintptr_t> seen;
            CollectArrays(s.obj, 0, 3, arrays, seen);

            // Rank by what is INSIDE each array, not by how long it is. The
            // first version of this ranked by length and led straight to an
            // array of exactly 32768 that turned out to be a hash table - 32768
            // is a power of two, and bucket arrays are always a power of two, so
            // the number proved nothing at all.
            //
            // What does prove something is that we already have the block name
            // registry. A section of world is a few thousand values that nearly
            // all resolve to a real block name; a hash table resolved about one
            // in a hundred. That is not a close call, and it needs no guess
            // about lengths, widths or compression.
            for (ArrayHit& a : arrays) ScoreAsBlocks(a);

            std::stable_sort(arrays.begin(), arrays.end(),
                [](const ArrayHit& a, const ArrayHit& b) {
                    if (a.namedPct != b.namedPct) return a.namedPct > b.namedPct;
                    return a.len > b.len;
                });

            if (arrays.empty()) {
                report += "  no arrays below it\n\n";
                continue;
            }
            snprintf(line, sizeof(line),
                "  %zu array(s) below it, best-looking first:\n", arrays.size());
            report += line;

            for (size_t k = 0; k < arrays.size() && k < 10; ++k) {
                const ArrayHit& a = arrays[k];
                const char* mark = (a.namedPct >= 80 && a.sampledSolid >= 32)
                    ? "  <== reads like block ids" : "";

                // Keep the best one so the next step has something to read
                // without being told which.
                if (k == 0 && a.namedPct >= 80 && a.sampledSolid >= 32)
                    WorldSectionNote(a.arr, a.len);

                snprintf(line, sizeof(line),
                    "    0x%llX  %lld long  %d%% named, %d%% empty, %d distinct"
                    "  (from 0x%llX+0x%X, depth %d)%s\n",
                    (unsigned long long)a.arr, (long long)a.len,
                    a.namedPct, a.emptyPct, a.distinct,
                    (unsigned long long)a.via, a.offset, a.depth, mark);
                report += line;
            }
            report += "\n";
        }

        { std::lock_guard lock(g_wrMtx); g_reportText = report; }

        SetStatus("arrays ranked by how much of each one resolves to a real "
            "block name. Anything near 100% named is the world; the rest is "
            "bookkeeping that happens to be the right size.");
        g_worldScanning.store(false);
        });
}

// =============================================================================
// Reading the section
// =============================================================================
// An array of exactly 32768 turned up two hops below a chunk coordinate, which
// is one 32 x 32 x 32 section with nothing packed. Two hops the other way, in
// the same walk, sat an array of exactly 5775 - the length of the block name
// registry that was confirmed weeks of work ago. That number appearing on its
// own is the best correctness check this search has produced, and it is why
// this is worth reading rather than scanning around further.
//
// Reading it settles the question outright. If those 32768 shorts are block
// ids, nearly all of them resolve to real names out of the registry and a
// handful of values account for most of the volume, the way a chunk of world
// actually looks. If they are something else, almost nothing resolves and the
// report says so plainly.
//
// Then the only thing left is which way round the three axes go, and that is
// not guessed at either: the blocks placed this session are known ids at known
// coordinates, so every ordering is tried and the one that reads them all back
// correctly wins. If none does, none is adopted.

namespace {

uintptr_t g_sectionArr = 0;
int64_t   g_sectionLen = 0;

struct Order { const char* name; int64_t sx, sy, sz; };

// Every way three axes can be nested. Minecraft-like formats use y, z, x; the
// server source is not explicit about it and there is no reason to assume.
std::vector<Order> OrdersFor(int edge) {
    const int64_t e = edge, e2 = (int64_t)edge * edge;
    return {
        { "y,z,x", 1,  e2, e  },
        { "y,x,z", e,  e2, 1  },
        { "z,y,x", 1,  e,  e2 },
        { "z,x,y", e,  1,  e2 },
        { "x,y,z", e2, e,  1  },
        { "x,z,y", e2, 1,  e  },
    };
}

int FloorDivN(int v, int n) { return (v >= 0) ? (v / n) : -(((-v) + n - 1) / n); }

} // namespace

void WorldSectionNote(uintptr_t arr, int64_t len) {
    std::lock_guard lock(g_wrMtx);
    g_sectionArr = arr;
    g_sectionLen = len;
}

void WorldReadSection() {
    if (g_worldScanning.exchange(true)) return;

    StartWorker([]() {
        WorkerGuard guard;

        uintptr_t arr = 0;
        int64_t len = 0;
        {
            std::lock_guard lock(g_wrMtx);
            arr = g_sectionArr;
            len = g_sectionLen;
        }
        // Any cube will do. Sections are 32 on this build, but nothing here
        // needs that to be true - the edge comes from the length.
        int edge = 0;
        for (int e = 8; e <= 64; ++e) if ((int64_t)e * e * e == len) { edge = e; break; }
        if (!arr || !edge) {
            SetStatus("no array that reads like blocks yet - press the button "
                "above this one and look for one near 100% named.");
            g_worldScanning.store(false);
            return;
        }

        SetStatus("reading the section...");

        // Elements start after the MethodTable and the length.
        std::vector<uint16_t> cells((size_t)len, 0);
        const uintptr_t base = arr + 0x10;
        for (size_t off = 0; off < cells.size(); off += 4096) {
            const size_t n = (std::min)((size_t)4096, cells.size() - off);
            if (!SafeRead((const void*)(base + off * 2), cells.data() + off, n * 2)) {
                SetStatus("that array could not be read all the way through.");
                g_worldScanning.store(false);
                return;
            }
        }

        // What is actually in it. A chunk of world is a few values repeated
        // thousands of times, not 32768 different numbers.
        std::vector<std::pair<uint16_t, int>> counts;
        {
            std::vector<uint32_t> tally(65536, 0);
            for (uint16_t v : cells) ++tally[v];
            for (uint32_t v = 0; v < 65536; ++v)
                if (tally[v]) counts.push_back({ (uint16_t)v, (int)tally[v] });
        }
        std::stable_sort(counts.begin(), counts.end(),
            [](const std::pair<uint16_t, int>& a, const std::pair<uint16_t, int>& b) {
                return a.second > b.second;
            });

        int named = 0, inRange = 0;
        for (const auto& c : counts) {
            if (c.first < 5775) inRange += c.second;
            if (!BuildGetBlockName(c.first).empty()) named += c.second;
        }

        std::string report;
        char line[320];
        snprintf(line, sizeof(line),
            "section at 0x%llX, %lld entries of 2 bytes\n"
            "%zu distinct value(s)\n"
            "%d of %lld are inside the id range, %d resolve to a name\n\n"
            "the most common:\n",
            (unsigned long long)arr, (long long)len, counts.size(), inRange,
            (long long)len, named);
        report = line;

        for (size_t i = 0; i < counts.size() && i < 14; ++i) {
            std::string nm = BuildGetBlockName(counts[i].first);
            if (nm.empty()) nm = "(no name)";
            snprintf(line, sizeof(line), "  %5u  x%-6d  %s\n",
                counts[i].first, counts[i].second, nm.c_str());
            report += line;
        }

        if ((int64_t)named * 4 < len) {
            report += "\nMost of this does not resolve to a block name, so it is "
                      "not block ids.\n";
            { std::lock_guard lock(g_wrMtx); g_reportText = report; }
            SetStatus("that array is not blocks - too little of it resolves to a "
                "name. Nothing has been adopted.");
            g_worldScanning.store(false);
            return;
        }

        // Which way round the axes go. Every block placed this session is a
        // known id at a known coordinate, so the orderings can be scored
        // against them rather than picked.
        const std::vector<BlockProbe> probes = BuildGetProbes();
        report += "\nfitting the axes against blocks placed this session:\n";

        int bestScore = -1, bestOx = 0, bestOy = 0, bestOz = 0;
        const Order* bestOrder = nullptr;
        int bestOfTotal = 0;

        for (const Order& o : OrdersFor(edge)) {
            // The section origin is unknown in y, so take it from each probe in
            // turn and let the scoring decide.
            for (const BlockProbe& seed : probes) {
                const int ox = FloorDivN(seed.x, edge) * edge;
                const int oy = FloorDivN(seed.y, edge) * edge;
                const int oz = FloorDivN(seed.z, edge) * edge;

                int hit = 0, total = 0;
                for (const BlockProbe& p : probes) {
                    if (FloorDivN(p.x, edge) * edge != ox) continue;
                    if (FloorDivN(p.y, edge) * edge != oy) continue;
                    if (FloorDivN(p.z, edge) * edge != oz) continue;
                    ++total;
                    const int64_t idx = (int64_t)(p.x - ox) * o.sx +
                                        (int64_t)(p.y - oy) * o.sy +
                                        (int64_t)(p.z - oz) * o.sz;
                    if (idx < 0 || idx >= len) continue;
                    const uint32_t got = cells[(size_t)idx];
                    // Either id space: the packet counts ten above the registry.
                    if (got == p.id || got + 10 == p.id || got == p.id + 10) ++hit;
                }
                if (total >= 2 && hit > bestScore) {
                    bestScore = hit; bestOfTotal = total; bestOrder = &o;
                    bestOx = ox; bestOy = oy; bestOz = oz;
                }
            }
        }

        if (!bestOrder || bestScore <= 0) {
            snprintf(line, sizeof(line),
                "  no ordering read any of the %zu known block(s) back "
                "correctly.\n  Most likely this is a different section from the "
                "one they are in - place a few blocks where you are standing "
                "now and press this again.\n", probes.size());
            report += line;
            { std::lock_guard lock(g_wrMtx); g_reportText = report; }
            SetStatus("the array reads like blocks, but none of the known "
                "coordinates land in it. Place a few blocks right where you are "
                "and press this again.");
            g_worldScanning.store(false);
            return;
        }

        snprintf(line, sizeof(line),
            "  %s explains %d of %d, section starting at %d, %d, %d\n",
            bestOrder->name, bestScore, bestOfTotal, bestOx, bestOy, bestOz);
        report += line;

        const bool sure = (bestScore == bestOfTotal && bestOfTotal >= 3);
        if (sure) {
            ChunkLayout L;
            L.found = true;
            L.base = base;
            L.originX = bestOx; L.originY = bestOy; L.originZ = bestOz;
            L.width = 2;
            L.sx = bestOrder->sx; L.sy = bestOrder->sy; L.sz = bestOrder->sz;
            L.edge = edge;
            L.explains = bestScore;
            std::lock_guard lock(g_wrMtx);
            g_layout = L;
            g_reportText = report;
        }
        else {
            std::lock_guard lock(g_wrMtx);
            g_reportText = report;
        }

        if (sure) {
            SetStatus("blocks can be read. Every known coordinate reads back "
                "correctly, so the section is adopted and a region inside it can "
                "be copied.");
        }
        else {
            snprintf(line, sizeof(line),
                "best ordering only explains %d of %d, which is not enough to "
                "adopt. Place three or four blocks where you are standing and "
                "press this again.", bestScore, bestOfTotal);
            SetStatus(line);
        }

        g_worldScanning.store(false);
        });
}

// =============================================================================
// Fitting the array to coordinates
// =============================================================================
// The array hanging off the chunk object reads like world - every non-empty
// value resolves to a real block name - but its length is 137065, which is not
// a cube, not a power of two and not any chunk dimension. So the layout is not
// going to be guessed from the size, and after five searches built on guesses
// about shape that is a relief rather than a problem.
//
// It does not need to be guessed. Place a block and one entry changes, which
// says what index that coordinate has. Place another one step east and the gap
// between the two indices IS the x stride. Same for up and for south. Four
// blocks and the whole mapping falls out, with nothing assumed about ordering,
// packing or where the array begins.
//
// The first version of this watched ONE array - whichever scored best - and
// reported "0 entries changed", because scoring best at looking like world is
// not the same as being the chunk you are standing in. So it watches all of
// them now and lets the placement decide. A block landing in exactly one of
// thirty candidates is a far better answer than any ranking, and it costs
// nothing extra: the person still presses once and places four blocks.

namespace {

struct FitSample { int x, y, z; int64_t index; };

struct FitCand {
    uintptr_t arr = 0;
    int64_t len = 0;
    int namedPct = 0;
    std::vector<uint16_t> snap;
};

std::vector<FitCand>   g_fitCands;
std::vector<FitSample> g_fitSamples;
uintptr_t g_fitArr = 0;            // the one a block actually landed in
int64_t   g_fitLen = 0;
bool g_fitArmed = false;
bool g_fitHaveX = false, g_fitHaveY = false, g_fitHaveZ = false;

bool ReadArrayInto(uintptr_t arr, int64_t len, std::vector<uint16_t>& out) {
    out.assign((size_t)len, 0);
    const uintptr_t base = arr + 0x10;
    for (size_t off = 0; off < out.size(); off += 4096) {
        const size_t n = (std::min)((size_t)4096, out.size() - off);
        if (!SafeRead((const void*)(base + off * 2), out.data() + off, n * 2))
            return false;
    }
    return true;
}

// How many elements one step on an axis is. Only pairs differing on a single
// axis can say anything, and a stride two pairs disagree about is not adopted.
bool StrideFor(const std::vector<FitSample>& s, int axis, int64_t& out) {
    bool have = false;
    int64_t got = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        for (size_t j = i + 1; j < s.size(); ++j) {
            const int d[3] = { s[j].x - s[i].x, s[j].y - s[i].y, s[j].z - s[i].z };
            int moved = -1;
            bool single = true;
            for (int a = 0; a < 3; ++a) {
                if (!d[a]) continue;
                if (moved >= 0) { single = false; break; }
                moved = a;
            }
            if (!single || moved != axis || !d[axis]) continue;
            const int64_t diff = s[j].index - s[i].index;
            if (diff % d[axis]) return false;
            const int64_t st = diff / d[axis];
            if (have && st != got) return false;
            got = st; have = true;
        }
    }
    if (!have) return false;
    out = got;
    return true;
}

void FitAfterPlacement(int x, int y, int z, uint32_t id);

} // namespace

void WorldFitAddCandidate(uintptr_t arr, int64_t len, int namedPct) {
    std::lock_guard lock(g_wrMtx);
    if (g_fitCands.size() >= 48) return;
    for (const FitCand& c : g_fitCands) if (c.arr == arr) return;
    FitCand c;
    c.arr = arr; c.len = len; c.namedPct = namedPct;
    g_fitCands.push_back(std::move(c));
}

void WorldFitClearCandidates() {
    std::lock_guard lock(g_wrMtx);
    g_fitCands.clear();
}

size_t WorldFitCandidateCount() {
    std::lock_guard lock(g_wrMtx);
    return g_fitCands.size();
}

void WorldAutoFind();

void WorldFitArm() {
    if (g_worldScanning.exchange(true)) return;

    StartWorker([]() {
        WorkerGuard guard;

        std::vector<FitCand> cands;
        { std::lock_guard lock(g_wrMtx); cands = g_fitCands; }

        if (cands.empty()) {
            // Pressing the wrong button should not be a thing a person has to
            // diagnose. Go and find them, then carry on as if it had been
            // pressed.
            SetStatus("finding the world first...");
            g_worldScanning.store(false);
            WorldAutoFind();
            while (g_worldScanning.load() && !g_shuttingDown.load()) Sleep(50);
            if (g_worldScanning.exchange(true)) return;
            { std::lock_guard lock(g_wrMtx); cands = g_fitCands; }
        }
        if (cands.empty()) {
            SetStatus("nothing below the game instance reads like world. Open the "
                "Position tab, let it resolve, and try again.");
            g_worldScanning.store(false);
            return;
        }

        // Wait for the world to stop moving before taking the copy that the
        // placement gets measured against. This is the thing that kept the fit
        // from working: chunks stream in constantly, so a single block showed
        // up as 1683 entries changing, and no amount of filtering wins against
        // a buffer that is being rewritten while you look at it.
        //
        // Nothing is asked of the person here. It just reads the arrays twice a
        // second until two reads agree, and gives up waiting after a while
        // rather than hanging on a world that never settles.
        size_t bytes = 0;
        int settled = 0, waited = 0;
        std::vector<uint16_t> now;

        for (waited = 0; waited < 16; ++waited) {
            char busy[160];
            snprintf(busy, sizeof(busy),
                "waiting for the world to stop loading... (%d)", waited + 1);
            SetStatus(busy);

            size_t moved = 0;
            bytes = 0;
            for (FitCand& c : cands) {
                if (!ReadArrayInto(c.arr, c.len, now)) { c.snap.clear(); continue; }
                if (!c.snap.empty()) {
                    for (size_t i = 0; i < now.size() && i < c.snap.size(); ++i)
                        if (now[i] != c.snap[i]) ++moved;
                }
                c.snap = now;
                bytes += c.snap.size() * 2;
            }

            if (waited && moved < 64) { settled = (int)moved; break; }
            settled = (int)moved;
            Sleep(600);
        }

        char msg[260];
        {
            std::lock_guard lock(g_wrMtx);
            g_fitCands = std::move(cands);
            g_fitArmed = true;
            snprintf(msg, sizeof(msg),
                "watching %zu array(s), %zu KB, %d still moving. Place a block - "
                "whichever one it lands in is the world.",
                g_fitCands.size(), bytes >> 10, settled);
            g_statusText = msg;
        }
        g_worldScanning.store(false);
        });
}

void WorldFitForget() {
    std::lock_guard lock(g_wrMtx);
    g_fitSamples.clear();
    for (FitCand& c : g_fitCands) c.snap.clear();
    g_fitArmed = false;
    g_fitArr = 0; g_fitLen = 0;
    g_fitHaveX = g_fitHaveY = g_fitHaveZ = false;
    g_statusText = "fit reset";
}

bool WorldFitIsArmed() { std::lock_guard lock(g_wrMtx); return g_fitArmed; }
size_t WorldFitSamples() { std::lock_guard lock(g_wrMtx); return g_fitSamples.size(); }

std::string WorldFitNeeds() {
    std::lock_guard lock(g_wrMtx);
    if (g_layout.found) return "done";
    std::string s;
    if (!g_fitHaveX) s += "one EAST, ";
    if (!g_fitHaveY) s += "one ABOVE, ";
    if (!g_fitHaveZ) s += "one SOUTH, ";
    if (s.size() > 2) s.resize(s.size() - 2);
    return s.empty() ? "working it out" : s;
}

void WorldFitOnPlacement(int x, int y, int z, uint32_t id) {
    {
        std::lock_guard lock(g_wrMtx);
        if (!g_fitArmed) return;
        g_fitArmed = false;
    }
    // Called on the client's packet thread. Waiting for the change to land and
    // then re-reading several megabytes here would stall the connection.
    StartWorker([x, y, z, id]() {
        WorkerGuard guard;
        FitAfterPlacement(x, y, z, id);
        });
}

namespace {

// Which of the two id spaces the world keeps was never settled - the placement
// packet counts ten above the name registry - so both are accepted rather than
// picked.
bool SameBlock(uint16_t got, uint32_t placed) {
    return (uint32_t)got == placed ||
        (uint32_t)got + 10 == placed ||
        (uint32_t)got == placed + 10;
}

void FitAfterPlacement(int x, int y, int z, uint32_t id) {
    Sleep(500);

    struct Landed { uintptr_t arr; int64_t len, index; uint16_t was, now; };
    std::vector<Landed> landed;
    size_t busyArrays = 0;
    std::string report;
    char line[320];

    snprintf(line, sizeof(line), "placed at %d, %d, %d\n", x, y, z);
    report = line;

    {
        std::lock_guard lock(g_wrMtx);
        std::vector<uint16_t> now;
        for (FitCand& c : g_fitCands) {
            if (c.snap.empty()) continue;
            if (!ReadArrayInto(c.arr, c.len, now)) continue;

            // A placement is not the only thing writing here. The world streams
            // in while we wait, so this array legitimately changes in hundreds
            // of places at once and "exactly one entry changed" was never going
            // to hold - 1683 of them moved for a single block. What does hold
            // is the value: of everything that moved, we want the entry that is
            // now the block that was placed.
            std::vector<int64_t> changed, matching;
            for (size_t i = 0; i < now.size() && i < c.snap.size(); ++i) {
                if (now[i] == c.snap[i]) continue;
                changed.push_back((int64_t)i);
                if (id && SameBlock(now[i], id)) matching.push_back((int64_t)i);
            }

            // Placing into air is the ordinary case, so when several entries
            // became that block, prefer the one that was empty beforehand.
            if (matching.size() > 1) {
                std::vector<int64_t> fromAir;
                for (int64_t i : matching)
                    if (c.snap[(size_t)i] == 0) fromAir.push_back(i);
                if (fromAir.size() == 1) matching.swap(fromAir);
            }

            const bool one = (matching.size() == 1) ||
                             (matching.empty() && changed.size() == 1);
            const int64_t at = one ? (matching.empty() ? changed[0] : matching[0]) : 0;

            if (one) {
                landed.push_back(Landed{ c.arr, c.len, at,
                    c.snap[(size_t)at], now[(size_t)at] });
            }
            else if (!changed.empty()) {
                ++busyArrays;
                snprintf(line, sizeof(line),
                    "  0x%llX  %zu changed, %zu of them now hold %u\n",
                    (unsigned long long)c.arr, changed.size(), matching.size(), id);
                report += line;

                // Say what actually moved. Three rounds of this failed with a
                // report that gave no way to tell a wrong id from a wrong array
                // from a world that would not sit still, which is three rounds
                // of guessing that a dozen lines here would have prevented.
                const size_t show = (std::min)(changed.size(), (size_t)8);
                for (size_t k = 0; k < show; ++k) {
                    const int64_t i = changed[k];
                    std::string was = BuildGetBlockName(c.snap[(size_t)i]);
                    std::string nowNm = BuildGetBlockName(now[(size_t)i]);
                    snprintf(line, sizeof(line),
                        "      index %lld: %u -> %u   %s -> %s\n",
                        (long long)i, c.snap[(size_t)i], now[(size_t)i],
                        was.empty() ? "-" : was.c_str(),
                        nowNm.empty() ? "-" : nowNm.c_str());
                    report += line;
                }
            }
            c.snap = now;     // ready for the next block, no re-arming needed
        }
    }

    snprintf(line, sizeof(line),
        "%zu array(s) changed by exactly one entry, %zu changed in bulk\n\n",
        landed.size(), busyArrays);
    report += line;

    for (const Landed& l : landed) {
        std::string nm = BuildGetBlockName(l.now);
        if (nm.empty()) nm = "(no name)";
        snprintf(line, sizeof(line), "  0x%llX index %lld: %u -> %u  %s\n",
            (unsigned long long)l.arr, (long long)l.index, l.was, l.now, nm.c_str());
        report += line;
    }

    if (landed.empty()) {
        snprintf(line, sizeof(line),
            "\nNothing pinned down. The block placed was id %u, so the entry we\n"
            "want should have become %u or %u. If nothing above shows that, the\n"
            "world is not kept in any array being watched.\n",
            id, id, id >= 10 ? id - 10 : id);
        report += line;
    }

    // The array a block lands in is the world. Once one has been picked, later
    // placements only count if they land in the SAME one - a different array
    // changing is a different thing entirely and must not be mixed in.
    {
        std::lock_guard lock(g_wrMtx);
        if (!g_fitArr && landed.size() == 1) {
            g_fitArr = landed[0].arr;
            g_fitLen = landed[0].len;
        }
        for (const Landed& l : landed) {
            if (l.arr != g_fitArr) continue;
            g_fitSamples.push_back(FitSample{ x, y, z, l.index });
            break;
        }
    }

    std::vector<FitSample> s;
    uintptr_t arr = 0;
    int64_t len = 0;
    {
        std::lock_guard lock(g_wrMtx);
        s = g_fitSamples; arr = g_fitArr; len = g_fitLen;
    }

    if (!arr) {
        { std::lock_guard lock(g_wrMtx); g_reportText = report; g_fitArmed = true; }
        SetStatus("that block did not land in any of the arrays being watched. "
            "Try placing again - still armed.");
        return;
    }

    snprintf(line, sizeof(line), "\nusing 0x%llX (%lld entries), %zu sample(s)\n",
        (unsigned long long)arr, (long long)len, s.size());
    report += line;
    for (size_t i = 0; i < s.size() && i < 8; ++i) {
        snprintf(line, sizeof(line), "  %d, %d, %d -> index %lld\n",
            s[i].x, s[i].y, s[i].z, (long long)s[i].index);
        report += line;
    }

    int64_t sx = 0, sy = 0, sz = 0;
    const bool haveX = StrideFor(s, 0, sx);
    const bool haveY = StrideFor(s, 1, sy);
    const bool haveZ = StrideFor(s, 2, sz);

    snprintf(line, sizeof(line), "\nx step %s, y step %s, z step %s\n",
        haveX ? std::to_string(sx).c_str() : "not known yet",
        haveY ? std::to_string(sy).c_str() : "not known yet",
        haveZ ? std::to_string(sz).c_str() : "not known yet");
    report += line;

    if (haveX && haveY && haveZ && sx && sy && sz) {
        // The extents follow from the strides: the axis with the smallest step
        // runs until it reaches the next one along.
        int64_t st[3] = { sx, sy, sz };
        int ord[3] = { 0, 1, 2 };
        for (int a = 0; a < 3; ++a)
            for (int b = a + 1; b < 3; ++b)
                if (std::llabs(st[ord[b]]) < std::llabs(st[ord[a]]))
                    std::swap(ord[a], ord[b]);

        int64_t ext[3] = { 0, 0, 0 };
        for (int a = 0; a < 3; ++a) {
            const int64_t mine = std::llabs(st[ord[a]]);
            const int64_t next = (a < 2) ? std::llabs(st[ord[a + 1]]) : len;
            ext[ord[a]] = mine ? (next / mine) : 0;
        }

        const FitSample& r = s.front();
        ChunkLayout L;
        L.found = true;
        L.base = arr + 0x10;
        L.width = 2;
        L.sx = sx; L.sy = sy; L.sz = sz;
        L.ex = (int)ext[0]; L.ey = (int)ext[1]; L.ez = (int)ext[2];
        L.count = len;
        L.edge = L.ex ? L.ex : 32;
        L.explains = (int)s.size();
        L.originX = r.x - (int)((r.index / sx) % (L.ex ? L.ex : 1));
        L.originY = r.y - (int)((r.index / sy) % (L.ey ? L.ey : 1));
        L.originZ = r.z - (int)((r.index / sz) % (L.ez ? L.ez : 1));

        { std::lock_guard lock(g_wrMtx); g_layout = L; }

        // Check it rather than trust the arithmetic: every sample has to land
        // back on the index it came from.
        int good = 0;
        for (const FitSample& p : s) {
            uintptr_t at = 0;
            ChunkLayout cur = WorldGetLayout();
            const int64_t idx = (int64_t)(p.x - cur.originX) * cur.sx +
                                (int64_t)(p.y - cur.originY) * cur.sy +
                                (int64_t)(p.z - cur.originZ) * cur.sz;
            (void)at;
            if (idx == p.index) ++good;
        }
        snprintf(line, sizeof(line),
            "extents %d x %d x %d, origin %d, %d, %d\n%d of %zu sample(s) line up\n",
            L.ex, L.ey, L.ez, L.originX, L.originY, L.originZ, good, s.size());
        report += line;

        if (good != (int)s.size()) {
            std::lock_guard lock(g_wrMtx);
            g_layout = ChunkLayout{};
            g_fitArmed = true;
            report += "\nThe origin does not hold up, so nothing was adopted.\n";
            g_statusText = "strides found but the origin is wrong - place one more "
                "a few steps away.";
        }
        else {
            std::lock_guard lock(g_wrMtx);
            g_fitArmed = false;
            g_statusText = "blocks can be read. Mark out a selection in section A "
                "and copy it.";
        }
    }
    else {
        std::lock_guard lock(g_wrMtx);
        g_fitHaveX = haveX; g_fitHaveY = haveY; g_fitHaveZ = haveZ;
        g_fitArmed = true;      // stays armed; the snapshots were refreshed above
        char msg[240];
        snprintf(msg, sizeof(msg), "%zu block(s) placed. Still need: %s%s%s",
            s.size(),
            haveX ? "" : "one EAST of the first  ",
            haveY ? "" : "one ABOVE it  ",
            haveZ ? "" : "one SOUTH of it");
        g_statusText = msg;
    }

    { std::lock_guard lock(g_wrMtx); g_reportText = report; }
}

} // namespace

// =============================================================================
// Doing it in one press
// =============================================================================
// The walking was there to tell a real chunk object from the thousands of small
// numbers that happen to match one once. It worked, but it is no longer the
// best test available: we can now ask whether an object holds the chunk
// coordinate AND points at an array that reads as real blocks. A stray pair of
// integers will not also have a hundred thousand block names hanging off it.
// That conjunction cannot happen by accident, so there is nothing left for the
// walking to rule out.
//
// One press, and the only thing a person still has to do by hand is place four
// blocks - which cannot be automated away, because it is the only thing that
// ties an index in that array to a coordinate in the world.

void WorldAutoFind() {
    if (g_worldScanning.exchange(true)) return;

    StartWorker([]() {
        WorkerGuard guard;

        const GsdkPlayerInfo me = GsdkGetPlayerInfo();
        if (!me.valid || !me.gameInstance) {
            SetStatus("the game is not resolved yet - open the Position tab and "
                "let it settle, then press this again.");
            g_worldScanning.store(false);
            return;
        }

        const int px = (int)std::floor(me.x);
        const int pz = (int)std::floor(me.z);

        WorldFitClearCandidates();
        SetStatus("walking the object graph...");
        std::vector<uintptr_t> objects;
        WalkGraph(me.gameInstance, objects);

        SetStatus("looking for a chunk holding real blocks...");

        struct Best {
            uintptr_t obj = 0, arr = 0, via = 0;
            uint32_t coordOff = 0, arrOff = 0;
            int64_t len = 0;
            int namedPct = 0, emptyPct = 0, distinct = 0, div = 0;
        };
        Best best;
        int checked = 0, withCoord = 0;

        std::vector<uint8_t> buf(512);
        for (uintptr_t obj : objects) {
            if (g_shuttingDown.load()) break;
            size_t got = 0;
            for (size_t len = buf.size(); len >= 32; len >>= 1) {
                if (SafeRead((const void*)obj, buf.data(), len)) { got = len; break; }
            }
            if (!got) continue;
            ++checked;

            // Does this object hold the chunk we are standing in?
            uint32_t coordOff = 0;
            int div = 0;
            for (int d : { 32, 16 }) {
                const int cx = FloorDivN(px, d), cz = FloorDivN(pz, d);
                for (size_t a = 0; a + 4 <= got && !div; a += 4) {
                    int32_t va = 0;
                    memcpy(&va, buf.data() + a, 4);
                    if (va != cx) continue;
                    for (size_t b = a + 4; b + 4 <= got && b <= a + 64; b += 4) {
                        int32_t vb = 0;
                        memcpy(&vb, buf.data() + b, 4);
                        if (vb != cz) continue;
                        coordOff = (uint32_t)a;
                        div = d;
                        break;
                    }
                }
                if (div) break;
            }
            if (!div) continue;
            ++withCoord;

            // And does it point at something that reads like world? This is the
            // half that makes it certain. Two integers are cheap; a hundred
            // thousand entries that all resolve to block names are not.
            // Two hops, not one. In one session the array hung directly off
            // the chunk object and in the next it was a field further down, so
            // checking only direct fields would have missed it outright.
            std::vector<ArrayHit> found;
            std::vector<uintptr_t> seen;
            CollectArrays(obj, 0, 1, found, seen);

            for (const ArrayHit& hit : found) {
                const uintptr_t p = hit.arr;
                const int64_t n = hit.len;
                if (n < 4096) continue;          // too small to be a chunk of world

                ArrayHit a{};
                a.arr = (uintptr_t)p;
                a.len = n;
                ScoreAsBlocks(a);

                // Watch anything that even half looks like world. Watching a
                // few extra arrays costs a little memory; watching too few
                // costs a whole session, which is what happened when this
                // picked one on score alone and the block landed elsewhere.
                // The block decides, not the ranking.
                if (a.namedPct >= 60 && a.distinct >= 4)
                    WorldFitAddCandidate(a.arr, a.len, a.namedPct);

                if (a.namedPct < 90 || a.sampledSolid < 256) continue;
                // A chunk that is almost entirely one value is lighting or
                // biome data, not blocks.
                if (a.distinct < 8) continue;

                const bool better = !best.obj ||
                    a.namedPct > best.namedPct ||
                    (a.namedPct == best.namedPct && a.distinct > best.distinct);
                if (!better) continue;

                best.obj = obj;
                best.arr = a.arr;
                best.coordOff = coordOff;
                best.arrOff = hit.offset;
                best.via = hit.via;
                best.len = a.len;
                best.namedPct = a.namedPct;
                best.emptyPct = a.emptyPct;
                best.distinct = a.distinct;
                best.div = div;
            }
        }

        std::string report;
        char line[360];
        snprintf(line, sizeof(line),
            "standing at %d, %d - chunk %d, %d\n"
            "%d object(s) below the game instance, %d hold that chunk\n\n",
            px, pz, FloorDivN(px, 32), FloorDivN(pz, 32), checked, withCoord);
        report = line;

        snprintf(line, sizeof(line), "%zu array(s) worth watching\n\n",
            WorldFitCandidateCount());
        report += line;

        if (!best.obj) {
            report += "None of them points at anything that reads like blocks.\n";
            { std::lock_guard lock(g_wrMtx); g_reportText = report; }
            SetStatus("found chunk objects but no block array below them. Move "
                "somewhere with more variety around you and press again.");
            g_worldScanning.store(false);
            return;
        }

        snprintf(line, sizeof(line),
            "chunk object 0x%llX\n"
            "  chunk coordinate at +0x%X (chunks of %d)\n"
            "  blocks at 0x%llX+0x%X -> 0x%llX\n"
            "  %lld entries, %d%% named, %d%% empty, %d distinct\n\n"
            "Best-looking of them, but all %zu are being watched and whichever\n"
            "your first block lands in is the one that gets used.\n",
            (unsigned long long)best.obj, best.coordOff, best.div,
            (unsigned long long)best.via, best.arrOff, (unsigned long long)best.arr,
            (long long)best.len, best.namedPct, best.emptyPct, best.distinct,
            WorldFitCandidateCount());
        report += line;
        { std::lock_guard lock(g_wrMtx); g_reportText = report; }

        snprintf(line, sizeof(line),
            "found it: %lld blocks, %d%% of them real block names. Now press the "
            "button below and place four blocks.", (long long)best.len, best.namedPct);
        SetStatus(line);
        g_worldScanning.store(false);
        });
}

// =============================================================================
// Taking the world off the wire instead
// =============================================================================
// Five searches through memory and the honest answer is that the client does
// not keep a plain array of blocks anywhere reachable from the chunk object.
// The last candidate looked perfect - 137065 entries, every one a real block
// name - and it never changed when a block was placed, because it is the block
// STATE DEFINITION table. A table of blocks and a world of blocks read exactly
// alike, which is why "does this look like world?" could never settle it.
//
// So: the server has to send the client every chunk it draws, and the inbound
// packet hook has been installed this whole time. Whatever the client does with
// that data afterwards, the data itself goes past this point first, in the
// server's own format - and that format is not a guess, it is in the unpacked
// server source: sections of 32x32x32, a palette of distinct block ids plus a
// grid of indices into it.
//
// This does not decode anything yet. It answers the question that has to come
// first: WHICH packet carries it. Chunk data is large and arrives in bursts
// when you walk into new terrain, so it looks nothing like the small packets
// that make up the rest of the traffic.

namespace {

struct SniffType {
    uint64_t mt = 0;
    uint64_t seen = 0;
    int64_t  biggest = 0;      // longest array found hanging off it
    uint32_t atOffset = 0;
    int64_t  totalBytes = 0;
};

std::mutex g_sniffMtx;
std::vector<SniffType> g_sniffTypes;

} // namespace

std::atomic<bool> g_chunkSniff{ false };

// Runs on the client's network thread for every inbound packet, so it stays
// cheap: one short read, a look at the pointer fields, and nothing else.
void WorldSniffInbound(void* packet) {
    if (!packet) return;

    uint8_t buf[256];
    size_t got = 0;
    for (size_t len = sizeof(buf); len >= 32; len >>= 1) {
        if (SafeRead(packet, buf, len)) { got = len; break; }
    }
    if (got < 16) return;

    uint64_t mt = 0;
    memcpy(&mt, buf, 8);
    if (!mt) return;

    int64_t biggest = 0;
    uint32_t at = 0;
    for (size_t k = 8; k + 8 <= got; k += 8) {
        uint64_t p = 0;
        memcpy(&p, buf + k, 8);
        if (!p || (p & 7) || !IsPlausiblePtr((const void*)p)) continue;

        uint64_t amt = 0;
        int64_t n = 0;
        if (!SafeRead((const void*)p, &amt, 8)) continue;
        if (!amt || (amt & 7) || !IsPlausiblePtr((const void*)amt)) continue;
        if (!SafeRead((const void*)(p + 8), &n, 8)) continue;
        if (n <= 0 || n > (1 << 24)) continue;
        if (n > biggest) { biggest = n; at = (uint32_t)k; }
    }

    std::lock_guard lock(g_sniffMtx);
    for (SniffType& t : g_sniffTypes) {
        if (t.mt != mt) continue;
        ++t.seen;
        t.totalBytes += biggest;
        if (biggest > t.biggest) { t.biggest = biggest; t.atOffset = at; }
        return;
    }
    if (g_sniffTypes.size() >= 256) return;
    g_sniffTypes.push_back(SniffType{ mt, 1, biggest, at, biggest });
}

void WorldSniffStart() {
    {
        std::lock_guard lock(g_sniffMtx);
        g_sniffTypes.clear();
    }
    // Install them here rather than assuming. The first version of this only
    // switched capture on, so it listened to a hook that was not there and
    // reported "0 packets arrived" - which reads exactly like the hook being
    // unavailable on this build, and was taken to mean that. It was live all
    // along; nothing had installed it yet.
    // Only quiche now. The socket, TLS and send hooks were how the transport
    // was identified - they found TLS on one connection and QUIC on another -
    // and that job is done. Every one of them is a hook on a thread the game
    // needs, kept alive for information already obtained, so they go.
    InstallPacketHooks();
    WorldQuicHook();
    WorldQuicReset();

    // Both, not just inbound. The counters only mean something next to each
    // other - "inbound 0, outbound 0" says nothing at all, while "inbound 0,
    // outbound climbing" says the inbound address is wrong.
    g_pktCaptureIn.store(true);     // the hook only calls us while capture is on
    g_pktCaptureOut.store(true);
    g_chunkSniff.store(true);

    if (!g_pktInHooked.load()) {
        g_chunkSniff.store(false);
        SetStatus("the inbound hook did not install on this build, so nothing "
            "can arrive. Nothing to listen to.");
        return;
    }
    SetStatus("listening. Walk somewhere you have not been - chunks only arrive "
        "when new ground loads.");
}

void WorldSniffStop() {
    g_chunkSniff.store(false);

    std::vector<SniffType> types;
    { std::lock_guard lock(g_sniffMtx); types = g_sniffTypes; }

    // Biggest payload first. Chunk data dwarfs everything else on this
    // connection, so the ordering does the identifying on its own.
    std::stable_sort(types.begin(), types.end(),
        [](const SniffType& a, const SniffType& b) { return a.biggest > b.biggest; });

    std::string report;
    char line[300];
    snprintf(line, sizeof(line), "%zu kind(s) of packet arrived\n\n",
        types.size());
    report = line;

    for (size_t i = 0; i < types.size() && i < 20; ++i) {
        const SniffType& t = types[i];
        const char* mark = (t.biggest >= 4096) ? "  <== big enough to be a chunk" : "";
        snprintf(line, sizeof(line),
            "  type 0x%llX  x%llu  biggest array %lld at +0x%X%s\n",
            (unsigned long long)t.mt, (unsigned long long)t.seen,
            (long long)t.biggest, t.atOffset, mark);
        report += line;
    }

    WorldSockReport(report);
    WorldTlsReport(report);
    WorldQuicReport(report);
    WorldQuicTypeReport(report);
    WorldGrabReport(report);
    WorldTransportReport(report);
    { std::lock_guard lock(g_wrMtx); g_reportText = report; }

    if (types.empty()) {
        SetStatus("no packets arrived at all - the inbound hook is not installed "
            "on this build.");
    }
    else {
        SetStatus("listed by payload size. The chunk packet is the one carrying "
            "thousands of bytes; everything else on this connection is small.");
    }
}

// =============================================================================
// Listening at the socket instead
// =============================================================================
// Everything above depends on a byte pattern still matching this build, and
// that dependency has now cost several rounds: the inbound handler was declared
// found, installed, and live at +0x224464, and it has never once fired. A
// signature that matches the wrong function looks exactly like a signature that
// matches the right one.
//
// recv and WSARecv are exported from ws2_32. There is no pattern to drift, no
// version to track and nothing to re-derive: if the client is talking to a
// server, it is calling these. Whatever framing or compression sits on top, the
// bytes go through here first, and the format underneath is in the unpacked
// server source rather than being guessed at.
//
// This only counts and samples. It does not parse, and it does not send.

namespace {

typedef int (WSAAPI* tRecv)(SOCKET, char*, int, int);
typedef int (WSAAPI* tWSARecv)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD,
    LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

typedef int (WSAAPI* tRecvFrom)(SOCKET, char*, int, int, sockaddr*, int*);
typedef int (WSAAPI* tWSARecvFrom)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD,
    sockaddr*, LPINT, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

tRecv        oRecvFn = nullptr;
tWSARecv     oWSARecvFn = nullptr;
tRecvFrom    oRecvFromFn = nullptr;
tWSARecvFrom oWSARecvFromFn = nullptr;

// Which receive call the client actually uses. Worth knowing on its own: the
// first version hooked only recv and the non-overlapped half of WSARecv,
// reported zero, and that zero was nearly read as "there is no socket traffic"
// when it only meant "not through those two". A UDP game transport uses
// recvfrom, and anything built for throughput uses overlapped reads.
uint64_t g_useRecv = 0, g_useWSARecv = 0, g_useRecvFrom = 0, g_useWSARecvFrom = 0;
uint64_t g_overlapped = 0;

std::mutex g_sockMtx;
uint64_t g_sockCalls = 0;
uint64_t g_sockBytes = 0;
int      g_sockBiggest = 0;
std::vector<uint8_t> g_sockSample;      // the largest read seen, capped
bool g_sockHooked = false;

constexpr size_t kSockSampleMax = 4096;

void NoteBytes(const char* buf, int len) {
    if (len <= 0 || !buf) return;
    std::lock_guard lock(g_sockMtx);
    ++g_sockCalls;
    g_sockBytes += (uint64_t)len;
    if (len > g_sockBiggest) {
        g_sockBiggest = len;
        const size_t take = (std::min)((size_t)len, kSockSampleMax);
        g_sockSample.assign((const uint8_t*)buf, (const uint8_t*)buf + take);
    }
}

int WSAAPI hkRecvFn(SOCKET s, char* buf, int len, int flags) {
    const int got = oRecvFn ? oRecvFn(s, buf, len, flags) : -1;
    if (g_chunkSniff.load() && got > 0) {
        try {
            { std::lock_guard lock(g_sockMtx); ++g_useRecv; }
            NoteBytes(buf, got);
        }
        catch (...) {}
    }
    return got;
}

int WSAAPI hkRecvFromFn(SOCKET s, char* buf, int len, int flags,
    sockaddr* from, int* fromLen) {
    const int got = oRecvFromFn
        ? oRecvFromFn(s, buf, len, flags, from, fromLen) : -1;
    if (g_chunkSniff.load() && got > 0) {
        try {
            { std::lock_guard lock(g_sockMtx); ++g_useRecvFrom; }
            NoteBytes(buf, got);
        }
        catch (...) {}
    }
    return got;
}

int WSAAPI hkWSARecvFromFn(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD got,
    LPDWORD flags, sockaddr* from, LPINT fromLen, LPWSAOVERLAPPED ov,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    const int r = oWSARecvFromFn
        ? oWSARecvFromFn(s, bufs, count, got, flags, from, fromLen, ov, cr) : -1;
    if (!g_chunkSniff.load()) return r;
    try {
        std::lock_guard lock(g_sockMtx);
        ++g_useWSARecvFrom;
        if (ov) ++g_overlapped;
    }
    catch (...) {}
    if (r == 0 && got && *got && bufs && !ov) {
        try {
            DWORD left = *got;
            for (DWORD i = 0; i < count && left; ++i) {
                const DWORD take = (std::min)(left, bufs[i].len);
                NoteBytes(bufs[i].buf, (int)take);
                left -= take;
            }
        }
        catch (...) {}
    }
    return r;
}

// An overlapped read has not happened yet when WSARecv returns - the buffer
// fills later, off an IO completion port, which is why sampling at the call
// found nothing at all.
//
// It can still be read without hooking the completion path. The client reuses
// its receive buffers, so by the time it posts the NEXT read on the same
// OVERLAPPED, the previous one has completed and its bytes are still sitting
// where they landed. Remembering the buffer and looking at it one call later
// costs nothing and adds no hooks.
//
// What it cannot know is how many of those bytes were actually delivered, so
// the sample is the buffer and the report says so rather than implying a length
// it does not have.
struct PendingRead { LPWSAOVERLAPPED ov; char* buf; ULONG len; };
std::vector<PendingRead> g_sockPending;      // guarded by g_sockMtx

int WSAAPI hkWSARecvFn(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD got,
    LPDWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {

    // REMOVED: this used to read the buffer from the PREVIOUS read on this
    // OVERLAPPED, to recover overlapped bytes without hooking the completion
    // port. It is a use-after-free. On relog the connection tears down and
    // those buffers are freed, and the next read on a recycled OVERLAPPED then
    // reads freed memory - which is exactly when the game was crashing.
    //
    // It also has no purpose any more: it was how the transport got identified,
    // and the transport turned out to be QUIC, whose plaintext comes from
    // quiche directly.
    if (false) {
        try {
            char* prev = nullptr;
            ULONG prevLen = 0;
            {
                std::lock_guard lock(g_sockMtx);
                for (const PendingRead& p : g_sockPending) {
                    if (p.ov != ov) continue;
                    prev = p.buf;
                    prevLen = p.len;
                    break;
                }
            }
            if (prev && prevLen) NoteBytes(prev, (int)(std::min)(prevLen, 8192ul));
        }
        catch (...) {}
    }

    const int r = oWSARecvFn ? oWSARecvFn(s, bufs, count, got, flags, ov, cr) : -1;
    if (!g_chunkSniff.load()) return r;

    try {
        std::lock_guard lock(g_sockMtx);
        ++g_useWSARecv;
        if (ov) ++g_overlapped;

        if (ov && bufs && count) {
            bool known = false;
            for (PendingRead& p : g_sockPending) {
                if (p.ov != ov) continue;
                p.buf = bufs[0].buf;
                p.len = bufs[0].len;
                known = true;
                break;
            }
            if (!known && g_sockPending.size() < 64)
                g_sockPending.push_back(PendingRead{ ov, bufs[0].buf, bufs[0].len });
        }
    }
    catch (...) {}

    if (r == 0 && got && *got && bufs && !ov) {
        try {
            DWORD left = *got;
            for (DWORD i = 0; i < count && left; ++i) {
                const DWORD take = (std::min)(left, bufs[i].len);
                NoteBytes(bufs[i].buf, (int)take);
                left -= take;
            }
        }
        catch (...) {}
    }
    return r;
}

} // namespace

bool WorldSockHook() {
    if (g_sockHooked) return true;

    HMODULE ws = GetModuleHandleW(L"ws2_32.dll");
    if (!ws) ws = LoadLibraryW(L"ws2_32.dll");
    if (!ws) return false;

    void* pRecv = (void*)GetProcAddress(ws, "recv");
    void* pWSARecv = (void*)GetProcAddress(ws, "WSARecv");

    if (pRecv && MH_CreateHook(pRecv, &hkRecvFn, (void**)&oRecvFn) == MH_OK)
        MH_EnableHook(pRecv);
    if (pWSARecv && MH_CreateHook(pWSARecv, &hkWSARecvFn, (void**)&oWSARecvFn) == MH_OK)
        MH_EnableHook(pWSARecv);

    // UDP, which is what a game transport usually is.
    void* pRecvFrom = (void*)GetProcAddress(ws, "recvfrom");
    void* pWSARecvFrom = (void*)GetProcAddress(ws, "WSARecvFrom");
    if (pRecvFrom &&
        MH_CreateHook(pRecvFrom, &hkRecvFromFn, (void**)&oRecvFromFn) == MH_OK)
        MH_EnableHook(pRecvFrom);
    if (pWSARecvFrom &&
        MH_CreateHook(pWSARecvFrom, &hkWSARecvFromFn, (void**)&oWSARecvFromFn) == MH_OK)
        MH_EnableHook(pWSARecvFrom);

    g_sockHooked = oRecvFn || oWSARecvFn || oRecvFromFn || oWSARecvFromFn;
    return g_sockHooked;
}

std::string WorldSockStatus() {
    std::lock_guard lock(g_sockMtx);
    char buf[220];
    if (!g_sockHooked) return "socket not hooked yet";
    snprintf(buf, sizeof(buf),
        "%llu read(s), %llu KB, biggest %d bytes",
        (unsigned long long)g_sockCalls, (unsigned long long)(g_sockBytes >> 10),
        g_sockBiggest);
    return buf;
}

void WorldSockReset() {
    std::lock_guard lock(g_sockMtx);
    g_sockCalls = 0;
    g_sockBytes = 0;
    g_sockBiggest = 0;
    g_sockSample.clear();
    g_useRecv = g_useWSARecv = g_useRecvFrom = g_useWSARecvFrom = 0;
    g_overlapped = 0;
    g_sockPending.clear();
}

// Appends what the socket saw to the report, including a look at the largest
// read. Whether it is readable or compressed is the whole question, and a
// glance at the bytes answers it: plain block ids repeat, compressed data does
// not repeat at all.
void WorldSockReport(std::string& report) {
    std::lock_guard lock(g_sockMtx);
    char line[240];
    snprintf(line, sizeof(line),
        "\nsocket: %llu sample(s), biggest buffer %d bytes\n"
        "(overlapped reads, so these are buffers looked at one call late - how\n"
        "much was actually delivered is not knowable here, so the byte total\n"
        "would be made up)\n",
        (unsigned long long)g_sockCalls, g_sockBiggest);
    report += line;

    snprintf(line, sizeof(line),
        "which call: recv %llu, WSARecv %llu, recvfrom %llu, WSARecvFrom %llu"
        " (%llu overlapped)\n",
        (unsigned long long)g_useRecv, (unsigned long long)g_useWSARecv,
        (unsigned long long)g_useRecvFrom, (unsigned long long)g_useWSARecvFrom,
        (unsigned long long)g_overlapped);
    report += line;

    if (g_sockSample.empty()) {
        if (g_overlapped)
            report += "reads are overlapped, so the bytes arrive after the call "
                      "returns - they have to be collected at completion.\n";
        else
            report += "nothing came through any socket, so this world is not on "
                      "the network at all - the server is running inside this "
                      "process. Try it on a real server.\n";
        return;
    }

    // How many distinct byte values, and how much of it is zero. Compressed or
    // encrypted traffic uses nearly all 256 values evenly and has almost no
    // zeros; a world full of air has a great many.
    int counts[256] = { 0 };
    for (uint8_t b : g_sockSample) ++counts[b];
    int distinct = 0;
    for (int i = 0; i < 256; ++i) if (counts[i]) ++distinct;
    const int zeros = counts[0];

    snprintf(line, sizeof(line),
        "sample of %zu bytes: %d distinct values, %d%% zero\n",
        g_sockSample.size(), distinct,
        (int)((zeros * 100) / (int)g_sockSample.size()));
    report += line;
    // No verdict here. This buffer's delivered length is unknown, so most of
    // those zeros are unfilled tail rather than data - which is exactly how
    // "17 03 03 00 13" (a 19-byte TLS record in an 8 KB buffer) got called
    // "plain enough to read" twice. Entropy on a buffer of unknown length
    // measures the empty space.
    report += "(no verdict - the delivered length is unknown, so most of those "
              "zeros are unused buffer)\n";

    report += "first bytes: ";
    for (size_t i = 0; i < g_sockSample.size() && i < 64; ++i) {
        snprintf(line, sizeof(line), "%02X ", g_sockSample[i]);
        report += line;
    }
    report += "\n";
}

// =============================================================================
// After the decryption
// =============================================================================
// The socket carries TLS. The first bytes off the wire were 17 03 03 00 13 -
// application data, TLS 1.2, nineteen bytes - so there was never going to be
// anything readable at the socket, and the "plain enough to read" verdict was
// wrong for a reason worth recording: it measured 79% zeros and called that low
// entropy, when the zeros were the unfilled tail of an 8 KB buffer holding a
// 19-byte record. Judging entropy without knowing the delivered length measures
// the empty space.
//
// Windows TLS is SChannel, and SChannel decrypts through DecryptMessage, which
// is an ordinary export of secur32. Hooking it needs no signature and no
// version tracking, and unlike everything tried so far it hands over plaintext
// WITH its length attached - the SecBuffer says how many bytes there are.
//
// Reads only. Nothing is decrypted that the client was not already decrypting,
// nothing is sent, and no key material is touched.

namespace {

typedef SECURITY_STATUS(SEC_ENTRY* tDecryptMessage)(
    PCtxtHandle, PSecBufferDesc, ULONG, PULONG);

tDecryptMessage oDecryptMessage = nullptr;

std::mutex g_tlsMtx;
uint64_t g_tlsCalls = 0;
uint64_t g_tlsBytes = 0;
int      g_tlsBiggest = 0;
std::vector<uint8_t> g_tlsSample;       // the largest plaintext record seen
bool g_tlsHooked = false;

constexpr size_t kTlsSampleMax = 8192;

SECURITY_STATUS SEC_ENTRY hkDecryptMessage(PCtxtHandle ctx, PSecBufferDesc msg,
    ULONG seq, PULONG qop) {

    const SECURITY_STATUS r = oDecryptMessage
        ? oDecryptMessage(ctx, msg, seq, qop) : SEC_E_UNSUPPORTED_FUNCTION;

    if (r != SEC_E_OK || !g_chunkSniff.load() || !msg || !msg->pBuffers)
        return r;

    try {
        for (ULONG i = 0; i < msg->cBuffers; ++i) {
            const SecBuffer& b = msg->pBuffers[i];
            // SECBUFFER_DATA is the plaintext; the others are the record's
            // header, trailer and padding, which are of no interest.
            if (b.BufferType != SECBUFFER_DATA) continue;
            if (!b.pvBuffer || !b.cbBuffer) continue;

            std::lock_guard lock(g_tlsMtx);
            ++g_tlsCalls;
            g_tlsBytes += b.cbBuffer;
            if ((int)b.cbBuffer > g_tlsBiggest) {
                g_tlsBiggest = (int)b.cbBuffer;
                const size_t take = (std::min)((size_t)b.cbBuffer, kTlsSampleMax);
                const uint8_t* p = (const uint8_t*)b.pvBuffer;
                g_tlsSample.assign(p, p + take);
            }
        }
    }
    catch (...) {}
    return r;
}

} // namespace

bool WorldTlsHook() {
    if (g_tlsHooked) return true;

    // secur32 forwards to sspicli; either export works, so try both rather
    // than depending on which one this Windows build exposes.
    const wchar_t* dlls[] = { L"secur32.dll", L"sspicli.dll" };
    for (const wchar_t* name : dlls) {
        HMODULE m = GetModuleHandleW(name);
        if (!m) m = LoadLibraryW(name);
        if (!m) continue;
        void* p = (void*)GetProcAddress(m, "DecryptMessage");
        if (!p) continue;
        if (MH_CreateHook(p, &hkDecryptMessage, (void**)&oDecryptMessage) == MH_OK &&
            MH_EnableHook(p) == MH_OK) {
            g_tlsHooked = true;
            break;
        }
        MH_RemoveHook(p);
    }
    return g_tlsHooked;
}

void WorldTlsReset() {
    std::lock_guard lock(g_tlsMtx);
    g_tlsCalls = 0;
    g_tlsBytes = 0;
    g_tlsBiggest = 0;
    g_tlsSample.clear();
}

std::string WorldTlsStatus() {
    std::lock_guard lock(g_tlsMtx);
    char buf[200];
    if (!g_tlsHooked) return "not hooked";
    snprintf(buf, sizeof(buf), "%llu record(s), %llu KB, biggest %d bytes",
        (unsigned long long)g_tlsCalls, (unsigned long long)(g_tlsBytes >> 10),
        g_tlsBiggest);
    return buf;
}

void WorldTlsReport(std::string& report) {
    std::lock_guard lock(g_tlsMtx);
    char line[300];

    snprintf(line, sizeof(line),
        "\ndecrypted: %llu record(s), %llu KB total, biggest record %d bytes\n",
        (unsigned long long)g_tlsCalls, (unsigned long long)(g_tlsBytes >> 10),
        g_tlsBiggest);
    report += line;

    if (!g_tlsHooked) {
        report += "DecryptMessage is not hooked, so there is nothing to show.\n";
        return;
    }
    if (g_tlsSample.empty()) {
        report += "nothing decrypted through SChannel. Either the client brings "
                  "its own TLS rather than using Windows, or nothing arrived.\n";
        return;
    }

    // Entropy over the ACTUAL record this time, not a buffer with a tail of
    // unused space, so the verdict means something.
    int counts[256] = { 0 };
    for (uint8_t b : g_tlsSample) ++counts[b];
    int distinct = 0;
    for (int i = 0; i < 256; ++i) if (counts[i]) ++distinct;
    const int zeros = counts[0];
    const int n = (int)g_tlsSample.size();

    snprintf(line, sizeof(line),
        "sample of %d bytes: %d distinct values, %d%% zero\n",
        n, distinct, (zeros * 100) / n);
    report += line;
    report += (distinct > 240 && zeros * 20 < n)
        ? "still high entropy - the payload is compressed inside the TLS\n"
        : "readable - this is the protocol in the clear\n";

    report += "first bytes: ";
    for (size_t i = 0; i < g_tlsSample.size() && i < 96; ++i) {
        snprintf(line, sizeof(line), "%02X ", g_tlsSample[i]);
        report += line;
    }
    report += "\n";
}

// =============================================================================
// Which transport the game actually uses
// =============================================================================
// DecryptMessage worked and decrypted "HTTP/1.1 201 Created ... text/html" -
// a web request, not the game. And twenty WSARecv calls across thirty seconds
// of terrain streaming is nowhere near enough to be carrying chunks; that is
// the volume of an idle side-connection, which is what the HTTPS traffic is.
//
// So the game's own connection is going somewhere else, and there is no point
// guessing at a fourth API. Two cheap facts settle it: what networking
// libraries the process has loaded, and whether the send side moves at all. The
// client sends over a hundred and seventy thousand ring packets, so if those
// leave through Winsock the send counters will climb hard; if they do not, the
// transport is something else entirely and the module list will name it.

namespace {

typedef int (WSAAPI* tSend)(SOCKET, const char*, int, int);
typedef int (WSAAPI* tWSASend)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,
    LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI* tSendTo)(SOCKET, const char*, int, int, const sockaddr*, int);

tSend    oSendFn = nullptr;
tWSASend oWSASendFn = nullptr;
tSendTo  oSendToFn = nullptr;

std::atomic<uint64_t> g_sendCalls{ 0 };
std::atomic<uint64_t> g_wsaSendCalls{ 0 };
std::atomic<uint64_t> g_sendToCalls{ 0 };
std::atomic<uint64_t> g_sendBytes{ 0 };
bool g_sendHooked = false;

int WSAAPI hkSendFn(SOCKET s, const char* b, int len, int f) {
    if (g_chunkSniff.load()) {
        g_sendCalls.fetch_add(1);
        if (len > 0) g_sendBytes.fetch_add((uint64_t)len);
    }
    return oSendFn ? oSendFn(s, b, len, f) : -1;
}

int WSAAPI hkWSASendFn(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent,
    DWORD f, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    if (g_chunkSniff.load()) {
        g_wsaSendCalls.fetch_add(1);
        if (bufs && count) g_sendBytes.fetch_add(bufs[0].len);
    }
    return oWSASendFn ? oWSASendFn(s, bufs, count, sent, f, ov, cr) : -1;
}

int WSAAPI hkSendToFn(SOCKET s, const char* b, int len, int f,
    const sockaddr* to, int toLen) {
    if (g_chunkSniff.load()) {
        g_sendToCalls.fetch_add(1);
        if (len > 0) g_sendBytes.fetch_add((uint64_t)len);
    }
    return oSendToFn ? oSendToFn(s, b, len, f, to, toLen) : -1;
}

// Libraries that would explain a transport we are not watching. Anything here
// means the game is not talking through plain Winsock calls.
const wchar_t* kInteresting[] = {
    L"msquic", L"quic", L"enet", L"steam", L"libssl", L"libcrypto",
    L"boringssl", L"openssl", L"nghttp", L"winhttp", L"webrtc", L"rtc",
    L"udp", L"net", L"socket", L"rio", L"mswsock", L"ws2_32", L"iphlpapi",
};

bool Interesting(const std::wstring& name) {
    std::wstring lower;
    for (wchar_t c : name) lower += (wchar_t)towlower(c);
    for (const wchar_t* k : kInteresting)
        if (lower.find(k) != std::wstring::npos) return true;
    return false;
}

} // namespace

bool WorldSendHook() {
    if (g_sendHooked) return true;
    HMODULE ws = GetModuleHandleW(L"ws2_32.dll");
    if (!ws) return false;

    struct { const char* name; void* hook; void** orig; } wanted[] = {
        { "send",   (void*)&hkSendFn,    (void**)&oSendFn },
        { "WSASend",(void*)&hkWSASendFn, (void**)&oWSASendFn },
        { "sendto", (void*)&hkSendToFn,  (void**)&oSendToFn },
    };
    for (auto& w : wanted) {
        void* p = (void*)GetProcAddress(ws, w.name);
        if (!p) continue;
        if (MH_CreateHook(p, w.hook, w.orig) == MH_OK) MH_EnableHook(p);
    }
    g_sendHooked = oSendFn || oWSASendFn || oSendToFn;
    return g_sendHooked;
}

void WorldSendReset() {
    g_sendCalls.store(0);
    g_wsaSendCalls.store(0);
    g_sendToCalls.store(0);
    g_sendBytes.store(0);
}

void WorldTransportReport(std::string& report) {
    char line[320];
    snprintf(line, sizeof(line),
        "\nsend side: send %llu, WSASend %llu, sendto %llu, %llu KB\n",
        (unsigned long long)g_sendCalls.load(),
        (unsigned long long)g_wsaSendCalls.load(),
        (unsigned long long)g_sendToCalls.load(),
        (unsigned long long)(g_sendBytes.load() >> 10));
    report += line;

    const uint64_t total = g_sendCalls.load() + g_wsaSendCalls.load() +
        g_sendToCalls.load();
    if (total < 50)
        report += "barely any sending through Winsock, while the client queued "
                  "tens of thousands of packets - so the game is not talking "
                  "through these calls at all.\n";
    else
        report += "the game does send through Winsock, so its receive side is "
                  "here somewhere too.\n";

    // What is loaded. A transport we are not watching has to come from
    // somewhere, and this is the cheapest way to find out its name.
    report += "\nnetwork-ish modules loaded:\n";
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        report += "  could not list them\n";
        return;
    }
    const size_t count = (std::min)((size_t)(needed / sizeof(HMODULE)),
        (size_t)512);
    int shown = 0;
    for (size_t i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH];
        if (!GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH))
            continue;
        if (!Interesting(name)) continue;
        char narrow[MAX_PATH];
        const int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow,
            sizeof(narrow), nullptr, nullptr);
        if (n <= 0) continue;
        snprintf(line, sizeof(line), "  %s\n", narrow);
        report += line;
        if (++shown >= 30) break;
    }
    if (!shown) report += "  none - which is itself the answer\n";
    snprintf(line, sizeof(line), "  (%zu modules loaded in total)\n", count);
    report += line;
}

// =============================================================================
// Out of quiche, decrypted
// =============================================================================
// The transport is QUIC. Two facts from the last run name it between them:
// sendto fired 7,234 times for 857 KB, so the game talks UDP, and quiche.DLL is
// loaded - Cloudflare's QUIC. That also explains the receive side reading zero
// on every Winsock function: a QUIC stack takes WSARecvMsg through WSAIoctl
// with SIO_GET_EXTENSION_FUNCTION_POINTER rather than importing it, so there is
// no export to hook and nothing an export hook could ever have seen.
//
// Capturing the datagrams would be pointless anyway - QUIC encrypts its
// payload. But quiche is a DLL with real exports, and it hands the application
// its decrypted data through quiche_conn_stream_recv and, for the unreliable
// path a game is likely to prefer, quiche_conn_dgram_recv. Hooking those gets
// the game's own protocol in the clear, with a length, from a name rather than
// a byte pattern.
//
// The one wrinkle is that quiche_conn_stream_recv gained a parameter between
// versions - older builds take five, newer ones six. Both put the buffer and
// its length in the same places and both return the number of bytes written,
// so this declares six and passes six: on x64 a callee that takes five simply
// ignores the extra stack slot. Getting a signature wrong here would crash the
// game, so it is worth not guessing.

namespace {

typedef intptr_t(*tStreamRecv)(void* conn, uint64_t streamId, uint8_t* out,
    size_t outLen, bool* fin, uint64_t* errCode);
typedef intptr_t(*tDgramRecv)(void* conn, uint8_t* out, size_t outLen);

tStreamRecv oStreamRecv = nullptr;
tDgramRecv  oDgramRecv = nullptr;

std::mutex g_quicMtx;
uint64_t g_quicStreamCalls = 0, g_quicDgramCalls = 0;
uint64_t g_quicBytes = 0;
int      g_quicBiggest = 0;
std::vector<uint8_t> g_quicSample;
std::string g_quicFound;
bool g_quicHooked = false;

// A whole message, not a prefix. The biggest stream read was 65535 bytes, and a
// zstd frame cut short cannot be unpacked at all - capping this at 8192 would
// have guaranteed failure at the last step.
constexpr size_t kQuicSampleMax = 65536;

// =============================================================================
// Putting the messages back together
// =============================================================================
// A stream read returns at most one buffer's worth - 65535 bytes here - and the
// messages are bigger than that, so decompressing what one read returned was
// always going to fail on a third of a frame. The header says as much, once you
// read it as what it is:
//
//   C8 20 01 00  F1 00 00 00   ->  length 0x000120C8 = 73928, type 241
//   A5 1B 02 00  F1 00 00 00   ->  length 0x00021BA5 = 138149, type 241
//
// Four bytes of length, four of type, and both lengths exceed a single read.
// So a stream is a byte stream, not a sequence of messages: accumulate per
// stream id, and a message is complete once its declared length has arrived.
// Nothing here is guessed - the length is read from the message rather than
// inferred from how much happened to turn up.
struct StreamBuf {
    uint64_t id = 0;
    std::vector<uint8_t> data;
};

std::vector<StreamBuf> g_streams;          // guarded by g_quicMtx
uint64_t g_quicMessages = 0;
uint32_t g_quicLastType = 0;
uint32_t g_quicLastLen = 0;

// Per type, because the messages are not all the same thing. 24% of one
// payload resolving to block names says it is structured - headers, entities,
// whatever - rather than a flat array of ids, and the biggest message is
// whichever happened to be biggest rather than the one we want. Keeping the
// largest example of each type and scoring them separately is what says which
// type carries the world.
struct TypeStat {
    uint32_t type = 0;
    uint64_t count = 0;
    uint32_t biggest = 0;
    std::vector<uint8_t> sample;      // the largest body, still compressed
};
std::vector<TypeStat> g_quicTypes;
constexpr size_t kTypeSampleCap = 512u << 10;

// Several samples of the interesting types, each stamped with where the player
// was standing when it arrived. The headers of types 131 and 136 both read
// "01/05, 1, 4, <varies>", which looks like a chunk coordinate - but looking
// like one is exactly the sort of thing that has been wrong at every stage
// here, so it gets checked against a coordinate we already know instead.
struct Grabbed {
    uint32_t type = 0;
    std::vector<uint8_t> body;        // still compressed
    int px = 0, py = 0, pz = 0;       // where the player was
};
std::vector<Grabbed> g_grabbed;
constexpr size_t kGrabMax = 24;

// Cached on the frame tick. Reading the player from the network thread would
// mean walking the object graph while the game is mid-frame.
std::atomic<int> g_lastPX{ 0 }, g_lastPY{ 0 }, g_lastPZ{ 0 };

// Last run grabbed forty samples and thirty-nine of them were sky - uniform
// sections that compress to sixteen bytes and carry no palette worth diffing.
// One useful specimen out of forty is why the palette could not be decoded, so
// the filter now goes on SIZE: a section with real terrain in it compresses to
// hundreds of bytes, a uniform one to about thirty.
// Sections are handed off compressed and decompressed later on a thread of our
// own. The queue is small and drops rather than grows: falling behind should
// cost a missed section, never the game.
void QueueRawStream(uint64_t streamId, const uint8_t* buf, size_t len);
void WorldDrainRaw();
void QueueSectionBody(const uint8_t* body, uint32_t len);
void WorldDrainSections();

bool WantSample(uint32_t type, uint32_t msgLen) {
    if (type == 136) return msgLen >= 150;     // a real section, not sky
    if (type == 138) return msgLen >= 64;      // the likely palette message
    if (type == 131) return msgLen >= 400;
    return false;
}

constexpr size_t kStreamCap = 2u << 20;    // a runaway stream is dropped
constexpr size_t kMessageCap = 1u << 20;

// Caller holds g_quicMtx. Appends this read and pulls out every message that
// has become complete - back-to-back messages in one read are normal.
void FeedStream(uint64_t streamId, const uint8_t* buf, intptr_t len) {
    StreamBuf* sb = nullptr;
    for (StreamBuf& s : g_streams) {
        if (s.id != streamId) continue;
        sb = &s;
        break;
    }
    if (!sb) {
        if (g_streams.size() >= 32) return;
        g_streams.push_back(StreamBuf{ streamId, {} });
        sb = &g_streams.back();
    }

    sb->data.insert(sb->data.end(), buf, buf + len);
    if (sb->data.size() > kStreamCap) { sb->data.clear(); return; }

    for (;;) {
        if (sb->data.size() < 8) break;
        uint32_t msgLen = 0, msgType = 0;
        memcpy(&msgLen, sb->data.data(), 4);
        memcpy(&msgType, sb->data.data() + 4, 4);
        if (!msgLen || msgLen > kMessageCap) { sb->data.clear(); break; }
        if (sb->data.size() < (size_t)8 + msgLen) break;

        ++g_quicMessages;
        g_quicLastType = msgType;
        g_quicLastLen = msgLen;

        // Keep the biggest complete message for the report to unpack. The
        // whole message, not a prefix: a zstd frame cut short cannot be
        // decompressed at all.
        if ((int)msgLen > g_quicBiggest) {
            g_quicBiggest = (int)msgLen;
            g_quicSample.assign(sb->data.begin() + 8,
                sb->data.begin() + 8 + msgLen);
        }

        TypeStat* ts = nullptr;
        for (TypeStat& t : g_quicTypes) {
            if (t.type != msgType) continue;
            ts = &t;
            break;
        }
        if (!ts && g_quicTypes.size() < 24) {
            g_quicTypes.push_back(TypeStat{ msgType, 0, 0, {} });
            ts = &g_quicTypes.back();
        }
        // Every full section goes into the cache, whether or not it is also
        // kept as a diagnostic sample. This is what the copy reads from.
        // Copy the bytes and get out. This runs on the client's own network
        // thread, and decompressing here - a zstd call plus a megabyte of
        // allocation per message - crashed the game on relog, when the server
        // sends the entire world at once. Nothing heavy belongs on this thread.
        //
        // No size filter either. The >=150 rule exists to skip SKY for the
        // diagnostic samples, and reusing it for the cache threw away exactly
        // the sections under the player: ground that is mostly one block
        // compresses just as small as sky does.
        if (msgType == 136) QueueSectionBody(sb->data.data() + 8, msgLen);

        if (WantSample(msgType, msgLen) && g_grabbed.size() < kGrabMax &&
            msgLen <= kTypeSampleCap) {
            Grabbed g;
            g.type = msgType;
            g.body.assign(sb->data.begin() + 8, sb->data.begin() + 8 + msgLen);
            g.px = g_lastPX.load();
            g.py = g_lastPY.load();
            g.pz = g_lastPZ.load();
            g_grabbed.push_back(std::move(g));
        }

        if (ts) {
            ++ts->count;
            if (msgLen > ts->biggest && msgLen <= kTypeSampleCap) {
                ts->biggest = msgLen;
                ts->sample.assign(sb->data.begin() + 8,
                    sb->data.begin() + 8 + msgLen);
            }
        }
        sb->data.erase(sb->data.begin(), sb->data.begin() + 8 + msgLen);
    }
}

intptr_t hkStreamRecv(void* conn, uint64_t streamId, uint8_t* out,
    size_t outLen, bool* fin, uint64_t* errCode) {
    const intptr_t r = oStreamRecv
        ? oStreamRecv(conn, streamId, out, outLen, fin, errCode) : -1;
    // Copy and return. That is all this thread does now. Reassembly, message
    // framing, zstd and caching all happen on the frame tick instead, because
    // this is the client's own network thread and stalling or throwing here
    // takes the connection - and the game - with it.
    if (g_chunkSniff.load() && r > 0) {
        try { QueueRawStream(streamId, out, (size_t)r); }
        catch (...) {}
    }
    return r;
}

intptr_t hkDgramRecv(void* conn, uint8_t* out, size_t outLen) {
    const intptr_t r = oDgramRecv ? oDgramRecv(conn, out, outLen) : -1;
    if (g_chunkSniff.load() && r > 0) {
        try {
            std::lock_guard lock(g_quicMtx);
            ++g_quicDgramCalls;
            g_quicBytes += (uint64_t)r;
            // A datagram arrives whole, so there is nothing to reassemble.
            if ((int)r > g_quicBiggest) {
                g_quicBiggest = (int)r;
                g_quicSample.assign(out, out + r);
            }
        }
        catch (...) {}
    }
    return r;
}

} // namespace

bool WorldQuicHook() {
    if (g_quicHooked) return true;

    HMODULE q = GetModuleHandleW(L"quiche.dll");
    if (!q) q = GetModuleHandleW(L"quiche.DLL");
    if (!q) {
        std::lock_guard lock(g_quicMtx);
        g_quicFound = "quiche.dll is not loaded";
        return false;
    }

    std::string found;
    void* pStream = (void*)GetProcAddress(q, "quiche_conn_stream_recv");
    void* pDgram = (void*)GetProcAddress(q, "quiche_conn_dgram_recv");

    if (pStream &&
        MH_CreateHook(pStream, &hkStreamRecv, (void**)&oStreamRecv) == MH_OK &&
        MH_EnableHook(pStream) == MH_OK) {
        found += "quiche_conn_stream_recv ";
    }
    if (pDgram &&
        MH_CreateHook(pDgram, &hkDgramRecv, (void**)&oDgramRecv) == MH_OK &&
        MH_EnableHook(pDgram) == MH_OK) {
        found += "quiche_conn_dgram_recv ";
    }

    {
        std::lock_guard lock(g_quicMtx);
        g_quicFound = found.empty()
            ? "quiche.dll is loaded but neither receive export is there"
            : found;
    }
    g_quicHooked = (oStreamRecv != nullptr) || (oDgramRecv != nullptr);
    return g_quicHooked;
}

void WorldQuicReset() {
    std::lock_guard lock(g_quicMtx);
    g_quicStreamCalls = 0;
    g_quicDgramCalls = 0;
    g_quicBytes = 0;
    g_quicBiggest = 0;
    g_quicSample.clear();
    g_streams.clear();
    g_quicTypes.clear();
    g_grabbed.clear();
    g_quicMessages = 0;
    g_quicLastType = 0;
    g_quicLastLen = 0;
}

std::string WorldQuicStatus() {
    std::lock_guard lock(g_quicMtx);
    char buf[240];
    if (!g_quicHooked) return g_quicFound.empty() ? "not hooked" : g_quicFound;
    snprintf(buf, sizeof(buf), "%llu stream + %llu dgram, %llu KB, biggest %d",
        (unsigned long long)g_quicStreamCalls,
        (unsigned long long)g_quicDgramCalls,
        (unsigned long long)(g_quicBytes >> 10), g_quicBiggest);
    return buf;
}

void WorldZstdReport(const std::vector<uint8_t>& sample, std::string& report);

void WorldQuicReport(std::string& report) {
    std::lock_guard lock(g_quicMtx);
    char line[320];

    snprintf(line, sizeof(line), "\nquiche: %s\n", g_quicFound.c_str());
    report += line;
    if (!g_quicHooked) return;

    snprintf(line, sizeof(line),
        "%llu stream read(s), %llu datagram read(s), %llu KB\n"
        "%llu whole message(s) reassembled, biggest %d bytes, last type %u\n",
        (unsigned long long)g_quicStreamCalls,
        (unsigned long long)g_quicDgramCalls,
        (unsigned long long)(g_quicBytes >> 10),
        (unsigned long long)g_quicMessages, g_quicBiggest, g_quicLastType);
    report += line;

    if (g_quicSample.empty()) {
        report += "nothing came out of quiche, so the game's data leaves it by "
                  "some other call.\n";
        return;
    }

    // Length is known here, so the entropy verdict finally means something.
    int counts[256] = { 0 };
    for (uint8_t b : g_quicSample) ++counts[b];
    int distinct = 0;
    for (int i = 0; i < 256; ++i) if (counts[i]) ++distinct;
    const int zeros = counts[0];
    const int n = (int)g_quicSample.size();

    snprintf(line, sizeof(line),
        "sample of %d bytes: %d distinct values, %d%% zero\n",
        n, distinct, (zeros * 100) / n);
    report += line;
    report += (distinct > 240 && zeros * 20 < n)
        ? "compressed inside the QUIC stream - needs unpacking next\n"
        : "this is the game protocol in the clear\n";

    report += "first bytes: ";
    for (size_t i = 0; i < g_quicSample.size() && i < 128; ++i) {
        snprintf(line, sizeof(line), "%02X ", g_quicSample[i]);
        report += line;
    }
    report += "\n";

    WorldZstdReport(g_quicSample, report);
}

// =============================================================================
// Under the zstd
// =============================================================================
// quiche handed over 3.3 MB across 1,954 stream reads and the payload starts:
//
//   C8 20 01 00 F1 00 00 00   28 B5 2F FD   A0 C0 28 03 ...
//   \_ eight byte header _/   \_ zstd _/
//
// 28 B5 2F FD is the zstd magic number, so every message is a short header
// followed by a zstd frame. That is the last layer, and unlike everything else
// in this search it is a published format with a published decompressor - there
// is nothing here to reverse.
//
// zstd is not linked into this DLL and writing a decoder is not sensible (FSE
// and Huffman tables, thousands of lines). The client has one statically linked
// inside it, but finding that without symbols means pattern-matching again, and
// pattern-matching is what has gone wrong at every stage. So: load the official
// libzstd.dll if it is present. It is a single file to drop next to the game,
// and it either loads or it says so.

namespace {

typedef size_t(*tZstdDecompress)(void* dst, size_t dstCap,
    const void* src, size_t srcSize);
typedef unsigned long long(*tZstdFrameSize)(const void* src, size_t srcSize);
typedef unsigned(*tZstdIsError)(size_t code);

tZstdDecompress pZstdDecompress = nullptr;
tZstdFrameSize  pZstdFrameSize = nullptr;
tZstdIsError    pZstdIsError = nullptr;
bool g_zstdTried = false;
std::string g_zstdWhere;

bool ZstdReady() {
    if (g_zstdTried) return pZstdDecompress != nullptr;
    g_zstdTried = true;

    // Beside the injected DLL, beside the game, or anywhere on the search path.
    const wchar_t* names[] = { L"libzstd.dll", L"zstd.dll" };
    for (const wchar_t* n : names) {
        HMODULE m = GetModuleHandleW(n);
        if (!m) m = LoadLibraryW(n);
        if (!m) continue;
        pZstdDecompress = (tZstdDecompress)GetProcAddress(m, "ZSTD_decompress");
        pZstdFrameSize = (tZstdFrameSize)GetProcAddress(m,
            "ZSTD_getFrameContentSize");
        pZstdIsError = (tZstdIsError)GetProcAddress(m, "ZSTD_isError");
        if (pZstdDecompress) {
            char narrow[64];
            WideCharToMultiByte(CP_UTF8, 0, n, -1, narrow, sizeof(narrow),
                nullptr, nullptr);
            g_zstdWhere = narrow;
            return true;
        }
    }
    g_zstdWhere.clear();
    return false;
}

// Where the zstd frame starts inside a captured message. The header is eight
// bytes on the sample seen, but searching for the magic rather than assuming
// eight means a different header length does not break it.
size_t FindZstdFrame(const std::vector<uint8_t>& buf) {
    for (size_t i = 0; i + 4 <= buf.size() && i < 64; ++i) {
        if (buf[i] == 0x28 && buf[i + 1] == 0xB5 &&
            buf[i + 2] == 0x2F && buf[i + 3] == 0xFD)
            return i;
    }
    return (size_t)-1;
}

} // namespace

// Appends what is under the compression. Caller holds g_quicMtx.
void WorldZstdReport(const std::vector<uint8_t>& sample, std::string& report) {
    char line[320];

    const size_t at = FindZstdFrame(sample);
    if (at == (size_t)-1) {
        report += "no zstd frame in this sample.\n";
        return;
    }
    snprintf(line, sizeof(line), "zstd frame starts at byte %zu\n", at);
    report += line;

    if (!ZstdReady()) {
        report += "libzstd.dll is not available, so it cannot be unpacked here.\n"
                  "Put libzstd.dll next to HytaleClient.exe and press this again "
                  "- it is the official zstd build and nothing else is needed.\n";
        return;
    }
    snprintf(line, sizeof(line), "using %s\n", g_zstdWhere.c_str());
    report += line;

    const uint8_t* src = sample.data() + at;
    const size_t srcLen = sample.size() - at;

    unsigned long long want = pZstdFrameSize ? pZstdFrameSize(src, srcLen) : 0;
    // ZSTD_CONTENTSIZE_UNKNOWN / _ERROR come back as huge values.
    if (!want || want > (16u << 20)) want = 1u << 20;

    std::vector<uint8_t> out((size_t)want, 0);
    const size_t got = pZstdDecompress(out.data(), out.size(), src, srcLen);

    if (pZstdIsError && pZstdIsError(got)) {
        // The capture is a prefix of the message, so a frame longer than the
        // sample cannot decompress. Worth saying which it is rather than
        // reporting a flat failure.
        snprintf(line, sizeof(line),
            "the frame did not unpack from %zu captured bytes - most likely the\n"
            "message is longer than the capture, so the whole read has to be\n"
            "kept rather than a prefix.\n", srcLen);
        report += line;
        return;
    }

    out.resize(got);
    snprintf(line, sizeof(line), "unpacked to %zu bytes\n", got);
    report += line;
    if (!got) return;

    // The registry check, which is the one test in this whole search that has
    // ever meant anything: chunk data is block ids, and we know all 5,775 of
    // their names. If most of these 16-bit values resolve, this is the world.
    int solid = 0, named = 0;
    for (size_t i = 0; i + 2 <= out.size(); i += 2) {
        uint16_t v = 0;
        memcpy(&v, out.data() + i, 2);
        if (!v) continue;
        ++solid;
        if (!BuildGetBlockName(v).empty()) ++named;
    }
    snprintf(line, sizeof(line),
        "as 16-bit values: %d non-zero, %d resolve to block names (%d%%)\n",
        solid, named, solid ? (named * 100 / solid) : 0);
    report += line;

    report += "first bytes: ";
    for (size_t i = 0; i < out.size() && i < 128; ++i) {
        snprintf(line, sizeof(line), "%02X ", out[i]);
        report += line;
    }
    report += "\n";
}

// =============================================================================
// Which message type carries the world
// =============================================================================
// The pipeline works end to end: 111,605 compressed bytes became 242,660, from
// 8,785 reassembled messages. But 24% of that payload resolving to block names
// means it is a structured message rather than a run of ids, and the one that
// got sampled was simply the largest of all types rather than the one wanted.
//
// So: score each type on its own, and write the decompressed payloads out. A
// hundred-byte preview is not enough to work out a format - the bytes have to
// be looked at properly, and a file on disk can be.

void WorldQuicTypeReport(std::string& report) {
    char line[360];

    std::vector<TypeStat> types;
    { std::lock_guard lock(g_quicMtx); types = g_quicTypes; }
    if (types.empty()) return;

    std::stable_sort(types.begin(), types.end(),
        [](const TypeStat& a, const TypeStat& b) { return a.biggest > b.biggest; });

    // Somewhere the person can actually find them, next to the project rather
    // than in a temporary folder that gets swept.
    const char* dir = "C:\\Users\\chris\\source\\repos\\HytaleChatLogger\\quicdump";
    CreateDirectoryA(dir, nullptr);

    snprintf(line, sizeof(line), "\n%zu message type(s):\n", types.size());
    report += line;

    for (const TypeStat& t : types) {
        snprintf(line, sizeof(line), "  type %-6u x%-7llu biggest %u bytes",
            t.type, (unsigned long long)t.count, t.biggest);
        report += line;

        if (t.sample.empty() || !ZstdReady()) { report += "\n"; continue; }

        const size_t at = FindZstdFrame(t.sample);
        if (at == (size_t)-1) { report += "   not zstd\n"; continue; }

        const uint8_t* src = t.sample.data() + at;
        const size_t srcLen = t.sample.size() - at;
        unsigned long long want = pZstdFrameSize ? pZstdFrameSize(src, srcLen) : 0;
        if (!want || want > (32u << 20)) want = 4u << 20;

        std::vector<uint8_t> out((size_t)want, 0);
        const size_t got = pZstdDecompress(out.data(), out.size(), src, srcLen);
        if (pZstdIsError && pZstdIsError(got)) {
            report += "   would not unpack\n";
            continue;
        }
        out.resize(got);

        int solid = 0, named = 0;
        for (size_t i = 0; i + 2 <= out.size(); i += 2) {
            uint16_t v = 0;
            memcpy(&v, out.data() + i, 2);
            if (!v) continue;
            ++solid;
            if (!BuildGetBlockName(v).empty()) ++named;
        }
        const int pct = solid ? (named * 100 / solid) : 0;

        snprintf(line, sizeof(line), " -> %zu bytes, %d%% block names", got, pct);
        report += line;

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\type_%u.bin", dir, t.type);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "wb") == 0 && f) {
            fwrite(out.data(), 1, out.size(), f);
            fclose(f);
            snprintf(line, sizeof(line), ", saved type_%u.bin", t.type);
            report += line;
        }
        report += "\n";
    }

    snprintf(line, sizeof(line), "\nsaved to %s\n", dir);
    report += line;
}

// Called once a frame so the network thread never has to walk the object graph
// to find out where the player is.
void WorldTickPlayer() {
    if (!g_chunkSniff.load()) return;
    const GsdkPlayerInfo me = GsdkGetPlayerInfo();
    if (!me.valid) return;
    const int px = (int)std::floor(me.x);
    const int py = (int)std::floor(me.y);
    const int pz = (int)std::floor(me.z);
    g_lastPX.store(px);
    g_lastPY.store(py);
    g_lastPZ.store(pz);
    WorldDrainRaw();
    WorldDrainSections();
    WorldCalibrateTick(px, py, pz);
}

// Writes every stamped sample out with a manifest. The point is the manifest:
// a header field that is really a chunk coordinate has to track the player's
// own chunk across samples, and one that merely looks like one will not.
void WorldGrabReport(std::string& report) {
    std::vector<Grabbed> grabbed;
    { std::lock_guard lock(g_quicMtx); grabbed = g_grabbed; }
    if (grabbed.empty()) return;

    char line[400];
    const char* dir = "C:\\Users\\chris\\source\\repos\\HytaleChatLogger\\quicdump";
    CreateDirectoryA(dir, nullptr);

    char manifestPath[MAX_PATH];
    snprintf(manifestPath, sizeof(manifestPath), "%s\\manifest.txt", dir);
    FILE* mf = nullptr;
    fopen_s(&mf, manifestPath, "wb");

    int written = 0, sections = 0;
    for (size_t i = 0; i < grabbed.size(); ++i) {
        const Grabbed& g = grabbed[i];
        if (g.body.empty() || !ZstdReady()) continue;

        const size_t at = FindZstdFrame(g.body);
        if (at == (size_t)-1) continue;

        const uint8_t* src = g.body.data() + at;
        const size_t srcLen = g.body.size() - at;
        unsigned long long want = pZstdFrameSize ? pZstdFrameSize(src, srcLen) : 0;
        if (!want || want > (32u << 20)) want = 4u << 20;

        std::vector<uint8_t> out((size_t)want, 0);
        const size_t got = pZstdDecompress(out.data(), out.size(), src, srcLen);
        if (pZstdIsError && pZstdIsError(got)) continue;
        out.resize(got);

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\s%02zu_type%u.bin", dir, i, g.type);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "wb") != 0 || !f) continue;
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        ++written;

        if (!mf) continue;

        // The manifest carries everything needed to work the palette out by
        // comparison, so the files are a backup rather than the only copy. The
        // confirmed layout of a section message is:
        //
        //   01 | int32 cx | int32 cy | int32 cz | varint blobLen
        //      | <blobLen - 32768 bytes of palette metadata>
        //      | 32768 bytes, one per block, low nibble block, high nibble rotation
        //
        // What is written per sample: the coordinates, the metadata bytes in
        // hex, and the nibble histograms of the grid. A palette entry has to
        // account for the values that actually appear, so putting the two side
        // by side is what identifies the field rather than guessing an offset.
        int cx = 0, cy = 0, cz = 0;
        if (out.size() >= 13) {
            memcpy(&cx, out.data() + 1, 4);
            memcpy(&cy, out.data() + 5, 4);
            memcpy(&cz, out.data() + 9, 4);
        }

        snprintf(line, sizeof(line),
            "s%02zu type=%u bytes=%zu lead=%u chunk=%d,%d,%d player=%d,%d,%d\r\n",
            i, g.type, out.size(), out.empty() ? 0u : out[0], cx, cy, cz,
            g.px, g.py, g.pz);
        fwrite(line, 1, strlen(line), mf);

        if (g.type == 136 && out.size() > 13) {
            // The varint after the coordinates, then however much metadata sits
            // before the grid.
            size_t p = 13;
            uint64_t blobLen = 0;
            int shift = 0;
            while (p < out.size() && shift < 64) {
                const uint8_t b = out[p++];
                blobLen |= (uint64_t)(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80)) break;
            }
            const size_t gridLen = (out.size() > p) ? (out.size() - p) : 0;
            const size_t metaLen = (gridLen > 32768) ? (gridLen - 32768) : 0;

            snprintf(line, sizeof(line),
                "     blobLen=%llu afterVarint=%zu meta=%zu grid=%zu\r\n",
                (unsigned long long)blobLen, gridLen, metaLen,
                gridLen >= 32768 ? (size_t)32768 : gridLen);
            fwrite(line, 1, strlen(line), mf);

            if (metaLen && metaLen < 256) {
                std::string hex = "     meta=";
                for (size_t k = 0; k < metaLen; ++k) {
                    char bb[4];
                    snprintf(bb, sizeof(bb), "%02X ", out[p + k]);
                    hex += bb;
                }
                hex += "\r\n";
                fwrite(hex.c_str(), 1, hex.size(), mf);
            }

            if (gridLen >= 32768) {
                ++sections;
                const uint8_t* grid = out.data() + p + metaLen;
                int lowCount[16] = { 0 }, highCount[16] = { 0 };
                for (size_t k = 0; k < 32768; ++k) {
                    ++lowCount[grid[k] & 0x0F];
                    ++highCount[grid[k] >> 4];
                }
                std::string lo = "     low ", hi = "     high ";
                for (int v = 0; v < 16; ++v) {
                    if (lowCount[v]) {
                        char bb[32];
                        snprintf(bb, sizeof(bb), "%d:%d ", v, lowCount[v]);
                        lo += bb;
                    }
                    if (highCount[v]) {
                        char bb[32];
                        snprintf(bb, sizeof(bb), "%d:%d ", v, highCount[v]);
                        hi += bb;
                    }
                }
                lo += "\r\n"; hi += "\r\n";
                fwrite(lo.c_str(), 1, lo.size(), mf);
                fwrite(hi.c_str(), 1, hi.size(), mf);
            }
        }
    }
    if (mf) fclose(mf);

    snprintf(line, sizeof(line),
        "\n%d sample(s) written to quicdump, %d of them FULL SECTIONS\n",
        written, sections);
    report += line;
    // Say outright whether the run was any use. Last time thirty-nine of forty
    // samples were sky, and that only became clear afterwards.
    report += (sections >= 6)
        ? "enough to work the palette out by comparison\n"
        : "NOT ENOUGH full sections - fly LOW through varied ground (trees, "
          "buildings, hills), not over water or in the sky, and run it again\n";
}

// =============================================================================
// Copying the shape of a build
// =============================================================================
// What is confirmed: type 136 carries a section as 32768 bytes, one per block,
// and a byte is non-zero exactly where there is a block. That is the geometry
// of anything built, which is most of what a build guide needs - the materials
// can be chosen when rebuilding.
//
// What is NOT confirmed is how those 32768 bytes are laid out. The section could
// be 32x32x32, or 64x8x64, or several other shapes, in any of six axis
// orderings, and the data cannot settle it: every candidate scores about 100%
// on geometric coherence because the filled cells are so clustered. Picking one
// by eye would mean handing over builds that are silently mirrored or
// transposed, which is worse than handing over nothing.
//
// So it is not picked. The player is standing on the world: the block under
// their feet is solid and the one above their head is not, and that stays true
// wherever they walk. Only the right layout predicts it every time. Nothing is
// adopted until one candidate has been right hundreds of times and the rest
// have been wrong - and if none manages that, it says so rather than guessing.

namespace {

struct SectionKey {
    int cx, cy, cz;
    bool operator==(const SectionKey& o) const {
        return cx == o.cx && cy == o.cy && cz == o.cz;
    }
};

struct CachedSection {
    SectionKey key{};
    std::vector<uint8_t> grid;     // 32768 bytes, non-zero means a block is there
};

std::mutex g_secMtx;
std::vector<CachedSection> g_sections;
constexpr size_t kMaxSections = 512;        // 512 x 32 KB = 16 MB

// One way the section could be arranged.
//
// The first version of this assumed one byte per block and every candidate
// scored negative - no arrangement put solid ground under the player's feet.
// The palette said why: two entries means ONE BIT per block, not eight. So
// 16384 bytes is 131072 blocks, which is 64 x 32 x 64 - and that is the only
// reading that also matches every coordinate measured. Player x -39 gives
// cx -1 at 64 wide, z -440 gives cz -7, y 145 gives cy 4 at 32 tall. The second
// 16384 bytes is the rotation field, which is why the two halves agreed 99%.
//
// Two things still cannot be reasoned out and so are left to the calibration:
// which way round the bits run inside a byte, and whether a set bit means solid
// or empty. Both are just more candidates.
struct Layout {
    int w, h, d;                   // blocks the section spans on each axis
    int64_t sx, sy, sz;            // blocks per step on each axis
    int bits = 1;                  // 1 or 8 bits per block
    bool msbFirst = false;         // bit order within a byte
    bool invert = false;           // a set bit means empty rather than solid
    int right = 0, wrong = 0;      // how often it predicted the ground correctly
};

std::vector<Layout> g_layouts;
int g_layoutChosen = -1;
int g_calibChecks = 0;
int g_calibNoSection = 0;   // frames where the player was in no cached section

void BuildLayoutCandidates() {
    if (!g_layouts.empty()) return;

    // Bit-packed first, because that is what a two-entry palette implies.
    // 131072 blocks per field, 16384 bytes.
    const int packed[][3] = {
        { 64, 32, 64 }, { 64, 64, 32 }, { 32, 64, 64 }, { 128, 8, 128 },
    };
    // Kept as a fallback in case a section ever uses a byte per block.
    const int bytes[][3] = {
        { 32, 32, 32 }, { 64, 8, 64 }, { 16, 32, 64 }, { 64, 32, 16 },
    };

    auto addShape = [&](int w, int h, int d, int bits) {
        const int64_t vol = (int64_t)w * h * d;
        if (bits == 1 && vol != 131072) return;
        if (bits == 8 && vol != 32768) return;
        const int64_t opts[6][3] = {
            { 1, (int64_t)w * d, w },
            { 1, (int64_t)w * h, w },
            { (int64_t)d, 1, (int64_t)w * h },
            { (int64_t)h, 1, (int64_t)w * h },
            { (int64_t)h * d, 1, (int64_t)h },
            { (int64_t)h * d, (int64_t)d, 1 },
        };
        for (const auto& o : opts) {
            if (o[0] * (w - 1) + o[1] * (h - 1) + o[2] * (d - 1) != vol - 1) continue;
            for (int mo = 0; mo < 2; ++mo) {
                for (int inv = 0; inv < 2; ++inv) {
                    Layout L;
                    L.w = w; L.h = h; L.d = d;
                    L.sx = o[0]; L.sy = o[1]; L.sz = o[2];
                    L.bits = bits;
                    L.msbFirst = (mo != 0);
                    L.invert = (inv != 0);
                    if (bits == 8 && mo) continue;   // bit order is meaningless
                    g_layouts.push_back(L);
                }
            }
        }
    };

    for (const auto& sh : packed) addShape(sh[0], sh[1], sh[2], 1);
    for (const auto& sh : bytes)  addShape(sh[0], sh[1], sh[2], 8);
}

int FloorDivL(int v, int n) { return (v >= 0) ? (v / n) : -(((-v) + n - 1) / n); }

const CachedSection* FindSection(const SectionKey& k) {
    for (const CachedSection& s : g_sections)
        if (s.key == k) return &s;
    return nullptr;
}

// Is there a block at this world coordinate, according to one layout? Returns
// false when the section is not cached, which is different from "empty" and is
// why the copy counts readable cells separately.
bool SolidAt(const Layout& L, int x, int y, int z, bool& known) {
    known = false;
    SectionKey k{ FloorDivL(x, L.w), FloorDivL(y, L.h), FloorDivL(z, L.d) };
    const CachedSection* s = FindSection(k);
    if (!s || s->grid.size() < 32768) return false;

    const int lx = x - k.cx * L.w, ly = y - k.cy * L.h, lz = z - k.cz * L.d;
    const int64_t i = (int64_t)lx * L.sx + (int64_t)ly * L.sy + (int64_t)lz * L.sz;

    bool set = false;
    if (L.bits == 8) {
        if (i < 0 || i >= 32768) return false;
        set = s->grid[(size_t)i] != 0;
    }
    else {
        // One bit a block, first field only - the second 16384 bytes hold
        // rotation and say nothing about whether a block is there.
        if (i < 0 || i >= 131072) return false;
        const size_t byteAt = (size_t)(i >> 3);
        const int bit = (int)(i & 7);
        const uint8_t mask = L.msbFirst ? (uint8_t)(0x80 >> bit) : (uint8_t)(1 << bit);
        set = (s->grid[byteAt] & mask) != 0;
    }
    known = true;
    return L.invert ? !set : set;
}

} // namespace

// Called from the network thread when a whole section arrives.
void WorldCacheSection(int cx, int cy, int cz, const uint8_t* grid) {
    std::lock_guard lock(g_secMtx);
    SectionKey k{ cx, cy, cz };
    for (CachedSection& s : g_sections) {
        if (!(s.key == k)) continue;
        s.grid.assign(grid, grid + 32768);
        return;
    }
    if (g_sections.size() >= kMaxSections) g_sections.erase(g_sections.begin());
    CachedSection s;
    s.key = k;
    s.grid.assign(grid, grid + 32768);
    g_sections.push_back(std::move(s));
}

// Called once a frame. The ground under the player is the known value that
// decides between the layouts.
void WorldCalibrateTick(int px, int py, int pz) {
    std::lock_guard lock(g_secMtx);
    BuildLayoutCandidates();
    if (g_sections.empty() || g_layoutChosen >= 0) return;

    bool any = false;
    bool sawSection = false;
    for (Layout& L : g_layouts) {
        bool k1 = false, k2 = false;
        const bool under = SolidAt(L, px, py - 1, pz, k1);
        const bool over = SolidAt(L, px, py + 2, pz, k2);
        if (k1 || k2) sawSection = true;
        if (!k1 || !k2) continue;
        any = true;
        // Standing on something, with clear air above the head.
        if (under && !over) ++L.right; else ++L.wrong;
    }
    if (!sawSection) ++g_calibNoSection;
    if (!any) return;
    ++g_calibChecks;

    // Adopt only on a clear, sustained win. A layout that is merely ahead after
    // a few frames has proved nothing.
    if (g_calibChecks < 240) return;
    int bestIdx = -1, bestScore = 0, runnerUp = 0;
    for (size_t i = 0; i < g_layouts.size(); ++i) {
        const Layout& L = g_layouts[i];
        const int score = L.right - L.wrong;
        if (score > bestScore) { runnerUp = bestScore; bestScore = score; bestIdx = (int)i; }
        else if (score > runnerUp) runnerUp = score;
    }
    if (bestIdx >= 0 && bestScore >= 150 && bestScore > runnerUp * 2)
        g_layoutChosen = bestIdx;
}

bool WorldShapeReady() { std::lock_guard lock(g_secMtx); return g_layoutChosen >= 0; }

std::string WorldShapeStatus() {
    std::lock_guard lock(g_secMtx);
    char buf[260];
    if (g_sections.empty())
        return "no sections yet - press Start listening and walk around";
    if (g_layoutChosen >= 0) {
        const Layout& L = g_layouts[(size_t)g_layoutChosen];
        snprintf(buf, sizeof(buf),
            "ready - sections are %dx%dx%d, %zu cached", L.w, L.h, L.d,
            g_sections.size());
        return buf;
    }
    // Name the leading candidate, not just its score. When this failed with
    // "best score 0" there was no way to tell whether nothing was close or
    // everything was wrong by the same amount.
    const Layout* lead = nullptr;
    int best = -999999;
    for (const Layout& L : g_layouts) {
        const int sc = L.right - L.wrong;
        if (sc > best) { best = sc; lead = &L; }
    }
    if (!lead) return "no candidates";
    snprintf(buf, sizeof(buf),
        "working it out: %zu section(s), %d check(s), %d frame(s) with your "
        "section missing. Best so far %dx%dx%d "
        "%s%s, score %d (%d right, %d wrong) of %zu candidates - keep walking",
        g_sections.size(), g_calibChecks, g_calibNoSection,
        lead->w, lead->h, lead->d,
        lead->bits == 1 ? "1-bit" : "1-byte", lead->invert ? " inverted" : "",
        best, lead->right, lead->wrong, g_layouts.size());
    return buf;
}

void WorldShapeForget() {
    std::lock_guard lock(g_secMtx);
    g_sections.clear();
    g_layouts.clear();
    g_layoutChosen = -1;
    g_calibChecks = 0;
    g_calibNoSection = 0;
}

// Is there a block here? False for "no" and for "not cached" alike, so the
// caller is told separately how much of the box could actually be read.
bool WorldShapeAt(int x, int y, int z, bool& known) {
    std::lock_guard lock(g_secMtx);
    known = false;
    if (g_layoutChosen < 0) return false;
    return SolidAt(g_layouts[(size_t)g_layoutChosen], x, y, z, known);
}

void WorldShapeCoverage(int minX, int minY, int minZ, int maxX, int maxY, int maxZ,
    long long& readable, long long& total) {
    readable = 0;
    total = 0;
    for (int y = minY; y <= maxY; ++y)
        for (int z = minZ; z <= maxZ; ++z)
            for (int x = minX; x <= maxX; ++x) {
                ++total;
                bool known = false;
                WorldShapeAt(x, y, z, known);
                if (known) ++readable;
            }
}

namespace {

// A bounded hand-off queue. Bounded is the point: the previous version did the
// work inline on the network thread and a relog - which resends the whole world
// at once - took the game down with it.
std::mutex g_secQMtx;
std::vector<std::vector<uint8_t>> g_secQueue;
constexpr size_t kSecQueueMax = 48;
std::atomic<uint64_t> g_secDropped{ 0 };

// Raw stream bytes, straight off the network thread, bounded and dropping.
// Falling behind costs a section; blocking that thread cost the whole game.
struct RawStream { uint64_t stream; std::vector<uint8_t> bytes; };
std::mutex g_rawMtx;
std::vector<RawStream> g_rawQueue;
size_t g_rawBytes = 0;
constexpr size_t kRawQueueBytes = 8u << 20;

void QueueRawStream(uint64_t streamId, const uint8_t* buf, size_t len) {
    if (!buf || !len || len > (1u << 20)) return;
    std::lock_guard lock(g_rawMtx);
    if (g_rawBytes + len > kRawQueueBytes) { g_secDropped.fetch_add(1); return; }
    g_rawQueue.push_back(RawStream{ streamId, std::vector<uint8_t>(buf, buf + len) });
    g_rawBytes += len;
}

// Frame thread. Everything that used to happen inside the hook happens here.
void WorldDrainRaw() {
    std::vector<RawStream> batch;
    {
        std::lock_guard lock(g_rawMtx);
        if (g_rawQueue.empty()) return;
        batch.swap(g_rawQueue);
        g_rawBytes = 0;
    }
    std::lock_guard lock(g_quicMtx);
    for (const RawStream& r : batch) {
        ++g_quicStreamCalls;
        g_quicBytes += r.bytes.size();
        FeedStream(r.stream, r.bytes.data(), (intptr_t)r.bytes.size());
    }
}

void QueueSectionBody(const uint8_t* body, uint32_t len) {
    if (!body || !len || len > (1u << 20)) return;
    std::lock_guard lock(g_secQMtx);
    if (g_secQueue.size() >= kSecQueueMax) { g_secDropped.fetch_add(1); return; }
    g_secQueue.emplace_back(body, body + len);
}

// Runs on the frame thread, where a slow moment costs a frame and not the
// connection. Decompresses whatever has queued up and caches the grids.
void WorldDrainSections() {
    std::vector<std::vector<uint8_t>> batch;
    {
        std::lock_guard lock(g_secQMtx);
        if (g_secQueue.empty()) return;
        batch.swap(g_secQueue);
    }
    if (!ZstdReady()) return;

    std::vector<uint8_t> out;
    for (const std::vector<uint8_t>& body : batch) {
        const size_t at = FindZstdFrame(body);
        if (at == (size_t)-1) continue;

        const uint8_t* src = body.data() + at;
        const size_t srcLen = body.size() - at;
        unsigned long long want = pZstdFrameSize ? pZstdFrameSize(src, srcLen) : 0;
        if (!want || want > (1u << 20)) want = 1u << 20;

        out.assign((size_t)want, 0);
        const size_t got = pZstdDecompress(out.data(), out.size(), src, srcLen);
        if ((pZstdIsError && pZstdIsError(got)) || got < 14) continue;

        int cx = 0, cy = 0, cz = 0;
        memcpy(&cx, out.data() + 1, 4);
        memcpy(&cy, out.data() + 5, 4);
        memcpy(&cz, out.data() + 9, 4);

        size_t p = 13;
        int shift = 0;
        while (p < got && shift < 64) {
            const uint8_t b = out[p++];
            shift += 7;
            if (!(b & 0x80)) break;
        }
        if (got < p + 32768) continue;          // not a full section
        const size_t meta = got - p - 32768;
        WorldCacheSection(cx, cy, cz, out.data() + p + meta);
    }
}

} // namespace
