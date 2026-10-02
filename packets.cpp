#include "pch.h"
#include "packets.hpp"
#include "placewatch.hpp"
#include "hooks.hpp"
#include "worldread.hpp"    // SafeRead / IsPlausiblePtr / GameModulePatternScan / ReadSystemString
#include "common.hpp"   // LogToFile / NowSteadyMs / GetThisModuleDirectory
#include "chat.hpp"     // g_stringVT
#include "debugshape.hpp"   // DbgInspectPacket - learns from a real DisplayDebug
#include "hookthunk.hpp"
#include <cstdio>
#include <fstream>

std::atomic<bool> g_pktCaptureIn{ false };
std::atomic<bool> g_pktCaptureOut{ false };
std::atomic<bool> g_pktPaused{ false };
std::atomic<bool> g_pktLogToFile{ false };
std::atomic<int>  g_pktCaptureBytes{ 64 };
std::atomic<int>  g_pktMaxEntries{ 1000 };

std::atomic<bool> g_pktInHooked{ false };
std::atomic<bool> g_pktOutHooked{ false };
std::atomic<uint64_t> g_pktTotalIn{ 0 };
std::atomic<uint64_t> g_pktTotalOut{ 0 };
std::atomic<uint64_t> g_pktDropped{ 0 };
std::atomic<bool>     g_pktCanDispatch{ false };
std::atomic<uintptr_t> g_pktInRva{ 0 };
std::atomic<uintptr_t> g_pktOutRva{ 0 };
std::atomic<bool> g_pktInSigFound{ false };
std::atomic<bool> g_pktOutSigFound{ false };

