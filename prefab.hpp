#pragma once
// =============================================================================
// Prefabs, and building one by hand with the game showing you where
// =============================================================================
// A Hytale prefab is a JSON file listing every block in a structure as an
// offset from an anchor, with a name and sometimes a rotation. That is the same
// information a schematic mod works from, and now that we can draw cubes in the
// world at arbitrary coordinates, it is enough to stand somewhere and be shown
// where the next block goes.
//
// What this does NOT do is place blocks. Nothing here writes to the world or
// sends anything to a server - the whole feature is an overlay on the client's
// own debug renderer plus a list of instructions. You still build it.
//
// Two things about the format are worth knowing before reading the parser:
//
//   * "blocks" is the only array that matters. "fluids" is a parallel list that
//     is almost entirely "Empty", and reading it would double the work to no
//     purpose.
//   * "name" is sometimes prefixed with '*', which marks a block state variant
//     rather than a plain block ("*Wood_Village_Wall_White_Full_State_Definitions_Bottom").
//     We keep the name exactly as written, because that is what identifies it,
//     but the UI shows the readable tail of it.
#include "pch.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct PrefabBlock {
    int32_t x = 0, y = 0, z = 0;   // relative to the prefab's anchor
    int32_t nameId = 0;            // index into Prefab::names
    int32_t rotation = -1;         // as written; -1 when the file omitted it
};

struct Prefab {
    std::string name;              // the file's stem, for the UI
    std::string path;
    std::vector<std::string> names;    // unique block names, in first-seen order
    std::vector<PrefabBlock> blocks;   // already sorted into build order
    int32_t minX = 0, minY = 0, minZ = 0;
    int32_t maxX = 0, maxY = 0, maxZ = 0;
    bool loaded = false;
    size_t airSkipped = 0;             // air cells dropped while parsing

    // Which block sits in which cell, keyed by PrefabCellKey of its LOCAL
    // coordinates. Every placement the client reports has to be turned into
    // "which block was that", and walking fifty thousand blocks to answer it -
    // on the client's network thread, once per block you place - was the old
    // way.
    std::unordered_map<uint64_t, uint32_t> cells;

    // A hash of the blocks and names exactly as sorted. A saved session stores
    // it, so progress recorded against block N is only ever applied to a prefab
    // whose block N is the same block.
    uint64_t fingerprint = 0;

    int32_t SizeX() const { return maxX - minX + 1; }
    int32_t SizeY() const { return maxY - minY + 1; }
    int32_t SizeZ() const { return maxZ - minZ + 1; }
};

// One key per cell. Each axis is masked to 21 bits before shifting: without the
// mask a negative coordinate sign-extends into the next axis and two different
// cells share a key.
uint64_t PrefabCellKey(int32_t x, int32_t y, int32_t z);

// Parses one file. Returns false and fills `err` on anything it cannot make
// sense of. Slow enough for a 100k-block file that it belongs on a worker.
bool PrefabParseFile(const std::string& path, Prefab& out, std::string& err);

// Writes a parsed prefab back out in the same format the loader reads, in its
// sorted order with the anchor already applied - so parsing the result gives
// the same blocks at the same indices and the same fingerprint. That is what a
// session keeps its own copy in: the original file can be edited, moved or
// deleted afterwards and the session still opens exactly as it was.
bool PrefabWriteJson(const Prefab& p, const std::wstring& path, std::string& err);

// Writes beside the target and swaps it in, so a crash leaves the old file or
// the new one and never half of either. With `keepBackup`, the file replaced
// is kept as <path>.bak.
bool WriteFileAtomic(const std::wstring& path, const std::string& data, std::string& err,
    bool keepBackup);

// Lists *.json in the "prefabs" folder next to the DLL, creating the folder if
// it is not there - an empty folder is a clearer instruction than an error.
void PrefabScanFolder();
std::vector<std::string> PrefabGetFiles();     // full paths
std::string PrefabGetFolder();

// The prefab being built, or null. Shared rather than copied: the old
// `Prefab p = g_prefab` copied every block several times a frame for the UI and
// once a frame on the game thread, which for a big statue is megabytes a frame
// spent on nothing.
std::shared_ptr<const Prefab> PrefabCurrent();

