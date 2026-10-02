#pragma once
// =============================================================================
// AI-written builds: a small "build script" that expands into a prefab
// =============================================================================
// A good model can design a lovely building, but it cannot reliably write out
// forty thousand `{ "x": 12, "y": 3, "z": -7, "name": ... }` records - it loses
// count, and every one costs tokens. So it writes what an architect would
// instead: fill this box with stone brick, hollow this one out, put a gable
// roof on that footprint, run a staircase up here, mirror the left half. This
// file turns that script into the same prefab JSON the Build tab already loads.
//
// Everything here is plain logic with no game or UI dependency, so it can be
// tested on its own (tests/scripttest) and used by a command line converter.
//
// Two facts about the game are baked in, both read out of real exported
// prefabs rather than guessed:
//
//   * A stair or roof piece's rotation 0..3 says which side is TALL: the
//     neighbour one step up is always north, west, south, east for 0, 1, 2, 3.
//     8..11 are the same pieces upside down. Logs and beams: 0 is upright, 4
//     runs along Z, 5 along X.
//   * The game chooses state variants (stair corners, open/closed doors, fence
//     connections) itself when a block is placed, so a script names the plain
//     block. State names are accepted and reduced to their plain form.
#include "pch.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace BuildScript {

struct Block {
    int32_t x = 0, y = 0, z = 0;
    std::string name;
    int32_t rotation = -1;      // -1: not written to the file
};

// Which block names exist. Built from the game's own registry; when it is empty
// nothing is checked, and the result says so.
class NameBook {
public:
    void Add(const std::string& name);
    bool Empty() const { return names_.empty(); }
    size_t Size() const { return names_.size(); }
    bool Known(const std::string& name) const { return names_.count(name) != 0; }
    // The canonical spelling of a name that differs only by case, or "".
    std::string CaseFix(const std::string& name) const;
    // The closest known names, best first.
    std::vector<std::string> Suggest(const std::string& name, size_t max = 3) const;

private:
    std::unordered_map<std::string, int> names_;         // canonical name -> 1
    std::unordered_map<std::string, std::string> lower_; // lower-case -> canonical
    std::vector<std::string> list_;
};

struct Options {
    size_t maxBlocks = 600000;   // a mistake catcher, not a design limit
    bool stripStates = true;     // "*X_State_Definitions_Y" -> "X"
};

struct Unknown {
    std::string name;
    size_t count = 0;
    std::vector<std::string> suggestions;
};

struct Result {
    std::string name;                   // the script's own name
    std::vector<Block> blocks;          // in build order, never air
    std::vector<std::string> errors;    // the script could not be understood
    std::vector<std::string> warnings;  // it was, but something looks off
    std::vector<Unknown> unknown;       // names the game does not have
    std::vector<std::pair<std::string, size_t>> materials;   // most used first
    int32_t minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    size_t opsRun = 0;
    size_t erased = 0;                  // cells cleared after being filled
    bool checkedNames = false;

    bool Ok() const { return errors.empty() && unknown.empty() && !blocks.empty(); }
    int32_t SizeX() const { return blocks.empty() ? 0 : maxX - minX + 1; }
    int32_t SizeY() const { return blocks.empty() ? 0 : maxY - minY + 1; }
    int32_t SizeZ() const { return blocks.empty() ? 0 : maxZ - minZ + 1; }
};

// Pulls the JSON object out of an AI reply: strips markdown fences and any prose
// around it. Returns "" when there is no object at all.
std::string ExtractJson(const std::string& reply);

// Runs the script. Returns false only when the text is not usable at all; the
// result's errors and unknown names say what else went wrong.
bool Expand(const std::string& scriptText, const NameBook& book, const Options& opt, Result& out);

// The prefab file the Build tab loads (same layout PrefabWriteJson writes).
std::string ToPrefabJson(const Result& r);

// A short human summary, and a message to send back to the AI that lists every
// problem with the fix for each, so it can correct the script and resend it.
std::string Summary(const Result& r);
std::string FixRequest(const Result& r);

// A file-name-safe version of a build's name.
std::string Slug(const std::string& name);

// What a block's rotation means, in words, for the player standing in the world.
// `buildTurns` is how many quarter turns the guide has turned the prefab. Empty
// for blocks whose rotation is not understood - it never guesses out loud.
std::string RotationHint(const std::string& blockName, int rotation, int buildTurns);

// The rotation number for a stair-like block whose TALL side faces `facing`
// ('n','e','s','w'); `upsideDown` selects the 8..11 set. Exposed for tests.
int StairRotation(char facing, bool upsideDown);

} // namespace BuildScript