namespace {

// Signatures from the Hytale-Signatures project (Turkeii); the inbound one is
// also what Hytale-Carbon's packet catcher uses.
const char* kSigProcessPacket =
"57 56 55 53 48 83 EC ? 33 C0 48 89 44 24 ? 48 89 44 24 ? 48 8B D9 48 8B F2 48 8B CE E8";
const char* kSigRingEnqueue =
"56 53 48 83 EC ? 4C 8B 41 ? 8B 99";

// The `this` the client calls its inbound handler with. Captured from the
// hook rather than looked up, because we have no way to resolve it otherwise.
std::atomic<uintptr_t> g_pktInHandlerSelf{ 0 };

std::mutex g_pktMtx;
std::deque<PacketEntry> g_pktEntries;                      // guarded by g_pktMtx
std::unordered_map<uint64_t, PacketTypeInfo> g_pktTypes;   // guarded by g_pktMtx
uint32_t g_pktNextTypeId = 1;                              // guarded by g_pktMtx

bool ReadPtr(uintptr_t at, uintptr_t& out) {
    uint64_t v = 0;
    if (!SafeRead((const void*)at, &v, 8)) return false;
    out = (uintptr_t)v;
    return true;
}

bool LooksLikeObject(uintptr_t p) {
    if (!p || !IsPlausiblePtr((const void*)p)) return false;
    if (p & 7) return false;
    uintptr_t mt = 0;
    if (!ReadPtr(p, mt)) return false;
    return mt && IsPlausiblePtr((const void*)mt) && !(mt & 7);
}

// Pull out any System.String fields in the object's first slots. This is the
// single most useful thing for identifying a packet by eye - chat, entity and
// world packets almost always carry a name somewhere near the front.
std::string GatherStrings(uintptr_t obj, uint32_t window) {
    if (!g_stringVT.load()) return {};   // never let this teach chat a wrong vtable
    std::string out;
    int found = 0;
    for (uint32_t off = 8; off < window && found < 4; off += 8) {
        uintptr_t p = 0;
        if (!ReadPtr(obj + off, p) || !LooksLikeObject(p)) continue;
        std::string s;
        if (!ReadSystemString((void*)p, s)) continue;
        if (s.empty() || s.size() > 160) continue;
        if (!out.empty()) out += " | ";
        out += s;
        ++found;
    }
    return out;
}

void AppendToLogFile(const PacketEntry& e) {
    static std::mutex fileMtx;
    std::lock_guard lock(fileMtx);
    std::ofstream f(GetThisModuleDirectory() + L"packets.log", std::ios::app);
    if (!f) return;
    char head[160];
    snprintf(head, sizeof(head), "[%lld] %s type#%u mt=%llX len=%u",
        (long long)e.whenMs, e.outbound ? "OUT" : "IN ", e.typeId,
        (unsigned long long)e.mt, (unsigned)e.bytes.size());
    f << head;
    if (!e.strings.empty()) f << "  \"" << e.strings << "\"";
    f << "\n";
    for (size_t i = 0; i < e.bytes.size(); i += 16) {
        char line[128];
        int n = snprintf(line, sizeof(line), "  %04zX  ", i);
        for (size_t j = 0; j < 16 && i + j < e.bytes.size(); ++j)
            n += snprintf(line + n, sizeof(line) - n, "%02X ", e.bytes[i + j]);
        f << line << "\n";
    }
}

// The one place both hooks funnel into. Runs on the game's network thread, so
// it stays short: a bounded read, a map bump, and a push into a capped deque.
void Record(uintptr_t packet, bool outbound) {
    if (!packet || !LooksLikeObject(packet)) return;

    uintptr_t mt = 0;
    if (!ReadPtr(packet, mt) || !mt) return;

    if (outbound) g_pktTotalOut.fetch_add(1, std::memory_order_relaxed);
    else          g_pktTotalIn.fetch_add(1, std::memory_order_relaxed);

    // Learn the DisplayDebug layout from whichever direction it turns up on.
    // In a singleplayer world the client is hosting the server in the same
    // process, so a server-to-client packet can perfectly well be seen on the
    // send path - which is exactly where the first attempt failed to look.
    // Deliberately before the pause check: pausing is about the log, not this.
    if (g_dbgLearnArmed.load()) {
        try { DbgInspectPacket(packet); }
        catch (...) {}
    }

    int want = g_pktCaptureBytes.load();
    if (want < 16) want = 16;
    if (want > 256) want = 256;

    uint8_t buf[256];
    size_t got = 0;
    // Objects are of unknown length, so step down until a read succeeds rather
    // than assuming the whole window is mapped.
    for (int len = want; len >= 16; len >>= 1) {
        if (SafeRead((const void*)packet, buf, (size_t)len)) { got = (size_t)len; break; }
    }
    if (!got) return;

    const std::string strings = GatherStrings(packet, (uint32_t)got);
    const long long now = NowSteadyMs();

    PacketEntry entry;
    entry.whenMs = now;
    entry.outbound = outbound;
    entry.mt = (uint64_t)mt;
    entry.bytes.assign(buf, buf + got);
    entry.strings = strings;

    {
        std::lock_guard lock(g_pktMtx);
        auto it = g_pktTypes.find((uint64_t)mt);
        if (it == g_pktTypes.end()) {
            PacketTypeInfo info;
            info.id = g_pktNextTypeId++;
            info.mt = (uint64_t)mt;
            it = g_pktTypes.emplace((uint64_t)mt, std::move(info)).first;
        }
        if (outbound) it->second.outCount++;
        else          it->second.inCount++;
        it->second.lastSeenMs = now;
        entry.typeId = it->second.id;

        if (g_pktPaused.load()) {
            g_pktDropped.fetch_add(1, std::memory_order_relaxed);
        }
        else {
            size_t cap = (size_t)g_pktMaxEntries.load();
            if (cap < 50) cap = 50;
            if (cap > 20000) cap = 20000;
            g_pktEntries.push_back(entry);
            while (g_pktEntries.size() > cap) g_pktEntries.pop_front();
        }
    }

    if (g_pktLogToFile.load() && !g_pktPaused.load())
        AppendToLogFile(entry);
}

// ---- inbound ----
using tProcessPacket = void* (__fastcall*)(void* self, void* packet);
tProcessPacket oProcessPacket = nullptr;

// Both hooks run through hookthunk.hpp: our work first, then a jump to the
// game's function, so no frame of ours is under the game's code while it runs.
uintptr_t ProcessPacketPre(HookRegs* r) {
    HookGuard guard;
    void* const self = (void*)r->rcx;
    void* const packet = (void*)r->rdx;
    // Remember the handler instance: dispatching a packet of our own needs the
    // same `this` the client passes, and there is no other way to learn it.
    if (self) g_pktInHandlerSelf.store((uintptr_t)self);

    if (!g_shuttingDown.load() && g_pktCaptureIn.load()) {
        try { Record((uintptr_t)packet, false); }
        catch (...) {}
        // Chunk data has to come past here before the client can draw it.
        if (g_chunkSniff.load()) {
            try { WorldSniffInbound(packet); }
            catch (...) {}
        }
    }
    return (uintptr_t)oProcessPacket;
}

// ---- outbound ----
// This is the connection's ring-buffer enqueue, not a packet-specific entry
// point, so it can also see non-packet items. They show up as their own
// MethodTable ids and are easy to ignore - and it is the only outbound
// chokepoint available without resolving the QUIC vtable.
using tRingEnqueue = uint64_t(__fastcall*)(uint64_t a1, uint64_t a2);
tRingEnqueue oRingEnqueue = nullptr;

// The connection the client sends through. There is no way to resolve it, so
// it is taken from the hook the same way the inbound handler's `this` is.
std::atomic<uint64_t> g_ringSelf{ 0 };

uintptr_t RingEnqueuePre(HookRegs* r) {
    HookGuard guard;
    const uint64_t a1 = r->rcx;
    const uint64_t a2 = r->rdx;
    if (a1) {
        g_ringSelf.store(a1, std::memory_order_relaxed);
        g_pktCanSend.store(true, std::memory_order_relaxed);
    }
    if (!g_shuttingDown.load()) {
        if (g_pktCaptureOut.load()) {
            try { Record((uintptr_t)a2, true); }
            catch (...) {}
        }
        // The build guide watches for the client announcing a block placement.
        // Deliberately not behind the capture flag: following along while you
        // build has nothing to do with whether anyone is reading a packet log,
        // and it costs one atomic read when nobody is.
        if (PlaceWantsOutbound() && LooksLikeObject((uintptr_t)a2)) {
            uintptr_t mt = 0;
            if (ReadPtr((uintptr_t)a2, mt) && mt) {
                try { PlaceOnOutbound((uintptr_t)a2, (uint64_t)mt, a1); }
                catch (...) {}
            }
        }

        // Anything of ours goes out here - just ahead of the client's own
        // packet now, since after it is no longer somewhere we can be. This is
        // the only place we can be sure of being on the thread the client
        // itself queues packets from. It is still a call into the game from
        // under our frame, so it only happens while a placement burst runs.
        try { PlaceDrainSends(); }
        catch (...) {}
    }
    return (uintptr_t)oRingEnqueue;
}

// One thunk each, made on first use and shared by both install paths.
void* ProcessPacketThunk() {
    static void* const t = MakePreHookThunk(&ProcessPacketPre);
    return t;
}
void* RingEnqueueThunk() {
    static void* const t = MakePreHookThunk(&RingEnqueuePre);
    return t;
}

std::wstring LabelFilePath() {
    return GetThisModuleDirectory() + L"packet_types.txt";
}

} // namespace

