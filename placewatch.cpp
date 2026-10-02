#include "pch.h"
#include "placewatch.hpp"
#include "common.hpp"
#include "hooks.hpp"
#include "prefab.hpp"
#include "debugshape.hpp"   // DbgAllocObject, and the allocator it proved
#include "packets.hpp"      // PacketSendOutbound - the one thing here that sends
#include "worldread.hpp"    // the watcher wants to know when a block changed
#include "position.hpp"     // where you are, to rule out coincidences

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

// =============================================================================
// Reading a candidate object
// =============================================================================
namespace {

// How far into a packet to look. Placement packets are small; anything past a
// couple of hundred bytes is a different kind of object entirely.
constexpr uint32_t kScanBytes = 256;
constexpr uint32_t kPtrScanBytes = 96;
constexpr uint32_t kMaxPtrFields = 24;

// How long after the first sighting a placement is considered finished. One
// click produces several packets over a few frames, and they all have to be
// weighed together or the first one to arrive wins on its own.
constexpr long long kRoundMs = 1200;

bool ReadBlock(uintptr_t obj, uint8_t* out, uint32_t want, uint32_t& got) {
    // Objects are of unknown length, so step down until a read succeeds rather
    // than assuming the whole window is mapped. Same approach the packet
    // logger uses, and for the same reason.
    for (uint32_t len = want; len >= 16; len >>= 1) {
        if (SafeRead((const void*)obj, out, (size_t)len)) { got = len; return true; }
    }
    got = 0;
    return false;
}

bool ValueMatches(const uint8_t* buf, uint32_t at, PlaceCoordKind kind, int want) {
    switch (kind) {
    case PlaceCoordKind::Int32: {
        int32_t v; memcpy(&v, buf + at, 4); return v == want;
    }
    case PlaceCoordKind::Int16: {
        int16_t v; memcpy(&v, buf + at, 2); return (int)v == want;
    }
    case PlaceCoordKind::Float: {
        float v; memcpy(&v, buf + at, 4);
        return std::isfinite(v) && std::fabs(v - (float)want) < 0.01f;
    }
    case PlaceCoordKind::FloatMid: {
        float v; memcpy(&v, buf + at, 4);
        return std::isfinite(v) && std::fabs(v - ((float)want + 0.5f)) < 0.01f;
    }
    case PlaceCoordKind::Double: {
        double v; memcpy(&v, buf + at, 8);
        return std::isfinite(v) && std::fabs(v - (double)want) < 0.01;
    }
    }
    return false;
}

uint32_t KindWidth(PlaceCoordKind k) {
    switch (k) {
    case PlaceCoordKind::Int16:  return 2;
    case PlaceCoordKind::Double: return 8;
    default:                     return 4;
    }
}

const PlaceCoordKind kKinds[] = {
    PlaceCoordKind::Int32, PlaceCoordKind::Int16,
    PlaceCoordKind::Float, PlaceCoordKind::FloatMid, PlaceCoordKind::Double
};
const int32_t kStrides[] = { 2, 4, 8, 12, 16 };

// Is this value anywhere in the buffer at all, in any encoding? One cheap pass
// that rejects nearly everything. It matters: this runs on the network thread
// for every packet the client sends, and the client sends a movement packet
// several times a second whether or not anyone is building.
bool ContainsValue(const uint8_t* buf, uint32_t got, int want) {
    for (uint32_t at = 8; at + 2 <= got; at += 2) {
        int16_t s; memcpy(&s, buf + at, 2);
        if ((int)s == want) return true;
        if (at + 4 <= got) {
            int32_t i; memcpy(&i, buf + at, 4);
            if (i == want) return true;
            float f; memcpy(&f, buf + at, 4);
            if (std::isfinite(f) && (std::fabs(f - (float)want) < 0.01f ||
                std::fabs(f - ((float)want + 0.5f)) < 0.01f)) return true;
        }
        if (at + 8 <= got) {
            double d; memcpy(&d, buf + at, 8);
            if (std::isfinite(d) && std::fabs(d - (double)want) < 0.01) return true;
        }
    }
    return false;
}

// One possible answer: "the coordinates are three <kind> starting at <xOffset>,
// <stride> apart, in this object".
struct Candidate {
    bool viaPointer = false;
    uint32_t ptrOffset = 0;
    uint32_t xOffset = 0;
    int32_t stride = 4;
    PlaceCoordKind kind = PlaceCoordKind::Int32;