// =============================================================================
// A build in progress
// =============================================================================
// Progress is tracked here rather than read back out of the world, because we
// have no way to ask the client what block is at a coordinate. It moves when
// the client reports a placement (follow along), when you say so, or in bulk.
//
// All of it is saved - see buildsession.hpp. Nothing in this file touches the
// disk for progress; it only exports and imports a BuildSnapshot.
enum class BuildBlockState : uint8_t { Todo = 0, Done = 1, Skipped = 2 };

// Where a change came from. Kept in the history so "undo the last thing" and
// "rewind the last five minutes" can say what they are undoing.
enum class BuildSource : uint8_t {
    Manual = 0,    // a button or a hotkey
    Follow = 1,    // the client reported the placement
    Click  = 2,    // right-click counting
    Bulk   = 3,    // a catch-up tool
};

struct BuildTarget {
    bool valid = false;
    int32_t wx = 0, wy = 0, wz = 0;   // world block coordinates
    std::string name;                 // what to place - your swap, if you made one
    std::string wanted;               // what the prefab itself names
    int32_t rotation = -1;
    size_t index = 0;                 // into the prefab's block list
    double distance = 0.0;
    double dx = 0, dy = 0, dz = 0;    // from the player to the block centre
};

// Where the prefab's anchor sits in the world, and how it is turned.
extern std::atomic<int> g_buildOriginX, g_buildOriginY, g_buildOriginZ;
extern std::atomic<int> g_buildRotation;      // 0..3 quarter turns about Y
extern std::atomic<bool> g_buildActive;

// Once you have started building, moving the anchor would move every block you
// have already placed out from under the guide. So the first placement the
// client confirms locks it, and moving it again is a deliberate unlock. The
// arrow keys, the pad and "put it where I am standing" all refuse while locked.
extern std::atomic<bool> g_buildAnchorLocked;

// =============================================================================
// How much of it to draw
// =============================================================================
enum class BuildShow : int {
    WholeBuild = 0,   // every block still to place
    ThisLayer  = 1,   // every block still to place on the layer you are on
    NextFew    = 2    // just the next handful, for working in a tight space
};
extern std::atomic<int> g_buildShowMode;

// Ceiling on outlined blocks. What is outlined is whatever is NEAREST YOU, so
// eight thousand blocks around you is plenty however large the build is. It was
// raised to 32768 once and the client refused a list that size, so nothing drew
// at all - see the history in prefab.cpp before touching it.
constexpr int kBuildMaxGhosts = 8192;

// How many the current mode would draw if nothing were in its way, and how
// many it actually managed. When these differ the shape list is the reason.
extern std::atomic<int> g_buildGhostsNeeded;

// Grow the shape list to fit what is being shown.
extern std::atomic<bool> g_buildAutoCapacity;

extern std::atomic<int>   g_buildGhostCount;    // blocks to outline in NextFew
extern std::atomic<bool>  g_buildLayerOnly;     // the cursor stays on one layer
extern std::atomic<float> g_buildGhostScale;    // cube side, in blocks: 1.0 is one block

// Layers other than the one you are working on are drawn faint, so the shape
// of the build reads without burying the blocks you are placing.
extern std::atomic<bool>  g_buildFadeOtherLayers;
extern std::atomic<float> g_buildOpacityNear;
extern std::atomic<float> g_buildOpacityFar;

// Blocks already placed, drawn as faint green cubes. This is how you check a
// build against the world after a server rollback: a green cube with no block
// in it is one the server lost. Placing the block again fixes the world, and
// the guide already has it as done.
extern std::atomic<bool>  g_buildShowDone;

// Where inside the block the cube is centred. 0.5 assumes block coordinates
// name the corner, which is the usual convention - but it is a number you can
// change rather than one baked in.
extern std::atomic<float> g_buildNudgeX, g_buildNudgeY, g_buildNudgeZ;
extern std::atomic<bool>  g_buildColourByType;  // colour each block by its name

// =============================================================================
// Moving the ghost once you can see it
// =============================================================================
extern std::atomic<bool> g_buildNudgeMode;     // arrow keys move the build
extern std::atomic<int>  g_buildNudgeStep;     // blocks per press

// All three refuse, and return false, while the anchor is locked.
bool BuildMoveAnchor(int dx, int dy, int dz);  // relative, live
bool BuildRotateAnchor(int quarterTurns);
bool BuildSetOriginToPlayer();

// Which way the arrow keys should push the build: the way you are looking.
bool BuildNudgeRelative(int forward, int right, int up);

// A stable colour per block name - the same one the ghost is drawn in - so a
// legend can show which colour means which block.
void BuildColourForName(const std::string& name, float& r, float& g, float& b);

