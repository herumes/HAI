#pragma once
// =============================================================================
// Packet logger
// =============================================================================
// Two detours either side of the client's network boundary:
//
//   inbound   PacketHandler::ProcessPacket - called once per packet the server
//             sends us, with the deserialized packet object in rdx.
//   outbound  the connection's ring-buffer enqueue - called with each object
//             the client is about to put on the wire.
//
// Neither hook changes anything; they copy a prefix of the object and get out
// of the way. Both are off by default, because they sit on the network thread
// and there is no reason to pay for them when nobody is looking.
//
// The client is NativeAOT, so there are no type names to read - a packet's
// identity is its MethodTable pointer. We hand each distinct MethodTable a
// small stable id, count them, and let you name the ones you work out, which
// is enough to tell "the one that arrives when I take damage" from the rest.
#include "pch.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct PacketEntry {
    long long whenMs = 0;
    bool outbound = false;
    uint64_t mt = 0;                 // MethodTable pointer - the packet's type identity
    uint32_t typeId = 0;             // small stable id we assign to that MethodTable
    std::vector<uint8_t> bytes;      // captured prefix of the object
    std::string strings;             // any System.String fields found, joined with " | "
};

struct PacketTypeInfo {
    uint32_t id = 0;
    uint64_t mt = 0;
    uint64_t inCount = 0;
    uint64_t outCount = 0;
    long long lastSeenMs = 0;
    std::string label;               // yours to fill in from the Packets tab
};

// Capture is opt-in and costs nothing while off.
extern std::atomic<bool> g_pktCaptureIn;
extern std::atomic<bool> g_pktCaptureOut;
extern std::atomic<bool> g_pktPaused;        // keep hooks counting but stop recording entries
extern std::atomic<bool> g_pktLogToFile;
extern std::atomic<int>  g_pktCaptureBytes;  // prefix length per packet, 16..256
extern std::atomic<int>  g_pktMaxEntries;    // ring buffer depth

extern std::atomic<bool> g_pktInHooked;
extern std::atomic<bool> g_pktOutHooked;
// Module-relative addresses of whatever the signatures matched, and whether the
// signature was found at all. Worth surfacing: a signature can match nothing on
// a newer build, and "installed" told you nothing about which of the two it was.
extern std::atomic<uintptr_t> g_pktInRva;
extern std::atomic<uintptr_t> g_pktOutRva;
extern std::atomic<bool> g_pktInSigFound;
extern std::atomic<bool> g_pktOutSigFound;
extern std::atomic<uint64_t> g_pktTotalIn;
extern std::atomic<uint64_t> g_pktTotalOut;
extern std::atomic<uint64_t> g_pktDropped;   // seen while paused or over budget

// Installs whichever of the two hooks' signatures match this build. Safe to
// call repeatedly; returns true once at least one is in place.
bool InstallPacketHooks();

// Just the outbound one. Installed at startup so the build guide can follow
// along; the inbound hook stays behind a button because its signature matches
// the wrong function on this build.
bool InstallOutboundPacketHook();

// Snapshots for the UI. Both copy under the lock so the caller never holds it.
std::vector<PacketEntry> PacketsSnapshot(size_t maxEntries);
std::vector<PacketTypeInfo> PacketTypesSnapshot();

void PacketsClear();
void PacketSetTypeLabel(uint64_t mt, const std::string& label);

// Labels live next to the DLL in packet_types.txt so the names you work out
// survive a restart.
void PacketLabelsLoad();
bool PacketLabelsSave();

// Renders one entry's captured bytes as a classic offset/hex/ascii dump.
std::string PacketHexDump(const PacketEntry& e);

// Hands a packet object straight to the client's own inbound handler, as if it
// had arrived from the server. Used by debugshape.cpp to draw in-world shapes;
// nothing is transmitted anywhere. Must be called on the game thread, and only
// with an object the client will recognise. False if the hook isn't installed.
bool PacketDispatchInbound(void* packetObject);

// The address of the inbound handler, exposed so callers can tell whether
// dispatching is possible at all.
extern std::atomic<bool> g_pktCanDispatch;

// =============================================================================
// Sending
// =============================================================================
// Hands an object to the client's own outbound queue, exactly as the client
// would have. The connection pointer cannot be resolved from anywhere, so it is
// captured from the hook - which means at least one real packet has to have
// been sent this session before anything of ours can be.
//
// MUST be called from inside the ring-enqueue hook: that is the thread the
// client itself builds and queues packets on, and the ring buffer is not
// something to touch from anywhere else.
//
// Unlike everything else in this file, this one does change what leaves the
// machine.
// `channel` is the first argument the client passed when it enqueued a packet
// of the SAME kind. It is not one global connection: this enqueue is generic,
// it is called with different first arguments for different things, and sending
// down the wrong one puts an object in front of a consumer that will cast it to
// something else. That is what crashed the client the first two times - the
// allocation and the enqueue both completed, and it died afterwards, reading
// what we had queued.
bool PacketSendOutbound(void* packetObject, uint64_t channel);
extern std::atomic<bool> g_pktCanSend;
