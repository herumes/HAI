#pragma once
// =============================================================================
// In-world debug shapes
// =============================================================================
// The client can already draw spheres, cubes, cylinders and so on in the world:
// that is what the server's DisplayDebug packet (id 114) is for, and what the
// server's own `/debug shape cube` command sends. It is a ToClient packet, so
// there is no client-side "draw a shape" call to make - the way to get a shape
// on screen is to hand the client a DisplayDebug object as though it had just
// arrived from the server. PacketHandler::ProcessPacket, which packets.cpp
// already detours, takes exactly that.
//
// The catch is building the object. It is a .NET object, so it needs its
// MethodTable, and a NativeAOT binary has no names to look one up by. Rather
// than guess a layout - which has been wrong for every published offset we have
// tried on this build - we learn it from a real one:
//
//   1. Join a SINGLEPLAYER world, where you have command permission and the
//      client is hosting the server itself.
//   2. Turn on inbound packet capture.
//   3. Run  /debug shape cube  in chat.
//   4. The server sends a genuine DisplayDebug. We recognise it by its 4x4
//      matrix - a float[16] whose last element is 1.0 and whose translation
//      row sits at your position - and record the MethodTable and the field
//      offsets off the real thing.
//
// After that the details are saved to debugshape.txt and shapes can be drawn
// on any world, including multiplayer, because we are constructing the object
// locally rather than asking a server for anything. Nothing is ever sent
// outward: this is the client drawing on its own screen.
#include "pch.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Matches com.hypixel.hytale.protocol.DebugShape.
enum class DbgShape : int {
    Sphere = 0, Cylinder = 1, Cone = 2, Cube = 3, Frustum = 4, Sector = 5, Disc = 6
};
const char* DbgShapeName(DbgShape s);

// Matches the server's DebugUtils flag bits.
constexpr int DBG_FLAG_NONE = 0;
constexpr int DBG_FLAG_FADE = 1 << 0;
constexpr int DBG_FLAG_NO_WIREFRAME = 1 << 1;
constexpr int DBG_FLAG_NO_SOLID = 1 << 2;

// What we learned off a real packet. Everything here is discovered, never
// assumed; until `learned` is true nothing can be drawn.
struct DbgLayout {
    bool learned = false;
    uint64_t packetMTRva = 0;      // module-relative, so it survives a restart
    uint64_t floatArrayMTRva = 0;  // MethodTable of float[], taken off the real matrix
    uint32_t objectSize = 0;       // bytes we saw the client's object occupy
    uint32_t offMatrix = 0;        // float[] reference
    uint32_t offColor = 0;         // 3 floats
    uint32_t offTime = 0;          // float
    uint32_t offOpacity = 0;       // float
    uint32_t offShape = 0;         // byte or int enum
    uint32_t offFlags = 0;         // byte
    bool shapeIsByte = true;
    bool haveColor = false, haveTime = false, haveOpacity = false;
    bool haveShape = false, haveFlags = false;

    std::string ToString() const;
};

extern std::mutex g_dbgMtx;
extern DbgLayout  g_dbgLayout;     // guarded by g_dbgMtx

extern std::atomic<bool> g_dbgLearnArmed;      // while true, inbound packets get inspected
extern std::atomic<uint64_t> g_dbgSeenCount;     // DisplayDebug-shaped packets seen
extern std::atomic<uint64_t> g_dbgInspected;    // packets we looked at while learning
extern std::atomic<uint64_t> g_dbgSawMatrix;    // ... that carried a float[16] at all
extern std::mutex g_dbgStatusMtx;
extern std::string g_dbgStatus;
std::string DbgGetStatus();

// Called from the inbound packet hook for every packet, while learning is armed.
// Cheap and bails out immediately on anything that isn't shaped like the one we
// want. Returns true if it learned something from this packet.
bool DbgInspectPacket(uintptr_t packetObj);

