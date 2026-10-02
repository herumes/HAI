#pragma once
// =============================================================================
// Watching what the client sends when you place a block
// =============================================================================
// Marking blocks off by hand is the last tedious thing about the build guide,
// and the client already announces every placement to the server. If we can
// recognise that announcement we can stop asking.
//
// The documented packet for it is ClientPlaceBlock, id 117, carrying position,
// rotation and placedBlockId. Two reasons not to write that down and be done:
//
//   * The published list is from server build 2026.01.13, and Update 6 is
//     reported to have removed ClientPlaceBlock in favour of the interaction
//     system. Placement may now travel inside MouseInteraction (111), which
//     carries activeSlot, itemInHandId, mouseButton and a worldInteraction.
//   * We do not see packet ids anyway. The outbound hook is the connection's
//     ring-buffer enqueue, which hands us a .NET object; in a NativeAOT binary
//     its identity is its MethodTable pointer and nothing else. The id exists
//     on the wire, after serialisation, which is past where we are standing.
//
// So this learns it the way everything else in this project got learned: place
// a block whose coordinates we already know, and find what in the outgoing
// traffic is carrying them. That works whatever the packet is called, whatever
// its id is, and whether or not the published layout still holds - and it
// cannot be fooled into pointing at the wrong thing without two placements at
// two different coordinates agreeing.
//
// Nothing here ever sends anything. It reads what the client was going to send
// regardless, and the only thing it changes is which blocks the guide thinks
// you have placed.
#include "pch.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// How the coordinates are stored in whatever object turned out to carry them.
enum class PlaceCoordKind : int {
    Int32 = 0,
    Int16 = 1,
    Float = 2,     // matched against the block corner
    FloatMid = 3,  // ... or its centre, x + 0.5
    Double = 4
};
const char* PlaceCoordKindName(PlaceCoordKind k);

struct PlaceLayout {
    bool learned = false;
    uint64_t mt = 0;            // MethodTable of the packet the client sends
    bool viaPointer = false;    // the coordinates live in an object it points at
    uint32_t ptrOffset = 0;     // ... reached through this field
    uint32_t xOffset = 0;       // offset of X within whichever object holds it
    int32_t stride = 4;         // bytes from X to Y and Y to Z
    PlaceCoordKind kind = PlaceCoordKind::Int32;
    uint32_t size = 0;          // the packet object's size, 0 when not known

    std::string ToString() const;
};

extern std::mutex g_placeMtx;
extern PlaceLayout g_placeLayout;      // guarded by g_placeMtx

// ---- learning ---------------------------------------------------------------
// Arm, place the highlighted block, and repeat once more somewhere else. Two
// samples at different coordinates is the point: one coordinate can turn up by
// coincidence in a packet that has nothing to do with placement, and it did
// take three wrong heuristics elsewhere in this project to learn that.
void PlaceLearnArm();
void PlaceLearnCancel();
void PlaceForget();

extern std::atomic<bool> g_placeLearning;
extern std::atomic<int>  g_placeSamples;       // placements seen so far
extern std::atomic<int>  g_placeCandidates;    // layouts still consistent

// What it is waiting for, what it found, and - when it fails - what it saw
// instead, which is the part worth reading.
std::string PlaceGetStatus();
std::string PlaceGetReport();

bool PlaceSaveToDisk();
bool PlaceLoadFromDisk();

// ---- watching ---------------------------------------------------------------
// Once the layout is known, every outgoing packet of that type gets its
// coordinates read and handed to the build guide. Off unless you turn it on.
extern std::atomic<bool> g_placeAutoAdvance;
extern std::atomic<uint64_t> g_placeMatched;    // placements that were in the prefab
extern std::atomic<uint64_t> g_placeIgnored;    // ... and that were not
extern std::atomic<uint64_t> g_placeWrong;      // ... where it wanted a different block

// The packet's block id runs a fixed distance from the id the name table uses
// (ten, on this client). Learned by vote across placements rather than from any
// single one, because a single placement of the wrong block would teach the
// wrong offset - and the offset is what tells a wrong block from a right one.
bool PlaceIdDeltaKnown();
int32_t PlaceIdDelta();

