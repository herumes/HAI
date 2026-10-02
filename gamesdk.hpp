#pragma once
// =============================================================================
// Game object graph ("SDK")
// =============================================================================
// position.cpp finds the player position the hard way: it scans the whole heap
// for coordinate-shaped numbers and narrows the list as you move. That works,
// but only for the session you did it in - ASLR and the GC move everything on
// the next launch, so it has to be redone by hand every time.
//
// This file is the permanent version of the same thing. Instead of searching
// for the *value*, it gets handed the *object that owns it*:
//
//   1. We detour a GameInstance method the client calls every frame. The only
//      thing the detour does is record `this` (rcx) and call the original, so
//      from the first rendered frame after joining a world we hold a live
//      pointer to GameInstance - no scanning, no ASLR to undo.
//   2. From that root we walk a short chain of field offsets down to the local
//      player entity and read its position. The chain is relative to the root,
//      so it means the same thing on every launch and is saved to disk once.
//
// The chain is discovered rather than hardcoded, because published offsets go
// stale on every client patch. Three ways to get it, cheapest first:
//   - the known-good offsets below (instant, if your build still matches),
//   - "learn" from an address you already locked via the Position tab's manual
//     scan (instant, and turns that one-time manual find into a permanent one),
//   - a guided auto-find that watches which float triple in the object graph
//     tracks your movement (a few seconds of walking around).
//
// Everything here is read-only against the game. Nothing is written back.
#include "pch.h"
#include <atomic>
#include <mutex>
#include <string>
#include <functional>
#include <vector>

// =============================================================================
// Root capture (the per-frame hook)
// =============================================================================
extern std::atomic<uintptr_t> g_gsdkRoot;          // live GameInstance, set by the frame hook
extern std::atomic<long long> g_gsdkRootLastMs;    // NowSteadyMs() of the last hook call
extern std::atomic<bool>      g_gsdkHookInstalled;
extern std::atomic<uintptr_t> g_gsdkHookRva;       // module-relative address we hooked (diagnostic)
extern std::mutex             g_gsdkHookNameMtx;
extern std::string            g_gsdkHookName;      // which candidate signature matched

// Installs the frame hook. Safe to call repeatedly; does nothing once installed.
// Called from the same installer loop that installs the chat hooks.
bool GsdkInstallFrameHook();

// True while the frame hook is running on the thread the game sends chat from.
// Bot messages are then sent from the frame hook - in place of one frame's call
// where the hooked function allows it - and the chat hooks leave them alone.
bool GsdkFrameCarriesChatSends();

// True if the frame hook has fired recently enough that the root is live
// (i.e. you are in a world and the client is rendering).
bool GsdkRootLive();

// =============================================================================
// Fallback root: Application.App located by MethodTable
// =============================================================================
// If none of the frame-hook signatures match your client build, we can still
// find the App object the way Hytale-Carbon does: resolve its MethodTable from
// a `lea rcx, [App_MethodTable]` site, then scan the heap once for the single
// object whose first qword is that MethodTable. App lives for the whole
// process, so this costs one background scan per launch and nothing after.
extern std::atomic<uintptr_t> g_gsdkAppMT;         // resolved MethodTable pointer
extern std::atomic<uintptr_t> g_gsdkApp;           // located App instance
extern std::atomic<bool>      g_gsdkAppScanning;
extern std::atomic<long long> g_gsdkAppScanMs;

// Runs on a worker thread. Resolves the MethodTable then scans for the instance.
void GsdkFindApp();

// App -> AppInGame -> GameInstance, using the known offsets. Returns 0 if the
// App root isn't available or the client isn't in a world.
uintptr_t GsdkRootFromApp();

// The root the rest of this file uses: the hooked GameInstance if it's live,
// otherwise the one derived from App. 0 if neither is available.
uintptr_t GsdkCurrentRoot();

// =============================================================================
// The resolved path from root to position
// =============================================================================
enum class GsdkValueType : int { Float = 0, Double = 1 };

struct GsdkPath {
    // Pointer field offsets dereferenced in order, starting at the root.
    // Empty means the position lives directly in the root object.
    std::vector<uint32_t> derefs;
    uint32_t finalOffset = 0;                       // offset of X inside the last object
    GsdkValueType type = GsdkValueType::Float;
    bool found = false;