// Force-learn from a specific captured packet, for when auto-recognition did
// not fire but you can see the packet in the log yourself.
bool DbgLearnFrom(uintptr_t packetObj);

void DbgForget();
bool DbgSaveToDisk();
bool DbgLoadFromDisk();

// =============================================================================
// Live shapes already in the world
// =============================================================================
// The inbound packet signature is stale on this build - it never fires - and
// DisplayDebug does not go through the outbound ring buffer either, so neither
// hook will ever see one. But a shape the server already drew is sitting in the
// client's memory with its 4x4 matrix intact, and we have a working object
// graph to find it in. That gives us the MethodTables for free, and in the
// meantime it gives us something better than a screenshot: take a shape the
// server drew and move it, which proves the whole read/write path end to end.
struct DbgLiveShape {
    std::vector<uint32_t> path;      // field offsets from the game root to the holder
    uint32_t fieldOffset = 0;        // offset of the X component
    bool viaArray = false;           // the value lives inside an array the holder points at
    uint32_t arrayFieldOffset = 0;   // ... reached through this field
    bool isDouble = false;
    uint64_t holderMT = 0;
    float x = 0, y = 0, z = 0;       // what it read at scan time
    std::string context;             // e.g. "4x4 matrix, scale 2.00" when we can tell

    std::string ToString() const;
};

extern std::mutex g_dbgLiveMtx;
extern std::vector<DbgLiveShape> g_dbgLiveShapes;   // guarded by g_dbgLiveMtx
extern std::atomic<bool> g_dbgScanning;
extern std::atomic<size_t> g_dbgScanVisited;

// =============================================================================
// The known layout - try this first, always
// =============================================================================
// The shape list was located once by searching, and its address is three fixed
// field offsets from the game root. Walking straight there gives an exact
// answer instantly; searching the graph for a position also turns up every
// render and animation buffer that happens to contain it, which is what made
// the search go from one correct result to two hundred useless ones.
//
// Refreshes g_dbgLiveShapes with one entry per live shape, and fills in
// everything spawning needs at the same time. Cheap enough to call on a timer.
bool DbgRefreshKnownShapes();
void DbgKnownTick();                     // throttled; call once per frame
void DbgShapeFrameTick();                // call once per frame, on the game thread

extern std::atomic<bool> g_dbgKnownWorks;  // the three offsets still resolve
extern std::atomic<int>  g_dbgKnownCount;  // live shapes found last refresh

// Worker. With useCoords, finds every place in the object graph holding that
// particular position - which is how you find a shape you drew somewhere else
// and then walked away from. Without it, falls back to "anything that looks
// like a world position", which is far weaker and mostly finds cameras.
void DbgFindShapes(bool useCoords, double x, double y, double z, double tol);

// Rewrites one of those values to the player's position every frame, so the
// shape follows you. Index into the list above; -1 detaches.
void DbgAttachToPlayer(int index);
extern std::atomic<int> g_dbgAttachedIndex;
extern std::atomic<uint64_t> g_dbgAttachWrites;

// =============================================================================
// The container the client keeps its shapes in
// =============================================================================
// Worked out from a shape that was found rather than assumed: if the last hop
// of the path lands at +0x10 of the previous object, that object is an array
// and the shape is one of its elements. Everything needed to add another one
// then follows - the element type's MethodTable, a free slot, and whichever
// int the client keeps the count in.
struct DbgContainer {
    bool valid = false;

    // MethodTables, as ABSOLUTE addresses, resolved fresh every session.
    //
    // They used to be stored module-relative and written to the seed, which was
    // simply wrong: List<Entry> and Entry[] are generic instantiations, and
    // NativeAOT builds those types at RUNTIME rather than baking them into the
    // image. Their addresses move relative to the module base from one launch
    // to the next - the same entry type read +0x225D568 in one session and
    // +0x2263CA8 in another - so a saved offset pointed at nothing.
    //
    // Both are derived live instead, from the list itself: the array object
    // carries its own MethodTable, and a MethodTable's related-type pointer at
    // +8 is, for an array, its element type. That works on a server with an
    // empty list, because Array.Empty<Entry>() is still an Entry[].
    uint64_t entryMTAbs = 0;
    uint64_t arrayMTAbs = 0;

