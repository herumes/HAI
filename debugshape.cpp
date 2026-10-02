#include "pch.h"
#include "debugshape.hpp"
#include <unordered_map>
#include "hooks.hpp"    // SafeRead / IsPlausiblePtr / GameModulePatternScan / GetGameModuleBaseSize
#include "common.hpp"   // LogToFile / GetThisModuleDirectory
#include "packets.hpp"  // PacketDispatchInbound
#include "gamesdk.hpp"  // GsdkWalkGraph / GsdkFollow / GsdkCurrentRoot
#include "chat.hpp"     // g_stringVT, and QueueSend for growing the list
#include "position.hpp" // g_posX/Y/Z - where an attached shape gets moved to
#include <cmath>
#include <cstdio>
#include <deque>
#include <cstdarg>
#include <fstream>

std::mutex g_dbgMtx;
DbgLayout  g_dbgLayout;

std::atomic<bool>     g_dbgLearnArmed{ true };
std::atomic<uint64_t> g_dbgSeenCount{ 0 };
std::atomic<uint64_t> g_dbgInspected{ 0 };
std::atomic<uint64_t> g_dbgSawMatrix{ 0 };
std::atomic<uint64_t> g_dbgSentCount{ 0 };
std::atomic<uint64_t> g_dbgFailCount{ 0 };
std::atomic<bool>     g_dbgRuntimeReady{ false };

std::mutex  g_dbgStatusMtx;
std::string g_dbgStatus;

static void DbgSetStatus(const std::string& s) {
    { std::lock_guard lock(g_dbgStatusMtx); g_dbgStatus = s; }
    LogToFile("debugshape: " + s);
}
std::string DbgGetStatus() {
    std::lock_guard lock(g_dbgStatusMtx);
    return g_dbgStatus;
}

const char* DbgShapeName(DbgShape s) {
    switch (s) {
    case DbgShape::Sphere:   return "Sphere";
    case DbgShape::Cylinder: return "Cylinder";
    case DbgShape::Cone:     return "Cone";
    case DbgShape::Cube:     return "Cube";
    case DbgShape::Frustum:  return "Frustum";
    case DbgShape::Sector:   return "Sector";
    case DbgShape::Disc:     return "Disc";
    }
    return "?";
}

std::string DbgLayout::ToString() const {
    if (!learned) return "(nothing learned yet)";
    char buf[256];
    snprintf(buf, sizeof(buf),
        "packetMT +0x%llX  float[]MT +0x%llX  matrix@+0x%X%s%s%s%s%s",
        (unsigned long long)packetMTRva, (unsigned long long)floatArrayMTRva, offMatrix,
        haveColor ? "  color" : "", haveTime ? "  time" : "",
        haveOpacity ? "  opacity" : "", haveShape ? "  shape" : "",
        haveFlags ? "  flags" : "");
    return buf;
}

namespace {

// NativeAOT array layout: [MethodTable][int64 length][elements...]. The length
// is written as a native-width field, so the payload starts at +0x10.
constexpr uint32_t kArrayDataOffset = 0x10;
constexpr uint32_t kArrayLengthOffset = 0x08;

// How far into the packet object we look for fields.
constexpr uint32_t kPacketWindow = 0x80;

bool ReadPtr(uintptr_t at, uintptr_t& out) {
    uint64_t v = 0;
    if (!SafeRead((const void*)at, &v, 8)) return false;
    out = (uintptr_t)v;
    return true;
}

bool PlausiblePtr(uintptr_t p) {
    return p && IsPlausiblePtr((const void*)p) && !(p & 7);
}

// A 4x4 transform as the server builds it: the bottom-right element is 1 and
// three of the cells hold a world position. Checking that is what lets us
// recognise the packet without knowing anything about its class.
bool LooksLikeTransform(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    if (std::fabs(m[15] - 1.0f) > 0.001f) return false;

    // JOML writes column-major, so the translation lands in 12,13,14; accept
    // the row-major arrangement too rather than assume which one crossed the
    // wire.
    auto plausiblePos = [](float a, float b, float c) {
        return std::fabs(a) > 0.01f && std::fabs(a) < 30000.0f &&
               std::fabs(c) > 0.01f && std::fabs(c) < 30000.0f &&
               b > -512.0f && b < 1024.0f;
        };
    return plausiblePos(m[12], m[13], m[14]) || plausiblePos(m[3], m[7], m[11]);
}

std::wstring LayoutFilePath() {
    return GetThisModuleDirectory() + L"debugshape.txt";
}

// ---- runtime entry points -------------------------------------------------
// Signatures from the Hytale-Signatures project; both are call sites, so the
// rel32 that follows the E8 has to be resolved to get the real function.
const char* kSigRhpNewFast =
"E8 ? ? ? ? 4C 8B 45 ? 41 0F 10 00 0F 11 40 ? 41 0F 10 40";
const char* kSigRhpNewFastArray =
"E8 ? ? ? ? 48 8B F8 85 F6 7E ? 8B D3";

using tRhpNewFast = void* (__fastcall*)(void* methodTable);
using tRhpNewFastArray = void* (__fastcall*)(void* methodTable, int32_t count);

tRhpNewFast      pRhpNewFast = nullptr;
tRhpNewFastArray pRhpNewFastArray = nullptr;

uintptr_t ResolveCallTarget(uintptr_t siteAddr) {
    if (!siteAddr) return 0;
    int32_t rel = 0;
    if (!SafeRead((const void*)(siteAddr + 1), &rel, 4)) return 0;
    return siteAddr + 5 + (intptr_t)rel;
}

std::mutex g_dbgQueueMtx;
std::deque<DbgRequest> g_dbgQueue;

} // namespace

bool DbgResolveRuntime() {
    if (g_dbgRuntimeReady.load()) return true;

    if (!pRhpNewFast) {
        const uintptr_t site = GameModulePatternScan(kSigRhpNewFast);
        pRhpNewFast = (tRhpNewFast)ResolveCallTarget(site);
    }
    if (!pRhpNewFastArray) {
        const uintptr_t site = GameModulePatternScan(kSigRhpNewFastArray);
        pRhpNewFastArray = (tRhpNewFastArray)ResolveCallTarget(site);
    }

    const bool ok = pRhpNewFast && pRhpNewFastArray;
    g_dbgRuntimeReady.store(ok);
    if (!ok)
        DbgSetStatus("couldn't resolve the runtime allocators (RhpNewFast) in this build");
    return ok;
}

bool DbgReady() {
    std::lock_guard lock(g_dbgMtx);
    return g_dbgLayout.learned && g_dbgRuntimeReady.load();
}

// =============================================================================
// Learning
// =============================================================================
bool DbgLearnFrom(uintptr_t packetObj) {
    if (!PlausiblePtr(packetObj)) return false;

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!modBase) return false;

    uint64_t packetMT = 0;
    if (!SafeRead((const void*)packetObj, &packetMT, 8) || !packetMT) return false;

    // Find the matrix: a reference field pointing at a float[16] that reads as
    // a transform. This is the whole identification - nothing else in the
    // packet is distinctive enough on its own.
    uint32_t matrixOff = 0;
    uintptr_t matrixArr = 0;
    float m[16];
    for (uint32_t off = 8; off < kPacketWindow; off += 8) {
        uintptr_t arr = 0;
        if (!ReadPtr(packetObj + off, arr) || !PlausiblePtr(arr)) continue;

        int64_t len = 0;
        if (!SafeRead((const void*)(arr + kArrayLengthOffset), &len, 8)) continue;
        if (len != 16) continue;

        if (!SafeRead((const void*)(arr + kArrayDataOffset), m, sizeof(m))) continue;
        g_dbgSawMatrix.fetch_add(1);
        if (!LooksLikeTransform(m)) continue;

        matrixOff = off;
        matrixArr = arr;
        break;
    }
    if (!matrixOff) return false;

    uint64_t arrMT = 0;
    if (!SafeRead((const void*)matrixArr, &arrMT, 8) || !arrMT) return false;

    DbgLayout lay;
    lay.learned = true;
    lay.packetMTRva = (uint64_t)(packetMT - modBase);
    lay.floatArrayMTRva = (uint64_t)(arrMT - modBase);
    lay.offMatrix = matrixOff;
    lay.objectSize = kPacketWindow;

    // Read the rest of the object and pick out the remaining fields by shape.
    // Each one is optional: a field we cannot identify is simply left at the
    // client's default rather than filled with a guess.
    uint8_t buf[kPacketWindow];
    if (SafeRead((const void*)packetObj, buf, sizeof(buf))) {
        // Colour: three consecutive floats all within [0,1], not all zero.
        for (uint32_t off = 8; off + 12 <= kPacketWindow; off += 4) {
            if (off == matrixOff) continue;
            float c[3];
            memcpy(c, buf + off, 12);
            bool ok = true;
            float sum = 0;
            for (float v : c) {
                if (!std::isfinite(v) || v < 0.0f || v > 1.0f) { ok = false; break; }
                sum += v;
            }
            if (ok && sum > 0.05f) {
                lay.offColor = off;
                lay.haveColor = true;
                break;
            }
        }

        // Shape enum: a byte in 0..6 that is not part of a float we already
        // claimed. The server's own cube command makes this a 3, which is the
        // easiest way to confirm we got the right byte.
        for (uint32_t off = 8; off < kPacketWindow; ++off) {
            if (lay.haveColor && off >= lay.offColor && off < lay.offColor + 12) continue;
            if (off >= matrixOff && off < matrixOff + 8) continue;
            const uint8_t v = buf[off];
            if (v <= 6) {
                lay.offShape = off;
                lay.haveShape = true;
                lay.shapeIsByte = true;
                break;
            }
        }
    }

    { std::lock_guard lock(g_dbgMtx); g_dbgLayout = lay; }
    DbgSaveToDisk();

    char msg[320];
    snprintf(msg, sizeof(msg),
        "learned DisplayDebug from a real packet: %s  (matrix translation %.2f, %.2f, %.2f)",
        lay.ToString().c_str(), m[12], m[13], m[14]);
    DbgSetStatus(msg);
    return true;
}

bool DbgInspectPacket(uintptr_t packetObj) {
    if (!g_dbgLearnArmed.load()) return false;
    {
        std::lock_guard lock(g_dbgMtx);
        if (g_dbgLayout.learned) return false;
    }
    g_dbgInspected.fetch_add(1);
    if (!DbgLearnFrom(packetObj)) return false;
    g_dbgSeenCount.fetch_add(1);
    return true;
}

void DbgForget() {
    { std::lock_guard lock(g_dbgMtx); g_dbgLayout = DbgLayout{}; }
    DbgSaveToDisk();
    DbgSetStatus("forgot the learned layout");
}

// =============================================================================
// Persistence
// =============================================================================
bool DbgSaveToDisk() {
    DbgLayout lay;
    { std::lock_guard lock(g_dbgMtx); lay = g_dbgLayout; }

    std::ofstream f(LayoutFilePath(), std::ios::trunc);
    if (!f) return false;
    f << "v1\n";
    if (!lay.learned) return true;
    f << "packetmt=" << lay.packetMTRva << "\n";
    f << "arrmt=" << lay.floatArrayMTRva << "\n";
    f << "matrix=" << lay.offMatrix << "\n";
    f << "color=" << (lay.haveColor ? (int)lay.offColor : -1) << "\n";
    f << "shape=" << (lay.haveShape ? (int)lay.offShape : -1) << "\n";
    f << "time=" << (lay.haveTime ? (int)lay.offTime : -1) << "\n";
    f << "opacity=" << (lay.haveOpacity ? (int)lay.offOpacity : -1) << "\n";
    f << "flags=" << (lay.haveFlags ? (int)lay.offFlags : -1) << "\n";
    return true;
}

bool DbgLoadFromDisk() {
    std::ifstream f(LayoutFilePath());
    if (!f) return false;

    DbgLayout lay;
    std::string line;
    bool sawVersion = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "v1") { sawVersion = true; continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const long long v = atoll(line.substr(eq + 1).c_str());
        if (k == "packetmt")     lay.packetMTRva = (uint64_t)v;
        else if (k == "arrmt")   lay.floatArrayMTRva = (uint64_t)v;
        else if (k == "matrix")  lay.offMatrix = (uint32_t)v;
        else if (k == "color") { if (v >= 0) { lay.offColor = (uint32_t)v; lay.haveColor = true; } }
        else if (k == "shape") { if (v >= 0) { lay.offShape = (uint32_t)v; lay.haveShape = true; } }
        else if (k == "time") { if (v >= 0) { lay.offTime = (uint32_t)v; lay.haveTime = true; } }
        else if (k == "opacity") { if (v >= 0) { lay.offOpacity = (uint32_t)v; lay.haveOpacity = true; } }
        else if (k == "flags") { if (v >= 0) { lay.offFlags = (uint32_t)v; lay.haveFlags = true; } }
    }
    if (!sawVersion || !lay.packetMTRva || !lay.floatArrayMTRva) return false;
    lay.learned = true;

    { std::lock_guard lock(g_dbgMtx); g_dbgLayout = lay; }
    DbgSetStatus("loaded learned layout: " + lay.ToString());
    return true;
}

// =============================================================================
// Finding a shape that is already in the world
// =============================================================================
std::mutex g_dbgLiveMtx;
std::vector<DbgLiveShape> g_dbgLiveShapes;
std::atomic<bool> g_dbgScanning{ false };
std::atomic<int>  g_dbgAttachedIndex{ -1 };
std::atomic<uint64_t> g_dbgAttachWrites{ 0 };
std::atomic<size_t> g_dbgScanVisited{ 0 };

std::string DbgLiveShape::ToString() const {
    std::string s = "root";
    char buf[64];
    for (uint32_t o : path) {
        snprintf(buf, sizeof(buf), "->+0x%X", o);
        s += buf;
    }
    if (viaArray) snprintf(buf, sizeof(buf), " arr@+0x%X [+0x%X]", arrayFieldOffset, fieldOffset);
    else          snprintf(buf, sizeof(buf), " @+0x%X", fieldOffset);
    s += buf;
    if (isDouble) s += " (double)";
    return s;
}

namespace {

// Fields are searched a good way into an object: we do not know whether the
// holder is the packet, a render entry or a list node, so the scan is what
// tells us rather than an assumption about size.
constexpr uint32_t kShapeWindow = 0x1800;
constexpr int      kShapeDepth = 5;

// Read as much of an object as is actually mapped. The first version used a
// plain fixed-size read and skipped the object entirely when it failed, which
// silently excluded every short object in the graph - the same mistake the
// position finder had, reintroduced here.
size_t ReadBest(void* dst, const void* src, size_t maxLen) {
    for (size_t len = maxLen; len >= 64; len >>= 1)
        if (SafeRead(src, dst, len)) return len;
    return 0;
}

bool SafeWrite(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Describes what surrounds a hit, so a 4x4 transform is recognisable in the
// list without having to guess from the numbers alone.
std::string DescribeContext(const uint8_t* base, size_t got, uint32_t off) {
    // A JOML matrix from makeMatrix(pos, s) is scale on the diagonal, 1 in the
    // last cell, translation at 12..14 and zeroes everywhere else.
    if (off >= 48 && off + 16 <= got) {
        float m[16];
        memcpy(m, base + off - 48, sizeof(m));
        bool finite = true;
        for (float v : m) if (!std::isfinite(v)) finite = false;
        if (finite && std::fabs(m[15] - 1.0f) < 0.001f &&
            std::fabs(m[0]) > 0.0001f && std::fabs(m[0] - m[5]) < 0.001f &&
            std::fabs(m[5] - m[10]) < 0.001f) {
            char buf[64];
            snprintf(buf, sizeof(buf), "4x4 matrix, scale %.2f", m[0]);
            return buf;
        }
    }
    return "";
}

} // namespace

void DbgFindShapes(bool useCoords, double wantX, double wantY, double wantZ, double tol) {
    if (g_dbgScanning.exchange(true)) return;
    StartWorker([useCoords, wantX, wantY, wantZ, tol]() {
        WorkerGuard guard;
        { std::lock_guard lock(g_dbgLiveMtx); g_dbgLiveShapes.clear(); }
        g_dbgScanVisited.store(0);
        g_dbgAttachedIndex.store(-1);

        const uintptr_t root = GsdkCurrentRoot();
        if (!root) {
            DbgSetStatus("no live game root - join a world first");
            g_dbgScanning.store(false);
            return;
        }

        DbgSetStatus(useCoords ? "searching the graph for that position..."
                               : "searching the graph for anything shaped like a transform...");

        auto isNear = [tol](double a, double b) { return std::fabs(a - b) <= tol; };
        auto matches = [&](double a, double b, double c) {
            if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c)) return false;
            if (useCoords)
                return isNear(a, wantX) && isNear(b, wantY) && isNear(c, wantZ);
            // Without a target, fall back to "could be a world position at all".
            return std::fabs(a) > 0.01 && std::fabs(a) < 30000.0 &&
                   std::fabs(c) > 0.01 && std::fabs(c) < 30000.0 &&
                   b > -512.0 && b < 1024.0;
            };

        std::vector<DbgLiveShape> found;
        std::vector<uint8_t> buf(kShapeWindow);
        std::vector<uint8_t> abuf(kShapeWindow);
        size_t visited = 0;

        GsdkWalkGraph(root, kShapeDepth, [&](uintptr_t obj, const std::vector<uint32_t>& path) {
            // Never abort on the cap: stopping the walk here is what made a
            // buffer full of repeated positions hide the real shape entirely.
            if (found.size() >= 2000) return true;
            g_dbgScanVisited.store(++visited);

            const size_t got = ReadBest(buf.data(), (const void*)obj, kShapeWindow);
            if (!got) return true;

            uint64_t holderMT = 0;
            memcpy(&holderMT, buf.data(), 8);

            auto consider = [&](const uint8_t* base, size_t len, uint32_t off,
                                bool viaArray, uint32_t arrOff, uint64_t mt) {
                if (off + 12 <= len) {
                    float f[3];
                    memcpy(f, base + off, 12);
                    if (matches(f[0], f[1], f[2])) {
                        DbgLiveShape s;
                        s.path = path; s.fieldOffset = off; s.isDouble = false;
                        s.viaArray = viaArray; s.arrayFieldOffset = arrOff;
                        s.holderMT = mt;
                        s.x = f[0]; s.y = f[1]; s.z = f[2];
                        s.context = DescribeContext(base, len, off);
                        found.push_back(std::move(s));
                        return;
                    }
                }
                if ((off & 7) == 0 && off + 24 <= len) {
                    double d[3];
                    memcpy(d, base + off, 24);
                    if (matches(d[0], d[1], d[2])) {
                        DbgLiveShape s;
                        s.path = path; s.fieldOffset = off; s.isDouble = true;
                        s.viaArray = viaArray; s.arrayFieldOffset = arrOff;
                        s.holderMT = mt;
                        s.x = (float)d[0]; s.y = (float)d[1]; s.z = (float)d[2];
                        found.push_back(std::move(s));
                    }
                }
                };

            // (a) directly in the object.
            for (uint32_t off = 8; off + 12 <= got; off += 4) {
                consider(buf.data(), got, off, false, 0, holderMT);
                if (found.size() >= 2000) break;
            }

            // (b) inside an array the object points at. The matrix arrives as a
            // float[16], and a list of shapes would be an array of references,
            // so both are worth following one step further than the walk goes.
            for (uint32_t aoff = 8; aoff + 8 <= got; aoff += 8) {
                uint64_t v;
                memcpy(&v, buf.data() + aoff, 8);
                const uintptr_t arr = (uintptr_t)v;
                if (!arr || (arr & 7) || !IsPlausiblePtr((const void*)arr)) continue;

                int64_t len = 0;
                if (!SafeRead((const void*)(arr + 8), &len, 8)) continue;
                if (len < 3 || len > 4096) continue;

                const size_t want = (size_t)(std::min)((int64_t)kShapeWindow, len * 4 + 0x10);
                const size_t agot = ReadBest(abuf.data(), (const void*)arr, want);
                if (agot <= 0x10) continue;

                uint64_t arrMT = 0;
                memcpy(&arrMT, abuf.data(), 8);
                for (uint32_t off = 0x10; off + 12 <= agot; off += 4) {
                    consider(abuf.data(), agot, off, true, aoff, arrMT);
                    if (found.size() >= 2000) break;
                }
            }
            return true;
            });

        // Anything that sits in a recognisable 4x4 is far more likely to be a
        // shape than a lone triple in a buffer, so those go to the top and the
        // rest gets trimmed rather than flooding the list.
        std::stable_sort(found.begin(), found.end(),
            [](const DbgLiveShape& a, const DbgLiveShape& b) {
                const bool am = !a.context.empty(), bm = !b.context.empty();
                if (am != bm) return am;
                return a.path.size() < b.path.size();
            });
        if (found.size() > 60) found.resize(60);

        { std::lock_guard lock(g_dbgLiveMtx); g_dbgLiveShapes = found; }

        if (found.empty()) {
            DbgSetStatus(useCoords
                ? "nothing in the graph holds that position. Either the shape is stored "
                  "somewhere the walk can't reach, or the client keeps it somewhere other "
                  "than as three floats."
                : "no transform-shaped values found at all");
        }
        else {
            DbgSetStatus("found " + std::to_string(found.size()) +
                " place(s) holding that position, across " + std::to_string(visited) +
                " objects - attach one and see which moves the shape");
        }
        g_dbgScanning.store(false);
        });
}