    bool operator==(const Candidate& o) const {
        return viaPointer == o.viaPointer && ptrOffset == o.ptrOffset &&
            xOffset == o.xOffset && stride == o.stride && kind == o.kind;
    }
};

// Every way this buffer could be holding (x, y, z). Strides wider than the
// value are worth trying because a position is often three fields of a struct
// with something else interleaved. If the three are not evenly spaced at all,
// nothing survives and the report says so rather than inventing an answer.
void CollectFrom(const uint8_t* buf, uint32_t got, bool viaPointer, uint32_t ptrOffset,
    int wx, int wy, int wz, std::vector<Candidate>& out) {

    if (!ContainsValue(buf, got, wx)) return;

    for (PlaceCoordKind kind : kKinds) {
        const uint32_t w = KindWidth(kind);
        for (int32_t stride : kStrides) {
            if (stride < (int32_t)w) continue;
            for (uint32_t at = 8; at + w <= got; at += 2) {
                const uint32_t yAt = at + (uint32_t)stride;
                const uint32_t zAt = at + (uint32_t)stride * 2;
                if (zAt + w > got) break;
                if (!ValueMatches(buf, at, kind, wx)) continue;
                if (!ValueMatches(buf, yAt, kind, wy)) continue;
                if (!ValueMatches(buf, zAt, kind, wz)) continue;

                Candidate c;
                c.viaPointer = viaPointer;
                c.ptrOffset = ptrOffset;
                c.xOffset = at;
                c.stride = stride;
                c.kind = kind;
                out.push_back(c);
            }
        }
    }
}

// Reads the three coordinates back out, once the layout is settled.
bool ReadTriple(uintptr_t packet, const PlaceLayout& lay, int& x, int& y, int& z) {
    uintptr_t obj = packet;
    if (lay.viaPointer) {
        uint64_t p = 0;
        if (!SafeRead((const void*)(packet + lay.ptrOffset), &p, 8)) return false;
        if (!p || (p & 7) || !IsPlausiblePtr((const void*)p)) return false;
        obj = (uintptr_t)p;
    }

    const uint32_t w = KindWidth(lay.kind);
    const uint32_t need = lay.xOffset + (uint32_t)lay.stride * 2 + w;
    uint8_t buf[kScanBytes];
    if (need > sizeof(buf)) return false;

    uint32_t got = 0;
    if (!ReadBlock(obj, buf, sizeof(buf), got) || got < need) return false;

    auto get = [&](uint32_t at) -> int {
        switch (lay.kind) {
        case PlaceCoordKind::Int32: { int32_t v; memcpy(&v, buf + at, 4); return v; }
        case PlaceCoordKind::Int16: { int16_t v; memcpy(&v, buf + at, 2); return (int)v; }
        case PlaceCoordKind::Float:
        case PlaceCoordKind::FloatMid: { float v; memcpy(&v, buf + at, 4);
                                        return (int)std::floor(v); }
        case PlaceCoordKind::Double: { double v; memcpy(&v, buf + at, 8);
                                       return (int)std::floor(v); }
        }
        return 0;
    };

    x = get(lay.xOffset);
    y = get(lay.xOffset + (uint32_t)lay.stride);
    z = get(lay.xOffset + (uint32_t)lay.stride * 2);
    return true;
}

// Everything a packet could be saying, packet and one level of reference.
void ScanObject(uintptr_t packet, int wx, int wy, int wz, std::vector<Candidate>& out) {
    uint8_t buf[kScanBytes];
    uint32_t got = 0;
    if (!ReadBlock(packet, buf, sizeof(buf), got)) return;

    CollectFrom(buf, got, false, 0, wx, wy, wz, out);

    // The coordinates may live in an object the packet points at - a
    // BlockPosition, or whatever the interaction system wraps one in - so
    // every plausible reference field gets the same treatment, one level deep.
    uint32_t followed = 0;
    for (uint32_t off = 8; off + 8 <= got && followed < kMaxPtrFields; off += 8) {
        uint64_t p = 0;
        memcpy(&p, buf + off, 8);
        if (!p || (p & 7) || !IsPlausiblePtr((const void*)p)) continue;

        uint8_t inner[kPtrScanBytes];
        uint32_t innerGot = 0;
        if (!ReadBlock((uintptr_t)p, inner, sizeof(inner), innerGot)) continue;
        ++followed;
        CollectFrom(inner, innerGot, true, off, wx, wy, wz, out);
    }
}

} // namespace

const char* PlaceCoordKindName(PlaceCoordKind k) {
    switch (k) {
    case PlaceCoordKind::Int32:    return "int32";
    case PlaceCoordKind::Int16:    return "int16";
    case PlaceCoordKind::Float:    return "float";
    case PlaceCoordKind::FloatMid: return "float, block centre";
    case PlaceCoordKind::Double:   return "double";
    }
    return "?";
}

std::string PlaceLayout::ToString() const {
    if (!learned) return "not learned";
    char buf[256];
    if (viaPointer)
        snprintf(buf, sizeof(buf),
            "packet 0x%llX -> +0x%X, then three %s at +0x%X, %d apart",
            (unsigned long long)mt, ptrOffset, PlaceCoordKindName(kind), xOffset, stride);
    else
        snprintf(buf, sizeof(buf),
            "packet 0x%llX, three %s at +0x%X, %d apart",
            (unsigned long long)mt, PlaceCoordKindName(kind), xOffset, stride);
    return buf;
}

// =============================================================================
// State
// =============================================================================
std::mutex g_placeMtx;
PlaceLayout g_placeLayout;

std::atomic<bool> g_placeLearning{ false };
std::atomic<int>  g_placeSamples{ 0 };
std::atomic<int>  g_placeCandidates{ 0 };
std::atomic<bool> g_placeAutoAdvance{ true };
std::atomic<uint64_t> g_placeMatched{ 0 };
std::atomic<uint64_t> g_placeIgnored{ 0 };
std::atomic<uint64_t> g_placeSeen{ 0 };
std::atomic<uint64_t> g_placeWrong{ 0 };

