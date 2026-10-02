#pragma once
// Live player-position finder.
//
// There is no network hook available for this (OnReceive is a UI text-node
// walker, not a raw packet dispatcher - see hooks.cpp/ProcessMessage). Instead
// this does what hooks.cpp already does for the chat functions, but for data
// instead of code: it heuristically locates the local player's live (x,y,z)
// in the game's own heap by scanning for a double-triple that looks like a
// world position and then narrowing the candidate list as the player moves,
// the same way a Cheat Engine "unknown initial value -> changed value" scan
// works. Once locked, it's just a SafeRead of 24 bytes per frame - no
// separate process, no network connection.
#include "pch.h"
#include <vector>
#include <atomic>
#include <mutex>
#include <cstdint>

struct PosCandidate {
    uintptr_t addr = 0;
    double x = 0.0, y = 0.0, z = 0.0;
};

// We don't know whether the client keeps its live transform as double
// (matches the network wire format - see hytale-python-main's protocol docs)
// or float (typical of most render engines, especially with a floating-
// origin large world). Try both from the Position tab.
enum class PosValueType : int { Double = 0, Float = 1 };
extern std::atomic<int> g_posValueType;

extern std::mutex g_posMtx;
extern std::vector<PosCandidate> g_posCandidates; // guarded by g_posMtx

extern std::atomic<bool>   g_posScanning;        // a scan/filter worker is running
extern std::atomic<bool>   g_posLastScanTruncated;
extern std::atomic<size_t> g_posLastScanRegions;
extern std::atomic<size_t> g_posLastScanBytes;
extern std::atomic<long long> g_posLastScanMs;

extern std::atomic<bool>     g_posLocked;
extern std::atomic<uintptr_t> g_posLockedAddr;
extern std::atomic<double>   g_posX;
extern std::atomic<double>   g_posY;
extern std::atomic<double>   g_posZ;
extern std::atomic<long long> g_posLastUpdateMs;
extern std::atomic<long long> g_posLastGoodMs; // last time the locked read looked plausible

// Plausibility bounds, tunable from the UI.
extern std::atomic<float> g_posBoundXZ;   // x and z must be within +/- this
extern std::atomic<float> g_posBoundYMin;
extern std::atomic<float> g_posBoundYMax;

// "Known coordinates" - e.g. typed in fresh off Hytale's own F7 debug
// overlay ("Feet (x, y, z)" line). When enabled, First Scan only keeps
// candidates within g_posGuessTolerance of this (turns a multi-second blind
// heap scan into a near-instant one) and PosFilterByGuess() re-applies it
// on demand against the current candidate list. It is deliberately NOT
// applied by PosFilterScan or PosFrameTick, since after you move your real
// position is expected to be far from wherever the guess was last set.
extern std::atomic<bool>  g_posUseGuess;
extern std::atomic<double> g_posGuessX, g_posGuessY, g_posGuessZ;
extern std::atomic<float>  g_posGuessTolerance;

// Runs on a worker thread (see StartWorker). Replaces g_posCandidates.
void PosFirstScan();

// Runs on a worker thread. Re-reads existing candidates, drops any that are
// no longer plausible or that did not move since the last pass.
void PosFilterScan();

// Runs on a worker thread. Re-reads existing candidates, keeps only ones
// currently near g_posGuessX/Y/Z (re-enter fresh numbers from F7 first).
// Much stronger than PosFilterScan when you have ground truth to check against.
void PosFilterByGuess();

void PosLock(uintptr_t addr);
void PosUnlock();

// Clears g_posCandidates safely: wipes the values first so they don't
// linger in freed-but-still-committed heap memory and get "found" again by
// a later scan. Use this instead of g_posCandidates.clear() directly.
void PosClearCandidates();

// Call once per rendered frame. Cheap (one SafeRead) when locked, a no-op
// otherwise. Auto-unlocks if the locked address stops looking like a
// position for a few seconds (freed/reused memory).
void PosFrameTick();

// =============================================================================
// Pointer chain finder
// =============================================================================
// A scanned/locked address is only good for this game session - ASLR moves
// everything on the next launch. To make it automatic, we need a path that's
// stable across launches: HytaleClient.exe's own module base (rebased by
// ASLR, but by the same amount for every static offset in it) plus a fixed
// offset to a pointer slot, dereferenced through zero or more further
// offsets, landing on the live position every time.
//
// This is found by walking backwards from a locked address: scan memory for
// something that points near it (within a small window - the field offset
// inside whatever struct holds it), note that offset, and recurse using the
// pointer's own location as the next target - until a hit lands inside
// HytaleClient.exe's own image, which is stable by construction.