void BuildStart();
void BuildStop();

// Installs a prefab as the build, with every block to do, and clears whatever
// the last one left on screen. Null unloads. The session layer calls this and
// then imports saved progress on top.
void BuildInstallPrefab(std::shared_ptr<const Prefab> p);

// These take a count because the overlay is a separate always-on-top window:
// the natural rhythm is to fill in a batch and then say so once. A count above
// one is a single step in the history, so one Undo takes it back.
void BuildMarkDone(int count = 1);
void BuildSkip(int count = 1);

// Undo is a real history now, not "the last block in build order". With follow
// along you place blocks in whatever order suits you, and undo takes back the
// one you actually just did.
bool BuildUndo(int count = 1);
bool BuildRedo(int count = 1);
size_t BuildUndoDepth();
size_t BuildRedoDepth();

// "Placed Rock_Stone_Brick at 12, 64, -3, 20s ago" - so Undo is never a guess.
std::string BuildLastActionText();

// Puts back everything done in the last `minutes`. A server that restarts can
// roll the world back a few minutes; the guide should roll back with it. It is
// one step in the history, so Redo restores it if you rewound too far.
// Returns how many blocks went back to "to do".
int BuildRewindMinutes(int minutes);

// How many blocks were changed in the last `minutes`, for the UI to show what
// a rewind would do before you press it.
int BuildChangedInLastMinutes(int minutes);

void BuildJumpToLayer(int layer);  // layer is a prefab-local Y
void BuildStepLayer(int direction);  // to the next layer that exists, up or down

// How many blocks the last redraw actually outlined.
extern std::atomic<int> g_buildGhostsDrawn;

BuildTarget BuildGetTarget();
int  BuildCurrentLayer();
int  BuildLayerRemaining();

struct BuildLayerInfo {
    bool valid = false;
    int current = 0;           // prefab-local Y
    int position = 0;          // 1-based among the layers that exist
    int count = 0;             // how many distinct layers
    int todoHere = 0, totalHere = 0;
    int lowestUnfinished = 0;
    bool anyUnfinished = false;
};
BuildLayerInfo BuildGetLayerInfo();

// =============================================================================
// Catching up
// =============================================================================
// For blocks placed while nothing was watching - built before the guide was
// up, on a client without the DLL, or before follow along learned the packet.
// Each is one step in the history.
enum class BuildBulk : int {
    ThisLayer = 0,     // every block on the layer the cursor is on
    BelowLayer = 1,    // every block on every layer below it
    NearMe = 2,        // within `radius` blocks of where you stand
    Everything = 3,
};
int BuildBulkSet(BuildBulk scope, int radius, BuildBlockState to);

// Everything back to Todo. Undoable, unlike the version it replaces.
void BuildReset();

// =============================================================================
// What order to build in
// =============================================================================
enum class BuildOrder : int {
    RowByRow  = 0,   // layer, then row - the order you would lay a floor
    ByMaterial = 1,  // layer, then material - one hotbar switch per material
    MaterialFirst = 2 // material, then layer - fewest switches, least stable
};
extern std::atomic<int> g_buildOrderMode;

// =============================================================================
// Hearing about placements
// =============================================================================
// What happened when the client reported a block at these world coordinates.
enum class BuildPlaceResult : int {
    NotInBuild = 0,    // the build wants nothing there
    Placed = 1,        // it wanted that, and now has it
    AlreadyDone = 2,   // it wanted something there, and already had it
    WrongBlock = 3,    // it wanted something ELSE there
};

// `placedName` is what the client says was placed, "" when it cannot tell. With
// a name, a block the build did not ask for is kept as a mistake rather than
// counted - the red cube stays until the right block goes in.
BuildPlaceResult BuildOnPlacement(int wx, int wy, int wz, const std::string& placedName,
    BuildSource source);

// The same with no material check - the learner and the burst sender use this.
bool BuildMarkPlacedAt(int wx, int wy, int wz);

// What the prefab wants at these world coordinates, "" if nothing.
std::string BuildWantsAt(int wx, int wy, int wz);

// True when the build still wants a block at these world coordinates. Cheap -
// one hash lookup - because the placement learner asks it for every packet.
bool BuildHasTodoAt(int wx, int wy, int wz);

