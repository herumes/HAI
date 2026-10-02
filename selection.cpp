#include "pch.h"
#include "selection.hpp"
#include "common.hpp"
#include "debugshape.hpp"
#include "position.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

std::atomic<int> g_selX1{ 0 }, g_selY1{ 0 }, g_selZ1{ 0 };
std::atomic<int> g_selX2{ 0 }, g_selY2{ 0 }, g_selZ2{ 0 };
std::atomic<bool> g_sel1Set{ false }, g_sel2Set{ false };
std::atomic<bool> g_selShow{ false };

namespace {

// Redrawing means clearing the shape list and refilling it, so it happens when
// the box actually changes and not sixty times a second.
std::atomic<uint64_t> g_selSignature{ 0 };
long long g_selLastDrawMs = 0;

void Dirty() { g_selSignature.store(0); }

int PlayerBlockX() { return (int)std::floor(g_posX.load()); }
int PlayerBlockY() { return (int)std::floor(g_posY.load()); }
int PlayerBlockZ() { return (int)std::floor(g_posZ.load()); }

} // namespace

void SelSetCorner1() {
    g_selX1.store(PlayerBlockX());
    g_selY1.store(PlayerBlockY());
    g_selZ1.store(PlayerBlockZ());
    g_sel1Set.store(true);
    Dirty();
}

void SelSetCorner2() {
    g_selX2.store(PlayerBlockX());
    g_selY2.store(PlayerBlockY());
    g_selZ2.store(PlayerBlockZ());
    g_sel2Set.store(true);
    Dirty();
}

void SelClear() {
    g_sel1Set.store(false);
    g_sel2Set.store(false);
    Dirty();
}

void SelBoxAtPlayer(int radius, int below, int above) {
    if (radius < 0) radius = 0;
    if (below < 0) below = 0;
    if (above < 0) above = 0;

    const int px = PlayerBlockX(), py = PlayerBlockY(), pz = PlayerBlockZ();
    g_selX1.store(px - radius); g_selY1.store(py - below); g_selZ1.store(pz - radius);
    g_selX2.store(px + radius); g_selY2.store(py + above); g_selZ2.store(pz + radius);
    g_sel1Set.store(true);
    g_sel2Set.store(true);
    Dirty();
}

// Every adjustment works on the sorted box and then writes it back, so it does
// not matter which corner happens to be the low one - pushing "+X out" always
// pushes the east face east.
namespace {

bool LoadSorted(int& minX, int& minY, int& minZ, int& maxX, int& maxY, int& maxZ) {
    if (!g_sel1Set.load() || !g_sel2Set.load()) return false;
    const int x1 = g_selX1.load(), y1 = g_selY1.load(), z1 = g_selZ1.load();
    const int x2 = g_selX2.load(), y2 = g_selY2.load(), z2 = g_selZ2.load();
    minX = (std::min)(x1, x2); maxX = (std::max)(x1, x2);
    minY = (std::min)(y1, y2); maxY = (std::max)(y1, y2);
    minZ = (std::min)(z1, z2); maxZ = (std::max)(z1, z2);
    return true;
}

void StoreSorted(int minX, int minY, int minZ, int maxX, int maxY, int maxZ) {
    // Never let a face cross past its opposite: a box is at minimum one block.
    if (maxX < minX) maxX = minX;
    if (maxY < minY) maxY = minY;
    if (maxZ < minZ) maxZ = minZ;
    g_selX1.store(minX); g_selY1.store(minY); g_selZ1.store(minZ);
    g_selX2.store(maxX); g_selY2.store(maxY); g_selZ2.store(maxZ);
    Dirty();
}

} // namespace

void SelNudgeFace(int face, int amount) {
    int a, b, c, d, e, f;
    if (!LoadSorted(a, b, c, d, e, f)) return;
    switch (face) {
    case 0: a -= amount; break;   // -X out is west
    case 1: d += amount; break;   // +X
    case 2: b -= amount; break;   // -Y (down)
    case 3: e += amount; break;   // +Y (up)
    case 4: c -= amount; break;   // -Z
    case 5: f += amount; break;   // +Z
    default: return;
    }
    StoreSorted(a, b, c, d, e, f);
}

void SelMove(int dx, int dy, int dz) {
    int a, b, c, d, e, f;
    if (!LoadSorted(a, b, c, d, e, f)) return;
    StoreSorted(a + dx, b + dy, c + dz, d + dx, e + dy, f + dz);
}