    std::string ToString() const;                   // "root->+0x10->+0x284 (float)"
};

extern std::mutex g_gsdkPathMtx;
extern GsdkPath   g_gsdkPath;                       // guarded by g_gsdkPathMtx

// Resolves the saved path against the current root. Returns the address of X,
// or 0 if any link is missing. Cheap enough to call every frame.
uintptr_t GsdkResolvePath(const GsdkPath& path, uintptr_t root);

// Reads a position triple from `addr` honouring `type`. Returns false if the
// memory isn't readable or the numbers don't look like a world position.
bool GsdkReadTriple(uintptr_t addr, GsdkValueType type, double& x, double& y, double& z);

// =============================================================================
// Discovery
// =============================================================================
// Offsets published by the Hytale-Carbon project for client build 2026.02.19.
// Tried first because when they're right this is instant; treated as a guess,
// never trusted without reading a plausible position back through them.
namespace GsdkKnown {
    constexpr uint32_t App_GameState = 0x0C;        // int, 4 == in-game
    constexpr uint32_t App_AppInGame = 0x88;
    constexpr uint32_t AppInGame_GameInstance = 0x10;
    constexpr uint32_t GameInstance_PlayerEntity = 0x10;
    constexpr uint32_t GameInstance_Chat = 0x28;
    constexpr uint32_t GameInstance_EntityStore = 0x140;
    constexpr uint32_t GameInstance_PlayerNetworkId = 0x218;
    constexpr uint32_t GameInstance_Fov = 0x21C;
    constexpr uint32_t Entity_NetworkId = 0x1B8;
    constexpr uint32_t Entity_Position = 0x284;      // Vector3 of floats - WRONG on this build

    // Position is not a field of the entity on this client. The entity holds a
    // sub-object at +0x210 - a transform of some kind - and the Vector3 sits at
    // +0x24 inside that. Confirmed against the F7 overlay exactly, which is why
    // every Entity+0x2xx guess failed and the corroboration check rejected the
    // published offset.
    constexpr uint32_t Entity_Transform = 0x210;
    constexpr uint32_t Transform_Position = 0x24;
    constexpr uint32_t Entity_BoundingBox = 0x2B4;
    constexpr uint32_t Entity_PitchYaw = 0x3DC;
    constexpr uint32_t Entity_PlayerName = 0x100;    // System.String
    constexpr int      App_GameState_InGame = 4;
}

// Tries GsdkKnown's offsets against the live root and adopts them if a
// plausible position reads back. Instant, no scanning. Returns true on success.
bool GsdkTryKnownOffsets();

// Walks the object graph from the root looking for the object that contains
// `lockedAddr`, and adopts the resulting path. This is the bridge from the
// manual scan: lock the address once the old way, press the button, and the
// path becomes permanent. Runs on a worker thread.
void GsdkLearnFromAddress(uintptr_t lockedAddr);

// Guided auto-find. Collects every float/double triple in the object graph that
// could be a position, then samples them while you walk around and keeps the
// one that tracks your movement. Runs on a worker thread; takes a few seconds.
void GsdkAutoFind();

// The fast, exact way when Hytale's own F7 overlay is on screen: you read the
// "Feet (x, y, z)" numbers off it and we look for that specific triple in the
// object graph. One walk, no sampling, and effectively no false positives -
// three independent floats matching to within a couple of units is not a
// coincidence. Runs on a worker thread.
void GsdkFindFromCoords(double x, double y, double z, double tolerance);

// True when the last graph walk hit its candidate budget and stopped early, so
// the UI can say "I did not look everywhere" instead of "not found".
extern std::atomic<bool> g_gsdkLastWalkTruncated;

// =============================================================================
// Fully automatic setup, via Hytale's own debug overlay
// =============================================================================
// The client already knows where you are and prints it on the F7 overlay:
// "... Feet (251.34, 150.00, -243.59) Chunk (7, -8)". That line exists in the
// heap as an ordinary .NET string, so rather than asking you to retype those
// numbers we read the line ourselves and feed it to the coordinate search.
// That makes first-time setup a no-op, and it is also how the tool recovers on
// its own after a client patch moves the fields: the saved path stops reading
// a sane value, so we go and find the new one.
//
// Requires F7's overlay to be on screen. Nothing else - in particular it no
// longer waits for chat, because the string vtable is worked out structurally.