// =============================================================================
// The wrong block
// =============================================================================
// The placement packet carries a block id as well as a position, so a block the
// build did not ask for can be noticed rather than counted.
//
// False means ANY block counts: the build is a shape, and the materials are
// yours to choose. Follow along still advances on every placement; nothing
// ever turns red. Set through BuildSetAnyBlock, which also settles any wrong
// blocks already flagged.
extern std::atomic<bool> g_buildCheckMaterial;
void BuildSetAnyBlock(bool anyBlockCounts);

struct BuildMistake {
    int32_t wx = 0, wy = 0, wz = 0;
    uint32_t index = 0;            // the block the build wanted there
    std::string placedName;
    int64_t unixMs = 0;
};
std::vector<BuildMistake> BuildGetMistakes();
// Counts it as right. With `always`, that material is accepted in place of
// the wanted one for the rest of this build.
void BuildAcceptMistake(size_t i, bool always);
void BuildDismissMistake(size_t i);
void BuildClearMistakes();

// =============================================================================
// Building it in your own materials
// =============================================================================
// A swap says "wherever the prefab wants Rock_Stone_Brick, I am using
// Wood_Oak_Planks". It changes everything that talks about that material: the
// ghost's colour, the name in the target line, the list of blocks you still
// need, the hotbar slot, and what the wrong-block check accepts (either one
// counts). Per build, saved with it.
//
// "" as `mine` removes the swap.
void BuildSetSwap(const std::string& wanted, const std::string& mine);
std::vector<std::pair<std::string, std::string>> BuildGetSwaps();   // wanted -> mine

// The name to show and place for a material the prefab names - the swap if
// there is one, otherwise the name itself.
std::string BuildEffectiveName(const std::string& wanted);

// The last block the client reported you placing, anywhere. The quickest way
// to pick a material for a swap is to place one and say "that one".
std::string BuildLastPlacedName();

// Every block name the game knows, sorted, state variants left out - for
// picking a swap by name.
std::vector<std::string> BuildBlockNameList();

// Writes the build as a prefab with the materials you actually used: the block
// the client saw you place where it knows, your swap where it does not, the
// original name otherwise. `placedKnown` says how many came from what you
// really placed.
bool BuildSaveMyVersion(const std::string& name, std::string& err, size_t& placedKnown);

// =============================================================================
// How it is going
// =============================================================================
struct BuildStats {
    size_t done = 0, skipped = 0, total = 0;
    int64_t activeMs = 0;          // time spent actually placing
    uint64_t placed = 0;           // blocks placed, not bulk-marked
    double perHour = 0.0;          // 0 until there is enough to say
    double etaHours = 0.0;         // 0 when unknown
};
BuildStats BuildGetStats();
void BuildProgress(size_t& done, size_t& skipped, size_t& total);

// One row per material the prefab names, with what you are using for it and
// how many are still to place. Sorted by count, descending. With `thisLayer`,
// only the layer the cursor is on - what you need in your hotbar right now, as
// opposed to what you need to go and gather.
struct BuildMaterialRow {
    std::string wanted;      // as the prefab names it
    std::string mine;        // the effective name: the swap, or `wanted`
    int left = 0;
};
std::vector<BuildMaterialRow> BuildMaterialRows(bool thisLayer = false);

// "14 blocks north-east, 3 up". North is -Z, the same convention the anchor
// pad uses.
std::string BuildDirectionText(double dx, double dy, double dz);

// Redraws the ghosts when they have gone stale. Game thread, once per frame.
// Also notices when the client has thrown our shapes away - a relog, a world
// change, a server restart, the selection box taking the list - and puts the
// build back without being asked.
void BuildFrameTick();

// Non-empty when the ghosts cannot be drawn and it is worth saying why.
std::string BuildGhostStatus();

// =============================================================================
// Saving and restoring
// =============================================================================
// Everything a session needs to put a build back exactly as it was. The prefab
// itself is not in here - the session keeps its own copy of that - only what
// happened to it.
struct BuildHistoryEntry {
    uint32_t index = 0;
    uint8_t from = 0, to = 0;      // BuildBlockState
    uint8_t source = 0;            // BuildSource
    uint32_t group = 0;            // entries sharing a non-zero group undo together
    int64_t unixMs = 0;
};

struct BuildSnapshot {
    int originX = 0, originY = 0, originZ = 0, rotation = 0;
    bool locked = false, shown = false;

    int orderMode = (int)BuildOrder::ByMaterial;
    int layer = 0;
    bool layerValid = false, layerOnly = false;