    uint64_t holderMTRva = 0;        // kept only for display
    uint32_t matrixOffset = 0;       // 4x4 inline in the entry
    uint32_t transOffset = 0;        // its translation cells

    bool isArray = false;
    std::vector<uint32_t> arrayPath; // root -> the array itself
    uint32_t arrayLength = 0;
    int liveSlots = 0;

    bool haveCount = false;
    std::vector<uint32_t> countPath; // root -> the object holding the count
    uint32_t countOffset = 0;
    int countCandidates = 0;

    // Fields inside an entry. These were read off two real shapes rather than
    // guessed: one was watched ageing to find the countdown, and the two were
    // diffed to find the colour, which /debug shape cube randomises per call.
    //
    //   +0x08  float  seconds remaining, ticking down at 1.0/s
    //   +0x0C  float  total duration, as the server sent it
    //   +0x10  float  opacity (0.8 is the server's default)
    //   +0x14  int    shape enum, 3 == Cube
    //   +0x18  float[16] transform, translation at +0x48
    //   +0x58  float[3]  colour
    bool haveColor = false;
    uint32_t colorOffset = 0;
    bool haveLife = false;
    uint32_t lifeOffset = 0;       // seconds remaining
    float lifeMax = 30.0f;
    bool haveLifeTotal = false;
    uint32_t lifeTotalOffset = 0;  // total duration
    bool haveOpacity = false;
    uint32_t opacityOffset = 0;
    bool haveShapeEnum = false;
    uint32_t shapeOffset = 0;
    bool manualFields = false;     // set by hand; stops the defaults overwriting them
    uint32_t entrySize = 0;        // bytes, from the MethodTable
    uint64_t arrayMTRva = 0;       // the list's own MethodTable, for making one
    bool seeded = false;           // the MethodTable and template came from disk
    bool ownArray = false;         // we allocated this list, so its count is ours
    bool countFromSeed = false;    // measured, rather than matched by value

    // A whole entry the client built, copied byte for byte. Cloning it means
    // the fields we never identified arrive already correct.
    bool haveTemplate = false;
    uint8_t templateBytes[0x200] = {};
};

extern std::mutex g_dbgContMtx;
extern DbgContainer g_dbgContainer;   // guarded by g_dbgContMtx

// Inspects the chain that reached a match and works out the container around
// it. Writes a human-readable report as it goes - if the heuristics are wrong,
// the report is what says so.
void DbgAnalyzeMatch(int index);
std::string DbgGetReport();


// =============================================================================
// Drawing
// =============================================================================
// Requests are queued rather than executed where they are asked for: allocating
// a GC object and calling into the client has to happen on the game's own
// thread, so the queue is drained from the per-frame hook in gamesdk.cpp.
struct DbgRequest {
    DbgShape shape = DbgShape::Sphere;
    float x = 0, y = 0, z = 0;
    float scaleX = 1, scaleY = 1, scaleZ = 1;
    float r = 1, g = 0, b = 0;
    float time = 30.0f;        // seconds the client keeps it alive
    float opacity = 0.8f;
    int   flags = DBG_FLAG_FADE;

    // Something the caller can find this shape by later. The build guide uses
    // the block's world coordinate, so when one block is placed it can change
    // that one shape instead of redrawing the build.
    uint64_t tag = 0;
};

// =============================================================================
// Changing one shape instead of all of them
// =============================================================================
// Placing a block changes two shapes: the one just placed stops being outlined,
// and the next one turns white. Redrawing the whole build for that is thousands
// of allocations for a two-shape edit, and it is what made the outline stutter
// every time you laid a block.
//
// A tagged shape can be found again. Updating one writes over the fields of the
// object already in the list - no allocation at all, so no collection, so none
// of the hazards that come with one.
bool DbgUpdateTagged(uint64_t tag, const DbgRequest& req);
// Hides a shape and frees its slot for reuse. It does NOT null the slot: the
// client walks entries 0.._size and dereferences them without a null check, so
// a hole there crashes it with a NullReferenceException the moment it renders.
bool DbgRemoveTagged(uint64_t tag);