namespace {

std::mutex g_statusMtx;
std::string g_statusText = "not learned yet";
std::string g_reportText;
std::string g_lastSeenText;

void SetStatus(const std::string& s) {
    std::lock_guard lock(g_statusMtx);
    g_statusText = s;
}

// A placement is a round, not a packet: one click puts several objects on the
// wire over a few frames, and they have to be weighed together or whichever
// arrives first decides on its own.
std::mutex g_learnMtx;
struct Round {
    bool open = false;
    long long startMs = 0;
    int tx = 0, ty = 0, tz = 0;
    std::unordered_map<uint64_t, std::vector<Candidate>> hits;
};
Round g_round;
std::unordered_map<uint64_t, std::vector<Candidate>> g_kept;   // across rounds
bool g_haveKept = false;

// A hint from disk: the offsets from a previous session. The MethodTable is
// useless across launches, but the offsets are a property of the type, so a
// single placement can confirm them instead of needing two to find them.
bool g_haveHint = false;
Candidate g_hint;
uint32_t g_hintSize = 0;      // the packet's size last time, 0 when not recorded

// The distance between the packet's block id and the name table's. Voted on,
// seeded from disk, and only trusted once most placements agree on it.
std::atomic<int32_t> g_idDelta{ 0 };
std::atomic<bool> g_idDeltaKnown{ false };
std::mutex g_deltaMtx;
std::unordered_map<int32_t, int> g_deltaVotes;
int g_deltaVoteTotal = 0;

std::wstring LayoutFilePath() {
    return GetThisModuleDirectory() + L"placeblock.txt";
}

void ResetLearnLocked() {
    g_round = Round{};
    g_kept.clear();
    g_haveKept = false;
    g_placeSamples.store(0);
    g_placeCandidates.store(0);
}

size_t TotalCandidates(const std::unordered_map<uint64_t, std::vector<Candidate>>& m) {
    size_t n = 0;
    for (const auto& kv : m) n += kv.second.size();
    return n;
}

void Accept(uint64_t mt, const Candidate& c, int samples,
    const std::unordered_map<uint64_t, std::vector<Candidate>>& all) {
    PlaceLayout lay;
    lay.learned = true;
    lay.mt = mt;
    lay.viaPointer = c.viaPointer;
    lay.ptrOffset = c.ptrOffset;
    lay.xOffset = c.xOffset;
    lay.stride = c.stride;
    lay.kind = c.kind;
    // The size of the object is a property of the type, like the offsets, so it
    // survives a relaunch and makes the next confirmation much harder to fool.
    uint32_t baseSize = 0;
    if (SafeRead((const void*)(uintptr_t)(mt + 4), &baseSize, 4) && baseSize >= 16 &&
        baseSize <= 0x10000)
        lay.size = baseSize;

    std::string rep = "learned from " + std::to_string(samples) + " placement(s)\n";
    rep += lay.ToString();
    rep += "\n";

    const size_t total = TotalCandidates(all);
    if (total > 1) {
        rep += "\n";
        rep += std::to_string(total - 1);
        rep += " other reading(s) also fit. They all take the coordinates out of\n"
               "the same packet, so any of them works - but if the guide starts\n"
               "advancing on the wrong blocks, one of these is why:\n";
        for (const auto& kv : all) {
            for (const Candidate& o : kv.second) {
                if (kv.first == mt && o == c) continue;
                char line[200];
                snprintf(line, sizeof(line), "  packet 0x%llX  %s+0x%X, %d apart, %s\n",
                    (unsigned long long)kv.first, o.viaPointer ? "via a pointer, " : "",
                    o.xOffset, o.stride, PlaceCoordKindName(o.kind));
                rep += line;
            }
        }
    }

    {
        std::lock_guard lock(g_placeMtx);
        g_placeLayout = lay;
    }
    {
        std::lock_guard lock(g_statusMtx);
        g_reportText = rep;
    }
    g_placeLearning.store(false);
    SetStatus("learned - the guide follows along by itself now");
    PlaceSaveToDisk();
}

// Call with g_learnMtx held. Weighs the round that just finished.
void CloseRoundLocked() {
    if (!g_round.open) return;

    const int placedX = g_round.tx, placedY = g_round.ty, placedZ = g_round.tz;
    auto hits = std::move(g_round.hits);
    g_round = Round{};

    if (hits.empty()) return;

    if (!g_haveKept) {
        g_kept = std::move(hits);
        g_haveKept = true;
    }
    else {
        // Only layouts that fitted this time as well survive, and only from a
        // packet type that turned up both times. One coordinate can appear
        // anywhere by chance; the same reading being right twice, at two
        // unrelated coordinates, cannot.
        std::unordered_map<uint64_t, std::vector<Candidate>> next;
        for (auto& kv : g_kept) {
            auto it = hits.find(kv.first);
            if (it == hits.end()) continue;
            std::vector<Candidate> keep;
            for (const Candidate& c : kv.second)
                if (std::find(it->second.begin(), it->second.end(), c) != it->second.end())
                    keep.push_back(c);
            if (!keep.empty()) next.emplace(kv.first, std::move(keep));
        }
        g_kept = std::move(next);
    }

    const int samples = g_placeSamples.fetch_add(1) + 1;
    g_placeCandidates.store((int)TotalCandidates(g_kept));

    // Move the guide on, so the next placement is at a different coordinate.
    // That is the whole basis of the test - two samples at the same place
    // prove nothing.
    BuildMarkPlacedAt(placedX, placedY, placedZ);

    if (g_kept.empty()) {
        ResetLearnLocked();
        g_placeLearning.store(false);
        SetStatus("nothing survived the second placement, so the first match was a "
            "coincidence. Arm it again and place two blocks well apart.");
        return;
    }

    // A hint from a previous session only needs confirming once.
    if (g_haveHint && samples >= 1) {
        for (const auto& kv : g_kept)
            for (const Candidate& c : kv.second)
                if (c == g_hint) { Accept(kv.first, c, samples, g_kept); return; }
    }

    const size_t total = TotalCandidates(g_kept);
    if (samples >= 2 && (total == 1 || samples >= 3)) {
        const auto& kv = *g_kept.begin();
        Accept(kv.first, kv.second.front(), samples, g_kept);
        return;
    }

    char buf[220];
    snprintf(buf, sizeof(buf),
        "%d placement(s) seen, %zu reading(s) still fit - place the highlighted "
        "block again, somewhere well away from the last one.", samples, total);
    SetStatus(buf);
}

} // namespace

std::string PlaceGetStatus() {
    std::lock_guard lock(g_statusMtx);
    return g_statusText;
}

std::string PlaceGetReport() {
    std::lock_guard lock(g_statusMtx);
    return g_reportText;
}

void PlaceLearnCancel() {
    g_placeLearning.store(false);
    std::lock_guard lock(g_learnMtx);
    ResetLearnLocked();
    SetStatus("learning cancelled");
}

void PlaceLearnArm() {
    {
        std::lock_guard lock(g_learnMtx);
        ResetLearnLocked();
    }
    g_placeLearning.store(true);
    bool hint = false;
    { std::lock_guard lock(g_learnMtx); hint = g_haveHint; }
    SetStatus(hint ? "place any block of the build to confirm it for this session"
                   : "place the highlighted block now");
}

void PlaceForget() {
    {
        std::lock_guard lock(g_placeMtx);
        g_placeLayout = PlaceLayout{};
    }
    {
        std::lock_guard lock(g_learnMtx);
        g_haveHint = false;
    }
    PlaceLearnCancel();
    SetStatus("forgotten");
}

bool PlaceIsWatching() {
    if (!g_placeAutoAdvance.load()) return false;
    std::lock_guard lock(g_placeMtx);
    return g_placeLayout.learned;
}