std::atomic<bool> g_pktCanSend{ false };

bool PacketSendOutbound(void* packetObject, uint64_t channel) {
    if (!packetObject || !oRingEnqueue) return false;
    // No falling back to "whatever was last seen". Being wrong about this is
    // not a degraded send, it is a crash a few milliseconds later in code we
    // never touched.
    const uint64_t self = channel;
    if (!self) return false;
    // Straight to the trampoline, so our own packet does not come back round
    // through the hook and get counted as something the client sent.
    oRingEnqueue(self, (uint64_t)packetObject);
    return true;
}

// Only the outbound hook, which is the one that works on this build and the one
// the build guide needs to notice you placing a block.
//
// The inbound signature matches SOME function here but never fires - it is not
// the packet dispatcher - so installing it at startup means detouring a
// function nobody has identified, for a capability that does not work. That is
// not a trade worth making just to save a button press.
bool InstallOutboundPacketHook() {
    if (g_pktOutHooked.load()) return true;

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!modBase) return false;

    const uintptr_t addr = GameModulePatternScan(kSigRingEnqueue);
    g_pktOutSigFound.store(addr != 0);
    if (!addr) {
        LogToFile("packets: outbound signature not found in this build");
        return false;
    }
    g_pktOutRva.store(addr - modBase);

    void* const thunk = RingEnqueueThunk();
    if (thunk && MH_CreateHook((void*)addr, thunk, (void**)&oRingEnqueue) == MH_OK &&
        MH_EnableHook((void*)addr) == MH_OK) {
        g_pktOutHooked.store(true);
        LogToFile("packets: outbound hook installed at +0x" +
            std::to_string(addr - modBase));
        return true;
    }
    MH_RemoveHook((void*)addr);
    LogToFile("packets: outbound hook failed to install");
    return false;
}