void DbgAttachToPlayer(int index) {
    g_dbgAttachedIndex.store(index);
    if (index < 0) DbgSetStatus("detached");
}

namespace {

// Resolves the attached entry and writes the player position into it. Runs on
// the game thread from DbgDrainQueue.
void DriveAttachedShape() {
    const int idx = g_dbgAttachedIndex.load();
    if (idx < 0) return;

    DbgLiveShape s;
    {
        std::lock_guard lock(g_dbgLiveMtx);
        if (idx >= (int)g_dbgLiveShapes.size()) return;
        s = g_dbgLiveShapes[idx];
    }

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return;
    const uintptr_t holder = GsdkFollow(root, s.path);
    if (!holder) return;

    uintptr_t at = 0;
    if (s.viaArray) {
        uint64_t arr = 0;
        if (!SafeRead((const void*)(holder + s.arrayFieldOffset), &arr, 8) || !arr) return;
        at = (uintptr_t)arr + s.fieldOffset;
    }
    else {
        at = holder + s.fieldOffset;
    }

    const double px = g_posX.load(), py = g_posY.load(), pz = g_posZ.load();
    if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) return;

    // Only write where something position-shaped still lives. Once a shape
    // expires the client will reuse that memory for something else, and
    // scribbling coordinates into it then would be genuinely bad.
    if (s.isDouble) {
        double cur[3];
        if (!SafeRead((const void*)at, cur, sizeof(cur))) return;
        for (double v : cur) if (!std::isfinite(v) || std::fabs(v) > 30000.0) return;
        const double t[3] = { px, py, pz };
        if (SafeWrite((void*)at, t, sizeof(t))) g_dbgAttachWrites.fetch_add(1);
    }
    else {
        float cur[3];
        if (!SafeRead((const void*)at, cur, sizeof(cur))) return;
        for (float v : cur) if (!std::isfinite(v) || std::fabs(v) > 30000.0f) return;
        const float t[3] = { (float)px, (float)py, (float)pz };
        if (SafeWrite((void*)at, t, sizeof(t))) g_dbgAttachWrites.fetch_add(1);
    }
}

} // namespace

// =============================================================================
// The known layout
// =============================================================================
// Found on 2026-09-19 by searching for a remembered position and confirmed by
// attaching to it: the cube from /debug shape cube lived at
//
//     GameInstance ->+0x10 ->+0x1F0 ->+0x70 ->+0x10   matrix inline at +0x18
//
// That last hop is +0x10 into an array, i.e. element 0, so ->+0x70 IS the
// client's list of live debug shapes. Knowing that, there is nothing to search
// for: walk the three field offsets, enumerate the array, and every live shape
// is right there. Field offsets are fixed for a build, so this is as stable as
// the position path is.
//
// Searching is kept as a fallback for when a client patch moves these, but it
// should never be the first thing tried - a graph-wide search for a position
// also finds every render and animation buffer that happens to hold it.
namespace DbgKnown {
const uint32_t kArrayPath[] = { 0x10, 0x1F0, 0x70 };
// The differential finder watched two adjacent ints go 2 -> 3 the moment a
// shape was added, at +0x78 and +0x7C, with the array itself at +0x70. That is
// List<T> laid out exactly as it always is: _items, then _size, then _version.
// So the count is _size, and _version sits beside it.
constexpr uint32_t kSizeOffset = 0x78;
constexpr uint32_t kVersionOffset = 0x7C;
constexpr uint32_t kMatrixOffset = 0x18;   // 4x4, column-major
constexpr uint32_t kTransOffset = 0x48;   // = kMatrixOffset + 12 * 4
constexpr int kMaxElements = 512;
}

std::atomic<bool> g_dbgKnownWorks{ false };
std::atomic<int>  g_dbgKnownCount{ 0 };

namespace {

std::vector<uint32_t> KnownArrayPath() {
    return std::vector<uint32_t>(std::begin(DbgKnown::kArrayPath),
        std::end(DbgKnown::kArrayPath));
}

// A shape entry: a 4x4 whose last cell is 1. Deliberately not fussy about the
// translation - an entry parked at the origin is still an entry, and being
// fussy about that is what made the generic search miss things.
bool EntryHasTransform(uintptr_t holder, float* outM) {
    float m[16];
    if (!SafeRead((const void*)(holder + DbgKnown::kMatrixOffset), m, sizeof(m)))
        return false;
    for (float v : m) if (!std::isfinite(v)) return false;
    if (std::fabs(m[15] - 1.0f) > 0.001f) return false;
    if (outM) memcpy(outM, m, sizeof(m));
    return true;
}

} // namespace

bool DbgRefreshKnownShapes() {
    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return false;

    // Whatever DbgLocateList last settled on, falling back to the path found in
    // singleplayer. That one is only a starting guess: it is a shortest path
    // through the player entity, which can alias the real owner.
    std::vector<uint32_t> arrayPath = KnownArrayPath();
    {
        std::lock_guard lock(g_dbgContMtx);
        if (!g_dbgContainer.arrayPath.empty()) arrayPath = g_dbgContainer.arrayPath;
    }
    const uintptr_t arr = GsdkFollow(root, arrayPath);
    if (!arr) { g_dbgKnownWorks.store(false); return false; }

    // The length is a 32-bit field; the four bytes above it are padding.
    uint32_t len32 = 0;
    if (!SafeRead((const void*)(arr + 8), &len32, 4)) { g_dbgKnownWorks.store(false); return false; }
    if (len32 > 65536) { g_dbgKnownWorks.store(false); return false; }
    const int64_t len = (int64_t)len32;

    // Note when the array object itself changes. If the client puts its own
    // empty one back, everything written into ours quietly stops existing.
    {
        const uintptr_t prev = g_dbgLastArrayAddr.exchange(arr);
        if (prev && prev != arr) g_dbgArraySwaps.fetch_add(1);
    }
    // len == 0 is the normal state before anything is added: List<T> points at
    // the shared Array.Empty<T>() until its first insert. Treating that as a
    // failure is what left the array MethodTable unknown on servers - and the
    // array MethodTable is exactly what making a real array needs.

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    const size_t modSizeForArr = modSize;

    std::vector<DbgLiveShape> out;
    uint64_t firstMT = 0;
    uintptr_t firstHolder = 0;
    int nonNull = 0;

    // Counting runs over the whole array - it is one read per slot, and a count
    // that stops at 512 is a lie once a build is outlined. Describing each shape
    // is what gets capped, because that list is only ever read by a person.
    for (int64_t i = 0; i < len; ++i) {
        uint64_t e = 0;
        if (!SafeRead((const void*)(arr + 0x10 + i * 8), &e, 8)) break;
        if (!e) continue;
        ++nonNull;
        if ((int)out.size() >= DbgKnown::kMaxElements) continue;

        const uintptr_t holder = (uintptr_t)e;
        float m[16];
        if (!EntryHasTransform(holder, m)) continue;

        DbgLiveShape s;
        s.path = arrayPath;
        s.path.push_back((uint32_t)(0x10 + i * 8));   // the element itself
        s.fieldOffset = DbgKnown::kTransOffset;
        s.viaArray = false;
        s.isDouble = false;
        SafeRead((const void*)holder, &s.holderMT, 8);
        s.x = m[12]; s.y = m[13]; s.z = m[14];
        char buf[64];
        snprintf(buf, sizeof(buf), "slot %lld, scale %.2f", (long long)i, m[0]);
        s.context = buf;
        out.push_back(std::move(s));

        if (!firstMT) { firstMT = s.holderMT; firstHolder = holder; }
    }

    g_dbgKnownWorks.store(true);
    g_dbgKnownCount.store(nonNull);

    {
        std::lock_guard lock(g_dbgLiveMtx);
        // Keep the attached entry pointing at the same slot across refreshes.
        const int attached = g_dbgAttachedIndex.load();
        std::vector<uint32_t> attachedPath;
        if (attached >= 0 && attached < (int)g_dbgLiveShapes.size())
            attachedPath = g_dbgLiveShapes[attached].path;

        g_dbgLiveShapes = out;

        if (!attachedPath.empty()) {
            int found = -1;
            for (size_t i = 0; i < g_dbgLiveShapes.size(); ++i)
                if (g_dbgLiveShapes[i].path == attachedPath) { found = (int)i; break; }
            g_dbgAttachedIndex.store(found);
        }
    }

    // Everything spawning needs falls out of the same walk. The array part works
    // with an empty list, which is the normal state on a server - only the
    // MethodTable and the template need a shape to exist, and those can come
    // from disk instead.
    if (modBase) {
        DbgContainer c;
        c.isArray = true;
        c.arrayPath = arrayPath;
        c.arrayLength = (uint32_t)len;
        c.liveSlots = nonNull;
        c.matrixOffset = DbgKnown::kMatrixOffset;
        c.transOffset = DbgKnown::kTransOffset;

        // Worth grabbing whenever the list exists: on a server where it does
        // not, this is the only way to make one.
        // Both MethodTables, live, from the array that is right here. Nothing
        // is remembered across sessions because nothing can be: these types are
        // built at runtime and move.
        uint64_t aMT = 0;
        if (SafeRead((const void*)arr, &aMT, 8) && aMT && !(aMT & 7)) {
            c.arrayMTAbs = aMT;
            c.arrayMTRva = (aMT > modBase && aMT < modBase + modSizeForArr)
                ? (aMT - modBase) : 0;

            // A MethodTable's related type is at +8; for an array that is the
            // element type, i.e. exactly the entry type we allocate.
            uint64_t eMT = 0;
            if (SafeRead((const void*)(aMT + 8), &eMT, 8) && eMT && !(eMT & 7)) {
                uint32_t ebase = 0;
                if (SafeRead((const void*)(eMT + 4), &ebase, 4) &&
                    ebase >= 32 && ebase <= 0x1000) {
                    c.entryMTAbs = eMT;
                    c.entrySize = ebase;
                    c.holderMTRva = (eMT > modBase) ? (eMT - modBase) : 0;
                }
            }
        }

        if (firstHolder) {
            // A live entry is the most direct evidence of the type; prefer it,
            // but the derivation above is what makes an empty list workable.
            c.entryMTAbs = firstMT;
            c.holderMTRva = (uint64_t)(firstMT - modBase);
            if (SafeRead((const void*)firstHolder, c.templateBytes, sizeof(c.templateBytes)))
                c.haveTemplate = true;
            uint32_t bs = 0;
            if (SafeRead((const void*)((uintptr_t)firstMT + 4), &bs, 4) &&
                bs >= 16 && bs <= 0x1000)
                c.entrySize = bs;
        }
        else {
            // Nothing live: the template still comes from before, but the types
            // stay as derived above rather than as remembered numbers.
            std::lock_guard lock(g_dbgContMtx);
            c.haveTemplate = g_dbgContainer.haveTemplate;
            c.seeded = g_dbgContainer.seeded;
            if (!c.entrySize) c.entrySize = g_dbgContainer.entrySize;
            if (!c.entryMTAbs) {
                c.entryMTAbs = g_dbgContainer.entryMTAbs;
                c.holderMTRva = g_dbgContainer.holderMTRva;
            }
            memcpy(c.templateBytes, g_dbgContainer.templateBytes, sizeof(c.templateBytes));
        }

        // The count the client iterates to. Looked for in the object that owns
        // the array, as the int agreeing with how many slots are in use.
        std::vector<uint32_t> ownerPath(arrayPath.begin(), arrayPath.end() - 1);
        c.countPath = ownerPath;
        {
            // A seeded offset wins: it was measured, the scan below only matches.
            std::lock_guard lock(g_dbgContMtx);
            if (g_dbgContainer.countFromSeed) {
                c.countFromSeed = true;
                c.haveCount = true;
                c.countOffset = g_dbgContainer.countOffset;
            }
            c.ownArray = g_dbgContainer.ownArray;
        }
        const uintptr_t owner = c.countFromSeed ? 0 : GsdkFollow(root, ownerPath);
        if (owner) {
            uint8_t buf[0x200];
            if (SafeRead((const void*)owner, buf, sizeof(buf))) {
                for (uint32_t off = 8; off + 4 <= sizeof(buf); off += 4) {
                    int32_t v;
                    memcpy(&v, buf + off, 4);
                    if (v == nonNull) {
                        if (!c.haveCount) {
                            c.countPath = ownerPath;
                            c.countOffset = off;
                            c.haveCount = true;
                        }
                        ++c.countCandidates;
                    }
                }
            }
        }

        // The layout worked out from the entry report. Applied by default so
        // nothing has to be discovered or typed in again; "Apply offsets" sets
        // manualFields and then these are left alone.
        {
            std::lock_guard lock(g_dbgContMtx);
            c.manualFields = g_dbgContainer.manualFields;
            if (c.manualFields) {
                c.haveColor = g_dbgContainer.haveColor;
                c.colorOffset = g_dbgContainer.colorOffset;
                c.haveLife = g_dbgContainer.haveLife;
                c.lifeOffset = g_dbgContainer.lifeOffset;
                c.lifeMax = g_dbgContainer.lifeMax;
            }
            else {
                // The count belongs to the object holding the array, not the
                // entry - but it is just as fixed, so it gets a default too.
                if (!c.countFromSeed) {
                    c.haveCount = true;
                    c.countOffset = DbgKnown::kSizeOffset;
                }
                c.haveLife = true;        c.lifeOffset = 0x08;
                c.haveLifeTotal = true;   c.lifeTotalOffset = 0x0C;
                c.haveOpacity = true;     c.opacityOffset = 0x10;
                c.haveShapeEnum = true;   c.shapeOffset = 0x14;
                c.haveColor = true;       c.colorOffset = 0x58;
                float total = 0;
                if (firstHolder &&
                    SafeRead((const void*)(firstHolder + 0x0C), &total, 4) &&
                    std::isfinite(total) && total > 1.0f && total < 100000.0f)
                    c.lifeMax = total;
                else if (g_dbgContainer.lifeMax > 1.0f)
                    c.lifeMax = g_dbgContainer.lifeMax;
            }
        }

        c.valid = c.haveTemplate && c.entryMTAbs != 0;
        {
            std::lock_guard lock(g_dbgContMtx);
            g_dbgContainer = c;
        }

        // Save the seed the first time a genuine shape hands us one. Forgetting
        // to press the button in a singleplayer world and then finding nothing
        // works on a server is a bad way to learn about it.
        static std::atomic<bool> seedSaved{ false };
        if (firstHolder && c.valid && !c.seeded && !seedSaved.exchange(true))
            DbgSaveSeed();
        return true;
    }

    return true;
}

void DbgShapeFrameTick() {
    DbgKeepAliveTick();
    DbgAutoTick();
}

void DbgKnownTick() {
    // Cheap: three pointer hops and a short array walk. Throttled anyway so a
    // stale list never sits on screen for long.
    static long long lastMs = 0;
    const long long now = NowSteadyMs();
    if (now - lastMs < 400) return;
    lastMs = now;
    DbgRefreshKnownShapes();

    // The allocator can only be identified once we know a MethodTable to
    // recognise allocation sites by, which the refresh above is what provides.
    // So the moment a shape exists, go and find it - on a worker, since this
    // runs on the game thread and the scan walks the whole image.
    static std::atomic<bool> allocTried{ false };
    if (!pRhpNewFast && !allocTried.load()) {
        bool haveMT = false;
        { std::lock_guard lock(g_dbgContMtx); haveMT = g_dbgContainer.holderMTRva != 0; }
        if (haveMT) {
            allocTried.store(true);
            StartWorker([]() {
                DbgResolveAllocator();
                // Reading code to find the barrier costs nothing and calls
                // nothing; only the test below ever calls it.
                DbgResolveWriteBarrier();
                });
        }
    }
}

// =============================================================================
// Finding the allocator without a signature for it
// =============================================================================
// The published RhpNewFast signature is a call site inside one particular
// function, and the code around it has changed - it matches nothing here. But
// NativeAOT allocates every object the same way:
//
//     lea  rcx, [SomeMethodTable]
//     call RhpNewFast
//
// and we already know two real MethodTables: the shape entry's, read off a live
// shape, and System.String's, worked out by shape elsewhere. So scan the image
// for that pair, keep only the sites whose lea lands on a MethodTable we
// recognise, and let the call targets vote. The winner is the allocator. This
// asks the binary what it does rather than hoping a byte pattern survived.
std::atomic<uintptr_t> g_dbgAllocRva{ 0 };
std::atomic<int>       g_dbgAllocVotes{ 0 };
std::atomic<bool>      g_dbgAllocVerified{ false };
std::atomic<bool>      g_dbgKeepAlive{ false };

namespace {
std::mutex g_dbgEvidenceMtx;
std::string g_dbgAllocEvidence;
}

std::string DbgGetAllocEvidence() {
    std::lock_guard lock(g_dbgEvidenceMtx);
    return g_dbgAllocEvidence;
}

namespace {

bool ScanChunk(std::vector<uint8_t>& buf, uintptr_t at, size_t want) {
    return SafeRead((const void*)at, buf.data(), want);
}

} // namespace

bool DbgResolveAllocator() {
    if (pRhpNewFast) return true;

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!modBase || !modSize) return false;

    // Every MethodTable we can vouch for. More is better - a site whose lea
    // lands on one of these is an allocation of a type we have actually seen.
    std::vector<uint64_t> known;
    {
        std::lock_guard lock(g_dbgContMtx);
        if (g_dbgContainer.holderMTRva)
            known.push_back((uint64_t)(modBase + g_dbgContainer.holderMTRva));
    }
    if (g_stringVT.load()) known.push_back(g_stringVT.load());

    if (known.empty()) {
        DbgSetStatus("can't find the allocator yet - draw a shape first so there is a "
            "MethodTable to recognise allocation sites by");
        return false;
    }

    std::unordered_map<uintptr_t, int> votes;
    constexpr size_t kChunk = 1 * 1024 * 1024;
    std::vector<uint8_t> buf(kChunk + 16);

    for (uintptr_t off = 0; off + 16 < modSize; ) {
        const size_t want = (size_t)(std::min)((uintptr_t)kChunk, (uintptr_t)(modSize - off));
        if (!ScanChunk(buf, modBase + off, want + 16 <= modSize - off ? want + 16 : want)) {
            off += want;
            continue;
        }
        const size_t limit = want;
        for (size_t i = 0; i + 12 < limit; ++i) {
            // lea rcx, [rip+disp32]
            if (buf[i] != 0x48 || buf[i + 1] != 0x8D || buf[i + 2] != 0x0D) continue;
            if (buf[i + 7] != 0xE8) continue;                  // must be followed by a call

            int32_t leaRel, callRel;
            memcpy(&leaRel, buf.data() + i + 3, 4);
            memcpy(&callRel, buf.data() + i + 8, 4);

            const uintptr_t site = modBase + off + i;
            const uintptr_t leaTarget = site + 7 + (intptr_t)leaRel;

            bool isKnown = false;
            for (uint64_t mt : known) if ((uintptr_t)mt == leaTarget) { isKnown = true; break; }
            if (!isKnown) continue;

            const uintptr_t callee = site + 12 + (intptr_t)callRel;
            if (callee < modBase || callee >= modBase + modSize) continue;
            ++votes[callee];
        }
        off += want;
    }

    // Votes alone are not evidence. Plenty of helpers take a MethodTable in rcx
    // - cast checks, type handles, static-constructor helpers - and calling one
    // of those instead hangs the process, which is exactly what happened. So
    // read each candidate's own code and require it to behave like an
    // allocator: RhpNewFast's first job is to fetch the object's size out of
    // the MethodTable it was handed, i.e. `mov eax, [rcx+4]` == 8B 41 04.
    auto looksLikeAllocator = [](uintptr_t fn, std::string& why) {
        uint8_t code[0x80];
        if (!SafeRead((const void*)fn, code, sizeof(code))) { why = "unreadable"; return false; }
        int sizeRead = -1, storeMT = -1;
        for (int i = 0; i + 3 < (int)sizeof(code); ++i) {
            if (sizeRead < 0 && code[i] == 0x8B && code[i + 1] == 0x41 && code[i + 2] == 0x04)
                sizeRead = i;                                   // mov eax, [rcx+4]
            if (storeMT < 0 && code[i] == 0x48 && code[i + 1] == 0x89 && code[i + 2] == 0x08)
                storeMT = i;                                    // mov [rax], rcx
        }
        char buf[160];
        snprintf(buf, sizeof(buf), "base-size read %s, MethodTable store %s",
            sizeRead >= 0 ? "found" : "MISSING", storeMT >= 0 ? "found" : "missing");
        why = buf;
        return sizeRead >= 0;
    };

    uintptr_t best = 0;
    int bestVotes = 0;
    std::string bestWhy;
    for (const auto& kv : votes) {
        std::string why;
        if (!looksLikeAllocator(kv.first, why)) continue;
        if (kv.second > bestVotes) { bestVotes = kv.second; best = kv.first; bestWhy = why; }
    }

    if (!best) {
        // Say what was rejected, so this is diagnosable rather than just "no".
        std::string ev = "no candidate passed the code check. Candidates:\n";
        for (const auto& kv : votes) {
            std::string why;
            looksLikeAllocator(kv.first, why);
            char line[200];
            snprintf(line, sizeof(line), "  +0x%llX  %d vote(s)  %s\n",
                (unsigned long long)(kv.first - modBase), kv.second, why.c_str());
            ev += line;
        }
        { std::lock_guard lock(g_dbgEvidenceMtx); g_dbgAllocEvidence = ev; }
        DbgSetStatus("the allocator could not be identified safely - nothing will be called");
        return false;
    }

    pRhpNewFast = (tRhpNewFast)best;
    g_dbgAllocRva.store(best - modBase);
    g_dbgAllocVotes.store(bestVotes);
    g_dbgAllocVerified.store(true);
    {
        std::lock_guard lock(g_dbgEvidenceMtx);
        g_dbgAllocEvidence = "chosen +0x" + std::to_string(best - modBase) + ": " + bestWhy;
    }

    char msg[160];
    snprintf(msg, sizeof(msg),
        "allocator identified at +0x%llX from %d allocation site(s)",
        (unsigned long long)(best - modBase), bestVotes);
    DbgSetStatus(msg);
    return true;
}