bool PlaceWantsOutbound() {
    if (g_placeLearning.load() || g_placeSnapshot.load()) return true;
    // A burst needs the hook running even with following-along turned off,
    // since the hook is both its clock and its way onto the right thread.
    if (!g_placeAutoAdvance.load() && g_placeBurstLeft.load() <= 0) return false;
    std::lock_guard lock(g_placeMtx);
    return g_placeLayout.learned;
}

// =============================================================================
// Placing them for you
// =============================================================================
std::atomic<int>   g_placeBurstLeft{ 0 };
std::atomic<int>   g_placeGapMs{ 220 };
std::atomic<float> g_placeMaxReach{ 6.0f };
std::atomic<uint64_t> g_placeSent{ 0 };
std::atomic<uint64_t> g_placeRefused{ 0 };
std::atomic<bool> g_placeProven{ false };

// The enqueue's first argument, taken from a real placement. This enqueue is
// generic - the packet logger sees non-packet items go through it too - so
// there is no single "the connection", and using the most recent one meant
// sending a placement down whatever channel the last movement packet used.
std::atomic<uint64_t> g_placeChannel{ 0 };

namespace {

std::mutex g_tmplMtx;
PlaceTemplate g_tmplInfo;
uint8_t g_tmplBytes[kScanBytes] = {};
uint32_t g_tmplGot = 0;

std::mutex g_buildStatusMtx;
std::string g_buildStatusText;

void SetBuildStatus(const std::string& s) {
    std::lock_guard lock(g_buildStatusMtx);
    g_buildStatusText = s;
}

// Keeps whatever the client just sent, so it can be sent again elsewhere.
void CaptureTemplate(uintptr_t packet, uint64_t mt, int x, int y, int z,
    const std::string& material) {
    uint8_t buf[kScanBytes];
    uint32_t got = 0;
    if (!ReadBlock(packet, buf, sizeof(buf), got)) return;

    // The object's real length, from its type. Copying a fixed window instead
    // is how the shape spawner first corrupted the heap: anything shorter than
    // the window had its clone run into the next object's header.
    uint32_t baseSize = 0;
    if (!SafeRead((const void*)((uintptr_t)mt + 4), &baseSize, 4)) return;
    if (baseSize < 16 || baseSize > kScanBytes || baseSize > got) return;

    // Decided here, while the packet is alive, rather than at send time when
    // everything in it is ambiguous. A field only counts as a reference if it
    // actually resolves to something carrying a MethodTable - which the qword
    // spanning z and the block id does not, so a packet of pure integers comes
    // out clean instead of being "repaired".
    bool hasRefs = false;
    for (uint32_t off = 8; off + 8 <= baseSize; off += 8) {
        uint64_t v = 0;
        memcpy(&v, buf + off, 8);
        if (!v || (v & 7)) continue;
        if (!IsPlausiblePtr((const void*)v)) continue;
        uint64_t probeMT = 0;
        if (!SafeRead((const void*)v, &probeMT, 8)) continue;
        if (!probeMT || (probeMT & 7) || !IsPlausiblePtr((const void*)probeMT)) continue;
        hasRefs = true;
        break;
    }

    // Display only, and deliberately not relied on: the replay copies the whole
    // packet, so it does not need to know where the block id lives. Showing it
    // is just how you tell at a glance that it is not zero.
    uint32_t blockId = 0;
    if (baseSize >= 0x18) memcpy(&blockId, buf + 0x14, 4);

    std::lock_guard lock(g_tmplMtx);
    g_tmplInfo.hasRefs = hasRefs;
    g_tmplInfo.blockId = blockId;
    memcpy(g_tmplBytes, buf, got);
    g_tmplGot = got;
    g_tmplInfo.have = true;
    g_tmplInfo.mt = mt;
    g_tmplInfo.size = baseSize;
    g_tmplInfo.material = material;
    g_tmplInfo.wx = x; g_tmplInfo.wy = y; g_tmplInfo.wz = z;
}

// What a send attempt came to. Deferred: nothing was allocated or sent, try the
// same block again on a later call.
enum PlaceSendResult : int { kPlaceFailed = 0, kPlaceSent = 1, kPlaceDeferred = 2 };

// Builds one placement at (x, y, z) and hands it to the client's own queue.
// Called from inside the ring-enqueue hook and nowhere else: that is the thread
// the client itself builds and queues packets on, which is what makes both the
// allocation and the enqueue safe. Allocating from a thread WE made is what
// took the client down when the write barrier was first called from the
// overlay, and the difference is exactly this.
int SendPlacementAt(const PlaceLayout& lay, int x, int y, int z) {
    // Every step is written to the log before it is taken. A crash here takes
    // the client with it, so the only evidence that survives is whatever was
    // already flushed to disk - and "it crashed" on its own says nothing about
    // whether the allocation or the enqueue was the thing that did it.
    uint64_t mt = 0;
    uint32_t size = 0;
    uint8_t tmp[kScanBytes];
    {
        std::lock_guard lock(g_tmplMtx);
        if (!g_tmplInfo.have || g_tmplInfo.mt != lay.mt) return kPlaceFailed;
        mt = g_tmplInfo.mt;
        size = g_tmplInfo.size;
        if (size > g_tmplGot) size = g_tmplGot;
        memcpy(tmp, g_tmplBytes, size);
    }

    // Only the shape the learner actually proved. A position reached through a
    // pointer would mean allocating that object too, and nothing has told us
    // what it is.
    const uint32_t need = lay.xOffset + (uint32_t)lay.stride * 2 + 4;
    if (lay.viaPointer || need > size) return kPlaceFailed;

    // Only when the allocation cannot start a collection - one that would walk
    // this thread's stack from inside our code and fail-fast the game. The
    // client refills the context with its own allocations, so the next packet
    // it sends will usually find room.
    if (!DbgAllocFitsNow((void*)(uintptr_t)mt)) return kPlaceDeferred;

    LogToFile("placesend: begin");
    LogToFile("placesend: allocating type 0x" + std::to_string(mt) +
        ", " + std::to_string(size) + " bytes");
    void* obj = DbgAllocObject((void*)(uintptr_t)mt);
    LogToFile("placesend: allocation returned");
    if (!obj) return kPlaceFailed;

    // If that was not really the allocator, what came back will not carry our
    // type. Check before writing a byte, exactly as the shape spawner does.
    uint64_t gotMT = 0;
    if (!SafeRead(obj, &gotMT, 8) || gotMT != mt) return kPlaceFailed;

    // Copied byte for byte. There used to be a pass here that nulled anything
    // looking like a dead reference, and it was actively destructive: this
    // packet is forty bytes of integers, and the qword spanning z and the block
    // id read as an 8-aligned address in plausible range that did not resolve.
    // It zeroed both, the position was patched back afterwards, and the block
    // id was not - so every request asked the server to place block type 0,
    // which is exactly what it placed.
    //
    // Whether a template holds real references is now decided once, when it is
    // captured, by checking whether a field actually resolves to an object. A
    // template that does is refused rather than repaired, because repairing a
    // field we cannot identify is how this went wrong in the first place.

    // Only the position changes. Rotation, block id and everything we never
    // identified travel exactly as the client wrote them.
    const int32_t cx = x, cy = y, cz = z;
    memcpy(tmp + lay.xOffset, &cx, 4);
    memcpy(tmp + lay.xOffset + lay.stride, &cy, 4);
    memcpy(tmp + lay.xOffset + lay.stride * 2, &cz, 4);

    // Never over the object header.
    memcpy((uint8_t*)obj + 8, tmp + 8, size - 8);

    // What is actually going out. The previous version of this logged the
    // working buffer twice and called one of them the client's, which made two
    // identical lines look like agreement - worse than no comparison at all.
    // The client's own packet is in the snapshot log instead, unmodified.
    {
        static const char* kHex = "0123456789ABCDEF";
        std::string mine;
        const uint8_t* got = (const uint8_t*)obj;
        for (uint32_t i = 0; i < size; ++i) {
            mine += kHex[got[i] >> 4]; mine += kHex[got[i] & 15]; mine += ' ';
        }
        LogToFile("placesend: sending " + mine);
    }

    const uint64_t channel = g_placeChannel.load();
    LogToFile("placesend: enqueueing on channel 0x" + std::to_string(channel));
    const bool ok = PacketSendOutbound(obj, channel);
    LogToFile(ok ? "placesend: enqueue returned" : "placesend: enqueue refused");
    return ok ? kPlaceSent : kPlaceFailed;
}

// __try cannot share a frame with C++ unwinding, so the guard gets its own.
int SendPlacementGuarded(const PlaceLayout& lay, int x, int y, int z) {
    __try { return SendPlacementAt(lay, x, y, z); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kPlaceFailed; }
}

// One step of a burst. Driven from the hook because the client sends something
// several times a second by itself, which is a steadier clock than one of ours
// and guarantees we are on the thread the ring buffer expects.
void DriveBurst(const PlaceLayout& lay) {
    if (g_placeBurstLeft.load() <= 0) return;

    static long long lastMs = 0;
    const long long now = NowSteadyMs();
    int gap = g_placeGapMs.load();
    if (gap < 80) gap = 80;
    if (now - lastMs < gap) return;

    std::string material;
    {
        std::lock_guard lock(g_tmplMtx);
        if (g_tmplInfo.have) material = g_tmplInfo.material;
    }
    if (material.empty()) {
        g_placeBurstLeft.store(0);
        SetBuildStatus("nothing to copy yet - place one where the prefab wants it");
        return;
    }

    const BuildTarget t = BuildGetTarget();
    if (!t.valid) {
        g_placeBurstLeft.store(0);
        SetBuildStatus("nothing left to place");
        return;
    }

    // It can only ever place the block it last watched you place. When the
    // prefab wants something else that is the end of the run, and the only way
    // to continue is to hold the new block and place one yourself - which is
    // also the only way to be sure you have any.
    if (t.name != material) {
        g_placeBurstLeft.store(0);
        SetBuildStatus("next is " + t.name + " but the copy places " + material +
            " - hold the right block and place one by hand");
        return;
    }

    if (t.distance > (double)g_placeMaxReach.load()) {
        g_placeRefused.fetch_add(1);
        SetBuildStatus("waiting - the next block is out of reach, walk closer");
        return;
    }

    const int sent = SendPlacementGuarded(lay, t.wx, t.wy, t.wz);
    if (sent == kPlaceDeferred) return;   // no room to allocate yet; next packet retries
    lastMs = now;
    if (sent != kPlaceSent) {
        g_placeBurstLeft.store(0);
        SetBuildStatus("the send failed - stopped");
        return;
    }

    g_placeSent.fetch_add(1);
    g_placeBurstLeft.fetch_sub(1);
    // It went out and we are still here, which is the only thing that has ever
    // distinguished a working call from one that kills the client.
    g_placeProven.store(true);

    // Marked on the way out, because nothing ever comes back to confirm it.
    // That is the whole reason this is a bounded burst: if the server is
    // refusing them, the guide is wrong by however many you asked for and no
    // more - and Undo walks it back.
    BuildMarkPlacedAt(t.wx, t.wy, t.wz);

    if (g_placeBurstLeft.load() <= 0) SetBuildStatus("burst finished");
}

} // namespace