bool InstallPacketHooks() {
    if (g_pktInHooked.load() && g_pktOutHooked.load()) return true;

    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!modBase) return false;

    if (!g_pktInHooked.load()) {
        const uintptr_t addr = GameModulePatternScan(kSigProcessPacket);
        g_pktInSigFound.store(addr != 0);
        if (addr) g_pktInRva.store(addr - modBase);
        if (addr) {
            void* const thunk = ProcessPacketThunk();
            if (thunk && MH_CreateHook((void*)addr, thunk, (void**)&oProcessPacket) == MH_OK &&
                MH_EnableHook((void*)addr) == MH_OK) {
                g_pktInHooked.store(true);
                g_pktInHandlerSelf.store(0);
                g_pktCanDispatch.store(true);
                LogToFile("packets: inbound hook installed at +0x" +
                    std::to_string(addr - modBase));
            }
            else {
                MH_RemoveHook((void*)addr);
                LogToFile("packets: inbound hook failed to install");
            }
        }
        else {
            LogToFile("packets: inbound signature not found in this build");
        }
    }

    if (!g_pktOutHooked.load()) {
        const uintptr_t addr = GameModulePatternScan(kSigRingEnqueue);
        g_pktOutSigFound.store(addr != 0);
        if (addr) g_pktOutRva.store(addr - modBase);
        if (addr) {
            void* const thunk = RingEnqueueThunk();
            if (thunk && MH_CreateHook((void*)addr, thunk, (void**)&oRingEnqueue) == MH_OK &&
                MH_EnableHook((void*)addr) == MH_OK) {
                g_pktOutHooked.store(true);
                LogToFile("packets: outbound hook installed at +0x" +
                    std::to_string(addr - modBase));
            }
            else {
                MH_RemoveHook((void*)addr);
                LogToFile("packets: outbound hook failed to install");
            }
        }
        else {
            LogToFile("packets: outbound signature not found in this build");
        }
    }

    return g_pktInHooked.load() || g_pktOutHooked.load();
}

std::vector<PacketEntry> PacketsSnapshot(size_t maxEntries) {
    std::lock_guard lock(g_pktMtx);
    if (g_pktEntries.empty()) return {};
    const size_t n = (std::min)(maxEntries, g_pktEntries.size());
    return std::vector<PacketEntry>(g_pktEntries.end() - n, g_pktEntries.end());
}