// =============================================================================
// Working out what the other fields in an entry are
// =============================================================================
// Cloning gets a shape drawn, but everything except the transform arrives as a
// copy of whatever the template happened to hold - which is why spawned shapes
// ignored the colour picker and vanished almost at once. The template is taken
// from a shape that has already been counting down, so a clone inherits its
// remaining life, not a full one.
//
// Both fields are findable by watching rather than guessing, which is the only
// approach that has worked on this build:
//   lifetime - the float that ticks steadily down while the shape is alive.
//   colour   - three consecutive floats in [0,1] that do not move at all.
// /debug shape cube picks a random colour every time, so the colour is also the
// thing that differs between two entries, which is a useful cross-check.
std::atomic<bool> g_dbgProfiling{ false };
std::atomic<uint64_t> g_dbgKeepAliveWrites{ 0 };

namespace {

// One live entry, if there is one. The list refresh keeps this current.
uintptr_t FirstLiveHolder() {
    std::lock_guard lock(g_dbgLiveMtx);
    if (g_dbgLiveShapes.empty()) return 0;
    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return 0;
    return GsdkFollow(root, g_dbgLiveShapes[0].path);
}

} // namespace

void DbgProfileEntry() {
    if (g_dbgProfiling.exchange(true)) return;
    StartWorker([]() {
        WorkerGuard guard;

        const uintptr_t holder = FirstLiveHolder();
        if (!holder) {
            DbgSetStatus("no live shape to study - run /debug shape cube first");
            g_dbgProfiling.store(false);
            return;
        }

        uint64_t mt = 0;
        uint32_t baseSize = 0;
        if (!SafeRead((const void*)holder, &mt, 8) || !mt ||
            !SafeRead((const void*)((uintptr_t)mt + 4), &baseSize, 4) ||
            baseSize < 32 || baseSize > 0x1000) {
            DbgSetStatus("couldn't read the entry's size");
            g_dbgProfiling.store(false);
            return;
        }

        DbgSetStatus("watching the shape for a couple of seconds...");

        constexpr int kSamples = 14;
        std::vector<std::vector<float>> series;
        const uint32_t slots = (baseSize - 8) / 4;

        for (int pass = 0; pass < kSamples; ++pass) {
            std::vector<uint8_t> buf(baseSize);
            if (!SafeRead((const void*)holder, buf.data(), baseSize)) break;
            std::vector<float> row(slots);
            for (uint32_t i = 0; i < slots; ++i)
                memcpy(&row[i], buf.data() + 8 + i * 4, 4);
            series.push_back(std::move(row));
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }

        if (series.size() < 6) {
            DbgSetStatus("the shape went away before it could be studied - try again "
                "right after drawing one");
            g_dbgProfiling.store(false);
            return;
        }

        // ---- lifetime: strictly decreasing, positive, steady ----
        int lifeSlot = -1;
        float lifeBest = 0, lifeStart = 0;
        for (uint32_t i = 0; i < slots; ++i) {
            float first = series.front()[i], last = series.back()[i];
            if (!std::isfinite(first) || !std::isfinite(last)) continue;
            if (first <= 0.0f || last < 0.0f) continue;
            if (first > 100000.0f) continue;

            bool monotonic = true;
            for (size_t p = 1; p < series.size(); ++p) {
                if (series[p][i] > series[p - 1][i] + 0.0001f) { monotonic = false; break; }
            }
            if (!monotonic) continue;

            const float drop = first - last;
            if (drop < 0.05f) continue;          // not actually counting down
            if (drop > lifeBest) { lifeBest = drop; lifeSlot = (int)i; lifeStart = first; }
        }

        // ---- colour: three consecutive floats in [0,1] that never move ----
        int colorSlot = -1;
        for (uint32_t i = 0; i + 2 < slots; ++i) {
            bool ok = true;
            float sum = 0;
            for (uint32_t k = 0; k < 3 && ok; ++k) {
                const float v = series.front()[i + k];
                if (!std::isfinite(v) || v < 0.0f || v > 1.0f) ok = false;
                sum += v;
                for (size_t p = 1; p < series.size() && ok; ++p)
                    if (std::fabs(series[p][i + k] - v) > 0.0001f) ok = false;
            }
            // All-zero and all-one triples are everywhere; a random colour is not.
            if (ok && sum > 0.05f && sum < 2.95f) { colorSlot = (int)i; break; }
        }

        std::string msg;
        {
            std::lock_guard lock(g_dbgContMtx);
            if (lifeSlot >= 0) {
                g_dbgContainer.haveLife = true;
                g_dbgContainer.lifeOffset = 8 + (uint32_t)lifeSlot * 4;
                g_dbgContainer.lifeMax = lifeStart;
            }
            if (colorSlot >= 0) {
                g_dbgContainer.haveColor = true;
                g_dbgContainer.colorOffset = 8 + (uint32_t)colorSlot * 4;
            }
            char buf[224];
            snprintf(buf, sizeof(buf),
                "lifetime %s%s%X (started at %.1f, dropped %.1f), colour %s%s%X",
                lifeSlot >= 0 ? "at +0x" : "NOT FOUND", lifeSlot >= 0 ? "" : " ",
                lifeSlot >= 0 ? g_dbgContainer.lifeOffset : 0, lifeStart, lifeBest,
                colorSlot >= 0 ? "at +0x" : "NOT FOUND", colorSlot >= 0 ? "" : " ",
                colorSlot >= 0 ? g_dbgContainer.colorOffset : 0);
            msg = buf;
        }
        DbgSetStatus(msg);
        g_dbgProfiling.store(false);
        });
}

// Refreshes the countdown on every live entry, so nothing expires. Cheaper and
// more surgical than rewriting the whole entry from a template, and it keeps
// spawned shapes alive too.
void DbgKeepAliveTick() {
    if (!g_dbgKeepAlive.load()) return;

    // Every live entry gets written every time this runs, so at two thousand
    // shapes a per-frame refresh is several thousand guarded writes a frame for
    // no benefit at all: the countdown it resets is measured in tens of
    // seconds. Twice a second is still two orders of magnitude of headroom.
    static long long lastMs = 0;
    const long long now = NowSteadyMs();
    if (now - lastMs < 500) return;
    lastMs = now;

    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    if (!c.valid || !c.haveLife) return;

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return;
    const uintptr_t arr = GsdkFollow(root, c.arrayPath);
    if (!arr) return;

    int64_t len = 0;
    if (!SafeRead((const void*)(arr + 8), &len, 8) || len < 1 || len > 65536) return;

    // Clamped deliberately: this value is written into every live entry every
    // frame, so if it were ever nonsense it would be nonsense everywhere at
    // once - which is the shape of the crash the client reported.
    float life = c.lifeMax;
    if (!std::isfinite(life) || life < 1.0f || life > 100000.0f) life = 30.0f;
    for (int64_t i = 0; i < len; ++i) {
        uint64_t e = 0;
        if (!SafeRead((const void*)(arr + 0x10 + i * 8), &e, 8) || !e) continue;
        float cur = 0;
        if (!SafeRead((const void*)((uintptr_t)e + c.lifeOffset), &cur, 4)) continue;
        if (!std::isfinite(cur)) continue;
        SafeWrite((void*)((uintptr_t)e + c.lifeOffset), &life, 4);
        if (c.haveLifeTotal)
            SafeWrite((void*)((uintptr_t)e + c.lifeTotalOffset), &life, 4);
        g_dbgKeepAliveWrites.fetch_add(1);
    }
}

// =============================================================================
// What is actually in an entry
// =============================================================================
// Three heuristics in a row have now picked the wrong field, so this stops
// guessing and prints the entry instead. Two things make a field identifiable
// without knowing the class:
//
//   - what CHANGES while one shape sits there ageing  -> the countdown
//   - what DIFFERS between two shapes of the same age -> colour, position, id
//
// /debug shape cube picks a random colour every time, so drawing two and
// diffing them isolates the colour precisely. Run the command twice before
// pressing the button and both columns get filled in.
std::mutex g_dbgEntryReportMtx;
std::string g_dbgEntryReport;

std::string DbgGetEntryReport() {
    std::lock_guard lock(g_dbgEntryReportMtx);
    return g_dbgEntryReport;
}

void DbgSetFieldOffsets(int colorOff, int lifeOff, float lifeMax) {
    std::lock_guard lock(g_dbgContMtx);
    g_dbgContainer.manualFields = true;
    g_dbgContainer.haveColor = colorOff >= 0;
    g_dbgContainer.colorOffset = colorOff >= 0 ? (uint32_t)colorOff : 0;
    g_dbgContainer.haveLife = lifeOff >= 0;
    g_dbgContainer.lifeOffset = lifeOff >= 0 ? (uint32_t)lifeOff : 0;
    if (lifeMax > 0.0f) g_dbgContainer.lifeMax = lifeMax;
}