    int showMode = (int)BuildShow::WholeBuild, ghostCount = 24;
    float scale = 1.0f, nudgeX = 0.5f, nudgeY = 0.5f, nudgeZ = 0.5f;
    float opNear = 0.55f, opFar = 0.15f;
    bool fade = true, byType = true, showDone = false, checkMaterial = true;

    std::vector<uint8_t> state;                 // one BuildBlockState per block
    std::vector<BuildHistoryEntry> history;     // oldest first
    std::vector<BuildHistoryEntry> redo;
    std::vector<BuildMistake> mistakes;
    std::vector<std::pair<std::string, std::string>> swaps;   // wanted -> mine
    // What was really placed in each block, where the client said: an index
    // into placedNames, -1 when unknown. For "save my version".
    std::vector<int32_t> placedAs;
    std::vector<std::string> placedNames;
    int64_t activeMs = 0;
    uint64_t placed = 0;
};
void BuildExportState(BuildSnapshot& out);

// Applies a snapshot to the installed prefab. State is taken only when its
// length matches; everything else always. `note` says what was not restored.
void BuildImportState(const BuildSnapshot& in, std::string& note);

// Goes up whenever anything worth saving changes. The session layer saves
// when this moves, rather than on a timer that writes whether or not anything
// happened.
uint64_t BuildRevision();

// Wall-clock milliseconds, for history and "last worked on".
int64_t BuildUnixMs();

// =============================================================================
// Which hotbar slot holds what
// =============================================================================
// Reading the inventory out of the client is its own reverse-engineering job.
// Writing down "oak trunk is in slot 3" is not, and it answers the same
// question at the moment you actually need it answered - so until the former
// works, this is the latter.
void BuildSetMaterialSlot(const std::string& material, int slot);  // 0 clears
int  BuildGetMaterialSlot(const std::string& material);
bool BuildSaveMaterialSlots();
bool BuildLoadMaterialSlots();

// =============================================================================
// Advancing without leaving the game
// =============================================================================
// The overlay is a separate always-on-top window, so every press of a button
// in it costs a trip out of the game and back. These are polled from the
// overlay's own message loop, exactly as its existing hotkeys are, and work
// while Hytale has focus - which is the only time they matter.
extern std::atomic<bool>  g_buildHotkeys;
extern std::atomic<int>   g_buildKeyAdvance;   // Win32 virtual key codes
extern std::atomic<int>   g_buildKeyUndo;
extern std::atomic<int>   g_buildKeySkip;

// Right-click already means "place a block", so counting it costs no new
// mechanism. It is wrong whenever you right-click at anything else, which is
// why it is off by default and only counts within reach of the target.
extern std::atomic<bool>  g_buildRightClickAdvance;
extern std::atomic<float> g_buildReachBlocks;

extern std::atomic<uint64_t> g_buildHotkeyAdvances;
extern std::atomic<uint64_t> g_buildClickAdvances;
extern std::atomic<uint64_t> g_buildClickIgnored;

// Called from the overlay's message loop. `overlayHwnd` is used to tell a
// right-click in the UI from one in the game.
void BuildPollHotkeys(void* overlayHwnd);

// =============================================================================
// Can the camera be aimed?
// =============================================================================
// Forging the placement packet turned out to be a dead end: the server treats
// it as advisory, so blocks appear locally and are gone on relog. The way that
// cannot fail is to make the CLIENT place the block - point it at the target
// and click, and it builds its own request, with its own raycast, its own
// inventory check and its own item.
//
// All of which rests on one question nobody can answer from here: does writing
// the look angles stick, or does the client recompute them from mouse input
// every frame? This is the cheapest possible way to find out - it turns the
// view by a given number of degrees and you watch whether it moves. No aiming
// maths, no clicking, nothing that could place anything.
bool BuildNudgeYaw(float degrees);

// =============================================================================
// Which number is which block
// =============================================================================
// The placement packet carries a block id, not a name. Normally that would be
// another reverse-engineering job - find the block registry, work out how it
// is keyed - but it answers itself: whenever you place a block the prefab asked
// for, we know the name it wanted AND we see the id that went out. Build one
// house and the table fills in on its own.
//
// Two things fall out of it. Placing the wrong block can be noticed rather than
// silently accepted, and a recorded build can be written back as a prefab with
// real names in it instead of numbers.
void BuildNoteBlockId(const std::string& name, uint32_t id);
uint32_t    BuildGetBlockId(const std::string& name);   // 0 when unknown
std::string BuildGetBlockName(uint32_t id);             // "" when unknown
size_t BuildKnownBlockIds();
bool BuildSaveBlockIds();
bool BuildLoadBlockIds();