PlaceTemplate PlaceGetTemplate() {
    std::lock_guard lock(g_tmplMtx);
    return g_tmplInfo;
}

std::string PlaceBuildStatus() {
    std::lock_guard lock(g_buildStatusMtx);
    return g_buildStatusText;
}

// Drained after the client's own enqueue has completed, not before it. Sending
// while the client is part-way through its own call is one more unknown on a
// path that already has several, and moving it costs nothing.
void PlaceDrainSends() {
    if (g_placeBurstLeft.load() <= 0) return;
    PlaceLayout lay;
    { std::lock_guard lock(g_placeMtx); lay = g_placeLayout; }
    if (!lay.learned) return;
    DriveBurst(lay);
}

void PlaceStartBurst(int count) {
    if (count < 1) count = 1;
    if (count > 512) count = 512;
    // Until one has gone out and the client has survived it, a burst is one.
    // The first attempt took the game down; there is no reason to find that out
    // sixteen times.
    if (!g_placeProven.load()) count = 1;
    g_placeBurstLeft.store(count);
    SetBuildStatus(g_placeProven.load() ? "placing..." : "trying one...");
}

void PlaceStopBurst() {
    g_placeBurstLeft.store(0);
    SetBuildStatus("stopped");
}

bool PlaceCanBuild(std::string& whyNot) {
    {
        std::lock_guard lock(g_placeMtx);
        if (!g_placeLayout.learned) {
            whyNot = "the placement packet has not been learned yet";
            return false;
        }
        if (g_placeLayout.viaPointer) {
            whyNot = "the coordinates live in a separate object, which this "
                     "cannot rebuild - only the simple layout is supported";
            return false;
        }
    }
    if (!g_placeChannel.load()) {
        whyNot = "the channel placements travel on is not known yet - place one "
                 "block by hand so it can be read off a real one";
        return false;
    }
    if (!g_dbgAllocVerified.load()) {
        whyNot = "the object allocator has not been verified - open Shapes";
        return false;
    }
    PlaceTemplate t = PlaceGetTemplate();
    if (!t.have || t.material.empty()) {
        whyNot = "place one block where the prefab wants it first - that is "
                 "what gets copied";
        return false;
    }
    if (t.hasRefs) {
        whyNot = "that placement carries a reference to another object, which "
                 "cannot be copied safely - only a packet of plain values can";
        return false;
    }
    return true;
}