void DbgBuildEntryReport() {
    if (g_dbgProfiling.exchange(true)) return;
    StartWorker([]() {
        WorkerGuard guard;

        std::vector<std::vector<uint32_t>> paths;
        {
            std::lock_guard lock(g_dbgLiveMtx);
            for (const auto& s : g_dbgLiveShapes) paths.push_back(s.path);
        }
        const uintptr_t root = GsdkCurrentRoot();
        if (paths.empty() || !root) {
            { std::lock_guard lock(g_dbgEntryReportMtx);
              g_dbgEntryReport = "no live shape - run /debug shape cube first"; }
            g_dbgProfiling.store(false);
            return;
        }

        const uintptr_t a = GsdkFollow(root, paths[0]);
        const uintptr_t b = paths.size() > 1 ? GsdkFollow(root, paths[1]) : 0;
        if (!a) {
            { std::lock_guard lock(g_dbgEntryReportMtx);
              g_dbgEntryReport = "the shape went away before it could be read"; }
            g_dbgProfiling.store(false);
            return;
        }

        uint64_t mt = 0;
        uint32_t baseSize = 0;
        if (!SafeRead((const void*)a, &mt, 8) || !mt ||
            !SafeRead((const void*)((uintptr_t)mt + 4), &baseSize, 4) ||
            baseSize < 16 || baseSize > 0x1000) {
            { std::lock_guard lock(g_dbgEntryReportMtx);
              g_dbgEntryReport = "couldn't read the entry size"; }
            g_dbgProfiling.store(false);
            return;
        }

        std::vector<uint8_t> first(baseSize), last(baseSize), other(baseSize, 0);
        if (!SafeRead((const void*)a, first.data(), baseSize)) {
            g_dbgProfiling.store(false);
            return;
        }
        const bool haveOther = b && SafeRead((const void*)b, other.data(), baseSize);

        for (int i = 0; i < 16; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (!SafeRead((const void*)a, last.data(), baseSize)) {
            g_dbgProfiling.store(false);
            return;
        }

        DbgContainer c;
        { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }

        std::string rep;
        char line[300];
        snprintf(line, sizeof(line),
            "entry is %u bytes; matrix at +0x%X; %s second shape to compare against\n"
            "offset   float        int          hex       notes\n",
            baseSize, c.matrixOffset, haveOther ? "using a" : "NO");
        rep += line;

        int bestLife = -1;
        float bestDrop = 0, bestStart = 0;

        for (uint32_t off = 8; off + 4 <= baseSize; off += 4) {
            float f0, f1, fo = 0;
            int32_t i0;
            memcpy(&f0, first.data() + off, 4);
            memcpy(&f1, last.data() + off, 4);
            memcpy(&i0, first.data() + off, 4);
            if (haveOther) memcpy(&fo, other.data() + off, 4);

            const bool inMatrix = (off >= c.matrixOffset && off < c.matrixOffset + 64);
            const bool changed = memcmp(first.data() + off, last.data() + off, 4) != 0;
            const bool differs = haveOther &&
                memcmp(first.data() + off, other.data() + off, 4) != 0;

            std::string notes;
            if (inMatrix) notes += "[matrix] ";
            if (changed) {
                char t[96];
                snprintf(t, sizeof(t), "CHANGED %.3f -> %.3f  ", f0, f1);
                notes += t;
                if (std::isfinite(f0) && std::isfinite(f1) && f0 > f1 && f1 >= 0.0f &&
                    f0 < 100000.0f && !inMatrix && (f0 - f1) > bestDrop) {
                    bestDrop = f0 - f1; bestLife = (int)off; bestStart = f0;
                }
            }
            if (differs) {
                char t[96];
                snprintf(t, sizeof(t), "DIFFERS (other %.3f)  ", fo);
                notes += t;
            }
            if (!inMatrix && std::isfinite(f0) && f0 >= 0.0f && f0 <= 1.0f && f0 != 0.0f)
                notes += "[0..1] ";

            // Only print rows that carry information; a full 192-byte dump of
            // zeroes is noise and buries the two rows that matter.
            if (notes.empty() && f0 == 0.0f && i0 == 0) continue;

            snprintf(line, sizeof(line), "+0x%03X  %11.4f  %11d  %08X  %s\n",
                off, f0, i0, (unsigned)i0, notes.c_str());
            rep += line;
        }

        if (bestLife >= 0) {
            snprintf(line, sizeof(line),
                "\nbest countdown guess: +0x%X (fell %.2f in 2.4s, started at %.2f)\n",
                bestLife, bestDrop, bestStart);
            rep += line;
            std::lock_guard lock(g_dbgContMtx);
            if (!g_dbgContainer.haveLife) {
                g_dbgContainer.haveLife = true;
                g_dbgContainer.lifeOffset = (uint32_t)bestLife;
                g_dbgContainer.lifeMax = bestStart > 1.0f ? bestStart : 30.0f;
            }
        }
        else {
            rep += "\nnothing counted down - the lifetime may not live in the entry, or "
                   "may be an absolute expiry time rather than a remaining one.\n";
        }
        if (!haveOther)
            rep += "Draw a SECOND cube and run this again: the colour is random per "
                   "command, so the fields that differ between two shapes pin it down.\n";

        { std::lock_guard lock(g_dbgEntryReportMtx); g_dbgEntryReport = rep; }
        DbgSetStatus("entry report ready");
        g_dbgProfiling.store(false);
        });
}

// =============================================================================
// The count field
// =============================================================================
// ArgumentOutOfRangeException is a managed exception, not a bad pointer - so
// the crash is not the writes, it is something indexing past the end of a
// collection. The count is the only thing here that was ever picked by a guess
// ("an int that happens to equal the number of slots in use"), and plenty of
// ints in an object equal 1 when one shape is out.
//
// So find it the way everything else here got found: watch what changes when a
// shape is genuinely added. Arm it, run /debug shape cube, and the int that
// goes from N to N+1 at the moment the shape appears is the count. Nothing
// else in the object will do that.
std::atomic<bool> g_dbgCountArmed{ false };
// Kept only so nothing else has to change; the count is always maintained now.
std::atomic<bool> g_dbgBumpCount{ true };
std::mutex g_dbgCountReportMtx;
std::string g_dbgCountReport;

std::string DbgGetCountReport() {
    std::lock_guard lock(g_dbgCountReportMtx);
    return g_dbgCountReport;
}

namespace {

int CountNonNullSlots(uintptr_t arr, int64_t len) {
    int n = 0;
    for (int64_t i = 0; i < len; ++i) {
        uint64_t e = 0;
        if (!SafeRead((const void*)(arr + 0x10 + i * 8), &e, 8)) break;
        if (e) ++n;
    }
    return n;
}

} // namespace

void DbgFindCountField() {
    if (g_dbgCountArmed.exchange(true)) return;
    StartWorker([]() {
        WorkerGuard guard;

        DbgContainer c;
        { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
        if (!c.isArray || c.arrayPath.empty()) {
            { std::lock_guard lock(g_dbgCountReportMtx);
              g_dbgCountReport = "no shape array resolved yet"; }
            g_dbgCountArmed.store(false);
            return;
        }

        std::vector<uint32_t> ownerPath(c.arrayPath.begin(), c.arrayPath.end() - 1);

        const uintptr_t root0 = GsdkCurrentRoot();
        const uintptr_t owner0 = GsdkFollow(root0, ownerPath);
        const uintptr_t arr0 = GsdkFollow(root0, c.arrayPath);
        int64_t len = 0;
        if (!owner0 || !arr0 || !SafeRead((const void*)(arr0 + 8), &len, 8) || len < 1) {
            { std::lock_guard lock(g_dbgCountReportMtx);
              g_dbgCountReport = "couldn't read the array"; }
            g_dbgCountArmed.store(false);
            return;
        }

        std::vector<uint8_t> before(0x200, 0);
        if (!SafeRead((const void*)owner0, before.data(), before.size())) {
            { std::lock_guard lock(g_dbgCountReportMtx);
              g_dbgCountReport = "couldn't read the owner object"; }
            g_dbgCountArmed.store(false);
            return;
        }
        const int slotsBefore = CountNonNullSlots(arr0, len);

        {
            std::lock_guard lock(g_dbgCountReportMtx);
            g_dbgCountReport = "armed - run /debug shape cube now (waiting up to 30s)";
        }

        // Wait for a shape to genuinely appear.
        std::vector<uint8_t> after(0x200, 0);
        bool sawIt = false;
        for (int i = 0; i < 150 && !g_shuttingDown.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const uintptr_t root = GsdkCurrentRoot();
            const uintptr_t arr = GsdkFollow(root, c.arrayPath);
            const uintptr_t owner = GsdkFollow(root, ownerPath);
            if (!arr || !owner) continue;
            if (CountNonNullSlots(arr, len) <= slotsBefore) continue;
            if (!SafeRead((const void*)owner, after.data(), after.size())) continue;
            sawIt = true;
            break;
        }

        if (!sawIt) {
            { std::lock_guard lock(g_dbgCountReportMtx);
              g_dbgCountReport = "no new shape appeared - nothing to compare"; }
            g_dbgCountArmed.store(false);
            return;
        }

        std::string rep = "ints that changed when a shape was added:\n";
        int best = -1;
        char line[200];
        for (uint32_t off = 8; off + 4 <= before.size(); off += 4) {
            int32_t a, b;
            memcpy(&a, before.data() + off, 4);
            memcpy(&b, after.data() + off, 4);
            if (a == b) continue;
            snprintf(line, sizeof(line), "  +0x%03X  %d -> %d%s\n", off, a, b,
                (b == a + 1) ? "   <- incremented by one" : "");
            rep += line;
            // The count goes up by exactly one per shape. Anything jumping
            // around is a timestamp or a version counter.
            if (b == a + 1 && b >= 0 && b <= (int)len && best < 0) best = (int)off;
        }

        if (best >= 0) {
            snprintf(line, sizeof(line),
                "\ncount field is +0x%X (was +0x%X by the old guess)\n",
                best, c.haveCount ? c.countOffset : 0);
            rep += line;
            std::lock_guard lock(g_dbgContMtx);
            g_dbgContainer.haveCount = true;
            g_dbgContainer.countPath = ownerPath;
            g_dbgContainer.countOffset = (uint32_t)best;
            g_dbgContainer.countCandidates = 1;
        }
        else {
            rep += "\nnothing incremented by exactly one. The client may not keep a "
                   "count here at all - in which case bumping one is what broke it, "
                   "and leaving \"bump the count\" off is the fix.\n";
        }

        { std::lock_guard lock(g_dbgCountReportMtx); g_dbgCountReport = rep; }
        g_dbgCountArmed.store(false);
        });
}

// Empties the list: nulls every slot and zeroes the count. A way back to a
// clean state after an experiment goes wrong, without restarting the game.
void DbgGrowList(int howMany) {
    if (howMany < 1) howMany = 1;
    if (howMany > 32) howMany = 32;

    // Staggered, because the sends are flushed one per gap and the server needs
    // to actually process each command for the list to grow.
    for (int i = 0; i < howMany; ++i) {
        PendingSend ps;
        ps.gameText = "/debug shape cube";
        ps.silent = true;              // ours, not something to echo back at us
        ps.delayBeforeMs = 250;
        QueueSend(ps);
    }
    DbgSetStatus("asked for " + std::to_string(howMany) +
        " shapes through the game's own command - watch the capacity above; "
        "a List doubles rather than growing by one");
}

namespace {
// Where the next spawn should start looking for a free slot. Filling a list
// means walking it from the front every single time, and at two thousand
// entries that is four million guarded reads for one redraw. The slots are
// handed out in order, so remembering where the last one landed turns the
// whole batch back into a single pass.
int64_t g_spawnSlotHint = 0;
}

// Which array slot a tagged shape ended up in. The slot holds a reference, so
// the collector keeps its contents correct even when the object moves - which
// is why this records the slot and never the object address.
std::mutex g_dbgTagMtx;
std::unordered_map<uint64_t, int64_t> g_dbgTagSlots;

// Slots whose object is still there but hidden. Reused rather than allocated
// against, and never nulled - see DbgRemoveTagged for why that matters.
std::vector<int64_t> g_dbgFreeSlots;

void DbgClearAllShapes() {
    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    if (!c.isArray) return;

    const uintptr_t root = GsdkCurrentRoot();
    const uintptr_t arr = GsdkFollow(root, c.arrayPath);
    if (!arr) return;
    int64_t len = 0;
    if (!SafeRead((const void*)(arr + 8), &len, 8) || len < 1 || len > 65536) return;

    const uint64_t zero = 0;
    for (int64_t i = 0; i < len; ++i)
        SafeWrite((void*)(arr + 0x10 + i * 8), &zero, 8);
    g_spawnSlotHint = 0;   // everything is free again
    {
        std::lock_guard lock(g_dbgTagMtx);
        g_dbgTagSlots.clear();
        g_dbgFreeSlots.clear();
    }

    if (c.haveCount) {
        const uintptr_t owner = GsdkFollow(root, c.countPath);
        const int32_t z = 0;
        if (owner) SafeWrite((void*)(owner + c.countOffset), &z, 4);
    }
    DbgSetStatus("cleared every shape slot");
}

// =============================================================================
// Working without a server that will draw shapes for us
// =============================================================================
// Everything so far has leaned on a shape the server drew: the entry's
// MethodTable came off it, and so did the 192 bytes we clone. That is fine in a
// singleplayer world and useless anywhere you cannot run /debug shape cube.
//
// Both are fixed things though. The MethodTable is at a fixed offset in the
// image, and the entry - as the field report showed - is 192 bytes of nothing
// but floats and ints, with not a single object reference in it. So both can
// simply be saved to disk and loaded on any later launch, on any server. A
// template full of numbers has nothing to go stale.
std::mutex g_dbgSeedMtx;
std::atomic<uintptr_t> g_dbgLastArrayAddr{ 0 };
std::atomic<int> g_dbgArraySwaps{ 0 };

namespace {

std::wstring SeedFilePath() {
    return GetThisModuleDirectory() + L"shape_entry.txt";
}

} // namespace

bool DbgSaveSeed() {
    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    if (!c.haveTemplate || !c.holderMTRva) return false;

    std::ofstream f(SeedFilePath(), std::ios::trunc);
    if (!f) return false;
    f << "v1\n";
    // Deliberately NOT saved: the entry and array MethodTables. They are
    // runtime-built generic instantiations whose addresses move between
    // launches, so a stored offset is worse than useless - it points somewhere
    // plausible and produces garbage. They are re-derived from the live list.
    f << "mt=0\n";
    f << "matrix=" << c.matrixOffset << "\n";
    f << "trans=" << c.transOffset << "\n";
    f << "life=" << (c.haveLife ? (int)c.lifeOffset : -1) << "\n";
    f << "lifetotal=" << (c.haveLifeTotal ? (int)c.lifeTotalOffset : -1) << "\n";
    f << "opacity=" << (c.haveOpacity ? (int)c.opacityOffset : -1) << "\n";
    f << "shape=" << (c.haveShapeEnum ? (int)c.shapeOffset : -1) << "\n";
    f << "color=" << (c.haveColor ? (int)c.colorOffset : -1) << "\n";
    f << "lifemax=" << c.lifeMax << "\n";
    f << "size=" << c.entrySize << "\n";
    f << "arrmt=0\n";
    // The count is a fixed field offset, and it can only be found where the
    // command works - so it has to travel with everything else.
    f << "count=" << (c.haveCount ? (int)c.countOffset : -1) << "\n";
    f << "template=";
    for (size_t i = 0; i < sizeof(c.templateBytes); ++i) {
        char b[4];
        snprintf(b, sizeof(b), "%02X", c.templateBytes[i]);
        f << b;
    }
    f << "\n";
    DbgSetStatus("saved the entry seed - shapes can now be spawned on any server");
    return true;
}

bool DbgLoadSeed() {
    std::ifstream f(SeedFilePath());
    if (!f) return false;

    DbgContainer c;
    std::string line;
    bool sawVersion = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "v1") { sawVersion = true; continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if (k == "mt")             c.holderMTRva = strtoull(v.c_str(), nullptr, 10);
        else if (k == "matrix")    c.matrixOffset = (uint32_t)atoi(v.c_str());
        else if (k == "trans")     c.transOffset = (uint32_t)atoi(v.c_str());
        else if (k == "lifemax")   c.lifeMax = (float)atof(v.c_str());
        else if (k == "size")      c.entrySize = (uint32_t)atoi(v.c_str());
        else if (k == "arrmt")     c.arrayMTRva = strtoull(v.c_str(), nullptr, 10);
        else if (k == "count")   { const int o = atoi(v.c_str());
                                   if (o >= 0) { c.countOffset = (uint32_t)o;
                                                 c.haveCount = true; c.countFromSeed = true; } }
        else if (k == "life")    { const int o = atoi(v.c_str()); c.haveLife = o >= 0; c.lifeOffset = o > 0 ? o : 0; }
        else if (k == "lifetotal") { const int o = atoi(v.c_str()); c.haveLifeTotal = o >= 0; c.lifeTotalOffset = o > 0 ? o : 0; }
        else if (k == "opacity") { const int o = atoi(v.c_str()); c.haveOpacity = o >= 0; c.opacityOffset = o > 0 ? o : 0; }
        else if (k == "shape")   { const int o = atoi(v.c_str()); c.haveShapeEnum = o >= 0; c.shapeOffset = o > 0 ? o : 0; }
        else if (k == "color")   { const int o = atoi(v.c_str()); c.haveColor = o >= 0; c.colorOffset = o > 0 ? o : 0; }
        else if (k == "template") {
            const size_t n = (std::min)(v.size() / 2, sizeof(c.templateBytes));
            for (size_t i = 0; i < n; ++i) {
                const std::string byteStr = v.substr(i * 2, 2);
                c.templateBytes[i] = (uint8_t)strtoul(byteStr.c_str(), nullptr, 16);
            }
            c.haveTemplate = n > 0;
        }
    }
    if (!sawVersion || !c.haveTemplate) return false;

    // Sanity-check the saved bytes. Everything in an entry is a float or an
    // int, so a wild float here means the file is stale or was captured from a
    // half-written entry - and it would be handed straight to the renderer.
    {
        const uint32_t n = (c.entrySize >= 16 && c.entrySize <= sizeof(c.templateBytes))
            ? c.entrySize : 192;
        for (uint32_t off = 8; off + 4 <= n; off += 4) {
            float v;
            memcpy(&v, c.templateBytes + off, 4);
            if (!std::isfinite(v) || std::fabs(v) > 1.0e6f) {
                DbgSetStatus("the saved seed has a bad value in it - re-save one from "
                    "a singleplayer world");
                return false;
            }
        }
    }

    std::lock_guard lock(g_dbgContMtx);
    // Keep whatever the live walk already knows about the array; only the parts
    // that need a server-drawn shape come from the file.
    c.isArray = g_dbgContainer.isArray;
    c.arrayPath = g_dbgContainer.arrayPath;
    c.arrayLength = g_dbgContainer.arrayLength;
    c.liveSlots = g_dbgContainer.liveSlots;
    // A count offset from the file is better than one the live walk guessed at,
    // since the guess matched 86 different ints. Keep the path either way.
    if (!c.countFromSeed) {
        c.haveCount = g_dbgContainer.haveCount;
        c.countOffset = g_dbgContainer.countOffset;
    }
    c.countPath = g_dbgContainer.countPath;
    if (!c.arrayMTRva) c.arrayMTRva = g_dbgContainer.arrayMTRva;
    c.seeded = true;
    c.valid = true;
    g_dbgContainer = c;
    return true;
}

// =============================================================================
// Allocating a bigger array
// =============================================================================
// Growing the list by asking the client for shapes only works where you can run
// the command. Everywhere else the array stays at whatever capacity the client
// made it, so to get more we have to allocate one.
//
// RhpNewArray is found the same way RhpNewFast was, with the same rule that
// nothing gets called until its own code has been read: an array allocator must
// fetch the element size out of the MethodTable it is handed, which lives in
// the first two bytes rather than at +4. Array MethodTables are distinguishable
// from ordinary ones precisely because that field is non-zero.
std::atomic<uintptr_t> g_dbgArrayAllocRva{ 0 };
std::atomic<bool>      g_dbgArrayAllocVerified{ false };
std::mutex g_dbgArrayEvidenceMtx;
std::string g_dbgArrayEvidence;

std::string DbgGetArrayAllocEvidence() {
    std::lock_guard lock(g_dbgArrayEvidenceMtx);
    return g_dbgArrayEvidence;
}

namespace {

using tRhpNewArray = void* (__fastcall*)(void* methodTable, int32_t count);
tRhpNewArray pRhpNewArray = nullptr;

// A call target is often a thunk - one jump to the real body - so follow those
// before reading anything. Without this the check reads `jmp` and nothing else.
uintptr_t FollowThunks(uintptr_t fn) {
    for (int hop = 0; hop < 4 && fn; ++hop) {
        uint8_t b[8];
        if (!SafeRead((const void*)fn, b, sizeof(b))) return fn;
        if (b[0] == 0xE9) {                       // jmp rel32
            int32_t rel; memcpy(&rel, b + 1, 4);
            fn = fn + 5 + (intptr_t)rel;
            continue;
        }
        if (b[0] == 0xEB) {                       // jmp rel8
            fn = fn + 2 + (int8_t)b[1];
            continue;
        }
        if (b[0] == 0xFF && b[1] == 0x25) {       // jmp [rip+rel32]
            int32_t rel; memcpy(&rel, b + 2, 4);
            uint64_t target = 0;
            if (!SafeRead((const void*)(fn + 6 + (intptr_t)rel), &target, 8)) return fn;
            fn = (uintptr_t)target;
            continue;
        }
        break;
    }
    return fn;
}

// Does this function behave like an array allocator? Reading the code rather
// than calling it, because calling the wrong thing here hangs the game.
//
// The first version looked only for `movzx <reg>, word ptr [rcx]` - fetching
// the element size out of the MethodTable - and rejected everything on this
// build. That was wrong in principle: NativeAOT emits SPECIALISED allocators
// with the element size baked into the instruction stream, so there is no such
// read to find. The real RhpNewArray here opens with
//
//     cmp rdx, 0x08000000          ; maximum array length
//     jae <slow path>
//     lea eax, [rdx*8 + 0x18]      ; size = count * 8 + 24
//
// What identifies it is the size arithmetic: the element COUNT, which arrives
// in rdx, used as a scaled index. That holds for the generic form too, so both
// are accepted.
bool LooksLikeArrayAllocator(uintptr_t fn, std::string& why) {
    fn = FollowThunks(fn);
    uint8_t code[0x140];
    if (!SafeRead((const void*)fn, code, sizeof(code))) { why = "unreadable"; return false; }

    bool sizeFromCount = false, componentRead = false, lengthCheck = false;

    for (int i = 0; i + 6 < (int)sizeof(code); ++i) {
        // lea <reg>, [rdx*N + disp32]. modrm mod=00 rm=100 (SIB follows), and a
        // SIB with index=rdx and no base: scale<<6 | 010<<3 | 101.
        int j = i;
        if ((code[j] & 0xF8) == 0x48) ++j;                 // optional REX.W
        if (code[j] == 0x8D && (code[j + 1] & 0xC7) == 0x04) {
            const uint8_t sib = code[j + 2];
            if ((sib & 0x3F) == 0x15) sizeFromCount = true;  // index=rdx, base=none
        }
        // The same shape with the 0x67 address-size prefix, which is what this
        // build actually emits.
        if (code[i] == 0x67 && code[i + 1] == 0x8D && (code[i + 2] & 0xC7) == 0x04) {
            const uint8_t sib = code[i + 3];
            if ((sib & 0x3F) == 0x15) sizeFromCount = true;
        }
        // cmp rdx, imm32 - the maximum-length guard.
        if (code[i] == 0x48 && code[i + 1] == 0x81 && code[i + 2] == 0xFA)
            lengthCheck = true;
        // The generic form, still worth accepting.
        j = i;
        if (code[j] == 0x44 || code[j] == 0x4C || code[j] == 0x48 || code[j] == 0x4D) ++j;
        if (code[j] == 0x0F && code[j + 1] == 0xB7) {
            const uint8_t modrm = code[j + 2];
            if ((modrm & 0x07) == 0x01) {
                const uint8_t mod = modrm >> 6;
                if (mod == 0 || (mod == 1 && code[j + 3] == 0x00)) componentRead = true;
            }
        }
    }

    char buf[160];
    snprintf(buf, sizeof(buf), "size-from-count %s, length guard %s, component read %s",
        sizeFromCount ? "yes" : "no", lengthCheck ? "yes" : "no",
        componentRead ? "yes" : "no");
    why = buf;
    return sizeFromCount || componentRead;
}

// First bytes of a candidate, so a rejected one can be looked at rather than
// guessed about.
std::string HeadBytes(uintptr_t fn) {
    uint8_t b[16];
    if (!SafeRead((const void*)fn, b, sizeof(b))) return "unreadable";
    std::string out;
    char t[8];
    for (int i = 0; i < 16; ++i) { snprintf(t, sizeof(t), "%02X ", b[i]); out += t; }
    return out;
}

} // namespace

bool DbgResolveArrayAllocator() {
    if (g_dbgArrayAllocVerified.load()) return true;

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!modBase || !modSize) return false;

    // The array type we actually want, if the live list has told us one. An
    // empty List<T> still points at Array.Empty<T>(), which carries exactly the
    // right MethodTable - so this is usually available even on a server where
    // no shape has ever been drawn.
    uintptr_t wantMT = 0;
    {
        std::lock_guard lock(g_dbgContMtx);
        wantMT = (uintptr_t)g_dbgContainer.arrayMTAbs;
    }

    // Two buckets. `exact` is sites allocating the very type we want; `refArr`
    // is sites allocating any array of references. The second exists because
    // List<T> usually grows through shared generic code rather than a literal
    // MethodTable, so the exact bucket can legitimately come up empty.
    //
    // What neither bucket contains is strings. System.String also has a
    // non-zero component size, and strings are allocated thousands of times
    // over - which is how the first attempt ended up voting in RhpNewString and
    // getting back something of length zero.
    std::unordered_map<uintptr_t, int> exact, refArr;
    constexpr size_t kChunk = 1 * 1024 * 1024;
    std::vector<uint8_t> buf(kChunk + 32);

    for (uintptr_t off = 0; off + 32 < modSize; ) {
        const size_t want = (size_t)(std::min)((uintptr_t)kChunk, (uintptr_t)(modSize - off));
        if (!SafeRead((const void*)(modBase + off), buf.data(), want)) { off += want; continue; }

        for (size_t i = 0; i + 24 < want; ++i) {
            // Two ways a MethodTable reaches rcx: a RIP-relative lea, or a
            // straight 64-bit immediate. Only checking the first was probably
            // why the exact-type bucket came up empty.
            uintptr_t mt = 0;
            size_t insnLen = 0;
            if (buf[i] == 0x48 && buf[i + 1] == 0x8D && buf[i + 2] == 0x0D) {
                int32_t leaRel;
                memcpy(&leaRel, buf.data() + i + 3, 4);
                mt = modBase + off + i + 7 + (intptr_t)leaRel;
                insnLen = 7;
            }
            else if (buf[i] == 0x48 && buf[i + 1] == 0xB9) {
                uint64_t imm;
                memcpy(&imm, buf.data() + i + 2, 8);
                mt = (uintptr_t)imm;
                insnLen = 10;
            }
            else continue;
            if (mt < modBase || mt >= modBase + modSize) continue;

            uint16_t comp = 0;
            uint32_t base = 0;
            if (!SafeRead((const void*)mt, &comp, 2)) continue;
            if (!SafeRead((const void*)(mt + 4), &base, 4)) continue;

            const bool isExact = wantMT && mt == wantMT;
            const bool isRefArray = (comp == 8 && base == 24);
            if (!isExact && !isRefArray) continue;

            for (size_t k = i + insnLen; k < i + insnLen + 18 && k + 5 < want; ++k) {
                if (buf[k] != 0xE8) continue;
                int32_t callRel;
                memcpy(&callRel, buf.data() + k + 1, 4);
                const uintptr_t callee = modBase + off + k + 5 + (intptr_t)callRel;
                if (callee < modBase || callee >= modBase + modSize) break;
                if (isExact) ++exact[callee];
                if (isRefArray) ++refArr[callee];
                break;
            }
        }
        off += want;
    }

    auto pick = [](const std::unordered_map<uintptr_t, int>& votes,
                   uintptr_t& outFn, int& outVotes) {
        outFn = 0; outVotes = 0;
        for (const auto& kv : votes) {
            std::string why;
            if (!LooksLikeArrayAllocator(kv.first, why)) continue;
            if (kv.second > outVotes) { outVotes = kv.second; outFn = kv.first; }
        }
    };

    uintptr_t best = 0;
    int bestVotes = 0;
    const char* which = "exact type";
    pick(exact, best, bestVotes);
    if (!best) { pick(refArr, best, bestVotes); which = "reference arrays"; }

    std::string ev;
    char line[260];
    if (!best) {
        // List every candidate with its first bytes. Guessing at why a check
        // failed has been wrong twice; this shows the code instead.
        ev = "no candidate passed. Sites found: " + std::to_string(exact.size()) +
             " exact-type, " + std::to_string(refArr.size()) + " reference-array.\n";
        std::vector<std::pair<uintptr_t, int>> all;
        for (const auto& kv : exact) all.push_back(kv);
        for (const auto& kv : refArr) all.push_back(kv);
        std::sort(all.begin(), all.end(),
            [](const std::pair<uintptr_t, int>& a, const std::pair<uintptr_t, int>& b) {
                return a.second > b.second;
            });
        int shown = 0;
        for (const auto& kv : all) {
            if (++shown > 10) break;
            const uintptr_t body = FollowThunks(kv.first);
            std::string why;
            LooksLikeArrayAllocator(kv.first, why);
            snprintf(line, sizeof(line), "  +0x%llX  %d vote(s)%s\n    %s\n    %s\n",
                (unsigned long long)(kv.first - modBase), kv.second,
                (body != kv.first) ? "  (thunk)" : "", HeadBytes(body).c_str(),
                why.c_str());
            ev += line;
        }
        { std::lock_guard lock(g_dbgArrayEvidenceMtx); g_dbgArrayEvidence = ev; }
        DbgSetStatus("array allocator not identified - nothing will be called");
        return false;
    }

    pRhpNewArray = (tRhpNewArray)best;
    g_dbgArrayAllocRva.store(best - modBase);
    std::string bestWhy;
    LooksLikeArrayAllocator(best, bestWhy);
    snprintf(line, sizeof(line),
        "candidate +0x%llX from %d site(s) via %s\n  %s\nNot called yet - press Test.",
        (unsigned long long)(best - modBase), bestVotes, which, bestWhy.c_str());
    ev = line;
    { std::lock_guard lock(g_dbgArrayEvidenceMtx); g_dbgArrayEvidence = ev; }
    DbgSetStatus(ev);
    return true;
}