void SelExpandAll(int amount) {
    int a, b, c, d, e, f;
    if (!LoadSorted(a, b, c, d, e, f)) return;
    StoreSorted(a - amount, b - amount, c - amount, d + amount, e + amount, f + amount);
}

bool SelBounds(int& minX, int& minY, int& minZ, int& maxX, int& maxY, int& maxZ) {
    return LoadSorted(minX, minY, minZ, maxX, maxY, maxZ);
}

void SelSize(int& w, int& h, int& d, long long& volume) {
    int a, b, c, x, y, z;
    if (!LoadSorted(a, b, c, x, y, z)) { w = h = d = 0; volume = 0; return; }
    w = x - a + 1;
    h = y - b + 1;
    d = z - c + 1;
    volume = (long long)w * (long long)h * (long long)d;
}

std::string SelSummary() {
    if (!g_sel1Set.load() && !g_sel2Set.load()) return "nothing selected";
    if (!g_sel1Set.load() || !g_sel2Set.load()) return "one corner set - set the other";

    int w, h, d;
    long long vol;
    SelSize(w, h, d, vol);
    int a, b, c, x, y, z;
    LoadSorted(a, b, c, x, y, z);

    char buf[220];
    snprintf(buf, sizeof(buf), "%d x %d x %d = %lld blocks   from %d,%d,%d to %d,%d,%d",
        w, h, d, vol, a, b, c, x, y, z);
    return buf;
}

// =============================================================================
// Drawing it
// =============================================================================
// One cube scaled to the whole box, plus a small one on each corner block. The
// scaled cube is the trick worth keeping: outlining a region by its edges would
// cost hundreds of shapes and look no better.
void SelFrameTick() {
    if (!g_selShow.load()) return;
    if (!DbgSpawnReady()) return;

    int a, b, c, d, e, f;
    if (!LoadSorted(a, b, c, d, e, f)) return;

    uint64_t sig = 1469598103934665603ull;
    auto mix = [&sig](uint64_t v) { sig ^= v; sig *= 1099511628211ull; };
    mix((uint32_t)a); mix((uint32_t)b); mix((uint32_t)c);
    mix((uint32_t)d); mix((uint32_t)e); mix((uint32_t)f);
    if (sig == g_selSignature.load()) return;

    const long long now = NowSteadyMs();
    if (now - g_selLastDrawMs < 120) return;
    g_selLastDrawMs = now;
    g_selSignature.store(sig);

    DbgRequestClearAll();

    // The volume. Block coordinates name a corner, so the box spans from a to
    // d+1 and its centre lands on the half-block.
    {
        DbgRequest r;
        r.shape = DbgShape::Cube;
        r.x = (float)(a + (d - a + 1) * 0.5);
        r.y = (float)(b + (e - b + 1) * 0.5);
        r.z = (float)(c + (f - c + 1) * 0.5);
        r.scaleX = (float)(d - a + 1);
        r.scaleY = (float)(e - b + 1);
        r.scaleZ = (float)(f - c + 1);
        r.r = 0.35f; r.g = 0.85f; r.b = 1.0f;
        r.opacity = 0.16f;
        r.time = 600.0f;
        r.flags = DBG_FLAG_NONE;
        DbgQueueSpawn(r);
    }

    // The two corners you actually put down, in different colours, so it is
    // obvious which one a nudge is about to move.
    struct Marker { int x, y, z; float r, g, b; };
    const Marker marks[2] = {
        { g_selX1.load(), g_selY1.load(), g_selZ1.load(), 0.3f, 1.0f, 0.3f },
        { g_selX2.load(), g_selY2.load(), g_selZ2.load(), 1.0f, 0.5f, 0.2f },
    };
    for (const Marker& m : marks) {
        DbgRequest r;
        r.shape = DbgShape::Cube;
        r.x = (float)m.x + 0.5f;
        r.y = (float)m.y + 0.5f;
        r.z = (float)m.z + 0.5f;
        r.scaleX = r.scaleY = r.scaleZ = 1.02f;
        r.r = m.r; r.g = m.g; r.b = m.b;
        r.opacity = 0.75f;
        r.time = 600.0f;
        r.flags = DBG_FLAG_NONE;
        DbgQueueSpawn(r);
    }
}