// =============================================================================
// The hook's entry point
// =============================================================================
// Runs on the network thread, for every object the client enqueues. Everything
// here is written on the assumption that it is called several times a second
// forever, whether or not anyone is building.
std::atomic<bool> g_placeSnapshot{ false };

namespace {

std::atomic<long long> g_snapUntilMs{ 0 };
std::atomic<int> g_snapCount{ 0 };
std::mutex g_snapStatusMtx;
std::string g_snapStatusText;

// Writes one object out whole. The point is to be able to read back the entire
// set of packets a single placement produced, so nothing is filtered here -
// filtering is how we ended up copying the wrong one.
void SnapshotOne(uintptr_t packetObj, uint64_t mt, uint64_t channel) {
    uint8_t buf[kScanBytes];
    uint32_t got = 0;
    if (!ReadBlock(packetObj, buf, sizeof(buf), got)) return;

    uint32_t baseSize = 0;
    if (!SafeRead((const void*)((uintptr_t)mt + 4), &baseSize, 4)) baseSize = 0;
    uint32_t show = (baseSize >= 16 && baseSize <= got) ? baseSize : got;
    if (show > 128) show = 128;

    static const char* kHex = "0123456789ABCDEF";
    std::string hex;
    for (uint32_t i = 0; i < show; ++i) {
        hex += kHex[buf[i] >> 4];
        hex += kHex[buf[i] & 15];
        hex += ' ';
    }

    char head[160];
    snprintf(head, sizeof(head), "placesnap: mt=0x%llX size=%u chan=0x%llX  ",
        (unsigned long long)mt, baseSize, (unsigned long long)channel);
    LogToFile(std::string(head) + hex);
    g_snapCount.fetch_add(1);
}

} // namespace

void PlaceSnapshotArm(int milliseconds) {
    if (milliseconds < 500) milliseconds = 500;
    if (milliseconds > 15000) milliseconds = 15000;
    g_snapCount.store(0);
    g_snapUntilMs.store(NowSteadyMs() + milliseconds);
    g_placeSnapshot.store(true);
    LogToFile("placesnap: ---- recording for " + std::to_string(milliseconds) +
        " ms, place ONE block now ----");
    std::lock_guard lock(g_snapStatusMtx);
    g_snapStatusText = "recording - place one block now";
}

std::string PlaceSnapshotStatus() {
    std::lock_guard lock(g_snapStatusMtx);
    return g_snapStatusText;
}

namespace {

// With offsets remembered from an earlier launch, the only thing left to learn
// is which packet type carries them this time - and ANY block placed on the
// build answers that, not just the white one. Before this, placements after a
// restart were silently dropped until you happened to place the one block the
// learner was watching for, which is not how anyone resumes a build.
//
// Three things must agree, and together they cannot by chance: the packet is
// the size it was last time, the remembered offsets read as a cell the build
// still wants, and that cell is within reach of where you are standing.
bool TryConfirmHint(uintptr_t packetObj, uint64_t mt) {
    Candidate hint;
    uint32_t hintSize = 0;
    {
        std::lock_guard lock(g_learnMtx);
        if (!g_haveHint) return false;
        hint = g_hint;
        hintSize = g_hintSize;
    }
    if (hintSize) {
        uint32_t baseSize = 0;
        if (!SafeRead((const void*)(uintptr_t)(mt + 4), &baseSize, 4) || baseSize != hintSize)
            return false;
    }

    PlaceLayout probe;
    probe.learned = true;
    probe.mt = mt;
    probe.viaPointer = hint.viaPointer;
    probe.ptrOffset = hint.ptrOffset;
    probe.xOffset = hint.xOffset;
    probe.stride = hint.stride;
    probe.kind = hint.kind;
    int x = 0, y = 0, z = 0;
    if (!ReadTriple(packetObj, probe, x, y, z)) return false;
    if (!BuildHasTodoAt(x, y, z)) return false;

    if (NowSteadyMs() - g_posLastUpdateMs.load() < 3000) {
        const double dx = x + 0.5 - g_posX.load(), dy = y + 0.5 - g_posY.load(),
                     dz = z + 0.5 - g_posZ.load();
        if (dx * dx + dy * dy + dz * dz > 14.0 * 14.0) return false;
    }

    std::lock_guard lock(g_learnMtx);
    if (!g_placeLearning.load()) return false;      // settled by something else
    const std::unordered_map<uint64_t, std::vector<Candidate>> none;
    Accept(mt, hint, 1, none);
    ResetLearnLocked();
    return true;
}

void VoteIdDelta(int32_t delta) {
    bool changed = false;
    {
        std::lock_guard lock(g_deltaMtx);
        ++g_deltaVotes[delta];
        ++g_deltaVoteTotal;
        int32_t best = 0;
        int bestVotes = 0;
        for (const auto& kv : g_deltaVotes)
            if (kv.second > bestVotes) { bestVotes = kv.second; best = kv.first; }
        // Three placements, and three quarters of all of them. One wrong block
        // among a dozen right ones changes nothing.
        if (bestVotes >= 3 && bestVotes * 4 >= g_deltaVoteTotal * 3 &&
            (!g_idDeltaKnown.load() || g_idDelta.load() != best)) {
            g_idDelta.store(best);
            g_idDeltaKnown.store(true);
            changed = true;
        }
        // Old votes fade, so the table stays small and can follow a change.
        if (g_deltaVoteTotal > 400) {
            g_deltaVoteTotal = 0;
            for (auto it = g_deltaVotes.begin(); it != g_deltaVotes.end(); ) {
                it->second /= 2;
                if (!it->second) it = g_deltaVotes.erase(it);
                else { g_deltaVoteTotal += it->second; ++it; }
            }
        }
    }
    if (changed) PlaceSaveToDisk();
}

} // namespace