// =============================================================================
// Allocating without ever starting a collection
// =============================================================================
// Calling the allocator from our own code is only safe while it takes its fast
// path, which bumps a pointer in this thread's allocation context and returns.
// The slow path can start a garbage collection, and the collector then walks
// this thread's stack starting from whoever called the allocator - us. It
// cannot unwind our code, so the runtime fail-fasts the game (0xC0000602). The
// odds were proportional to how much we allocated, which made a big outline a
// real chance of taking the game down.
//
// So the fast path's own test is made first, on the same two fields, and when
// the object will not fit nothing is called: the work waits for a later frame,
// by which time the game's own allocations have refilled the context. Where
// the fields are is read out of the allocator's code rather than assumed.
namespace {

struct AllocContextLayout {
    uintptr_t tlsIndexAddr = 0;   // the module's _tls_index
    uint32_t  ctxOffset = 0;      // the allocation context inside the TLS block
    uint32_t  limitOff = 0;       // within the context
    uint32_t  ptrOff = 0;
};

std::mutex g_allocCtxMtx;
std::atomic<int> g_allocCtxState{ 0 };   // 0 not read yet, 1 understood, -1 not understood
AllocContextLayout g_allocCtx;           // written once, before state becomes 1

// RhpNewFast on this build, which is the whole fast path:
//   8B 15 disp32                 mov  edx, [_tls_index]
//   65 48 8B 04 25 58 00 00 00   mov  rax, gs:[58h]        TEB.ThreadLocalStoragePointer
//   48 8B 04 D0                  mov  rax, [rax+rdx*8]
//   BA imm32                     mov  edx, <context offset>
//   48 03 D0                     add  rdx, rax
//   44 8B 41 04                  mov  r8d, [rcx+4]         base size
//   48 8B 42 disp8               mov  rax, [rdx+ptr]
//   4C 8B 0A                     mov  r9, [rdx]            limit
//   4C 2B C8 / 4D 3B C1 / 77     sub r9, rax / cmp r8, r9 / ja <slow path>
// The array allocator uses the same context, so this one reading covers both.
bool ReadAllocContextLayout(uintptr_t fn, AllocContextLayout& out) {
    static const int16_t kPat[] = {
        0x8B, 0x15, -1, -1, -1, -1,
        0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0x04, 0xD0,
        0xBA, -1, -1, -1, -1,
        0x48, 0x03, 0xD0,
        0x44, 0x8B, 0x41, 0x04,
        0x48, 0x8B, 0x42, -1,
        0x4C, 0x8B, 0x0A,
        0x4C, 0x2B, 0xC8, 0x4D, 0x3B, 0xC1, 0x77,
    };
    uint8_t c[sizeof(kPat) / sizeof(kPat[0])];
    if (!SafeRead((const void*)fn, c, sizeof(c))) return false;
    for (size_t i = 0; i < sizeof(c); ++i)
        if (kPat[i] >= 0 && c[i] != (uint8_t)kPat[i]) return false;

    int32_t disp = 0;
    memcpy(&disp, c + 2, 4);
    out.tlsIndexAddr = fn + 6 + (intptr_t)disp;
    memcpy(&out.ctxOffset, c + 20, 4);
    out.ptrOff = c[34];
    out.limitOff = 0;
    return out.ctxOffset < 0x10000 && out.ptrOff >= 8;
}

// How many bytes the fast path can hand out on THIS thread right now: SIZE_MAX when
// that cannot be told (behave as before), 0 when the context cannot be read.
// Must be asked on the thread that then allocates, with nothing in between.
size_t AllocBytesNow() {
    int st = g_allocCtxState.load(std::memory_order_acquire);
    if (st == 0) {
        if (!pRhpNewFast) return 0;   // nothing to call yet anyway
        std::lock_guard lock(g_allocCtxMtx);
        st = g_allocCtxState.load();
        if (st == 0) {
            AllocContextLayout lay;
            if (ReadAllocContextLayout((uintptr_t)pRhpNewFast, lay)) {
                g_allocCtx = lay;
                st = 1;
                char buf[160];
                snprintf(buf, sizeof(buf),
                    "debugshape: allocation context read (context +0x%X, ptr +0x%X) - "
                    "allocations will never start a collection",
                    lay.ctxOffset, lay.ptrOff);
                LogToFile(buf);
            }
            else {
                st = -1;
                LogToFile("debugshape: the allocator's fast path does not look as expected on "
                    "this build - allocating anyway, and each allocation carries a small "
                    "chance of the game fail-fasting");
            }
            g_allocCtxState.store(st, std::memory_order_release);
        }
    }
    if (st < 0) return SIZE_MAX;   // cannot tell - behave as before

    uint32_t idx = 0;
    if (!SafeRead((const void*)g_allocCtx.tlsIndexAddr, &idx, 4)) return 0;
    const uintptr_t tlsArray = (uintptr_t)__readgsqword(0x58);
    uint64_t block = 0;
    if (!tlsArray || !SafeRead((const void*)(tlsArray + (uintptr_t)idx * 8), &block, 8) || !block)
        return 0;
    const uintptr_t ctx = (uintptr_t)block + g_allocCtx.ctxOffset;
    uint64_t limit = 0, ptr = 0;
    if (!SafeRead((const void*)(ctx + g_allocCtx.limitOff), &limit, 8)) return 0;
    if (!SafeRead((const void*)(ctx + g_allocCtx.ptrOff), &ptr, 8)) return 0;
    if (!ptr || limit < ptr) return 0;
    return (size_t)(limit - ptr);
}

// True when allocating `bytes` on THIS thread right now takes the fast path.
bool AllocFitsWithoutCollecting(size_t bytes) { return bytes <= AllocBytesNow(); }

size_t ObjectBytes(void* mt) {
    uint32_t base = 0;
    if (!mt || !SafeRead((const void*)((uintptr_t)mt + 4), &base, 4)) return SIZE_MAX;
    return ((size_t)base + 7) & ~(size_t)7;
}

size_t ArrayBytes(void* mt, int32_t count) {
    uint16_t comp = 0;
    uint32_t base = 0;
    if (!mt || count < 0 ||
        !SafeRead(mt, &comp, 2) || !SafeRead((const void*)((uintptr_t)mt + 4), &base, 4))
        return SIZE_MAX;
    const size_t generic = ((size_t)base + (size_t)comp * (size_t)count + 7) & ~(size_t)7;
    // The array allocator on this build is specialised to count * 8 + 24 no
    // matter what it is handed, so never assume less than that.
    const size_t specialised = (size_t)count * 8 + 0x18;
    return (std::max)(generic, specialised);
}

} // namespace

bool DbgAllocFitsNow(void* mt) {
    const size_t n = ObjectBytes(mt);
    return n != SIZE_MAX && AllocFitsWithoutCollecting(n);
}

void* DbgAllocObject(void* mt) {
    if (!mt || !pRhpNewFast || !g_dbgAllocVerified.load()) return nullptr;
    if (!DbgAllocFitsNow(mt)) return nullptr;
    return pRhpNewFast(mt);
}

void* DbgAllocArray(void* mt, int32_t count) {
    if (!pRhpNewArray || !mt || count < 1) return nullptr;
    const size_t n = ArrayBytes(mt, count);
    if (n == SIZE_MAX || !AllocFitsWithoutCollecting(n)) return nullptr;
    return pRhpNewArray(mt, count);
}

// Allocates a small array and checks the header and the length field. Safe:
// nothing is stored anywhere, so a wrong guess costs one dead object.
bool DbgTestArrayAllocator(uintptr_t arrayMT) {
    if (!pRhpNewArray || !arrayMT) return false;

    void* a = DbgAllocArray((void*)arrayMT, 8);
    if (!a) {
        std::lock_guard lock(g_dbgArrayEvidenceMtx);
        g_dbgArrayEvidence = "returned null - not the array allocator";
        return false;
    }
    uint64_t gotMT = 0;
    uint32_t gotLen = 0;
    SafeRead(a, &gotMT, 8);
    SafeRead((const void*)((uintptr_t)a + 8), &gotLen, 4);

    uint16_t comp = 0;
    uint32_t base = 0;
    SafeRead((const void*)arrayMT, &comp, 2);
    SafeRead((const void*)(arrayMT + 4), &base, 4);

    char msg[300];
    const bool ok = (gotMT == (uint64_t)arrayMT) && (gotLen == 8);
    if (ok)
        snprintf(msg, sizeof(msg),
            "OK - allocated an 8-element array, header and length both correct "
            "(element size %u, base %u)", comp, base);
    else
        snprintf(msg, sizeof(msg),
            "WRONG - header %llX, length %u; wanted %llX and 8. The right header "
            "with a zero length means it allocated something that was not an "
            "8-element array. (type: element size %u, base %u)",
            (unsigned long long)gotMT, gotLen, (unsigned long long)arrayMT,
            comp, base);
    { std::lock_guard lock(g_dbgArrayEvidenceMtx); g_dbgArrayEvidence = msg; }
    g_dbgArrayAllocVerified.store(ok);
    DbgSetStatus(msg);
    return ok;
}

// Swaps in a larger array, carrying the existing shapes across.
bool DbgGrowArrayTo(int newLength) {
    if (!g_dbgArrayAllocVerified.load() || !pRhpNewArray) return false;
    if (newLength < 8 || newLength > 8192) return false;

    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    if (!c.isArray || c.arrayPath.empty()) return false;

    const uintptr_t root = GsdkCurrentRoot();
    const uintptr_t oldArr = GsdkFollow(root, c.arrayPath);
    if (!oldArr) return false;

    int64_t oldLen = 0;
    if (!SafeRead((const void*)(oldArr + 8), &oldLen, 8) || oldLen < 1) return false;
    if (newLength <= (int)oldLen) return false;

    uint64_t arrMT = 0;
    if (!SafeRead((const void*)oldArr, &arrMT, 8) || !arrMT) return false;

    void* neu = DbgAllocArray((void*)arrMT, newLength);
    if (!neu) return false;

    for (int64_t i = 0; i < oldLen; ++i) {
        uint64_t e = 0;
        if (!SafeRead((const void*)(oldArr + 0x10 + i * 8), &e, 8)) break;
        if (e) DbgStoreRef((void*)((uintptr_t)neu + 0x10 + i * 8), (void*)(uintptr_t)e);
    }

    // The owner's field that held the old array.
    std::vector<uint32_t> ownerPath(c.arrayPath.begin(), c.arrayPath.end() - 1);
    const uint32_t hop = c.arrayPath.back();
    const uintptr_t owner = GsdkFollow(root, ownerPath);
    if (!owner) return false;

    if (!DbgStoreRef((void*)(owner + hop), neu)) return false;

    DbgSetStatus("array grown from " + std::to_string(oldLen) + " to " +
        std::to_string(newLength) + " slots");
    return true;
}

// Defined further down, next to the spawn queue it belongs to.
namespace { void SetSpawnFailure(const std::string& s); }

// =============================================================================
// The GC write barrier
// =============================================================================
// Every reference stored so far has gone in with a plain memcpy. That is fine
// for floats and ints and wrong for references, and it is why a created list
// reads 32 slots and then 0 again a moment later.
//
// A generational GC does not scan an old object's fields during a young
// collection unless something has marked that region dirty. Storing a pointer
// to a brand new object into an older one without telling the GC means the new
// object is never found, so it is collected - and the field is left pointing at
// nothing. Clicking fast "worked" only because it beat the next collection.
//
// RhpAssignRef is what does the telling. It was in plain sight in the array
// allocator scan, as the one candidate with a single vote:
//
//     48 89 11              mov  [rcx], rdx        store the reference
//     4C 8B 1D ? ? ? ?      mov  r11, [rip+...]    load a GC global
//     49 83 FB 00           cmp  r11, 0
//     74 14                 je   ...
//
// The leading store followed immediately by a RIP-relative load of a global is
// the shape of it; an ordinary function would not open that way.
std::atomic<uintptr_t> g_dbgBarrierRva{ 0 };
std::atomic<bool> g_dbgBarrierVerified{ false };
std::mutex g_dbgBarrierMtx;
std::string g_dbgBarrierEvidence;

std::string DbgGetBarrierEvidence() {
    std::lock_guard lock(g_dbgBarrierMtx);
    return g_dbgBarrierEvidence;
}

namespace {

using tAssignRef = void(__fastcall*)(void** dst, void* ref);
tAssignRef pAssignRef = nullptr;

bool HasReturnSoon(uintptr_t fn) {
    uint8_t code[0x100];
    if (!SafeRead((const void*)fn, code, sizeof(code))) return false;
    for (size_t i = 0; i < sizeof(code); ++i)
        if (code[i] == 0xC3) return true;
    return false;
}

} // namespace

bool DbgResolveWriteBarrier() {
    if (pAssignRef) return true;

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!modBase || !modSize) return false;

    std::vector<uintptr_t> found;
    constexpr size_t kChunk = 1 * 1024 * 1024;
    std::vector<uint8_t> buf(kChunk + 32);

    for (uintptr_t off = 0; off + 32 < modSize && found.size() < 8; ) {
        const size_t want = (size_t)(std::min)((uintptr_t)kChunk, (uintptr_t)(modSize - off));
        if (!SafeRead((const void*)(modBase + off), buf.data(), want)) { off += want; continue; }

        for (size_t i = 0; i + 16 < want; ++i) {
            // mov [rcx], rdx
            if (buf[i] != 0x48 || buf[i + 1] != 0x89 || buf[i + 2] != 0x11) continue;
            // immediately followed by a RIP-relative load into an extended reg
            if (buf[i + 3] != 0x4C && buf[i + 3] != 0x48) continue;
            if (buf[i + 4] != 0x8B) continue;
            if ((buf[i + 5] & 0xC7) != 0x05) continue;      // mod=00, rm=101 (rip)

            const uintptr_t fn = modBase + off + i;
            if (!HasReturnSoon(fn)) continue;
            found.push_back(fn);
            if (found.size() >= 8) break;
        }
        off += want;
    }

    std::string ev;
    char line[200];
    if (found.empty()) {
        ev = "no write barrier found. References will be stored with a plain write, "
             "which the GC is not told about - expect things to vanish.";
        { std::lock_guard lock(g_dbgBarrierMtx); g_dbgBarrierEvidence = ev; }
        return false;
    }

    pAssignRef = (tAssignRef)found.front();
    g_dbgBarrierRva.store(found.front() - modBase);
    snprintf(line, sizeof(line),
        "candidate +0x%llX (%zu found). Not called yet - press Test.",
        (unsigned long long)(found.front() - modBase), found.size());
    ev = line;
    for (size_t i = 0; i < found.size() && i < 6; ++i) {
        snprintf(line, sizeof(line), "\\n  +0x%llX  %s",
            (unsigned long long)(found[i] - modBase), HeadBytes(found[i]).c_str());
        ev += line;
    }
    { std::lock_guard lock(g_dbgBarrierMtx); g_dbgBarrierEvidence = ev; }
    DbgSetStatus("write barrier candidate found");
    return true;
}

// Safe because the destination is a slot in an array we just made: the barrier
// writes there and nowhere else, and we can read the value straight back.
bool DbgTestWriteBarrier() {
    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    if (!pAssignRef || !c.arrayMTAbs) return false;

    void* scratch = DbgAllocArray((void*)(uintptr_t)c.arrayMTAbs, 4);
    if (!scratch) {
        std::lock_guard lock(g_dbgBarrierMtx);
        g_dbgBarrierEvidence = "could not allocate a scratch array to test with";
        return false;
    }

    // Store the array into its own first slot - a real reference, in the heap,
    // and nothing else can see it.
    void** dst = (void**)((uintptr_t)scratch + 0x10);
    pAssignRef(dst, scratch);

    uint64_t readBack = 0;
    SafeRead(dst, &readBack, 8);
    const bool ok = (readBack == (uint64_t)(uintptr_t)scratch);

    char msg[200];
    snprintf(msg, sizeof(msg), ok
        ? "OK - the barrier stored the reference and the GC has been told about it"
        : "WRONG - wrote %llX, read back %llX",
        (unsigned long long)(uintptr_t)scratch, (unsigned long long)readBack);
    { std::lock_guard lock(g_dbgBarrierMtx); g_dbgBarrierEvidence = msg; }
    g_dbgBarrierVerified.store(ok);
    DbgSetStatus(msg);
    return ok;
}

// Every reference store goes through here. Falls back to a plain write so the
// behaviour is no worse than before if the barrier was never found - but says
// so, because that is the difference between a shape staying and vanishing.
bool DbgStoreRef(void* slot, void* ref) {
    if (pAssignRef && g_dbgBarrierVerified.load()) {
        pAssignRef((void**)slot, ref);
        return true;
    }
    const uint64_t v = (uint64_t)(uintptr_t)ref;
    return SafeWrite(slot, &v, 8);
}

// =============================================================================
// Finding the shape list by what it IS, not where it was
// =============================================================================
// The path root->+0x10->+0x1F0->+0x70 was found by a shortest-path walk in a
// singleplayer world. It worked there, but the walk routes through the player
// entity - which has a back-pointer to GameInstance - so a shortest path can
// easily be a coincidental alias rather than the real owner. On a server it
// pointed at something else entirely: the MethodTable of the "array" at +0x70
// changed between two reads seconds apart, and an array type's MethodTable
// never changes.
//
// So locate it by identity instead. We know the list's element type from a
// singleplayer session, saved in the seed, which pins down the array type
// exactly. Then confirm the find structurally: a List<T>'s backing array sits
// at some field, with _size 8 bytes after it and _version 4 after that, and
// _size must be consistent with the array's own length.
std::mutex g_dbgLocateMtx;
std::string g_dbgLocateReport;
std::atomic<bool> g_dbgLocating{ false };

std::string DbgGetLocateReport() {
    std::lock_guard lock(g_dbgLocateMtx);
    return g_dbgLocateReport;
}

namespace {

// Is `owner + hop` the _items field of a List<T>? Checks the two fields that
// always follow it, and that the size makes sense for the array it points at.
bool LooksLikeListFields(uintptr_t owner, uint32_t hop, int64_t arrayLen) {
    int32_t size = 0, version = 0;
    if (!SafeRead((const void*)(owner + hop + 8), &size, 4)) return false;
    if (!SafeRead((const void*)(owner + hop + 12), &version, 4)) return false;
    if (size < 0 || size > arrayLen) return false;
    if (version < 0 || version > 1000000) return false;
    return true;
}

} // namespace

void DbgLocateList() {
    if (g_dbgLocating.exchange(true)) return;
    StartWorker([]() {
        WorkerGuard guard;

        DbgContainer c;
        { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }

        size_t modSize = 0;
        const uintptr_t modBase = GetGameModuleBaseSize(modSize);
        const uintptr_t root = GsdkCurrentRoot();
        if (!modBase || !root) {
            { std::lock_guard lock(g_dbgLocateMtx);
              g_dbgLocateReport = "not in a world"; }
            g_dbgLocating.store(false);
            return;
        }

        const uint64_t wantArrayMT = c.arrayMTAbs;
        const uint64_t wantEntryMT = c.entryMTAbs;
        if (!wantArrayMT && !wantEntryMT) {
            { std::lock_guard lock(g_dbgLocateMtx);
              g_dbgLocateReport = "no seed - save one from a singleplayer world first"; }
            g_dbgLocating.store(false);
            return;
        }

        std::string rep = "searching for the list by its element type...\n";
        struct Hit { std::vector<uint32_t> path; int64_t len; int32_t size; bool byElement; };
        std::vector<Hit> hits;

        std::vector<uint8_t> buf(0x800);
        GsdkWalkGraph(root, 5, [&](uintptr_t obj, const std::vector<uint32_t>& path) {
            if (hits.size() >= 32 || path.empty()) return true;

            uint64_t mt = 0;
            if (!SafeRead((const void*)obj, &mt, 8) || !mt) return true;

            bool byElement = false;
            if (!wantArrayMT || mt != wantArrayMT) {
                // Not the array type we expect. It could still be the list if
                // it holds entries of the type we know, which covers a client
                // that grew a differently-typed array.
                if (!wantEntryMT) return true;
                uint32_t len0 = 0;
                if (!SafeRead((const void*)(obj + 8), &len0, 4)) return true;
                if (len0 == 0 || len0 > 4096) return true;
                bool found = false;
                for (uint32_t i = 0; i < len0 && i < 64 && !found; ++i) {
                    uint64_t e = 0;
                    if (!SafeRead((const void*)(obj + 0x10 + i * 8), &e, 8) || !e) continue;
                    uint64_t emt = 0;
                    if (SafeRead((const void*)e, &emt, 8) && emt == wantEntryMT) found = true;
                }
                if (!found) return true;
                byElement = true;
            }

            uint32_t len32 = 0;
            if (!SafeRead((const void*)(obj + 8), &len32, 4) || len32 > 65536) return true;

            // The object that points at it, and the field it points from.
            std::vector<uint32_t> ownerPath(path.begin(), path.end() - 1);
            const uint32_t hop = path.back();
            const uintptr_t owner = GsdkFollow(root, ownerPath);
            if (!owner) return true;
            if (!LooksLikeListFields(owner, hop, (int64_t)len32)) return true;

            int32_t size = 0;
            SafeRead((const void*)(owner + hop + 8), &size, 4);
            hits.push_back(Hit{ path, (int64_t)len32, size, byElement });
            return true;
            });

        if (hits.empty()) {
            rep += "nothing found. The list may be deeper than the walk reaches, or the\n"
                   "element type on this world differs from the one in the seed.\n";
            { std::lock_guard lock(g_dbgLocateMtx); g_dbgLocateReport = rep; }
            DbgSetStatus("could not locate the shape list");
            g_dbgLocating.store(false);
            return;
        }

        // Prefer a list that already holds entries, then the shallowest.
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
            if ((a.size > 0) != (b.size > 0)) return a.size > 0;
            return a.path.size() < b.path.size();
            });

        char line[220];
        for (size_t i = 0; i < hits.size() && i < 8; ++i) {
            std::string ps = "root";
            char t[32];
            for (uint32_t o : hits[i].path) { snprintf(t, sizeof(t), "->+0x%X", o); ps += t; }
            snprintf(line, sizeof(line), "  %s   %lld slots, _size %d%s\n",
                ps.c_str(), (long long)hits[i].len, hits[i].size,
                hits[i].byElement ? "  (matched by its entries)" : "  (matched by type)");
            rep += line;
        }

        {
            std::lock_guard lock(g_dbgContMtx);
            g_dbgContainer.arrayPath = hits.front().path;
            g_dbgContainer.countPath.assign(hits.front().path.begin(),
                                            hits.front().path.end() - 1);
            g_dbgContainer.countOffset = hits.front().path.back() + 8;
            g_dbgContainer.haveCount = true;
            g_dbgContainer.countFromSeed = true;   // structural, not guessed by value
            g_dbgContainer.isArray = true;
            g_dbgContainer.ownArray = false;
        }
        rep += "\nadopted the first one. _size is the field 8 bytes after the array\n"
               "reference, which is where List<T> always keeps it.\n";
        { std::lock_guard lock(g_dbgLocateMtx); g_dbgLocateReport = rep; }
        DbgSetStatus("located the shape list by type");
        g_dbgLocating.store(false);
        });
}