struct PosPtrHit {
    uintptr_t slotAddr = 0;   // address of the 8-byte pointer we found
    int64_t offset = 0;       // currentTarget - *slotAddr (0 <= offset <= search window)
    bool inGameModule = false; // true if slotAddr itself is inside HytaleClient.exe's image - a usable chain root
};

extern std::mutex g_posPtrMtx;
extern std::vector<PosPtrHit> g_posPtrHits;      // guarded by g_posPtrMtx
extern std::atomic<bool> g_posPtrScanning;
extern std::atomic<uintptr_t> g_posPtrCurrentTarget; // address the next scan will search for pointers to
extern std::atomic<int> g_posPtrWindow;              // how far below target counts as a "hit" (bytes)

// Chain built so far, in discovery order (root end filled in last).
extern std::mutex g_posChainMtx;
extern std::vector<int64_t> g_posChainOffsets;   // discovery order: [offset-from-T, ..., offset-from-root]
extern std::atomic<bool> g_posChainDone;         // true once a static root has been chosen
extern std::wstring g_posChainModule;            // e.g. L"HytaleClient.exe"
extern uintptr_t g_posChainStaticOffset;         // module-relative offset of the root pointer slot
extern std::atomic<int> g_posChainValueType;     // PosValueType at the time the chain was built

// Start over: sets g_posPtrCurrentTarget to the given (usually the currently
// locked) address and clears any in-progress chain.
void PosChainBegin(uintptr_t startTarget);

// Runs on a worker thread. Scans all readable/writable memory (excluding our
// own module) for pointers landing within g_posPtrWindow bytes below
// g_posPtrCurrentTarget. Fills g_posPtrHits.
void PosFindPointersToCurrentTarget();

// Not a static hit: records the offset, advances the chain target to the
// hit's own slot address (so the next PosFindPointersToCurrentTarget looks
// for what points at THAT), one level further from the leaf.
void PosChainDrillInto(const PosPtrHit& hit);

// A static hit (hit.inGameModule): finalizes the chain, saves it to disk,
// and marks g_posChainDone.
void PosChainFinish(const PosPtrHit& hit);

bool PosChainSaveToDisk();
bool PosChainLoadFromDisk();

// Resolves the saved chain against the game module's CURRENT (ASLR-shifted)
// base and, if the result looks like a plausible position, locks it. Safe to
// call speculatively (e.g. once after the overlay starts) - does nothing if
// no chain is saved or the resolved address doesn't check out.
bool PosChainTryAutoLock();

// =============================================================================
// HUD-text tracking - the recommended, fast, automatic method
// =============================================================================
// Hytale's own F7 debug overlay writes a ".NET String" object every frame
// containing a line like "ck (...) Feet (166.48, 146.00, -162.27) Chunk (...)".
// A real String object is trivially recognizable (its first 8 bytes are a
// vtable pointer we already know from the chat hooks - see g_stringVT in
// chat.hpp), so searching for one whose text contains "Feet (" has almost no
// false positives - unlike scanning for coincidentally coordinate-shaped
// numbers. That makes this both faster to find AND automatic on every
// launch: no ASLR handling, no pointer chain, no float/double guess, just
// parse the decimal text. Needs the chat vtable known (type once in chat)
// and F7 toggled on in-game.
//
// Since Hytale likely allocates a fresh string each frame (typical of string
// interpolation), this does NOT lock a fixed address - it keeps re-finding
// the live debug line on a cheap periodic scan instead.

extern std::atomic<bool> g_posHudTextEnabled;
extern std::atomic<bool> g_posHudTextScanning;
extern std::atomic<long long> g_posHudTextLastAttemptMs;
extern std::atomic<long long> g_posHudTextLastScanDurMs;
extern std::atomic<long long> g_posHudTextLastFoundMs;
extern std::atomic<uintptr_t> g_posHudTextAddr; // where the line was last found (diagnostic only)
extern std::atomic<bool> g_posHudTextUsedFastPath; // true if the last scan used the cached region (fast) not a full walk
extern std::mutex g_posHudTextMtx;
extern std::string g_posHudTextLast;            // last matched debug line, for the UI to show

// Turns tracking on/off and persists the choice (position_hudtext.txt) so it
// resumes automatically on the next DLL load. Turning off also unlocks.
void PosHudTextSetEnabled(bool enabled);
void PosHudTextLoadFromDisk(); // called once at startup

// One scan pass, run on a worker thread. On a match, parses "Feet (x, y, z)"
// and updates the shared g_posX/Y/Z/g_posLocked display state directly.
void PosHudTextScanOnce();

// Call once per rendered frame. Self-throttling (only actually scans every
// ~500ms) and a no-op unless g_posHudTextEnabled is set.
void PosHudTextTick();