// Called from the outbound packet hook for every object the client enqueues.
// Runs on the network thread, so it stays short and never allocates when idle.
// `channel` is the first argument the client passed to the enqueue. It differs
// between kinds of traffic, so it is recorded alongside the packet rather than
// treated as one global connection.
void PlaceOnOutbound(uintptr_t packetObj, uint64_t mt, uint64_t channel);

// True when there is any reason to be looking at outbound traffic at all.
bool PlaceWantsOutbound();

// True when the layout is known and the guide is being driven by it. A better
// witness than a mouse button, so the mouse stands down when this is true.
bool PlaceIsWatching();

// How many placement packets of the learned type have been seen at all. The
// difference between "the hook is not firing" and "it fires but nothing
// matches" is the first thing worth knowing when this does not work, and
// matched/ignored alone cannot tell them apart - both are zero either way.
extern std::atomic<uint64_t> g_placeSeen;

// The coordinates of the last placement it read, as it read them. When the
// guide is not advancing, comparing these to the block it is asking for says
// immediately whether the anchor is wrong or the field is.
std::string PlaceGetLastSeen();

// =============================================================================
// Placing them for you
// =============================================================================
// Recognising the packet that announces a placement is also enough to send one.
// It is NOT enough to build one from scratch: the learner found the position
// and nothing else - not the rotation, not the block id, not whatever else the
// packet carries. Filling those in from a published field list would be exactly
// the mistake every published offset in this project has turned out to be.
//
// So nothing is built from scratch. A placement the client made is kept and
// re-sent with its position changed and everything else left as the client
// wrote it. The block id rides along untouched, which has a useful consequence:
// it can only ever place the block you last placed by hand. Hold the right
// thing, place one, and the rest of that material follows. It cannot place
// something you do not have, and it cannot place something we guessed at.
//
// The rotation rides along too, so a run places everything at the rotation of
// the one you placed. Full blocks do not care; stairs and beams do.
//
// This is the one part of this file that sends. Everything above only reads.
struct PlaceTemplate {
    bool have = false;
    uint64_t mt = 0;
    uint32_t size = 0;             // the object's real length, from its type
    std::string material;          // the prefab block it was seen placing
    int wx = 0, wy = 0, wz = 0;    // where it was placed
    uint32_t blockId = 0;          // +0x14 on this build - shown, never relied on
    bool hasRefs = false;          // holds a real object reference, so not replayable
};
PlaceTemplate PlaceGetTemplate();

// A bounded burst rather than a loop, and off until asked. Nothing ever comes
// back to say the server accepted a placement, so a free-running bot would
// march on through a rejection marking blocks done that do not exist. A run you
// re-arm is one you can watch, and it can only ever be wrong by its own length.
extern std::atomic<int>   g_placeBurstLeft;
extern std::atomic<int>   g_placeGapMs;      // minimum spacing between sends
extern std::atomic<float> g_placeMaxReach;   // refuse anything further than this
extern std::atomic<uint64_t> g_placeSent;
extern std::atomic<uint64_t> g_placeRefused;

void PlaceStartBurst(int count);
void PlaceStopBurst();
std::string PlaceBuildStatus();

// Drained from the outbound hook AFTER the client's own enqueue has returned.
// Sending while the client is part-way through its own call is one unknown too
// many on a path that already crashed once.
void PlaceDrainSends();

// Set once a send has gone out and the client has survived it. Until then a
// "burst" is one packet, because finding out it crashes is worth exactly one
// crash and not sixteen.
extern std::atomic<bool> g_placeProven;

// =============================================================================
// What one placement actually looks like
// =============================================================================
// Copying the packet that carries the coordinates turned out not to be enough:
// everything in it after the position is zero, so whatever tells the server
// WHICH block to place is not in there. Either placement takes more than one
// packet, or the one we found is a by-product and the real request is
// elsewhere.
//
// Guessing which would be the same mistake as trusting a published field list.
// So this records every object the client enqueues for a few seconds, in full,
// and writes it to the log. Place one block inside that window and the answer
// is in the file - the complete set of packets one placement produces, rather
// than the first one that happened to contain a coordinate.
//
// Reads only. Nothing is sent and nothing is altered.
void PlaceSnapshotArm(int milliseconds);
extern std::atomic<bool> g_placeSnapshot;
std::string PlaceSnapshotStatus();

// True when everything a burst needs is in place. The UI says which piece is
// missing rather than just disabling the button.
bool PlaceCanBuild(std::string& whyNot);