// =============================================================================
// Why spawning is or is not possible right now
// =============================================================================
// "It fails" is not something anyone can act on, and there are six separate
// links in this chain. This walks every one of them and says which broke, so a
// server that behaves differently from a singleplayer world is diagnosable
// rather than mysterious.
//
// The likely difference: in singleplayer, /debug shape cube made the client
// create its shape list. On a server where no debug shape ever arrives, that
// list may never have been created at all - so the array field is simply null
// and there is nowhere to put anything.
std::string DbgBuildReadinessReport() {
    std::string r;
    char line[300];

    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    const uintptr_t root = GsdkCurrentRoot();

    snprintf(line, sizeof(line), "game root          %s\n",
        root ? "ok" : "MISSING - not in a world");
    r += line;
    if (!root) return r;

    // Walk the chain to the array one hop at a time.
    const std::vector<uint32_t> path =
        c.arrayPath.empty() ? std::vector<uint32_t>{ 0x10, 0x1F0, 0x70 } : c.arrayPath;
    uintptr_t cur = root;
    bool chainOk = true;
    for (size_t i = 0; i < path.size(); ++i) {
        uintptr_t next = 0;
        const bool ok = SafeRead((const void*)(cur + path[i]), &next, 8) && next &&
            !(next & 7) && IsPlausiblePtr((const void*)next);
        snprintf(line, sizeof(line), "  +0x%-5X        %s\n", path[i],
            ok ? "ok" : "NULL - the client has not made this yet");
        r += line;
        if (!ok) { chainOk = false; break; }
        cur = next;
    }

    int64_t len = 0;
    if (chainOk) {
        if (SafeRead((const void*)(cur + 8), &len, 8) && len >= 0 && len < 65536) {
            int used = 0;
            for (int64_t i = 0; i < len; ++i) {
                uint64_t e = 0;
                if (!SafeRead((const void*)(cur + 0x10 + i * 8), &e, 8)) break;
                if (e) ++used;
            }
            snprintf(line, sizeof(line), "shape list         %lld slots, %d in use%s\n",
                (long long)len, used,
                // "0 slots" is not full, it is Array.Empty<T>() - the client
                // has a list but has never put anything in it, so there is no
                // real array yet. Saying FULL there sent us hunting the wrong
                // problem.
                (len == 0) ? "  - the shared empty array; a real one must be created"
                           : ((used >= (int)len) ? "  - full" : ""));
            r += line;
        }
        else {
            r += "shape list         unreadable length\n";
            chainOk = false;
        }
    }

    // Which list is in place, and whether it keeps being replaced. If the
    // client puts its own empty array back, everything written into ours stops
    // existing - and that looks exactly like "spawn says it worked but nothing
    // appears", so it needs to be visible rather than inferred.
    snprintf(line, sizeof(line), "list object        0x%llX%s\n",
        (unsigned long long)g_dbgLastArrayAddr.load(),
        c.ownArray ? "  (ours)" : "  (the client's)");
    r += line;
    snprintf(line, sizeof(line), "list replaced      %d time(s)%s\n",
        g_dbgArraySwaps.load(),
        g_dbgArraySwaps.load() > 0 ? "  <- something is swapping it back" : "");
    r += line;
    snprintf(line, sizeof(line), "count field        %s%X%s\n",
        c.haveCount ? "+0x" : "NOT KNOWN", c.haveCount ? c.countOffset : 0,
        c.countFromSeed ? "  (measured)"
                        : (c.haveCount ? "  (guessed by value - unreliable)" : ""));
    r += line;

    // Absolute, because these types are built at runtime and a module-relative
    // offset for them is meaningless - which is exactly what made every server
    // attempt allocate garbage.
    snprintf(line, sizeof(line), "entry type         0x%llX%s\n",
        (unsigned long long)c.entryMTAbs,
        c.entryMTAbs ? "  (derived from the list)" : "  MISSING");
    r += line;
    snprintf(line, sizeof(line), "entry size         %u bytes%s\n", c.entrySize,
        (c.entrySize == 192) ? "  (as expected)"
                             : (c.entrySize ? "  <- not 192, check this" : ""));
    r += line;

    snprintf(line, sizeof(line), "entry template     %s\n",
        c.haveTemplate ? "ok, 192 bytes" : "MISSING");
    r += line;

    snprintf(line, sizeof(line), "object allocator   %s\n",
        g_dbgAllocVerified.load() ? "verified" : "NOT verified - see the Runtime section");
    r += line;

    snprintf(line, sizeof(line), "array type         0x%llX%s\n",
        (unsigned long long)c.arrayMTAbs,
        c.arrayMTAbs ? "  (read from the list)" : "  MISSING");
    r += line;

    snprintf(line, sizeof(line), "write barrier      %s\n",
        g_dbgBarrierVerified.load() ? "verified - references are stored properly"
                                    : "NOT verified - references will be collected");
    r += line;

    snprintf(line, sizeof(line), "array allocator    %s\n",
        g_dbgArrayAllocVerified.load() ? "verified" : "not verified");
    r += line;

    r += "\n";
    if (!chainOk)
        r += "The client has not built its shape list here. Use \"Create the list\" -\n"
             "it allocates one and installs it, which is what /debug shape cube\n"
             "indirectly caused the client to do in your singleplayer world.\n";
    else if (!c.entryMTAbs)
        r += "The entry type could not be derived. Press \"Find the shape list\".\n";
    else if (!c.haveTemplate)
        r += "No seed. Draw one shape in a singleplayer world; it saves itself.\n";
    else if (!g_dbgAllocVerified.load())
        r += "The allocator has not been verified in this session.\n";
    else if (len > 0)
        r += "Everything needed is present.\n";

    (void)modBase;
    return r;
}

// Allocates a shape list and installs it, for a world where the client never
// made one. Same as growing an existing array, except there is nothing to copy.
bool DbgCreateShapeList(int length) {
    if (!g_dbgArrayAllocVerified.load()) {
        SetSpawnFailure("the array allocator has not been verified yet");
        return false;
    }
    if (length < 4) length = 4;
    if (length > 8192) length = 8192;

    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    if (!c.arrayMTAbs) {
        SetSpawnFailure("the array type could not be read from the list - "
            "press \"Find the shape list\" first");
        return false;
    }

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    const uintptr_t root = GsdkCurrentRoot();
    if (!modBase || !root) return false;

    const std::vector<uint32_t> path =
        c.arrayPath.empty() ? std::vector<uint32_t>{ 0x10, 0x1F0, 0x70 } : c.arrayPath;
    std::vector<uint32_t> ownerPath(path.begin(), path.end() - 1);
    const uint32_t hop = path.back();

    const uintptr_t owner = GsdkFollow(root, ownerPath);
    if (!owner) {
        SetSpawnFailure("the object that should hold the list is missing too - "
            "this world may keep debug shapes somewhere else entirely");
        return false;
    }

    // What is there now, so a smaller list is never swapped in for it.
    uintptr_t oldArr = 0;
    int64_t oldLen = 0;
    {
        uint64_t existing = 0;
        if (SafeRead((const void*)(owner + hop), &existing, 8) && existing) {
            oldArr = (uintptr_t)existing;
            int64_t l = 0;
            if (SafeRead((const void*)(oldArr + 8), &l, 8) && l > 0 && l <= 65536) oldLen = l;
        }
    }

    // Only as many slots as this thread's allocation context holds right now. A
    // request for thousands can never fit (the context is a few KB), and asking
    // for the whole thing meant the list never grew past the 64 it started with.
    // Take what fits; the caller comes back for more on a later frame.
    const size_t room = AllocBytesNow();
    if (room != SIZE_MAX) {
        const int64_t fit = room >= 0x18 ? (int64_t)((room - 0x18) / 8) : 0;
        if (length > fit) length = (int)fit;
    }
    if (length < 4 || length <= oldLen) {
        SetSpawnFailure("waiting for room: the client's allocator has space for about " +
            std::to_string(room == SIZE_MAX ? -1 : (long long)((room >= 0x18 ? (room - 0x18) / 8 : 0))) +
            " slots right now, and the list has " + std::to_string(oldLen));
        return false;
    }

    void* neu = DbgAllocArray((void*)(uintptr_t)c.arrayMTAbs, length);
    if (!neu) {
        SetSpawnFailure("the array allocation returned nothing");
        return false;
    }

    // Carry over anything that was already there.
    for (int64_t i = 0; oldArr && i < oldLen && i < length; ++i) {
        uint64_t e = 0;
        if (!SafeRead((const void*)(oldArr + 0x10 + i * 8), &e, 8)) break;
        if (e) DbgStoreRef((void*)((uintptr_t)neu + 0x10 + i * 8), (void*)(uintptr_t)e);
    }

    if (!DbgStoreRef((void*)(owner + hop), neu)) {
        SetSpawnFailure("could not install the new list");
        return false;
    }

    {
        std::lock_guard lock(g_dbgContMtx);
        g_dbgContainer.ownArray = true;
    }
    g_dbgArraySwaps.store(0);
    SetSpawnFailure("");
    DbgSetStatus("created a shape list with " + std::to_string(length) +
        " slots - it is ours, so its count is maintained automatically");
    LogToFile("debugshape: shape list grown from " + std::to_string(oldLen) + " to " +
        std::to_string(length) + " slots");
    return true;
}

// =============================================================================
// Container analysis, and spawning shapes of our own
// =============================================================================
// A found shape gives us three facts for free: the holder's MethodTable, that
// its transform is a 4x4 inline at a known offset, and the chain that reaches
// it. If the last hop lands at +0x10 of the previous object then that object is
// an array and the holder is element 0 - which is what a list of live debug
// shapes would look like.
//
// Given that, spawning does not require understanding the class at all: clone a
// holder the client itself built, patch the transform, drop it in the next free
// slot and bump the count. Everything we do not understand comes along for the
// ride with correct values already in it.
std::mutex g_dbgContMtx;
DbgContainer g_dbgContainer;
std::mutex g_dbgReportMtx;
std::string g_dbgReport;

std::string DbgGetReport() {
    std::lock_guard lock(g_dbgReportMtx);
    return g_dbgReport;
}

namespace {

void AppendLine(std::string& out, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out += buf;
    out += "\n";
}

// A .NET array: [MethodTable][native-width length][payload at +0x10].
bool LooksLikeArray(uintptr_t obj, int64_t& outLen) {
    int64_t len = 0;
    if (!SafeRead((const void*)(obj + 8), &len, 8)) return false;
    if (len < 1 || len > 65536) return false;
    outLen = len;
    return true;
}

} // namespace

void DbgAnalyzeMatch(int index) {
    DbgLiveShape s;
    {
        std::lock_guard lock(g_dbgLiveMtx);
        if (index < 0 || index >= (int)g_dbgLiveShapes.size()) return;
        s = g_dbgLiveShapes[index];
    }
    if (s.viaArray || s.path.empty()) {
        DbgSetStatus("that match isn't a plain field in an object - pick the one tagged "
            "as a 4x4 matrix");
        return;
    }

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) { DbgSetStatus("no live game root"); return; }

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!modBase) return;

    std::string rep;
    DbgContainer cont;

    const uintptr_t holder = GsdkFollow(root, s.path);
    if (!holder) { DbgSetStatus("the match no longer resolves - has the shape expired?"); return; }

    uint64_t holderMT = 0;
    SafeRead((const void*)holder, &holderMT, 8);

    AppendLine(rep, "holder      0x%llX  MT 0x%llX (+0x%llX)",
        (unsigned long long)holder, (unsigned long long)holderMT,
        (unsigned long long)(holderMT - modBase));
    AppendLine(rep, "transform   inline at +0x%X, translation at +0x%X",
        s.fieldOffset >= 48 ? s.fieldOffset - 48 : 0, s.fieldOffset);

    cont.holderMTRva = (uint64_t)(holderMT - modBase);
    cont.matrixOffset = s.fieldOffset >= 48 ? s.fieldOffset - 48 : 0;
    cont.transOffset = s.fieldOffset;

    // ---- the object one hop back ----
    std::vector<uint32_t> parentPath(s.path.begin(), s.path.end() - 1);
    const uint32_t lastHop = s.path.back();
    const uintptr_t parent = GsdkFollow(root, parentPath);
    AppendLine(rep, "");
    AppendLine(rep, "parent      0x%llX reached by +0x%X", (unsigned long long)parent, lastHop);

    int64_t arrLen = 0;
    int liveSlots = 0;
    if (parent && lastHop == 0x10 && LooksLikeArray(parent, arrLen)) {
        uint64_t parentMT = 0;
        SafeRead((const void*)parent, &parentMT, 8);
        AppendLine(rep, "            looks like an ARRAY, length %lld, MT 0x%llX (+0x%llX)",
            (long long)arrLen, (unsigned long long)parentMT,
            (unsigned long long)(parentMT - modBase));

        // How many slots hold something. That count is what a size field would
        // agree with, which is how we find the size field without guessing.
        const int64_t show = (arrLen < 16) ? arrLen : 16;
        for (int64_t i = 0; i < arrLen && i < 4096; ++i) {
            uint64_t e = 0;
            if (!SafeRead((const void*)(parent + 0x10 + i * 8), &e, 8)) break;
            if (e) ++liveSlots;
        }
        for (int64_t i = 0; i < show; ++i) {
            uint64_t e = 0;
            SafeRead((const void*)(parent + 0x10 + i * 8), &e, 8);
            uint64_t emt = 0;
            if (e) SafeRead((const void*)e, &emt, 8);
            AppendLine(rep, "            [%lld] 0x%llX%s", (long long)i,
                (unsigned long long)e,
                (emt && emt == holderMT) ? "   <- same type as the holder" : "");
        }
        AppendLine(rep, "            %d non-null slot(s)", liveSlots);

        cont.isArray = true;
        cont.arrayPath = parentPath;
        cont.arrayLength = (uint32_t)arrLen;
        cont.liveSlots = liveSlots;
    }
    else if (parent) {
        AppendLine(rep, "            NOT an array - the holder is a plain field here, so "
            "there is no list to append to");
    }

    // ---- the object two hops back: where a size/count field would live ----
    if (cont.isArray && parentPath.size() >= 1) {
        std::vector<uint32_t> ownerPath(parentPath.begin(), parentPath.end() - 1);
        const uint32_t arrayHop = parentPath.back();
        const uintptr_t owner = GsdkFollow(root, ownerPath);
        AppendLine(rep, "");
        AppendLine(rep, "owner       0x%llX holds the array at +0x%X",
            (unsigned long long)owner, arrayHop);

        if (owner) {
            uint8_t buf[0x200];
            const size_t got = SafeRead((const void*)owner, buf, sizeof(buf)) ? sizeof(buf) : 0;
            if (got) {
                AppendLine(rep, "            ints equal to the non-null count (%d):", liveSlots);
                int candidates = 0;
                for (uint32_t off = 8; off + 4 <= got; off += 4) {
                    int32_t v;
                    memcpy(&v, buf + off, 4);
                    if (v == liveSlots) {
                        AppendLine(rep, "              +0x%X", off);
                        if (candidates == 0) {
                            cont.countPath = ownerPath;
                            cont.countOffset = off;
                            cont.haveCount = true;
                        }
                        ++candidates;
                    }
                }
                if (!candidates)
                    AppendLine(rep, "              none - the count may live elsewhere, or "
                        "the array may be fully packed");
                cont.countCandidates = candidates;
            }
        }
    }

    // ---- snapshot the holder as a template ----
    if (SafeRead((const void*)holder, cont.templateBytes, sizeof(cont.templateBytes))) {
        cont.haveTemplate = true;
        AppendLine(rep, "");
        AppendLine(rep, "template    captured %u bytes of a holder the client built",
            (unsigned)sizeof(cont.templateBytes));
    }

    cont.valid = cont.isArray && cont.haveTemplate;
    { std::lock_guard lock(g_dbgContMtx); g_dbgContainer = cont; }
    { std::lock_guard lock(g_dbgReportMtx); g_dbgReport = rep; }

    DbgSetStatus(cont.valid
        ? "container analysed - try Spawn"
        : "analysed, but this doesn't look like an appendable list; see the report");
}

// =============================================================================
// Spawning
// =============================================================================
namespace {

std::mutex g_dbgSpawnMtx;
std::vector<DbgRequest> g_dbgSpawnQueue;

// Builds the column-major 4x4 the client's own shapes carry: scale down the
// diagonal, position in the translation column.
void FillMatrix(float* m, const DbgRequest& r) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = r.scaleX; m[5] = r.scaleY; m[10] = r.scaleZ;
    m[12] = r.x; m[13] = r.y; m[14] = r.z;
    m[15] = 1.0f;
}

// Everything about an entry that depends on the request. Shared by spawning and
// by updating one in place, so the two can never disagree about where a field
// lives or what has to be written alongside it.
void WriteEntryFields(void* obj, const DbgRequest& req, const DbgContainer& c,
    uint32_t baseSize) {
    if (c.matrixOffset + 64 <= baseSize) {
        float m[16];
        FillMatrix(m, req);
        memcpy((uint8_t*)obj + c.matrixOffset, m, sizeof(m));
    }

    // Without these the clone keeps the template's colour and whatever was left
    // of its countdown, which is why spawned shapes came out the wrong colour
    // and disappeared almost immediately.
    if (c.haveColor && c.colorOffset + 12 <= baseSize) {
        const float col[3] = { req.r, req.g, req.b };
        memcpy((uint8_t*)obj + c.colorOffset, col, sizeof(col));
    }
    const float life = req.time > 0.0f ? req.time : c.lifeMax;
    if (c.haveLife && c.lifeOffset + 4 <= baseSize)
        memcpy((uint8_t*)obj + c.lifeOffset, &life, 4);
    // Both halves matter: the countdown is what ticks, the total is what the
    // client fades against, so a mismatch makes a shape vanish or never fade.
    if (c.haveLifeTotal && c.lifeTotalOffset + 4 <= baseSize)
        memcpy((uint8_t*)obj + c.lifeTotalOffset, &life, 4);
    if (c.haveOpacity && c.opacityOffset + 4 <= baseSize)
        memcpy((uint8_t*)obj + c.opacityOffset, &req.opacity, 4);
    if (c.haveShapeEnum && c.shapeOffset + 4 <= baseSize) {
        const int32_t shapeVal = (int32_t)req.shape;
        memcpy((uint8_t*)obj + c.shapeOffset, &shapeVal, 4);
    }
}

// What SpawnOne reports. Deferred means nothing happened and the same request
// should be tried again on a later frame (see AllocFitsWithoutCollecting).
enum SpawnResult : int { kSpawnFailed = 0, kSpawnOk = 1, kSpawnDeferred = 2 };