bool PlaceIdDeltaKnown() { return g_idDeltaKnown.load(); }
int32_t PlaceIdDelta() { return g_idDelta.load(); }

void PlaceOnOutbound(uintptr_t packetObj, uint64_t mt, uint64_t channel) {
    if (!packetObj || !mt) return;

    if (g_placeSnapshot.load()) {
        if (NowSteadyMs() > g_snapUntilMs.load()) {
            g_placeSnapshot.store(false);
            const int n = g_snapCount.load();
            LogToFile("placesnap: ---- done, " + std::to_string(n) + " packets ----");
            std::lock_guard lock(g_snapStatusMtx);
            g_snapStatusText = "recorded " + std::to_string(n) +
                " packets to C:\\temp\\hyai_log.txt";
        }
        else {
            SnapshotOne(packetObj, mt, channel);
        }
    }

    if (g_placeLearning.load() && !TryConfirmHint(packetObj, mt)) {
        // What the guide is asking for is the one coordinate we can be certain
        // about. Cached, because working it out walks the prefab and this runs
        // far more often than the answer changes.
        static BuildTarget cached;
        static long long cachedAtMs = 0;
        const long long now = NowSteadyMs();
        if (now - cachedAtMs > 250) {
            cached = BuildGetTarget();
            cachedAtMs = now;
        }
        if (!cached.valid) return;

        std::vector<Candidate> found;
        ScanObject(packetObj, cached.wx, cached.wy, cached.wz, found);

        std::lock_guard lock(g_learnMtx);

        if (g_round.open &&
            (g_round.tx != cached.wx || g_round.ty != cached.wy || g_round.tz != cached.wz))
            CloseRoundLocked();

        if (!found.empty()) {
            if (!g_round.open) {
                g_round.open = true;
                g_round.startMs = now;
                g_round.tx = cached.wx; g_round.ty = cached.wy; g_round.tz = cached.wz;
            }
            auto& slot = g_round.hits[mt];
            for (const Candidate& c : found)
                if (std::find(slot.begin(), slot.end(), c) == slot.end())
                    slot.push_back(c);
        }

        if (g_round.open && now - g_round.startMs > kRoundMs) CloseRoundLocked();
        return;
    }

    PlaceLayout lay;
    { std::lock_guard lock(g_placeMtx); lay = g_placeLayout; }
    if (!lay.learned) return;
    if (lay.mt != mt) return;

    // Counted before anything can go wrong with it, because "did the hook even
    // see a placement" and "did the placement match the prefab" are different
    // questions with the same symptom, and matched/ignored answers neither when
    // both are zero.
    g_placeSeen.fetch_add(1);

    int x = 0, y = 0, z = 0;
    if (!ReadTriple(packetObj, lay, x, y, z)) return;

    {
        // Worth having when it is not working: the coordinates we read, next to
        // the ones the guide wanted. If those differ by a constant, the anchor
        // is wrong; if they are nonsense, the wrong field was learned.
        char buf[160];
        snprintf(buf, sizeof(buf), "last seen placement: %d, %d, %d", x, y, z);
        std::lock_guard lock(g_statusMtx);
        g_lastSeenText = buf;
    }

    // The channel this kind of packet actually travels on. Recorded here, from
    // a real placement, rather than taken from whatever the client enqueued
    // most recently - which is nearly always a movement packet, and is what we
    // were wrongly sending down.
    {
        const uint64_t prev = g_placeChannel.exchange(channel);
        if (prev && prev != channel)
            LogToFile("placesend: placement channel changed 0x" +
                std::to_string(prev) + " -> 0x" + std::to_string(channel));
    }

    // Kept whether or not it matched the prefab, but only labelled when it did.
    // The label is what stops a burst placing the wrong block, so a guess at it
    // would be worse than not having one at all.
    const std::string want = BuildWantsAt(x, y, z);
    CaptureTemplate(packetObj, mt, x, y, z, want);

    // The block id sits in the field straight after z - derived from the
    // learned position rather than written down, so it moves with it. Calling
    // it the block id is still a hypothesis, which is why the table complains
    // if one name ever reads two different values instead of quietly picking.
    uint32_t placedId = 0;
    {
        const uint32_t idOff = lay.xOffset + (uint32_t)lay.stride * 3;
        uintptr_t obj = packetObj;
        if (lay.viaPointer) {
            uint64_t p = 0;
            if (SafeRead((const void*)(packetObj + lay.ptrOffset), &p, 8) && p &&
                !(p & 7) && IsPlausiblePtr((const void*)p))
                obj = (uintptr_t)p;
            else
                obj = 0;
        }
        if (obj) SafeRead((const void*)(obj + idOff), &placedId, 4);
    }

    // Two uses, and they need each other: building from a prefab teaches the
    // table, and the table is what lets a recorded build be written back with
    // real names in it.
    std::string placedName;
    if (placedId) {
        // The number in this packet is NOT the number the world stores - it
        // runs a fixed distance off (ten, on this build), so feeding it
        // straight into the name table is how Unknown_1052 got in there.
        //
        // The distance is voted on: every placement where the build asked for
        // a known block casts one. It used to be taken from whichever placement
        // came last, on the assumption you had placed what was asked for - so
        // one wrong block taught the wrong distance, and the wrong distance is
        // exactly what the material check would then have trusted. State
        // variants ('*' names) carry their own ids and do not vote, and nothing
        // votes while "any block counts" is on - then what you place is
        // deliberately not what the prefab names.
        if (g_buildCheckMaterial.load() && !want.empty() && want[0] != '*') {
            const uint32_t wantId = BuildGetBlockId(want);
            if (wantId) VoteIdDelta((int32_t)placedId - (int32_t)wantId);
        }

        uint32_t trueId = 0;
        if (g_idDeltaKnown.load()) {
            const int64_t adj = (int64_t)placedId - (int64_t)g_idDelta.load();
            if (adj > 0 && adj < 100000) trueId = (uint32_t)adj;
            // What was actually placed, by name - which is what lets the guide
            // tell the right block from the wrong one.
            if (trueId) placedName = BuildGetBlockName(trueId);
        }
        else if (!want.empty()) {
            // Until the distance is settled, the best guess for recording is
            // the block that was asked for. Never used for the material check.
            trueId = BuildGetBlockId(want);
        }

        const uint32_t useId = trueId ? trueId : placedId;
        BuildRecordAdd(x, y, z, useId);
        // Always, not only while recording. A block you just placed is a known
        // value at a known place, which is the one thing every search here has
        // ever needed - and it costs no command and no permission.
        BuildAddProbe(x, y, z, useId);

        // And it is the moment the world changed, which is the only thing the
        // memory watcher needs.
        WorldWatchOnPlacement(x, y, z, placedId);
        WorldFitOnPlacement(x, y, z, useId);
    }

    if (!g_placeAutoAdvance.load()) return;
    switch (BuildOnPlacement(x, y, z, placedName, BuildSource::Follow)) {
    case BuildPlaceResult::Placed:
    case BuildPlaceResult::AlreadyDone: g_placeMatched.fetch_add(1); break;
    case BuildPlaceResult::WrongBlock:  g_placeWrong.fetch_add(1);   break;
    default:                            g_placeIgnored.fetch_add(1); break;
    }
}

