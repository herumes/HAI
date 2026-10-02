#pragma once
// =============================================================================
// Reading the block at a coordinate
// =============================================================================
// This is the one thing still missing before a region can be copied. The block
// name table answers "what is 1238 called"; it cannot answer "what is at
// 167, 82, -195". Only the client's own chunk storage knows that, and it has
// never been found.
//
// It is findable now in a way it was not before, because of /block get. That
// command prints the id at any coordinate, so we can walk along a row of a
// building, write down what is actually there, and then go looking for that
// exact run of numbers in memory. A run of six blocks is a twelve-byte
// signature of values we already know - the same lever that found the frame
// hook, the shape list, the placement packet and the name table.
//
// What it finds is one chunk's worth. Blocks within a chunk are laid out by a
// simple index, so once the element width and the three strides are known, any
// block in that chunk can be read. Reaching OTHER chunks needs whatever holds
// them, which is the step after this one.
//
// Everything here reads. Nothing is written to the world or sent anywhere.
#include "pch.h"
#include <atomic>
#include <string>

struct ChunkLayout {
    bool found = false;
    uintptr_t base = 0;            // address of the block at (originX, originY, originZ)
    int originX = 0, originY = 0, originZ = 0;
    uint32_t width = 2;            // bytes per block
    int64_t sx = 1, sy = 0, sz = 0;  // how many elements one step on each axis is
    int edge = 16;                 // blocks along a chunk edge (display only)
    int ex = 0, ey = 0, ez = 0;    // how far the storage runs on each axis
    int64_t count = 0;             // elements in the array
    int explains = 0;              // how many known blocks it reads back correctly

    std::string ToString() const;
};

// Does the whole thing: sends /block get along a few rows through whatever is
// selected, reads the answers back out of chat, and then runs the search. There
// is no reason for a person to type two dozen commands and paste the results
// when the chat path is already wired up in both directions.
//
// Needs permission to run commands, so: your own world, or somewhere you are op.
void WorldAutoProbe();
extern std::atomic<bool> g_worldProbing;

// Pulls any "Block at (...)" lines already in the chat log into the probe set.
// Safe to call repeatedly - probes are keyed by coordinate.
int WorldScrapeChat();

// Needs probes: at least four in a row along one axis to find the storage, and
// a few more elsewhere to pin the other two.
void WorldFindChunks();
extern std::atomic<bool> g_worldScanning;

// =============================================================================
// Watching a block change
// =============================================================================
// Every search so far has had to assume something: that ids are stored raw,
// that they sit side by side, that the number in the placement packet is the
// same number the chunk keeps. The last of those turned out to be false - the
// packet's id runs ten higher than the one /block get reports - and that is
// exactly the kind of assumption that wastes an evening.
//
// This assumes none of it. Take a copy of memory, place ONE block, and see what
// changed. Whatever the client stores and in whatever units, the slot for that
// coordinate has to change when the block does, and nothing else in the process
// changes in the same way at the same moment.
//
// It is the same method that found the player's position in the first place,
// and it is the only one here that cannot be fooled by a wrong guess about
// units, because it never needs to know them.
// Step 1, once per session. Takes a copy, waits a few seconds while you place
// NOTHING, and blacklists whatever moved anyway. Without this the search cannot
// converge: about a hundred thousand slots change in any given moment - the
// Windows clock page, thread stacks, GC bookkeeping - and a slot that changes
// every time survives every round of narrowing forever.
void WorldWatchLearnNoise();
size_t WorldWatchNoiseCount();

void WorldWatchArm();

// Step 3. Keeps the last round's hits as anchors, takes a fresh copy, and
// reports the next placement by how far each change landed from one of them.
// Block storage is an array, so a neighbouring coordinate lands a couple of
// bytes away; a chunk mesh being rebuilt lands anywhere. That distance is the
// only thing that separates the two, and it also gives the element width.
void WorldWatchStride();

// The one that works. Place a block, then the same kind one step east, and
// compare which slots hold that id each time rather than which slots changed.
// Diffing cannot win here: over a million slots move every second, and placing
// a block rebuilds the chunk mesh, which looks identical to the thing being
// looked for. A specific block id is rare, so there is nothing to subtract.
void WorldWatchPairStart();

void WorldWatchCancel();
extern std::atomic<bool> g_worldWatchArmed;
extern std::atomic<bool> g_worldWatchBusy;
std::string WorldWatchStatus();

// Called from the placement watcher. Runs the comparison against the snapshot
// and reports every slot that changed at the moment you placed that block.
void WorldWatchOnPlacement(int x, int y, int z, uint32_t packetId);
// =============================================================================
// Finding the chunks through the object graph
// =============================================================================
// What should have been tried first. Everything that was ever found here - the
// frame hook, the position, the shape list, the name table - came from walking
// the graph from the game root looking for a value already known, and nothing
// came from scanning memory for a shape. Four scans for block data found
// nothing but chance matches inside .NET object headers.
//
// The known value is the chunk you are standing in, and it changes when you
// walk. Press once, walk 32 blocks into the next chunk, press again: anything
// that followed you is holding a chunk coordinate, and the blocks are a field
// or two below that. Candidates have to survive every move or they are dropped.
void WorldChunkHunt();
// Collapses the candidates to distinct addresses - most of them are one field
// found through several overlapping object bases - and lists the managed arrays
// hanging below each. An array is the one thing here with a length written next
// to it, so a section's worth of blocks can be recognised rather than guessed:
// 32 x 32 x 32 is 32768.
void WorldChunkInspect();