// Locates the System.String MethodTable by shape (length field, null
// terminator, sane characters) rather than by waiting for a chat message.
// Caches into chat.hpp's g_stringVT when several objects agree. Returns 0 if
// it could not be established.
uint64_t GsdkFindStringVT();

// Finds the live F7 debug line in the object graph. Returns false if the
// overlay is off or the line has not been allocated yet this frame.
bool GsdkReadDebugLine(std::string& out);

// Worker: read the debug line, parse "Feet (x, y, z)", then run the coordinate
// search with it. This is GsdkFindFromCoords with the typing removed.
void GsdkAutoSetupFromDebugLine();

extern std::atomic<long long> g_gsdkAutoSetupLastTryMs;
extern std::atomic<bool>      g_gsdkAutoSetupEnabled;   // persisted
void GsdkAutoSetupSetEnabled(bool enabled);

// Called every frame. While no usable path exists and auto-setup is on, retries
// GsdkAutoSetupFromDebugLine every few seconds. Cheap and self-throttling.
void GsdkAutoSetupTick();

// =============================================================================
// Facing (yaw / pitch)
// =============================================================================
// The published rotation offset is wrong for this build, same as the position
// one was, so this is discovered the same way: sample floats while you look
// around and keep the ones that follow your view. Pitch is distinguishable
// from yaw because it is bounded to +/-90 degrees.
struct GsdkFacingPath {
    std::vector<uint32_t> derefs;    // from the root, to the object holding them
    uint32_t yawOffset = 0;
    uint32_t pitchOffset = 0;
    bool radians = false;            // detected from the observed value range
    // Calibration. We cannot know the client's angle convention from the
    // numbers alone - neither where its zero points nor which way it counts -
    // so both are measured from you standing in two known directions.
    float northOffsetDeg = 0.0f;     // "Set north" writes this
    int   yawSign = 1;               // +1 if yaw grows clockwise, -1 if anti
    bool  eastCalibrated = false;    // false until "Set east" has been used
    bool found = false;

    std::string ToString() const;
};

extern std::mutex g_gsdkFacingMtx;
extern GsdkFacingPath g_gsdkFacing;  // guarded by g_gsdkFacingMtx

// One candidate pair of angle fields, so you can pick if the top one is wrong.
struct GsdkFacingCandidate {
    GsdkFacingPath path;
    float yawRange = 0;      // how far the value swung while you looked around
    float pitchRange = 0;
    uint32_t gap = 0;        // bytes between the two fields; adjacent is a good sign
};
extern std::mutex g_gsdkFacingCandMtx;
extern std::vector<GsdkFacingCandidate> g_gsdkFacingCandidates;
void GsdkAdoptFacing(const GsdkFacingPath& path);

// Worker, ~13 seconds in three parts. It tells you what to do as it goes via
// GsdkGetFacingPrompt: hold still, then look around, then hold still again.
// The still phases are what make this reliable - a real angle is frozen while
// your hand is off the mouse, so anything that drifts is disqualified outright
// rather than merely ignored.
void GsdkFindFacing();

// Non-empty while GsdkFindFacing wants you to do something ("HOLD STILL...").
std::string GsdkGetFacingPrompt();

// Two-point compass calibration. Face north in-game and call the first; face
// east and call the second. North fixes where zero is, east fixes which way the
// numbers run - between them there is nothing left to guess.
void GsdkCalibrateNorth();
void GsdkCalibrateEast();

// The same correction as "Set east", for when you can already see that east and
// west are the wrong way round and would rather not walk somewhere to prove it.
// Getting the direction of count wrong swaps east and west while leaving north
// and south correct, so this is the usual thing left to fix.
void GsdkFlipYawDirection();

void GsdkForgetFacing();

// Raw yaw turned into a compass bearing: 0 = north, increasing clockwise,
// in [0, 360). Only meaningful once north has been set.
float GsdkYawToCompass(float rawYawDeg);

// Reads yaw/pitch in degrees through the discovered path. False if unset.
bool GsdkReadFacing(float& yawDeg, float& pitchDeg);