// Takes a freed slot and rewrites it, instead of allocating. Building churns
// shapes constantly, and this makes that churn cost nothing.
bool DbgReuseSlot(uint64_t tag, const DbgRequest& req);
void DbgForgetTags();                 // after a clear, nothing is where it was
bool DbgHasTag(uint64_t tag);

// How many of these tagged shapes are really still in the client's list: the
// slot we wrote holds an entry of our type. A relog or a world change replaces
// the list without telling anyone, and the tag table alone cannot see that.
// `missing` counts tags we have no record of at all (a clear ran). Returns -1
// when the list cannot be read right now, which is not the same as "gone".
int DbgCountTagsAlive(const std::vector<uint64_t>& tags, int& missing);

// Queues one shape. Safe from any thread. False if the layout isn't learned yet.
bool DbgQueueShape(const DbgRequest& req);

// Identifies RhpNewFast by finding real allocation sites for MethodTables we
// already know, rather than by a byte signature that goes stale. Needs at least
// one known MethodTable, so draw a shape once first.
bool DbgResolveAllocator();
extern std::atomic<uintptr_t> g_dbgAllocRva;
extern std::atomic<int> g_dbgAllocVotes;
// Only true once the candidate's own code has been read and shown to do what
// an allocator does. Nothing calls it otherwise - getting this wrong hangs the
// game, and an SEH guard does not save you from a hang.
extern std::atomic<bool> g_dbgAllocVerified;
std::string DbgGetAllocEvidence();

// Watches a live entry for a couple of seconds to locate its countdown and its
// colour. Without this, cloned shapes inherit the template's remaining life -
// which is nearly none, since the template came from a shape already ticking
// down - and ignore the colour picker entirely.
void DbgProfileEntry();
extern std::atomic<bool> g_dbgProfiling;

// Prints the entry instead of guessing at it: every non-empty field, flagged
// with what changed while a shape aged and what differs between two shapes of
// the same age. The countdown is what changes; the colour is what differs,
// since /debug shape cube randomises it. Draw two cubes before running it.
void DbgBuildEntryReport();
std::string DbgGetEntryReport();

// Set the two offsets by hand, once the report has shown where they are.
// Pass -1 to clear either.
void DbgSetFieldOffsets(int colorOff, int lifeOff, float lifeMax);

// Refreshes the countdown on every live entry each frame, so nothing expires.
extern std::atomic<bool> g_dbgKeepAlive;
extern std::atomic<uint64_t> g_dbgKeepAliveWrites;   // 0 means it is doing nothing
void DbgKeepAliveTick();

// Why the last spawn attempt did nothing. Empty when it worked.
std::string DbgGetSpawnFailure();

// Allocates one object and checks it came back sane, without touching the
// shape list. Isolates "is the allocator right" from "is the clone right",
// which matters because getting the first wrong takes the game with it.
void DbgRequestAllocTest();
std::string DbgGetAllocTestResult();

// Copies an existing entry's reference into a free slot. Allocates nothing, so
// it works even without the allocator - a way to prove appending to the list is
// what makes a shape appear, separately from being able to build one.
bool DbgQueueDuplicate();

// Watches the owner object while you add a shape, and takes the int that goes
// up by exactly one as the count. The old guess - "an int equal to the number
// of slots in use" - matches far too much, and a wrong one is the likeliest
// cause of the client indexing past the end of something.
void DbgFindCountField();
std::string DbgGetCountReport();
extern std::atomic<bool> g_dbgCountArmed;

// Whether to touch the count at all when adding a shape. Off by default: if
// the client iterates every slot and skips the empty ones, the count is not
// ours to maintain and writing it is pure risk.
extern std::atomic<bool> g_dbgBumpCount;