std::string PlaceGetLastSeen() {
    std::lock_guard lock(g_statusMtx);
    return g_lastSeenText;
}

// =============================================================================
// Keeping it
// =============================================================================
// The MethodTable is an absolute address and moves every launch, so only the
// offsets go to disk - they are a property of the type, not of where it landed.
// Learning still has to happen once per session, but with the offsets known it
// takes one placement instead of two or three.
bool PlaceSaveToDisk() {
    PlaceLayout lay;
    { std::lock_guard lock(g_placeMtx); lay = g_placeLayout; }
    // Never written without a layout: this file IS the layout, and writing it
    // with only the id distance in would throw away the offsets it holds.
    if (!lay.learned) return false;

    char buf[640];
    const int n = snprintf(buf, sizeof(buf),
        "# how this client announces a block placement\n"
        "# the MethodTable is deliberately not here: it moves every launch\n"
        "viaPointer=%d\nptrOffset=%u\nxOffset=%u\nstride=%d\nkind=%d\nsize=%u\n",
        lay.viaPointer ? 1 : 0, lay.ptrOffset, lay.xOffset, lay.stride, (int)lay.kind,
        lay.size);
    if (g_idDeltaKnown.load() && n > 0 && n < (int)sizeof(buf))
        snprintf(buf + n, sizeof(buf) - (size_t)n,
            "# packet block id minus name-table block id, voted over placements\n"
            "idDelta=%d\n", g_idDelta.load());

    HANDLE h = CreateFileW(LayoutFilePath().c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const DWORD len = (DWORD)strlen(buf);
    const BOOL ok = WriteFile(h, buf, len, &wrote, nullptr);
    CloseHandle(h);
    return ok && wrote == len;
}

bool PlaceLoadFromDisk() {
    HANDLE h = CreateFileW(LayoutFilePath().c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    char buf[1024] = {};
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok || !got) return false;

    int viaPtr = 0, kind = 0, stride = 4, idDelta = 0;
    unsigned ptrOff = 0, xOff = 0, size = 0;
    bool haveDelta = false;

    const char* at = nullptr;
    if ((at = strstr(buf, "viaPointer=")) != nullptr) sscanf_s(at + 11, "%d", &viaPtr);
    if ((at = strstr(buf, "ptrOffset=")) != nullptr)  sscanf_s(at + 10, "%u", &ptrOff);
    if ((at = strstr(buf, "xOffset=")) != nullptr)    sscanf_s(at + 8, "%u", &xOff);
    if ((at = strstr(buf, "stride=")) != nullptr)     sscanf_s(at + 7, "%d", &stride);
    if ((at = strstr(buf, "kind=")) != nullptr)       sscanf_s(at + 5, "%d", &kind);
    if ((at = strstr(buf, "\nsize=")) != nullptr)     sscanf_s(at + 6, "%u", &size);
    if ((at = strstr(buf, "idDelta=")) != nullptr)
        haveDelta = sscanf_s(at + 8, "%d", &idDelta) == 1;

    if (xOff < 8 || stride <= 0 || stride > 64 || kind < 0 || kind > 4) return false;

    {
        std::lock_guard lock(g_learnMtx);
        g_hint.viaPointer = viaPtr != 0;
        g_hint.ptrOffset = ptrOff;
        g_hint.xOffset = xOff;
        g_hint.stride = stride;
        g_hint.kind = (PlaceCoordKind)kind;
        g_hintSize = (size >= 16 && size <= 0x10000) ? size : 0;
        g_haveHint = true;
    }
    if (haveDelta && idDelta > -100000 && idDelta < 100000) {
        // Trusted from the start - it is what last launch settled on - but it
        // carries only three votes, so a consistent run of placements saying
        // otherwise can still overturn it.
        g_idDelta.store(idDelta);
        g_idDeltaKnown.store(true);
        std::lock_guard lock(g_deltaMtx);
        g_deltaVotes[idDelta] += 3;
        g_deltaVoteTotal += 3;
    }

    SetStatus("offsets remembered from last time - place any block of the build and "
        "follow along picks up from there");
    return true;
}