// =============================================================================
// Per-frame readers
// =============================================================================
// The same reads as GsdkFrameTick / GsdkReadFacing, but made to be called
// from the game's own frame hook every frame: they keep a per-thread copy of
// the paths (refreshed twice a second) instead of copying them under a lock
// each call, so nothing is allocated on the game thread.
bool GsdkReadPositionFast(float& x, float& y, float& z);
bool GsdkReadCompassFast(float& compassDeg);

// =============================================================================
// The tractor's own facing
// =============================================================================
// Riding something, the camera looks one way and the vehicle can point
// another, and it's the vehicle's facing that decides what a glitchy corner
// does. It lives in whatever you are riding, which is its own entity, so it
// is found the way the camera's was - by watching which number behaves like
// it - but with different instructions:
//   hold still            -> anything that moves is noise,
//   drive in circles      -> the real one changes, and points the way you go,
//   keep still, look round -> anything that follows the mouse is the camera.
// What survives is also checked against the direction you actually drove,
// which nothing but a facing can agree with.
//
// Where it is found in an entity near you (the tractor), that entity is found
// again by itself after a relog or remount: same class as the player, the one
// you're sitting on.
struct GsdkRidePath {
    bool found = false;
    int anchor = 0;                     // 0: from the game root; 1: from the ridden entity
    std::vector<uint32_t> derefs;       // anchor -> the object holding the angle
    uint32_t offset = 0;
    bool radians = false;
    std::vector<uint32_t> entityDerefs; // root -> the ridden entity, if a path was found (anchor 1)
    std::string ToString() const;
};
extern std::mutex g_gsdkRideMtx;
extern GsdkRidePath g_gsdkRide;         // guarded by g_gsdkRideMtx

struct GsdkRideCandidate {
    GsdkRidePath path;
    uintptr_t anchorObj = 0;            // the entity it was found in (anchor 1)
    float range = 0;                    // degrees it swung while you drove about
    float agree = -1;                   // 0..1 how well it pointed the way you went; -1 not measured
};
extern std::mutex g_gsdkRideCandMtx;
extern std::vector<GsdkRideCandidate> g_gsdkRideCandidates;

// The guided finder. Runs on a worker, about 25 seconds, telling you what to do
// through GsdkRideStep / GsdkRideStatus (and beeps, since you'll be in game).
void GsdkFindRideFacing();
void GsdkCancelRideFacing();
extern std::atomic<bool> g_gsdkRideFinding;
// 0 idle, 1 get ready, 2 hold still, 3 drive in circles, 4 look around, 5 hold still, 6 checking
extern std::atomic<int> g_gsdkRideStep;
extern std::atomic<long long> g_gsdkRideStepEndMs;
std::string GsdkRideStatus();

void GsdkAdoptRide(const GsdkRideCandidate& c);
// Loading a saved one: no candidates, no ridden entity yet (GsdkRideTick finds it).
void GsdkSetRidePath(const GsdkRidePath& p);
void GsdkForgetRide();
// Degrees, through the found path. Cheap and allocation-free: safe to call
// from the frame hook. False when not found, not riding, or not readable.
bool GsdkReadRideYaw(float& deg);
// Through a candidate, for showing live values next to each one.
bool GsdkReadRideCandidate(const GsdkRideCandidate& c, float& deg);
// Every quarter second or so from a worker: checks the ridden entity is still
// the one under you, and looks for it again when it isn't (relog, remount).
void GsdkRideTick();
// Is the ridden entity located right now (anchor 1), or looking for it?
bool GsdkRideEntityKnown();
bool GsdkRideSearching();

// Same, but through a path you pass in - so the UI can show live values for
// every candidate at once without disturbing the adopted one.
bool GsdkReadFacingPath(const GsdkFacingPath& path, float& yawDeg, float& pitchDeg);

// Survivors of the auto-find sampling pass, newest values first refreshed by
// GsdkFrameTick so the UI can show live numbers next to each one. If exactly
// one survives it is adopted automatically; otherwise you pick from this list.
struct GsdkCandidate {
    GsdkPath path;
    double x = 0, y = 0, z = 0;
    double movement = 0;      // total distance the triple travelled while sampling
    int    changes = 0;       // how many samples differed from the previous one
};
extern std::mutex g_gsdkCandMtx;
extern std::vector<GsdkCandidate> g_gsdkCandidates;  // guarded by g_gsdkCandMtx