std::vector<PacketTypeInfo> PacketTypesSnapshot() {
    std::vector<PacketTypeInfo> out;
    {
        std::lock_guard lock(g_pktMtx);
        out.reserve(g_pktTypes.size());
        for (const auto& kv : g_pktTypes) out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const PacketTypeInfo& a, const PacketTypeInfo& b) {
        return (a.inCount + a.outCount) > (b.inCount + b.outCount);
        });
    return out;
}

void PacketsClear() {
    std::lock_guard lock(g_pktMtx);
    g_pktEntries.clear();
    g_pktDropped.store(0);
}

void PacketSetTypeLabel(uint64_t mt, const std::string& label) {
    {
        std::lock_guard lock(g_pktMtx);
        auto it = g_pktTypes.find(mt);
        if (it == g_pktTypes.end()) return;
        it->second.label = label;
    }
    PacketLabelsSave();
}

// Labels are keyed by the MethodTable's module-relative offset, not its raw
// address: MethodTables live in the image, so the offset is the same on every
// launch while the address is not.
void PacketLabelsLoad() {
    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!modBase) return;

    std::ifstream f(LabelFilePath());
    if (!f) return;
    std::string line;
    std::lock_guard lock(g_pktMtx);
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t sep = line.find('=');
        if (sep == std::string::npos) continue;
        const uint64_t rva = strtoull(line.substr(0, sep).c_str(), nullptr, 16);
        const std::string label = line.substr(sep + 1);
        if (!rva || label.empty()) continue;
        const uint64_t mt = (uint64_t)modBase + rva;
        auto it = g_pktTypes.find(mt);
        if (it == g_pktTypes.end()) {
            PacketTypeInfo info;
            info.id = g_pktNextTypeId++;
            info.mt = mt;
            info.label = label;
            g_pktTypes.emplace(mt, std::move(info));
        }
        else {
            it->second.label = label;
        }
    }
}

bool PacketLabelsSave() {
    size_t modSize = 0;
    const uintptr_t modBase = GetGameModuleBaseSize(modSize);
    if (!modBase) return false;

    std::vector<std::pair<uint64_t, std::string>> rows;
    {
        std::lock_guard lock(g_pktMtx);
        for (const auto& kv : g_pktTypes) {
            if (kv.second.label.empty()) continue;
            if (kv.first < modBase || kv.first >= modBase + modSize) continue;
            rows.emplace_back(kv.first - modBase, kv.second.label);
        }
    }

    std::ofstream f(LabelFilePath(), std::ios::trunc);
    if (!f) return false;
    for (const auto& r : rows) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%llX", (unsigned long long)r.first);
        f << buf << "=" << r.second << "\n";
    }
    return true;
}

bool PacketDispatchInbound(void* packetObject) {
    if (!packetObject) return false;
    if (!g_pktInHooked.load() || !oProcessPacket) return false;

    const uintptr_t self = g_pktInHandlerSelf.load();
    if (!self) return false;   // no packet has arrived yet, so we don't know `this`

    // Call the original, not the detour: this packet is ours, and running it
    // back through our own capture path would be noise at best.
    oProcessPacket((void*)self, packetObject);
    return true;
}

std::string PacketHexDump(const PacketEntry& e) {
    std::string out;
    char line[160];
    for (size_t i = 0; i < e.bytes.size(); i += 16) {
        int n = snprintf(line, sizeof(line), "%04zX  ", i);
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < e.bytes.size())
                n += snprintf(line + n, sizeof(line) - n, "%02X ", e.bytes[i + j]);
            else
                n += snprintf(line + n, sizeof(line) - n, "   ");
            if (j == 7) n += snprintf(line + n, sizeof(line) - n, " ");
        }
        n += snprintf(line + n, sizeof(line) - n, " ");
        for (size_t j = 0; j < 16 && i + j < e.bytes.size(); ++j) {
            const uint8_t c = e.bytes[i + j];
            n += snprintf(line + n, sizeof(line) - n, "%c",
                (c >= 0x20 && c < 0x7F) ? (char)c : '.');
        }
        out += line;
        out += "\n";
    }
    return out;
}