// Nulls every slot and zeroes the count - a way back to a clean state.
void DbgClearAllShapes();

// =============================================================================
// Everything that touches the runtime runs on the game thread
// =============================================================================
// Allocating a GC object, and above all calling the write barrier, are things
// the runtime expects from a thread it knows about, in the right state. Spawning
// was queued to the frame hook from the start and has been fine; creating the
// list was not, and it called the barrier straight from the overlay thread,
// which took the client down on the spot.
//
// So these all queue now. Same rule for every one of them: if it calls into the
// runtime, it happens where the runtime expects.
void DbgRequestCreateList(int length);
void DbgRequestGrowArray(int length);
void DbgRequestAllocatorTests();
void DbgRequestDuplicate();
void DbgRequestClearAll();

// =============================================================================
// Making room for more than four
// =============================================================================
// The array starts at capacity 4, which is List<T>'s default, and it doubles
// the usual way when the client adds past the end. So rather than allocating a
// bigger array ourselves - which needs an array allocator we have not verified,
// and a reference store the GC is not told about - we ask the client to add
// some shapes through its own code path and let it grow its own list. The
// capacity it lands on is then ours to spawn into.
//
// Sends /debug shape cube through the same chat path the rest of the DLL uses,
// so it only works where you can run that command: a singleplayer world.
void DbgGrowList(int howMany);

// =============================================================================
// Using this where you cannot run /debug shape cube
// =============================================================================
// The entry's MethodTable sits at a fixed offset in the image, and the entry
// itself turned out to be 192 bytes of nothing but floats and ints - no object
// references at all. So both survive being written to disk and read back on a
// later launch, on any server. Save once from a singleplayer world and shapes
// can be spawned anywhere after that, with no server cooperation.
// Tracks whether the array we installed is still the one in place. If the
// client puts its own back, spawning starts failing again and this is how you
// see that rather than guess at it.
extern std::atomic<uintptr_t> g_dbgLastArrayAddr;
extern std::atomic<int> g_dbgArraySwaps;

bool DbgSaveSeed();
bool DbgLoadSeed();

// Growing by asking the client for shapes needs the command. Allocating a
// bigger array does not - but it needs an array allocator, found and verified
// the same careful way RhpNewFast was, and never called until Test passes.
// Walks every link spawning depends on and says which one is missing. The
// chain has six separate steps and a server can differ from a singleplayer
// world at any of them.
std::string DbgBuildReadinessReport();

// Allocates a shape list and installs it, for a world where the client never
// made one - which is the usual state anywhere no debug shape has arrived.
bool DbgCreateShapeList(int length);

// Finds the shape list by its element type rather than by a remembered path.
// The remembered one was a shortest-path alias that only held in singleplayer.
// The GC write barrier. Storing a reference without it means the collector is
// never told, so a freshly allocated object is not found during a young
// collection and is thrown away - leaving the field pointing at nothing. That
// is why a created list reads 32 slots and then 0 again.
bool DbgResolveWriteBarrier();
bool DbgTestWriteBarrier();
bool DbgStoreRef(void* slot, void* ref);
std::string DbgGetBarrierEvidence();
extern std::atomic<uintptr_t> g_dbgBarrierRva;
extern std::atomic<bool> g_dbgBarrierVerified;

void DbgLocateList();
std::string DbgGetLocateReport();
extern std::atomic<bool> g_dbgLocating;

bool DbgResolveArrayAllocator();
bool DbgTestArrayAllocator(uintptr_t arrayMT);
bool DbgGrowArrayTo(int newLength);
std::string DbgGetArrayAllocEvidence();
extern std::atomic<uintptr_t> g_dbgArrayAllocRva;
extern std::atomic<bool> g_dbgArrayAllocVerified;