int SpawnOne(const DbgRequest& req, const DbgContainer& c, uintptr_t root, uintptr_t modBase) {
    if (!pRhpNewFast) return kSpawnFailed;

    const uintptr_t arr = GsdkFollow(root, c.arrayPath);
    if (!arr) return kSpawnFailed;
    int64_t len = 0;
    if (!LooksLikeArray(arr, len)) return kSpawnFailed;

    // Find a free slot, starting where the last one landed. Never overwrite a
    // live shape - take the first empty one, and if the tail is full, come back
    // round for anything freed near the front.
    int64_t slot = -1;
    if (g_spawnSlotHint < 0 || g_spawnSlotHint >= len) g_spawnSlotHint = 0;
    for (int64_t i = g_spawnSlotHint; i < len; ++i) {
        uint64_t e = 0;
        if (!SafeRead((const void*)(arr + 0x10 + i * 8), &e, 8)) return kSpawnFailed;
        if (!e) { slot = i; break; }
    }
    if (slot < 0) {
        for (int64_t i = 0; i < g_spawnSlotHint; ++i) {
            uint64_t e = 0;
            if (!SafeRead((const void*)(arr + 0x10 + i * 8), &e, 8)) return kSpawnFailed;
            if (!e) { slot = i; break; }
        }
    }
    if (slot < 0) return kSpawnFailed;
    g_spawnSlotHint = slot + 1;

    void* const mt = (void*)(uintptr_t)c.entryMTAbs;

    // How big the object actually is. A NativeAOT MethodTable keeps its base
    // size at +4, and it is the whole ballgame: the first version copied a
    // flat 0x200 bytes into whatever RhpNewFast returned, so any entry smaller
    // than that had its clone run straight off the end of the allocation and
    // into the next object's header. That is heap corruption, and it is why
    // this took the game down.
    uint32_t baseSize = 0;
    if (!SafeRead((const void*)((uintptr_t)mt + 4), &baseSize, 4)) return kSpawnFailed;
    if (baseSize < 16 || baseSize > 0x10000) return kSpawnFailed;

    // Before the free-slot search's result is used for anything: if the object
    // would not fit in the allocation context, wait for a later frame rather
    // than let the allocator start a collection from under our frame.
    if (!AllocFitsWithoutCollecting(((size_t)baseSize + 7) & ~(size_t)7)) {
        g_spawnSlotHint = slot;   // give the slot back
        return kSpawnDeferred;
    }
    void* obj = pRhpNewFast(mt);
    if (!obj) return kSpawnFailed;

    // If this is not really the allocator, what comes back will not have our
    // MethodTable in its header. Check before writing a single byte.
    uint64_t gotMT = 0;
    if (!SafeRead(obj, &gotMT, 8)) return kSpawnFailed;
    if (gotMT != (uint64_t)(uintptr_t)mt) return kSpawnFailed;

    // Clone a holder the client built, so every field we have not identified
    // arrives with a value the renderer already accepted.
    uint8_t tmp[sizeof(c.templateBytes)];
    memcpy(tmp, c.templateBytes, sizeof(tmp));

    size_t copyBytes = baseSize;
    if (copyBytes > sizeof(tmp)) copyBytes = sizeof(tmp);   // rest stays zeroed

    // Every pointer-shaped field in the template is zeroed. Not probed - zeroed.
    //
    // The template is a snapshot of an object the client built once, and the
    // collector COMPACTS: anything it referenced has very likely moved since.
    // The previous version kept a pointer whenever the old address still read
    // like an object, which after a compaction it frequently does - the memory
    // is reused, not cleared. Writing that stale pointer into a live object
    // hands the collector a reference to nothing, and it dies the next time it
    // walks the heap.
    //
    // That is why the crash followed heavy redrawing rather than any single
    // draw: churn means collections, collections mean moves, and each clone was
    // another chance to plant a bad pointer. A null reference cannot do that.
    // Every field this code actually sets - matrix, colour, life, opacity,
    // shape - is a value type, so there is nothing here worth the risk.
    for (uint32_t off = 8; off + 8 <= copyBytes; off += 8) {
        uint64_t v;
        memcpy(&v, tmp + off, 8);
        if (!v) continue;
        if ((v & 7) || !IsPlausiblePtr((const void*)v)) continue;
        const uint64_t zero = 0;
        memcpy(tmp + off, &zero, 8);
    }

    // Never copy over the object header, and never past the object's own end.
    if (copyBytes > 8)
        memcpy((uint8_t*)obj + 8, tmp + 8, copyBytes - 8);

    WriteEntryFields(obj, req, c, baseSize);

    // Read the finished entry back before publishing it. The client validates
    // what it renders - it threw "'0' cannot be greater than -1.1396317E+09",
    // which is a range check on a float that had no business being there - and
    // an entry we never show it cannot crash it. Cheap, and it turns a crash
    // into a refusal.
    {
        float f[48];
        const size_t n = (baseSize - 8) / 4 > 48 ? 48 : (baseSize - 8) / 4;
        memcpy(f, (const uint8_t*)obj + 8, n * 4);
        for (size_t i = 0; i < n; ++i) {
            if (!std::isfinite(f[i]) || std::fabs(f[i]) > 1.0e6f) {
                char why[160];
                snprintf(why, sizeof(why),
                    "refused to publish an entry: +0x%X reads %g, which the client "
                    "would reject", (unsigned)(8 + i * 4), f[i]);
                SetSpawnFailure(why);
                return kSpawnFailed;
            }
        }
    }

    // Resolve the array AGAIN, now that the allocation is done.
    //
    // pRhpNewFast can collect, and a NativeAOT collection COMPACTS - it moves
    // objects, this array among them. The address read before allocating is
    // then wherever the array used to be, and publishing through it writes into
    // whatever now occupies that memory. That is heap corruption, and it is
    // what took the game down partway through drawing a large build: harmless
    // at eight spawns a frame, near-certain at a thousand.
    //
    // obj itself is safe to hold across this: nothing between the allocation
    // and the store allocates, so there is no collection that could move it.
    // (With the fit check above the allocation itself cannot collect either,
    // so the array has not moved - this stays as a cheap second opinion.)
    const uintptr_t arrNow = GsdkFollow(root, c.arrayPath);
    if (!arrNow) return kSpawnFailed;
    int64_t lenNow = 0;
    if (!LooksLikeArray(arrNow, lenNow)) return kSpawnFailed;
    if (slot >= lenNow) return kSpawnFailed;

    // And the slot may not be free any more if the move shuffled things, so
    // check rather than assume - overwriting a live entry leaks a shape the
    // client is still drawing.
    {
        uint64_t occupied = 0;
        if (!SafeRead((const void*)(arrNow + 0x10 + slot * 8), &occupied, 8)) return kSpawnFailed;
        if (occupied) {
            slot = -1;
            for (int64_t i = 0; i < lenNow; ++i) {
                uint64_t e = 0;
                if (!SafeRead((const void*)(arrNow + 0x10 + i * 8), &e, 8)) return kSpawnFailed;
                if (!e) { slot = i; break; }
            }
            if (slot < 0) return kSpawnFailed;
            g_spawnSlotHint = slot + 1;
        }
    }

    if (!DbgStoreRef((void*)(arrNow + 0x10 + slot * 8), obj)) return kSpawnFailed;

    if (req.tag) {
        std::lock_guard lock(g_dbgTagMtx);
        g_dbgTagSlots[req.tag] = slot;
    }

    // Always. The count is List<T>'s _size, measured rather than guessed, and
    // the client draws entries 0.._size - so not writing it means nothing is
    // drawn no matter what is in the array. It was optional back when the
    // offset was a guess that matched 86 different ints; it is not a guess now.
    if (c.haveCount) {
        const uintptr_t owner = GsdkFollow(root, c.countPath);
        if (owner) {
            int32_t cur = 0;
            if (SafeRead((const void*)(owner + c.countOffset), &cur, 4)) {
                int32_t next = (int32_t)(slot + 1) > cur ? (int32_t)(slot + 1) : cur + 1;
                if (next > (int32_t)lenNow) next = (int32_t)lenNow;
                if (next > cur) {
                    SafeWrite((void*)(owner + c.countOffset), &next, 4);
                    // _version sits right after _size and changes on every Add.
                    // Leaving it behind is what makes a live enumerator throw.
                    if (c.countOffset == DbgKnown::kSizeOffset) {
                        int32_t ver = 0;
                        if (SafeRead((const void*)(owner + DbgKnown::kVersionOffset), &ver, 4)) {
                            ++ver;
                            SafeWrite((void*)(owner + DbgKnown::kVersionOffset), &ver, 4);
                        }
                    }
                }
            }
        }
    }
    return kSpawnOk;
}

int SpawnOneGuarded(const DbgRequest& r, const DbgContainer& c,
    uintptr_t root, uintptr_t modBase) {
    __try { return SpawnOne(r, c, root, modBase); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kSpawnFailed; }
}

} // namespace

namespace {
std::mutex g_dbgFailMtx;
std::string g_dbgSpawnFailure;
void SetSpawnFailure(const std::string& s) {
    std::lock_guard lock(g_dbgFailMtx);
    g_dbgSpawnFailure = s;
}
}

std::string DbgGetSpawnFailure() {
    std::lock_guard lock(g_dbgFailMtx);
    return g_dbgSpawnFailure;
}

bool DbgQueueSpawn(const DbgRequest& req) {
    {
        std::lock_guard lock(g_dbgContMtx);
        if (!g_dbgContainer.valid) {
            SetSpawnFailure("no shape list resolved - draw one with /debug shape cube first");
            return false;
        }
    }
    // Only the object allocator matters here: the transform is inline in the
    // entry, so there is no array to allocate and RhpNewFastArray is irrelevant.
    if (!g_dbgAllocVerified.load() && !DbgResolveAllocator()) {
        SetSpawnFailure("no verified allocator - see the Runtime section. Use \"Duplicate\" "
            "or Keep alive in the meantime; neither allocates.");
        return false;
    }
    if (!g_dbgAllocVerified.load()) {
        SetSpawnFailure("the allocator candidate did not pass its code check");
        return false;
    }
    std::lock_guard lock(g_dbgSpawnMtx);
    // Outlining a build queues the whole thing at once and the drain empties
    // the queue every frame, so this only exists to stop a runaway. It has to
    // clear the largest outline anyone can ask for, or most of a building is
    // dropped here rather than drawn.
    // Has to clear the largest outline that can be asked for, which is
    // kDbgMaxCapacity, or the back of a big build is dropped here and never
    // drawn. It was 16384 against a capacity of 32768, so half a statue.
    if (g_dbgSpawnQueue.size() >= (size_t)kDbgMaxCapacity * 2) {
        SetSpawnFailure("spawn queue is full");
        return false;
    }
    g_dbgSpawnQueue.push_back(req);
    SetSpawnFailure("");
    return true;
}

// Appending an existing entry costs nothing and proves half the mechanism: if a
// second shape appears, the array and count are right and only allocation is
// missing. If it does not, the count field is the thing to doubt.
bool DbgQueueDuplicate() {
    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    if (!c.valid) {
        SetSpawnFailure("no shape list resolved yet");
        return false;
    }

    const uintptr_t root = GsdkCurrentRoot();
    const uintptr_t arr = GsdkFollow(root, c.arrayPath);
    if (!arr) { SetSpawnFailure("the shape array no longer resolves"); return false; }

    int64_t len = 0;
    if (!SafeRead((const void*)(arr + 8), &len, 8) || len < 2) {
        SetSpawnFailure("the shape array is too small to append to");
        return false;
    }

    uint64_t src = 0;
    int64_t freeSlot = -1;
    for (int64_t i = 0; i < len; ++i) {
        uint64_t e = 0;
        if (!SafeRead((const void*)(arr + 0x10 + i * 8), &e, 8)) break;
        if (e && !src) src = e;
        else if (!e && freeSlot < 0) freeSlot = i;
    }
    if (!src) { SetSpawnFailure("no existing shape to copy"); return false; }
    if (freeSlot < 0) { SetSpawnFailure("no free slot in the array"); return false; }

    // Through the barrier like every other reference store - this one happens
    // to be an object the array already held, but there is no reason to have
    // two rules.
    if (!DbgStoreRef((void*)(arr + 0x10 + freeSlot * 8), (void*)(uintptr_t)src)) {
        SetSpawnFailure("could not write the array slot");
        return false;
    }

    if (c.haveCount) {
        const uintptr_t owner = GsdkFollow(root, c.countPath);
        int32_t cur = 0;
        if (owner && SafeRead((const void*)(owner + c.countOffset), &cur, 4)) {
            int32_t next = (int32_t)(freeSlot + 1) > cur ? (int32_t)(freeSlot + 1) : cur + 1;
            if (next > (int32_t)len) next = (int32_t)len;
            if (next > cur) SafeWrite((void*)(owner + c.countOffset), &next, 4);
        }
    }

    SetSpawnFailure("");
    DbgSetStatus("duplicated an existing shape into slot " + std::to_string(freeSlot));
    return true;
}

namespace {
std::atomic<bool> g_dbgAllocTestWanted{ false };
std::mutex g_dbgAllocTestMtx;
std::string g_dbgAllocTestResult;

void RunAllocTest() {
    char msg[256];
    uint64_t mtRva = 0;
    { std::lock_guard lock(g_dbgContMtx); mtRva = g_dbgContainer.holderMTRva; }

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!modBase || !mtRva) {
        snprintf(msg, sizeof(msg), "no MethodTable known yet - draw a shape first");
    }
    else if (!pRhpNewFast || !g_dbgAllocVerified.load()) {
        snprintf(msg, sizeof(msg),
            "no verified allocator - refusing to call anything. Calling the wrong "
            "function here hangs the game rather than faulting, so a guess is not "
            "worth the crash.");
    }
    else {
        void* const mt = (void*)(modBase + mtRva);
        uint32_t baseSize = 0;
        SafeRead((const void*)((uintptr_t)mt + 4), &baseSize, 4);

        void* obj = DbgAllocFitsNow(mt) ? pRhpNewFast(mt) : nullptr;
        uint64_t gotMT = 0;
        if (obj) SafeRead(obj, &gotMT, 8);

        if (!obj)
            snprintf(msg, sizeof(msg), "allocator returned null (baseSize %u)", baseSize);
        else if (gotMT != (uint64_t)(uintptr_t)mt)
            snprintf(msg, sizeof(msg),
                "allocated 0x%llX but its header reads %llX, not our MethodTable - "
                "that function is NOT the allocator",
                (unsigned long long)(uintptr_t)obj, (unsigned long long)gotMT);
        else
            snprintf(msg, sizeof(msg),
                "OK - allocated 0x%llX, header correct, object is %u bytes",
                (unsigned long long)(uintptr_t)obj, baseSize);
    }
    std::lock_guard lock(g_dbgAllocTestMtx);
    g_dbgAllocTestResult = msg;
}