// Adopts one of the auto-find survivors as the live path and saves it.
void GsdkAdoptCandidate(const GsdkPath& path);

extern std::atomic<bool>   g_gsdkDiscovering;
extern std::atomic<int>    g_gsdkDiscoverPhase;     // 0 idle, 1 walking graph, 2 sampling
extern std::atomic<size_t> g_gsdkDiscoverCandidates;
extern std::atomic<size_t> g_gsdkDiscoverVisited;
extern std::mutex          g_gsdkStatusMtx;
extern std::string         g_gsdkStatus;            // one-line human readable state

void GsdkSetStatus(const std::string& s);
std::string GsdkGetStatus();

// =============================================================================
// Per-frame use
// =============================================================================
// Called once per overlay frame. If a path is known and the root is live, reads
// the position and publishes it through position.hpp's g_posX/Y/Z/g_posLocked,
// so everything that already displays those (the compact window strip, the
// Position tab) lights up with no further changes. Cheap: one pointer walk.
void GsdkFrameTick();

// True while GsdkFrameTick is successfully publishing coordinates. The manual
// finder in position.cpp writes the same globals and re-reads them with its own
// float/double setting, so the caller skips PosFrameTick while this is true
// rather than letting the two fight over g_posLockedAddr.
bool GsdkOwnsPosition();

bool GsdkPathSaveToDisk();
bool GsdkPathLoadFromDisk();
void GsdkForget();                                   // drop the path and start over

// =============================================================================
// Player details
// =============================================================================
struct GsdkPlayerInfo {
    bool valid = false;
    uintptr_t entity = 0;
    uintptr_t gameInstance = 0;
    double x = 0, y = 0, z = 0;
    int chunkX = 0, chunkY = 0, chunkZ = 0;
    bool haveRotation = false;
    float pitch = 0, yaw = 0;                        // degrees, north-calibrated
    float rawYaw = 0;                                // before the north offset
    std::string facing;                              // "North (-Z)" etc
    bool haveNetworkId = false;
    int networkId = 0;
    bool haveFov = false;
    float fov = 0;
    std::string playerName;                          // if a System.String was readable
    bool haveBox = false;
    float boxMinX = 0, boxMinY = 0, boxMinZ = 0, boxMaxX = 0, boxMaxY = 0, boxMaxZ = 0;
};

// Snapshot of everything we can currently read about the local player. Safe to
// call from the overlay thread; returns valid=false when nothing is resolved.
GsdkPlayerInfo GsdkGetPlayerInfo();

// =============================================================================
// Raw object inspector
// =============================================================================
// Generic dump of one .NET object's slots, for working out what a field is.
// Each row is one 8-byte slot rendered several ways at once.
struct GsdkSlot {
    uint32_t offset = 0;
    uint64_t raw = 0;
    float f0 = 0, f1 = 0;                            // the qword read as two floats
    int32_t i0 = 0, i1 = 0;                          // ... and as two ints
    double d = 0;                                    // ... and as one double
    bool isPointer = false;                          // points at committed readable memory
    uint64_t targetMT = 0;                           // first qword of the target, if pointer
    std::string text;                                // System.String contents, if it is one
};

// =============================================================================
// Object graph walking, for other files
// =============================================================================
// Breadth-first from `root` over everything reachable through pointer fields,
// bounded so it stays a graph walk rather than becoming a heap scan. `visit`
// gets each distinct object and the chain of field offsets that reached it;
// return false to stop early. This is the same walker the position finder uses.
void GsdkWalkGraph(uintptr_t root, int maxDepth,
    const std::function<bool(uintptr_t, const std::vector<uint32_t>&)>& visit);

// Follows a chain of field offsets from `root`. 0 if any link is missing.
uintptr_t GsdkFollow(uintptr_t root, const std::vector<uint32_t>& derefs);

// Reads `count` slots starting at obj+startOffset. Never throws; unreadable
// slots come back with raw=0 and isPointer=false.
std::vector<GsdkSlot> GsdkDumpObject(uintptr_t obj, uint32_t startOffset, uint32_t count);