// Queues a shape built by cloning an entry the client made - the route that
// does not need the packet path at all. Needs DbgAnalyzeMatch to have found the
// container first.
bool DbgQueueSpawn(const DbgRequest& req);
void DbgDrainSpawns();   // game thread only

// Drained by gamesdk's frame hook, on the game thread. Do not call elsewhere.
void DbgDrainQueue();

extern std::atomic<uint64_t> g_dbgSentCount;
extern std::atomic<uint64_t> g_dbgFailCount;

// True once both the layout and the runtime entry points we need are resolved.
bool DbgReady();

// Allocates one .NET object of the given type with the allocator the shape
// system found and proved. Null unless that allocator has been verified - a
// wrong one hangs or corrupts, and there is no recovering from either.
//
// Whoever calls this is responsible for being on a thread the runtime knows
// about. The game thread qualifies; so does any thread the client itself
// allocates on. A thread we created does not, which is what took the client
// down the first time the write barrier was called from the overlay.
//
// It also returns null when the object would not fit in the calling thread's
// allocation context (DbgAllocFitsNow): the allocator would then take its slow
// path, which can start a collection that walks our frames and fail-fasts the
// game. Try again on a later call - the game refills the context constantly.
void* DbgAllocObject(void* methodTable);

// True when one object of this type can be allocated on the calling thread
// right now without any chance of starting a garbage collection.
bool DbgAllocFitsNow(void* methodTable);

// Resolves RhpNewFast / RhpNewFastArray. Called lazily; safe to call often.
bool DbgResolveRuntime();
extern std::atomic<bool> g_dbgRuntimeReady;

// =============================================================================
// Getting ready without being asked
// =============================================================================
// Everything above works, but it works as a sequence of buttons: find the list,
// find the array allocator, test it, create the list, then spawn. Every one of
// those is something the code can check for itself, and the right order is
// fixed - so there is no reason a person should be driving it.
//
// This runs the same chain from the frame hook. Each step is attempted only
// when the step before it has actually succeeded, scans go to workers, and
// anything that calls into the runtime is queued as a game-thread op exactly
// as the buttons already did. It is also self-healing: if the client swaps its
// own empty list back in, the capacity check sees that and rebuilds.
enum class DbgAutoStage : int {
    NoRoot = 0,     // not in a world yet
    NoSeed,         // never seen a real shape, and no seed on disk
    Locating,       // hunting for the client's shape list
    Scanning,       // reading the image for the allocators and the barrier
    Testing,        // calling them once each, on a throwaway object
    Capacity,       // allocating a list big enough to hold what we want
    Ready
};

extern std::atomic<bool> g_dbgAutoPrepare;     // on by default
extern std::atomic<int>  g_dbgAutoStage;       // a DbgAutoStage
extern std::atomic<int>  g_dbgWantedCapacity;  // slots to keep available

// One line saying what it is doing and, when it is stuck, what would unstick it.
std::string DbgAutoStatus();

// True when a shape queued right now would actually appear.
bool DbgSpawnReady();

// Slots currently free in the live list, or -1 if the list does not resolve.
int DbgFreeSlots();

// How many slots the live list has at all, or -1 if it does not resolve. This
// is the hard ceiling on how much outline can be on screen at once.
int DbgListCapacity();

// The most slots a list is ever allowed to have. Thousands of translucent
// cubes is a rendering decision, not a memory one - the array itself is 8 bytes
// a slot - so this is set where it stops being something a person would want.
extern const int kDbgMaxCapacity;

// Shapes created per frame while a queued outline fills in. Each one allocates
// on the game thread, so draining thousands in a single frame stalls it.
extern std::atomic<int> g_dbgSpawnPerFrame;

// How much is still waiting, so a big build can show its progress rather than
// looking like it stopped halfway.
size_t DbgSpawnQueueDepth();

// Drop anything still queued. Loading a different prefab has to do this, or the
// tail of the previous one lands on top of the new one.
void DbgClearSpawnQueue();

// Called once per frame on the game thread, from DbgShapeFrameTick.
void DbgAutoTick();