bool RunAllocTestGuarded() {
    __try { RunAllocTest(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

void DbgRequestAllocTest() {
    { std::lock_guard lock(g_dbgAllocTestMtx); g_dbgAllocTestResult = "testing..."; }
    g_dbgAllocTestWanted.store(true);
}

std::string DbgGetAllocTestResult() {
    std::lock_guard lock(g_dbgAllocTestMtx);
    return g_dbgAllocTestResult;
}

// ---- work that has to happen on the game thread -------------------------
namespace {

enum class DbgOp { CreateList, GrowArray, AllocTests, Duplicate, ClearAll };
struct DbgPendingOp { DbgOp op; int arg; };

std::mutex g_dbgOpMtx;
std::deque<DbgPendingOp> g_dbgOps;

void QueueOp(DbgOp op, int arg) {
    std::lock_guard lock(g_dbgOpMtx);
    if (g_dbgOps.size() > 16) return;
    g_dbgOps.push_back(DbgPendingOp{ op, arg });
}

void RunOp(const DbgPendingOp& o) {
    switch (o.op) {
    case DbgOp::CreateList: DbgCreateShapeList(o.arg); break;
    case DbgOp::GrowArray:  DbgGrowArrayTo(o.arg); break;
    case DbgOp::Duplicate:  DbgQueueDuplicate(); break;
    case DbgOp::ClearAll:   DbgClearAllShapes(); break;
    case DbgOp::AllocTests: {
        DbgContainer cc;
        { std::lock_guard lock(g_dbgContMtx); cc = g_dbgContainer; }
        DbgTestArrayAllocator((uintptr_t)cc.arrayMTAbs);
        if (DbgResolveWriteBarrier()) DbgTestWriteBarrier();
        break;
    }
    }
}

bool RunOpGuarded(const DbgPendingOp& o) {
    __try { RunOp(o); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Whether an op's allocations all fit in this thread's allocation context right
// now, so none of them can start a collection (AllocFitsWithoutCollecting). An
// op that does not fit waits for a later frame instead of being run.
bool OpFitsNow(const DbgPendingOp& o) {
    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    void* const arrMT = (void*)(uintptr_t)c.arrayMTAbs;
    switch (o.op) {
    case DbgOp::CreateList: {
        const int len = o.arg < 4 ? 4 : (o.arg > 8192 ? 8192 : o.arg);
        return !arrMT || AllocFitsWithoutCollecting(ArrayBytes(arrMT, len));
    }
    case DbgOp::GrowArray:
        return !arrMT || AllocFitsWithoutCollecting(ArrayBytes(arrMT, o.arg));
    case DbgOp::AllocTests:
        // The array test allocates 8 slots and the barrier test 4, back to back.
        return !arrMT || AllocFitsWithoutCollecting(ArrayBytes(arrMT, 8) + ArrayBytes(arrMT, 4));
    case DbgOp::Duplicate:
    case DbgOp::ClearAll:
        return true;
    }
    return true;
}

} // namespace

// Growing the list is a standing wish, not a queued op. An op waits at the front
// of the queue until its whole allocation fits in the thread's allocation context,
// and a list of thousands never does - it waited for ever, silently, and blocked
// everything queued behind it. The wish is remembered instead, and every frame
// takes whatever step fits.
static std::atomic<int> g_dbgGrowWanted{ 0 };
static std::atomic<int> g_dbgBestRoom{ 0 };    // the most slots the allocator has offered at once
void DbgRequestCreateList(int length)  { if (length > 0) g_dbgGrowWanted.store(length); }
int DbgBestRoomSlots() { return g_dbgBestRoom.load(); }
void DbgRequestGrowArray(int length)   { QueueOp(DbgOp::GrowArray, length); }
void DbgRequestAllocatorTests()        { QueueOp(DbgOp::AllocTests, 0); }
void DbgRequestDuplicate()             { QueueOp(DbgOp::Duplicate, 0); }
void DbgRequestClearAll()              { QueueOp(DbgOp::ClearAll, 0); }

// One step toward the wanted list size, with whatever the allocator has free now.
static void DbgGrowStep() {
    const int wantLen = g_dbgGrowWanted.load();
    if (wantLen <= 0) return;

    const size_t bytes = AllocBytesNow();
    int room = -1;                                     // -1: cannot tell
    if (bytes != SIZE_MAX)
        room = bytes >= 0x18 ? (int)(std::min)((size_t)65536, (bytes - 0x18) / 8) : 0;
    if (room > g_dbgBestRoom.load()) g_dbgBestRoom.store(room);

    int cur = DbgListCapacity();
    if (cur < 0) cur = 0;
    const int target = (std::min)(wantLen, kDbgMaxCapacity);
    if (cur >= target) {
        int expect = wantLen;
        g_dbgGrowWanted.compare_exchange_strong(expect, 0);
        return;
    }
    const int can = room < 0 ? target : (std::min)(target, room);
    // Not a step of a few slots: wait for a moment with real room, or reach the goal.
    if (can < 4 || (can < target && can < cur + cur / 4 + 8)) return;
    if (!RunOpGuarded(DbgPendingOp{ DbgOp::CreateList, can }))
        SetSpawnFailure("growing the list faulted - see the log");
}

void DbgDrainSpawns() {
    DbgGrowStep();
    for (;;) {
        DbgPendingOp o;
        {
            std::lock_guard lock(g_dbgOpMtx);
            if (g_dbgOps.empty()) break;
            o = g_dbgOps.front();
            // Left at the front until there is room for it, so ops keep their order.
            if (!OpFitsNow(o)) break;
            g_dbgOps.pop_front();
        }
        if (!RunOpGuarded(o))
            SetSpawnFailure("that operation faulted - see the log");
    }

    if (g_dbgAllocTestWanted.load()) {
        uint64_t mtRva = 0;
        { std::lock_guard lock(g_dbgContMtx); mtRva = g_dbgContainer.holderMTRva; }
        size_t modSize = 0;
        const uintptr_t modBase = GetGameModuleBaseSize(modSize);
        const bool fits = !modBase || !mtRva || DbgAllocFitsNow((void*)(modBase + mtRva));
        if (fits && g_dbgAllocTestWanted.exchange(false)) {
            if (!RunAllocTestGuarded()) {
                std::lock_guard lock(g_dbgAllocTestMtx);
                g_dbgAllocTestResult = "the call faulted - that function is not the allocator";
            }
        }
    }

    // A budget per frame, not the whole queue. Draining everything meant a
    // 32000-block build did 32000 object allocations inside one frame, which
    // is a stall at best. Spread over frames it is a second of filling in,
    // and the game keeps running while it happens.
    std::vector<DbgRequest> batch;
    {
        std::lock_guard lock(g_dbgSpawnMtx);
        if (g_dbgSpawnQueue.empty()) return;
        const size_t take = (std::min)(g_dbgSpawnQueue.size(),
            (size_t)(std::max)(16, g_dbgSpawnPerFrame.load()));
        batch.assign(g_dbgSpawnQueue.begin(), g_dbgSpawnQueue.begin() + take);
        g_dbgSpawnQueue.erase(g_dbgSpawnQueue.begin(),
            g_dbgSpawnQueue.begin() + take);
    }

    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    if (!c.valid) return;

    const uintptr_t root = GsdkCurrentRoot();
    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!root || !modBase) return;

    for (size_t i = 0; i < batch.size(); ++i) {
        const int res = SpawnOneGuarded(batch[i], c, root, modBase);
        if (res == kSpawnDeferred) {
            // Out of allocation room for this frame: the rest go back to the
            // front of the queue, in order, for the next one.
            std::lock_guard lock(g_dbgSpawnMtx);
            g_dbgSpawnQueue.insert(g_dbgSpawnQueue.begin(), batch.begin() + i, batch.end());
            break;
        }
        if (res == kSpawnOk) g_dbgSentCount.fetch_add(1);
        else                 g_dbgFailCount.fetch_add(1);
    }
}

// =============================================================================
// Drawing
// =============================================================================
bool DbgQueueShape(const DbgRequest& req) {
    if (!DbgReady()) return false;
    std::lock_guard lock(g_dbgQueueMtx);
    if (g_dbgQueue.size() > 64) return false;      // never let this pile up
    g_dbgQueue.push_back(req);
    return true;
}

namespace {

// Builds the column-major 4x4 the client expects: a scale on the diagonal and
// the position in the translation column, which is what the server's
// makeMatrix() produces for a sphere or cube.
void BuildMatrix(const DbgRequest& r, float* m) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = r.scaleX;
    m[5] = r.scaleY;
    m[10] = r.scaleZ;
    m[12] = r.x;
    m[13] = r.y;
    m[14] = r.z;
    m[15] = 1.0f;
}

int EmitOne(const DbgRequest& req, const DbgLayout& lay, uintptr_t modBase) {
    if (!pRhpNewFast || !pRhpNewFastArray) return kSpawnFailed;

    void* const packetMT = (void*)(modBase + lay.packetMTRva);
    void* const arrayMT = (void*)(modBase + lay.floatArrayMTRva);

    // Both are unrooted until the client has the packet, so a collection
    // between the two allocations would free the array and leave the packet
    // pointing at nothing. Checking that BOTH fit before making either means
    // neither allocation can collect.
    const size_t arrBytes = ArrayBytes(arrayMT, 16);
    const size_t pktBytes = ObjectBytes(packetMT);
    if (arrBytes == SIZE_MAX || pktBytes == SIZE_MAX) return kSpawnFailed;
    if (!AllocFitsWithoutCollecting(arrBytes + pktBytes)) return kSpawnDeferred;

    void* arr = pRhpNewFastArray(arrayMT, 16);
    if (!arr) return kSpawnFailed;

    float m[16];
    BuildMatrix(req, m);
    memcpy((uint8_t*)arr + kArrayDataOffset, m, sizeof(m));

    void* packet = pRhpNewFast(packetMT);
    if (!packet) return kSpawnFailed;

    uint8_t* p = (uint8_t*)packet;
    memcpy(p + lay.offMatrix, &arr, 8);

    if (lay.haveColor) {
        const float c[3] = { req.r, req.g, req.b };
        memcpy(p + lay.offColor, c, sizeof(c));
    }
    if (lay.haveShape) {
        if (lay.shapeIsByte) p[lay.offShape] = (uint8_t)req.shape;
        else { const int32_t v = (int32_t)req.shape; memcpy(p + lay.offShape, &v, 4); }
    }
    if (lay.haveTime)    memcpy(p + lay.offTime, &req.time, 4);
    if (lay.haveOpacity) memcpy(p + lay.offOpacity, &req.opacity, 4);
    if (lay.haveFlags)   p[lay.offFlags] = (uint8_t)req.flags;

    return PacketDispatchInbound(packet) ? kSpawnOk : kSpawnFailed;
}

// __try/__except cannot live in a frame that also unwinds C++ objects (C2712),
// so the guard gets a frame of its own - same split hooks.cpp uses for SafeRead.
int EmitOneGuarded(const DbgRequest& req, const DbgLayout& lay, uintptr_t modBase) {
    __try { return EmitOne(req, lay, modBase); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kSpawnFailed; }
}

} // namespace

void DbgDrainQueue() {
    // Runs on the game thread, from the per-frame hook. Keep it short.
    DriveAttachedShape();
    DbgDrainSpawns();

    std::vector<DbgRequest> batch;
    {
        std::lock_guard lock(g_dbgQueueMtx);
        if (g_dbgQueue.empty()) return;
        while (!g_dbgQueue.empty() && batch.size() < 8) {
            batch.push_back(g_dbgQueue.front());
            g_dbgQueue.pop_front();
        }
    }

    DbgLayout lay;
    { std::lock_guard lock(g_dbgMtx); lay = g_dbgLayout; }
    if (!lay.learned) return;

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!modBase) return;

    for (size_t i = 0; i < batch.size(); ++i) {
        const int res = EmitOneGuarded(batch[i], lay, modBase);
        if (res == kSpawnDeferred) {
            std::lock_guard lock(g_dbgQueueMtx);
            g_dbgQueue.insert(g_dbgQueue.begin(), batch.begin() + i, batch.end());
            break;
        }
        if (res == kSpawnOk) g_dbgSentCount.fetch_add(1);
        else                 g_dbgFailCount.fetch_add(1);
    }
}

// =============================================================================
// Getting ready without being asked
// =============================================================================
// The pieces above were built one at a time, and each arrived with its own
// button because each one needed watching while it was new. None of them needs
// watching any more: the order is fixed, every step can check its own result,
// and the thing that decides whether to move on is a boolean the code already
// computes. So this drives the whole chain from the frame hook.
//
// Two rules carry over unchanged, because both were learned the hard way:
// scanning the image happens on a worker, and anything that calls into the
// runtime is queued as a game-thread op rather than called from wherever the
// decision was made.
std::atomic<bool> g_dbgAutoPrepare{ true };
std::atomic<int>  g_dbgAutoStage{ (int)DbgAutoStage::NoRoot };
std::atomic<int>  g_dbgWantedCapacity{ 64 };
const int kDbgMaxCapacity = 8192;

// How many shapes get created per frame. Every one is an object allocation on
// the game thread, so this is the dial between "the build appears instantly and
// the game hitches" and "it fills in smoothly over a second".
// 128 a frame is about 7,500 a second, so a full 8192-block outline fills in
// inside two seconds. Higher was not faster in any way that mattered and gave
// the collector far more chances to move things underneath us.
std::atomic<int> g_dbgSpawnPerFrame{ 128 };

size_t DbgSpawnQueueDepth() {
    std::lock_guard lock(g_dbgSpawnMtx);
    return g_dbgSpawnQueue.size();
}

void DbgClearSpawnQueue() {
    std::lock_guard lock(g_dbgSpawnMtx);
    g_dbgSpawnQueue.clear();
}


namespace {

std::mutex  g_dbgAutoMtx;
std::string g_dbgAutoStatusText = "waiting for a world";

void SetAutoStatus(DbgAutoStage stage, const std::string& s) {
    g_dbgAutoStage.store((int)stage);
    std::lock_guard lock(g_dbgAutoMtx);
    g_dbgAutoStatusText = s;
}

// Where the shape array is, whether or not a container has been analysed yet.
std::vector<uint32_t> AutoArrayPath(const DbgContainer& c) {
    return c.arrayPath.empty() ? std::vector<uint32_t>{ 0x10, 0x1F0, 0x70 } : c.arrayPath;
}

// A step that failed is worth retrying, but not every frame - a scan that found
// nothing will find nothing again a millisecond later, and re-queuing an op
// that faulted just faults again. Each step gets its own clock.
struct Cooldown {
    long long lastMs = 0;
    bool Ready(long long now, long long gapMs) {
        if (now - lastMs < gapMs) return false;
        lastMs = now;
        return true;
    }
};

} // namespace

std::string DbgAutoStatus() {
    std::lock_guard lock(g_dbgAutoMtx);
    return g_dbgAutoStatusText;
}

int DbgListCapacity() {
    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return -1;
    const uintptr_t arr = GsdkFollow(root, AutoArrayPath(c));
    if (!arr) return -1;

    int64_t len = 0;
    if (!SafeRead((const void*)(arr + 8), &len, 8) || len < 0 || len > 65536) return -1;
    return (int)len;
}

int DbgFreeSlots() {
    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return -1;
    const uintptr_t arr = GsdkFollow(root, AutoArrayPath(c));
    if (!arr) return -1;

    int64_t len = 0;
    if (!SafeRead((const void*)(arr + 8), &len, 8) || len < 0 || len > 65536) return -1;

    int free = 0;
    for (int64_t i = 0; i < len; ++i) {
        uint64_t e = 0;
        if (!SafeRead((const void*)(arr + 0x10 + i * 8), &e, 8)) break;
        if (!e) ++free;
    }
    return free;
}

bool DbgSpawnReady() {
    return g_dbgAutoStage.load() == (int)DbgAutoStage::Ready;
}

void DbgAutoTick() {
    if (!g_dbgAutoPrepare.load()) return;

    static long long lastMs = 0;
    const long long now = NowSteadyMs();
    if (now - lastMs < 500) return;
    lastMs = now;

    static Cooldown locateCd, scanCd, testCd, capacityCd;

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) {
        SetAutoStatus(DbgAutoStage::NoRoot, "not in a world yet");
        return;
    }

    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }

    // Step 0: a template. Cloning an entry the client built is what makes the
    // fields we never identified come out right, so without one there is
    // nothing to clone and no amount of setup helps.
    if (!c.haveTemplate) {
        SetAutoStatus(DbgAutoStage::NoSeed,
            "no shape template yet - load a singleplayer world once and run "
            "/debug shape cube; it saves itself and then works everywhere");
        return;
    }

    // Step 1: the list itself. The known path is tried first and costs three
    // reads; searching by element type is the fallback that found it originally.
    const uintptr_t arr = GsdkFollow(root, AutoArrayPath(c));
    if (!arr || !c.entryMTAbs) {
        if (!g_dbgLocating.load() && locateCd.Ready(now, 5000)) DbgLocateList();
        SetAutoStatus(DbgAutoStage::Locating,
            g_dbgLocating.load() ? "looking for the client's shape list"
                                 : "the shape list does not resolve here - retrying");
        return;
    }

    // Step 2: read the image for the three entry points. All three are pure
    // reads, so they can share one worker and nothing is called yet.
    const bool haveScans = g_dbgAllocRva.load() && g_dbgArrayAllocRva.load() &&
                           g_dbgBarrierRva.load();
    if (!haveScans) {
        if (scanCd.Ready(now, 6000)) {
            StartWorker([]() {
                WorkerGuard guard;
                if (!g_dbgAllocRva.load())      DbgResolveAllocator();
                if (!g_dbgArrayAllocRva.load()) DbgResolveArrayAllocator();
                if (!g_dbgBarrierRva.load())    DbgResolveWriteBarrier();
                });
        }
        std::string missing;
        if (!g_dbgAllocRva.load())      missing += "object allocator ";
        if (!g_dbgArrayAllocRva.load()) missing += "array allocator ";
        if (!g_dbgBarrierRva.load())    missing += "write barrier ";
        SetAutoStatus(DbgAutoStage::Scanning, "reading the client's code for: " + missing);
        return;
    }

    // Step 3: prove them. Each is called once on a throwaway array that nothing
    // else can see, which is the only way to tell a real allocator from a
    // function that merely looks like one.
    if (!g_dbgArrayAllocVerified.load() || !g_dbgBarrierVerified.load()) {
        static int attempts = 0;
        if (attempts < 6 && testCd.Ready(now, 3000)) {
            ++attempts;
            DbgRequestAllocatorTests();
        }
        SetAutoStatus(DbgAutoStage::Testing, attempts >= 6
            ? "the allocator or barrier test keeps failing - open Shapes and read the evidence"
            : "checking the allocator and the write barrier");
        return;
    }

    // Step 4: room. A list of four is List<T>'s default and nowhere near enough
    // to outline a build, and an empty world starts with Array.Empty<T>(), which
    // has none at all. This also catches the client putting its own back.
    int64_t len = 0;
    SafeRead((const void*)(arr + 8), &len, 8);
    int wanted = g_dbgWantedCapacity.load();
    if (wanted > kDbgMaxCapacity) wanted = kDbgMaxCapacity;
    // The client's allocator only has a few KB free at a time, so the list can only
    // grow so far. Once it is as big as the allocator has ever offered, that is the
    // size: outlining the nearest blocks with it beats waiting for a size that
    // cannot come.
    bool atCeiling = false;
    if (len < wanted) {
        DbgRequestCreateList(wanted);       // cheap; the frame hook takes whatever step fits
        const int best = g_dbgBestRoom.load();
        atCeiling = len >= 64 && best > 0 && len * 10 >= (int64_t)best * 9;
        if (!atCeiling) {
            char buf[200];
            snprintf(buf, sizeof(buf), "making room: %lld slots, want %d (the allocator has offered room for %d so far)",
                (long long)len, wanted, best);
            SetAutoStatus(DbgAutoStage::Capacity, buf);
            return;
        }
    }

    char buf[240];
    if (atCeiling)
        snprintf(buf, sizeof(buf), "ready - %lld slots, %d free (the client's allocator offers about %d at a time, "
            "so the nearest blocks are outlined and it follows you)",
            (long long)len, DbgFreeSlots(), g_dbgBestRoom.load());
    else
        snprintf(buf, sizeof(buf), "ready - %lld slots, %d free", (long long)len, DbgFreeSlots());
    SetAutoStatus(DbgAutoStage::Ready, buf);
}

// =============================================================================
// Changing one shape instead of all of them
// =============================================================================
// Updating writes over the fields of an object already in the list. No
// allocation, so no collection, so none of the hazards that come with one -
// and the array is resolved fresh each time rather than remembered.
bool DbgUpdateTagged(uint64_t tag, const DbgRequest& req) {
    if (!tag) return false;

    int64_t slot = -1;
    {
        std::lock_guard lock(g_dbgTagMtx);
        auto it = g_dbgTagSlots.find(tag);
        if (it == g_dbgTagSlots.end()) return false;
        slot = it->second;
    }

    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    if (!c.valid || !c.isArray) return false;

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return false;
    const uintptr_t arr = GsdkFollow(root, c.arrayPath);
    if (!arr) return false;
    int64_t len = 0;
    if (!LooksLikeArray(arr, len) || slot < 0 || slot >= len) return false;

    uint64_t objAddr = 0;
    if (!SafeRead((const void*)(arr + 0x10 + slot * 8), &objAddr, 8)) return false;
    if (!objAddr) return false;

    // The slot may have been reused by something else since. Its MethodTable
    // says whether this is still one of ours.
    uint64_t gotMT = 0;
    if (!SafeRead((const void*)objAddr, &gotMT, 8)) return false;
    if (gotMT != c.entryMTAbs) return false;

    uint32_t baseSize = 0;
    if (!SafeRead((const void*)((uintptr_t)gotMT + 4), &baseSize, 4)) return false;
    if (baseSize < 16 || baseSize > 0x10000) return false;

    WriteEntryFields((void*)(uintptr_t)objAddr, req, c, baseSize);
    return true;
}

bool DbgRemoveTagged(uint64_t tag) {
    if (!tag) return false;

    int64_t slot = -1;
    {
        std::lock_guard lock(g_dbgTagMtx);
        auto it = g_dbgTagSlots.find(tag);
        if (it == g_dbgTagSlots.end()) return false;
        slot = it->second;
        g_dbgTagSlots.erase(it);
    }

    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    if (!c.valid || !c.isArray) return false;

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return false;
    const uintptr_t arr = GsdkFollow(root, c.arrayPath);
    if (!arr) return false;
    int64_t len = 0;
    if (!LooksLikeArray(arr, len) || slot < 0 || slot >= len) return false;

    // NOT nulled. Hidden.
    //
    // The first version wrote null into the slot, on the reasoning that an
    // empty entry simply draws nothing. That was a guess and it was wrong: the
    // client walks entries 0.._size and dereferences them without checking, so
    // the first block placed took the game down with "Object reference not set
    // to an instance of an object".
    //
    // The invariant is that every slot below _size holds a real object. So a
    // removed shape keeps its object and is made invisible instead, and the
    // slot goes on a free list to be rewritten later - which also means the
    // usual churn of building costs no allocation at all.
    uint64_t objAddr = 0;
    if (!SafeRead((const void*)(arr + 0x10 + slot * 8), &objAddr, 8)) return false;
    if (!objAddr) return false;

    uint64_t gotMT = 0;
    if (!SafeRead((const void*)objAddr, &gotMT, 8)) return false;
    if (gotMT != c.entryMTAbs) return false;

    uint32_t baseSize = 0;
    if (!SafeRead((const void*)((uintptr_t)gotMT + 4), &baseSize, 4)) return false;
    if (baseSize < 16 || baseSize > 0x10000) return false;

    DbgRequest hide;
    hide.shape = DbgShape::Cube;
    hide.x = hide.y = hide.z = 0.0f;
    hide.scaleX = hide.scaleY = hide.scaleZ = 0.0f;
    hide.opacity = 0.0f;
    hide.r = hide.g = hide.b = 0.0f;
    hide.time = 600.0f;           // not expiring: the object must stay valid
    hide.flags = DBG_FLAG_NONE;
    WriteEntryFields((void*)(uintptr_t)objAddr, hide, c, baseSize);

    { std::lock_guard lock(g_dbgTagMtx); g_dbgFreeSlots.push_back(slot); }
    return true;
}

// Rewrite a hidden entry instead of allocating a new one. This is what makes
// building cheap: a block placed frees a slot, the next shape needed takes it,
// and nothing is created or collected.
bool DbgReuseSlot(uint64_t tag, const DbgRequest& req) {
    if (!tag) return false;

    int64_t slot = -1;
    {
        std::lock_guard lock(g_dbgTagMtx);
        if (g_dbgFreeSlots.empty()) return false;
        slot = g_dbgFreeSlots.back();
        g_dbgFreeSlots.pop_back();
    }

    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    if (!c.valid || !c.isArray) return false;

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return false;
    const uintptr_t arr = GsdkFollow(root, c.arrayPath);
    if (!arr) return false;
    int64_t len = 0;
    if (!LooksLikeArray(arr, len) || slot < 0 || slot >= len) return false;

    uint64_t objAddr = 0;
    if (!SafeRead((const void*)(arr + 0x10 + slot * 8), &objAddr, 8)) return false;
    if (!objAddr) return false;
    uint64_t gotMT = 0;
    if (!SafeRead((const void*)objAddr, &gotMT, 8)) return false;
    if (gotMT != c.entryMTAbs) return false;

    uint32_t baseSize = 0;
    if (!SafeRead((const void*)((uintptr_t)gotMT + 4), &baseSize, 4)) return false;
    if (baseSize < 16 || baseSize > 0x10000) return false;

    WriteEntryFields((void*)(uintptr_t)objAddr, req, c, baseSize);
    { std::lock_guard lock(g_dbgTagMtx); g_dbgTagSlots[tag] = slot; }
    return true;
}

void DbgForgetTags() {
    std::lock_guard lock(g_dbgTagMtx);
    g_dbgTagSlots.clear();
    g_dbgFreeSlots.clear();
}

bool DbgHasTag(uint64_t tag) {
    std::lock_guard lock(g_dbgTagMtx);
    return g_dbgTagSlots.find(tag) != g_dbgTagSlots.end();
}

int DbgCountTagsAlive(const std::vector<uint64_t>& tags, int& missing) {
    missing = 0;
    std::vector<int64_t> slots;
    slots.reserve(tags.size());
    {
        std::lock_guard lock(g_dbgTagMtx);
        for (uint64_t t : tags) {
            const auto it = g_dbgTagSlots.find(t);
            if (it == g_dbgTagSlots.end()) ++missing;
            else slots.push_back(it->second);
        }
    }
    if (slots.empty()) return 0;

    DbgContainer c;
    { std::lock_guard lock(g_dbgContMtx); c = g_dbgContainer; }
    if (!c.valid || !c.isArray || !c.entryMTAbs) return -1;

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return -1;
    const uintptr_t arr = GsdkFollow(root, c.arrayPath);
    if (!arr) return -1;
    int64_t len = 0;
    if (!LooksLikeArray(arr, len)) return -1;

    // Reads only. A slot counts when it still holds an object of the entry
    // type we spawn - a fresh list after a relog is nulls, or the client's own
    // few shapes, so most of a sample comes back dead.
    int alive = 0;
    for (int64_t slot : slots) {
        if (slot < 0 || slot >= len) continue;
        uint64_t obj = 0;
        if (!SafeRead((const void*)(arr + 0x10 + slot * 8), &obj, 8) || !obj) continue;
        uint64_t mt = 0;
        if (!SafeRead((const void*)obj, &mt, 8)) continue;
        if (mt == c.entryMTAbs) ++alive;
    }
    return alive;
}