void WorldReadSection();
void WorldSectionNote(uintptr_t arr, int64_t len);

// Fits the block array to world coordinates by placing blocks in it. One
// placement says which index a coordinate has; the difference between two of
// them on one axis is that axis step. Four blocks - one, then east, then up,
// then south - and the whole mapping is known without assuming an ordering.
// One press. Finds the chunk object and its block array together, which is a
// far stronger test than either alone: a stray pair of integers matching your
// chunk is common, and one that ALSO points at a hundred thousand entries that
// all resolve to block names does not happen. This is why the walking is gone.
void WorldAutoFind();

void WorldFitArm();
void WorldFitAddCandidate(uintptr_t arr, int64_t len, int namedPct);
void WorldFitClearCandidates();
size_t WorldFitCandidateCount();
std::string WorldFitNeeds();
void WorldFitForget();
void WorldFitOnPlacement(int x, int y, int z, uint32_t id);
bool   WorldFitIsArmed();
size_t WorldFitSamples();

void WorldChunkForget();
int    WorldChunkPass();
size_t WorldChunkCandidates();

// =============================================================================
// Taking the world off the wire
// =============================================================================
// The client does not keep a plain array of blocks anywhere the object graph
// reaches - the best candidate turned out to be the block state DEFINITION
// table, which reads exactly like world because it is a table of blocks. But
// the server has to send every chunk it wants drawn, and the inbound packet
// hook has been installed the whole time.
//
// First question: which packet carries it. Chunk data is large and bursty, so
// listening while new ground loads and ranking by payload size answers that on
// its own.
void WorldSniffStart();
void WorldSniffStop();
void WorldSniffInbound(void* packet);

// recv and WSARecv are exported from ws2_32, so there is no pattern to drift and
// nothing to re-derive per build. If the client is talking to a server it is
// calling these. Counts and samples only - no parsing, no sending.
bool WorldSockHook();
void WorldSockReset();
std::string WorldSockStatus();
void WorldSockReport(std::string& report);

// The socket carries TLS 1.2, so the wire was never going to be readable.
// Windows TLS is SChannel and SChannel decrypts through DecryptMessage, an
// ordinary export - no signature, and the plaintext arrives with its length
// attached, which is what every other approach here has lacked.
bool WorldTlsHook();
void WorldTlsReset();
std::string WorldTlsStatus();
void WorldTlsReport(std::string& report);

// DecryptMessage turned out to be carrying HTTPS ("HTTP/1.1 201 Created"), not
// the game, and twenty WSARecv calls across thirty seconds of terrain streaming
// is far too few to be carrying chunks. So rather than guess at a fourth API:
// count the send side, which the client hammers with 170k+ ring packets, and
// list the networking libraries the process has loaded. If Winsock sends stay
// flat, the transport is something else and the module list should name it.
bool WorldSendHook();
void WorldSendReset();
void WorldTransportReport(std::string& report);

// The transport is QUIC. sendto fired 7,234 times for 857 KB and quiche.DLL is
// loaded, which also explains every Winsock receive function reading zero: a
// QUIC stack takes WSARecvMsg through WSAIoctl rather than importing it, so
// there is no export to hook.
//
// Capturing datagrams would be useless - QUIC encrypts them - but quiche is a
// DLL with real exports, and it hands the application its DECRYPTED data back
// through quiche_conn_stream_recv and quiche_conn_dgram_recv. That is the game
// protocol in the clear, with a length, found by name rather than by pattern.
bool WorldQuicHook();
void WorldQuicReset();
std::string WorldQuicStatus();
void WorldQuicReport(std::string& report);
void WorldQuicTypeReport(std::string& report);
void WorldGrabReport(std::string& report);
void WorldTickPlayer();

// =============================================================================
// Copying the shape of a build
// =============================================================================
// Type 136 gives a section as 32768 bytes, one per block, non-zero where a
// block exists. That is the geometry of a build, which is most of what is
// needed to rebuild it - the materials can be chosen at the far end.
//
// How those bytes are arranged is NOT known: 32x32x32, 64x8x64 and several
// other shapes all fit, in any of six orderings, and geometric coherence cannot
// tell them apart because the filled cells are too clustered. So it is not
// guessed. The player stands on the world - solid under the feet, air above the
// head - and only the right layout predicts that everywhere they walk. Nothing
// is adopted until one candidate has been right hundreds of times.
void WorldCacheSection(int cx, int cy, int cz, const uint8_t* grid);
void WorldCalibrateTick(int px, int py, int pz);
bool WorldShapeReady();
std::string WorldShapeStatus();
void WorldShapeForget();
bool WorldShapeAt(int x, int y, int z, bool& known);
void WorldShapeCoverage(int minX, int minY, int minZ, int maxX, int maxY,
    int maxZ, long long& readable, long long& total);
extern std::atomic<bool> g_chunkSniff;

std::string WorldStatus();
std::string WorldReport();
bool WorldFound();
ChunkLayout WorldGetLayout();

// The block at a world coordinate, if it falls in the chunk that was found.
// False when the layout is unknown or the coordinate is outside it.
bool WorldReadBlock(int x, int y, int z, uint32_t& outId);

// How much of a box can actually be read right now. Says plainly how far the
// one-chunk limit goes, rather than half-copying a region and calling it done.
bool WorldCanReadBox(int minX, int minY, int minZ, int maxX, int maxY, int maxZ,
    long long& readable, long long& total);