// A name that gave two different ids, if one ever has. That would mean the
// field we are reading is not the block id after all, and the table is wrong
// rather than incomplete - worth saying out loud rather than quietly averaging.
std::string BuildBlockIdConflict();

// Any one pair we have learned, for something that needs a known value to go
// hunting with. False when nothing has been learned yet.
bool BuildAnyKnownBlockId(std::string& name, uint32_t& id);

// The pairs we know, longest name first. Searching memory with two of these
// rather than one is the difference between a candidate and an answer: one
// name at one index happens by accident all the time, two do not.
std::vector<std::pair<std::string, uint32_t>> BuildKnownBlockIdPairs(size_t limit);

// Reads pairs out of pasted "/block get" output. That command prints
//   Block at (167, 82, -195) is Rock_Stone_Brick (1,238, support=0)
// for any coordinate, which is the cheapest possible source of known values -
// no placing, no guessing, and it works on blocks somebody else built.
// Returns how many new pairs it learned.
// One block the game itself told us about: a coordinate and what is there.
// The coordinate is the valuable half - a known value at a known place is the
// only kind of clue that has ever cracked anything in this client.
struct BlockProbe {
    int x = 0, y = 0, z = 0;
    uint32_t id = 0;
};

int BuildLearnFromBlockGet(const std::string& text);
std::vector<BlockProbe> BuildGetProbes();

// A block YOU placed, with where it went. Every placement gives both halves
// already, and unlike /block get it needs no permission - it works anywhere you
// can build, which is the case that actually matters.
void BuildAddProbe(int x, int y, int z, uint32_t id);
void BuildClearProbes();

// Drops "Unknown_<id>" entries. A recorded build writes those for blocks it
// could not name, and building from that prefab teaches them back as if they
// were real - which then makes the registry search reject the correct array for
// disagreeing with a placeholder. Returns how many were removed.
int BuildPurgeUnknownNames();

// =============================================================================
// Every block name at once, instead of one per block you place
// =============================================================================
// Learning names by watching only ever names what you have placed, which is why
// a recorded build comes out full of Unknown_<id>. The client obviously knows
// all of them - it has to, to show them to you - so the table exists somewhere
// in memory and the job is to find it rather than to keep filling one in.
//
// The lever is the same one that has worked every other time here: we already
// know a value. Rock_Stone_Brick is 1248. So find the text "Rock_Stone_Brick"
// in memory, find what points at it, and check whether that reference sits at
// index 1248 of an array. If it does, that array is the registry and every
// other id resolves for free.
//
// It verifies before it adopts. An array that merely contains the string is not
// enough - the string has to be at the index its id says it should be.
void BlockRegFind();
extern std::atomic<bool> g_blockRegScanning;
std::string BlockRegStatus();
std::string BlockRegReport();

// Resolved straight out of the client's own table, for any id. Empty when the
// registry has not been found or the id is out of range.
std::string BlockRegLookup(uint32_t id);
bool BlockRegFound();
size_t BlockRegSize();

// Copies the whole registry into the id table and writes it to disk. The array
// is a heap object and its address is different every launch, so finding it is
// a per-session job - but the contents are not. Block ids are a property of the
// game, not of this run, so once they are on disk the search never needs to
// happen again. Returns how many names were taken.
size_t BlockRegSeedNames();

// =============================================================================
// Recording what you build
// =============================================================================
// Reading blocks out of the world would let you draw a box round something that
// already exists and copy it. We cannot read blocks. But we can see every block
// YOU place, with its position and its id - so anything you build can be
// captured as you build it, and saved as a prefab to put up again elsewhere.
extern std::atomic<bool> g_buildRecording;

void BuildRecordStart();
void BuildRecordStop();
void BuildRecordClear();
void BuildRecordAdd(int wx, int wy, int wz, uint32_t id);   // from the watcher
size_t BuildRecordCount();

// Bounds of what has been recorded so far, in world coordinates.
bool BuildRecordBounds(int& minX, int& minY, int& minZ,
    int& maxX, int& maxY, int& maxZ);

// Writes it to prefabs/<name>.json in the same format the loader reads. Blocks
// whose id is not in the table are written as "Unknown_<id>" rather than
// dropped, so nothing is silently lost and the file can be corrected by hand.
bool BuildRecordSavePrefab(const std::string& name, std::string& err);
std::string BuildRecordSummary();
