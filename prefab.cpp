#include "pch.h"
#include "prefab.hpp"
#include <string_view>
#include "common.hpp"
#include "debugshape.hpp"
#include "gamesdk.hpp"
#include "position.hpp"
#include "placewatch.hpp"
#include "hooks.hpp"       // SafeRead, for the one field this writes back

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

// =============================================================================
// Reading the file
// =============================================================================
// This is a scanner rather than a JSON parser, and deliberately so. The shape
// of the document is fixed and shallow - one array of flat objects - while a
// general parser would build a tree of a hundred thousand nodes to be thrown
// away immediately. The scanner walks the text once and writes straight into
// the vector.
//
// It is tolerant in the ways that matter: fields may appear in any order, any
// of them may be missing, and unknown fields ("filler", "support") are ignored
// rather than rejected. It is strict in the one way that matters: a block
// without a name is not a block, and is dropped.
namespace {

void SkipWs(std::string_view s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
}

// Reads a JSON string body starting at the opening quote. Handles the escapes
// that can legally appear in a block name; anything else is copied through.
bool ReadJsonString(std::string_view s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        const char ch = s[i];
        if (ch == '\\') {
            if (i + 1 >= s.size()) return false;
            const char esc = s[i + 1];
            switch (esc) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                // Block names are ASCII in practice; keep the escape intact
                // rather than pretending to decode UTF-16 surrogates.
                const size_t take = (s.size() - i >= 6) ? 6 : (s.size() - i);
                out.append(s, i, take);
                i += take;
                continue;
            }
            default: out += esc; break;
            }
            i += 2;
            continue;
        }
        if (ch == '"') { ++i; return true; }
        out += ch;
        ++i;
    }
    return false;
}

bool ReadJsonInt(std::string_view s, size_t& i, int32_t& out) {
    SkipWs(s, i);
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); ++i; }
    if (i >= s.size() || !isdigit((unsigned char)s[i])) return false;
    long long v = 0;
    while (i < s.size() && isdigit((unsigned char)s[i])) {
        v = v * 10 + (s[i] - '0');
        if (v > 100000000LL) v = 100000000LL;     // nothing sane gets near this
        ++i;
    }
    // A float where an int was expected - consume the rest and round towards 0.
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
    }
    out = (int32_t)(neg ? -v : v);
    return true;
}

// Steps over one JSON value of any type, leaving `i` just past it.
void SkipJsonValue(std::string_view s, size_t& i) {
    SkipWs(s, i);
    if (i >= s.size()) return;
    if (s[i] == '"') { std::string t; ReadJsonString(s, i, t); return; }
    if (s[i] == '{' || s[i] == '[') {
        const char open = s[i];
        const char close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (i < s.size()) {
            if (s[i] == '"') { std::string t; ReadJsonString(s, i, t); continue; }
            if (s[i] == open) ++depth;
            else if (s[i] == close) { --depth; if (depth == 0) { ++i; return; } }
            ++i;
        }
        return;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
}

size_t FindKey(std::string_view s, const char* key, size_t from = 0) {
    const std::string pat = std::string("\"") + key + "\"";
    return s.find(pat, from);
}

// Reads an integer that follows "key": somewhere before `limit`. Used only for
// the three anchor fields, which sit in the header of the document.
void ReadHeaderInt(std::string_view s, const char* key, size_t limit, int32_t& out) {
    const size_t at = FindKey(s, key);
    if (at == std::string::npos || at >= limit) return;
    size_t j = at + strlen(key) + 2;
    SkipWs(s, j);
    if (j < s.size() && s[j] == ':') { ++j; ReadJsonInt(s, j, out); }
}

// A prefab is read by MAPPING it, not by copying it into a string.
//
// The old version loaded the whole file into a std::string and refused anything
// over 128 MB. Both halves of that were wrong for a big build: the copy means
// the file exists twice in memory while parsing, and the limit was an arbitrary
// number rather than anything the machine cared about. A statue with a few
// hundred thousand blocks is an ordinary thing to want and should not need a
// justification.
//
// Mapping hands the parser the file's own pages. Nothing is copied, the address
// space is reserved rather than committed, and Windows pages it in as the parse
// walks forward - which is exactly the access pattern here.
// Small settings files - material slots, the block id table - still read the
// plain way. They are kilobytes, and mapping them would be ceremony.
bool ReadWholeFile(const std::string& path, std::string& out, std::string& err) {
    const std::wstring wide = Utf8ToWide(path);
    HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "could not open the file"; return false; }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        CloseHandle(h);
        err = "the file is empty";
        return false;
    }
    if (size.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(h);
        err = "that settings file is implausibly large";
        return false;
    }

    out.resize((size_t)size.QuadPart);
    size_t total = 0;
    while (total < out.size()) {
        DWORD got = 0;
        if (!::ReadFile(h, &out[total], (DWORD)(out.size() - total), &got, nullptr) || got == 0)
            break;
        total += got;
    }
    CloseHandle(h);
    out.resize(total);
    if (out.empty()) { err = "read nothing back"; return false; }
    return true;
}

struct MappedFile {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const char* data = nullptr;
    size_t size = 0;

    ~MappedFile() {
        if (data)    UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
};

bool MapWholeFile(const std::string& path, MappedFile& out, std::string& err) {
    const std::wstring wide = Utf8ToWide(path);
    out.file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (out.file == INVALID_HANDLE_VALUE) { err = "could not open the file"; return false; }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(out.file, &size) || size.QuadPart <= 0) {
        err = "the file is empty";
        return false;
    }

    // 32-bit address space is the only real ceiling, and this is a 64-bit
    // build, so the only limit worth keeping is one that catches a mistake
    // rather than a large build.
    if (size.QuadPart > 4LL * 1024 * 1024 * 1024) {
        err = "the file is larger than 4 GB - that is not a prefab";
        return false;
    }

    out.mapping = CreateFileMappingW(out.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!out.mapping) { err = "could not map the file"; return false; }

    out.data = (const char*)MapViewOfFile(out.mapping, FILE_MAP_READ, 0, 0, 0);
    if (!out.data) {
        err = "could not map the file into memory";
        return false;
    }
    out.size = (size_t)size.QuadPart;
    return true;
}

std::string FileStem(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

} // namespace

// Prefabs are often exported as a solid box - every cell of the bounding volume,
// air included. The dragon statue was 877,482 blocks for an 82x123x87 box, which
// is exactly its volume, and its first "block to place" was named Empty.
//
// Air is not something anyone places, outlines, or counts towards a build. It
// is dropped at parse time, which turns a 877k build into the few tens of
// thousands that are actually the statue.
uint64_t PrefabCellKey(int32_t x, int32_t y, int32_t z) {
    const uint64_t ux = (uint64_t)((uint32_t)x & 0x1FFFFFu);
    const uint64_t uy = (uint64_t)((uint32_t)y & 0x1FFFFFu);
    const uint64_t uz = (uint64_t)((uint32_t)z & 0x1FFFFFu);
    return ux | (uy << 21) | (uz << 42);
}

namespace {

// Hashes each block's NAME rather than its name index. The index depends on
// which name the file happened to mention first, and a session's own copy of
// the prefab is written in build order, so the same build can number its
// materials differently - the fingerprint must not care.
uint64_t PrefabFingerprint(const Prefab& p) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };

    std::vector<uint64_t> nameHash(p.names.size());
    for (size_t i = 0; i < p.names.size(); ++i) {
        uint64_t n = 1469598103934665603ull;
        for (char ch : p.names[i]) { n ^= (uint8_t)ch; n *= 1099511628211ull; }
        nameHash[i] = n;
    }
    mix(p.blocks.size());
    for (const PrefabBlock& b : p.blocks) {
        mix((uint32_t)b.x);
        mix((uint32_t)b.y);
        mix((uint32_t)b.z);
        mix((b.nameId >= 0 && (size_t)b.nameId < nameHash.size()) ? nameHash[(size_t)b.nameId] : 0);
        mix((uint32_t)b.rotation);
    }
    return h;
}

} // namespace

bool IsAirName(const std::string& n) {
    if (n.empty()) return true;
    std::string low;
    low.reserve(n.size());
    for (char c : n) low += (char)tolower((unsigned char)c);
    return low == "empty" || low == "air" || low == "minecraft:air" ||
           low == "void" || low == "none";
}

bool PrefabParseFile(const std::string& path, Prefab& out, std::string& err) {
    MappedFile mapped;
    if (!MapWholeFile(path, mapped, err)) return false;
    const std::string_view text(mapped.data, mapped.size);


    const size_t blocksAt = FindKey(text, "blocks");
    if (blocksAt == std::string::npos) {
        err = "no \"blocks\" array in this file";
        return false;
    }

    size_t i = blocksAt + 8;
    SkipWs(text, i);
    if (i >= text.size() || text[i] != ':') {
        err = "\"blocks\" is not followed by a value";
        return false;
    }
    ++i;
    SkipWs(text, i);
    if (i >= text.size() || text[i] != '[') {
        err = "\"blocks\" is not an array";
        return false;
    }
    ++i;

    out = Prefab{};
    out.path = path;
    out.name = FileStem(path);

    // Reserve after the reset, not before - assigning a fresh Prefab throws the
    // reservation away. Sized from the file with a ceiling, because a guess of
    // one block per 48 bytes is fine at 100 MB and asks for 1.8 GB at 4 GB.
    {
        const size_t guess = (size_t)(mapped.size / 48) + 16;
        out.blocks.reserve((std::min)(guess, (size_t)4000000));
    }

    // The anchor is what the offsets are relative to, so subtracting it means
    // the point we place in the world is the prefab's own anchor rather than
    // an arbitrary corner of its bounding box.
    int32_t anchorX = 0, anchorY = 0, anchorZ = 0;
    ReadHeaderInt(text, "anchorX", blocksAt, anchorX);
    ReadHeaderInt(text, "anchorY", blocksAt, anchorY);
    ReadHeaderInt(text, "anchorZ", blocksAt, anchorZ);

    std::unordered_map<std::string, int32_t> nameIds;
    bool haveBounds = false;
    size_t airSkipped = 0;
    out.airSkipped = 0;

    for (;;) {
        SkipWs(text, i);
        if (i >= text.size()) break;
        if (text[i] == ']') { ++i; break; }
        if (text[i] == ',') { ++i; continue; }
        if (text[i] != '{') { SkipJsonValue(text, i); continue; }
        ++i;   // past the opening brace

        PrefabBlock b;
        std::string name;
        bool haveX = false, haveY = false, haveZ = false;

        for (;;) {
            SkipWs(text, i);
            if (i >= text.size()) break;
            if (text[i] == '}') { ++i; break; }
            if (text[i] == ',') { ++i; continue; }
            if (text[i] != '"') { ++i; continue; }

            std::string key;
            if (!ReadJsonString(text, i, key)) break;
            SkipWs(text, i);
            if (i < text.size() && text[i] == ':') ++i;
            SkipWs(text, i);

            if (key == "x")             haveX = ReadJsonInt(text, i, b.x);
            else if (key == "y")        haveY = ReadJsonInt(text, i, b.y);
            else if (key == "z")        haveZ = ReadJsonInt(text, i, b.z);
            else if (key == "name")     ReadJsonString(text, i, name);
            else if (key == "rotation") ReadJsonInt(text, i, b.rotation);
            else                        SkipJsonValue(text, i);
        }

        if (name.empty() || !haveX || !haveY || !haveZ) continue;
        // Air is not placed, outlined, or counted. Dropping it here is what
        // turns a box-exported prefab back into the object inside the box.
        if (IsAirName(name)) { ++airSkipped; ++out.airSkipped; continue; }

        b.x -= anchorX;
        b.y -= anchorY;
        b.z -= anchorZ;

        auto it = nameIds.find(name);
        if (it == nameIds.end()) {
            b.nameId = (int32_t)out.names.size();
            nameIds.emplace(name, b.nameId);
            out.names.push_back(name);
        }
        else {
            b.nameId = it->second;
        }

        if (!haveBounds) {
            out.minX = out.maxX = b.x;
            out.minY = out.maxY = b.y;
            out.minZ = out.maxZ = b.z;
            haveBounds = true;
        }
        else {
            if (b.x < out.minX) out.minX = b.x;
            if (b.x > out.maxX) out.maxX = b.x;
            if (b.y < out.minY) out.minY = b.y;
            if (b.y > out.maxY) out.maxY = b.y;
            if (b.z < out.minZ) out.minZ = b.z;
            if (b.z > out.maxZ) out.maxZ = b.z;
        }

        out.blocks.push_back(b);
    }

    if (out.blocks.empty()) { err = "no usable blocks in this file"; return false; }

    // Build order: bottom layer first, then a raster within the layer. That is
    // the order a person builds in - you cannot stand on a floor you have not
    // laid - and it keeps the blocks being outlined within reach of each other.
    std::stable_sort(out.blocks.begin(), out.blocks.end(),
        [](const PrefabBlock& a, const PrefabBlock& b) {
            if (a.y != b.y) return a.y < b.y;
            if (a.z != b.z) return a.z < b.z;
            return a.x < b.x;
        });

    // Two blocks in one cell is not a buildable thing - only one of them can be
    // placed, so the other would sit in the guide as "to do" forever. The sort
    // is stable, so a run of duplicates is in file order; the last one wins, as
    // it would if the file were applied to a world.
    {
        size_t w = 0;
        for (size_t r = 0; r < out.blocks.size(); ++r) {
            const PrefabBlock& b = out.blocks[r];
            if (w > 0) {
                const PrefabBlock& prev = out.blocks[w - 1];
                if (prev.x == b.x && prev.y == b.y && prev.z == b.z) {
                    out.blocks[w - 1] = b;
                    continue;
                }
            }
            out.blocks[w++] = b;
        }
        out.blocks.resize(w);
    }

    out.cells.reserve(out.blocks.size() * 2);
    for (size_t k = 0; k < out.blocks.size(); ++k) {
        const PrefabBlock& b = out.blocks[k];
        out.cells.emplace(PrefabCellKey(b.x, b.y, b.z), (uint32_t)k);
    }
    out.fingerprint = PrefabFingerprint(out);

    out.loaded = true;
    return true;
}

bool PrefabWriteJson(const Prefab& p, const std::wstring& path, std::string& err) {
    // Written in stored order with the anchor already applied, so a parse of
    // this file sorts to exactly the same list: same blocks, same indices, same
    // fingerprint. No name header - a string that happened to be "blocks"
    // would be found before the real array.
    std::string json;
    json.reserve(p.blocks.size() * 64 + 256);
    json += "{\n  \"version\": 8,\n  \"blockIdVersion\": 10,\n";
    json += "  \"anchorX\": 0,\n  \"anchorY\": 0,\n  \"anchorZ\": 0,\n";
    json += "  \"blocks\": [\n";
    for (size_t i = 0; i < p.blocks.size(); ++i) {
        const PrefabBlock& b = p.blocks[i];
        char head[128];
        snprintf(head, sizeof(head), "    { \"x\": %d, \"y\": %d, \"z\": %d, \"name\": \"",
            b.x, b.y, b.z);
        json += head;
        if (b.nameId >= 0 && (size_t)b.nameId < p.names.size())
            json += JsonEscape(p.names[(size_t)b.nameId]);
        json += "\"";
        if (b.rotation != -1) {
            char rot[40];
            snprintf(rot, sizeof(rot), ", \"rotation\": %d", b.rotation);
            json += rot;
        }
        json += (i + 1 < p.blocks.size()) ? " },\n" : " }\n";
    }
    json += "  ],\n  \"fluids\": []\n}\n";
    return WriteFileAtomic(path, json, err, false);
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data, std::string& err,
    bool keepBackup) {
    // Written beside the real file and swapped in, so a crash - the client's,
    // or the whole machine's - leaves either the old file or the new one, never
    // half of one. That is the entire point of saving a build as you go.
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "could not create the file"; return false; }

    size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
        const DWORD want = (DWORD)(std::min)(data.size() - done, (size_t)(1u << 24));
        DWORD wrote = 0;
        if (!WriteFile(h, data.data() + done, want, &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        done += wrote;
    }
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        err = "the write did not complete";
        return false;
    }

    // ReplaceFile keeps the file being replaced as the backup, which is one
    // generation of history for free. It needs the target to exist, so a first
    // write falls through to a plain move.
    if (keepBackup && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring bak = path + L".bak";
        if (ReplaceFileW(path.c_str(), tmp.c_str(), bak.c_str(),
                REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
            return true;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        err = "could not put the new file in place";
        return false;
    }
    return true;
}

// =============================================================================
// The folder
// =============================================================================
namespace {

std::mutex g_prefabFilesMtx;
std::vector<std::string> g_prefabFiles;

} // namespace

std::string PrefabGetFolder() {
    return WideToUtf8Path(GetThisModuleDirectory() + L"prefabs");
}

void PrefabScanFolder() {
    const std::wstring dir = GetThisModuleDirectory() + L"prefabs";
    // Creating it is the point: an empty folder with an obvious name says what
    // to do far better than an error message about a missing path.
    CreateDirectoryW(dir.c_str(), nullptr);

    std::vector<std::string> found;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            found.push_back(WideToUtf8Path(dir + L"\\" + fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(found.begin(), found.end());

    std::lock_guard lock(g_prefabFilesMtx);
    g_prefabFiles.swap(found);
}

std::vector<std::string> PrefabGetFiles() {
    std::lock_guard lock(g_prefabFilesMtx);
    return g_prefabFiles;
}

// =============================================================================
// Build state
// =============================================================================
std::atomic<int>   g_buildOriginX{ 0 };
std::atomic<int>   g_buildOriginY{ 0 };
std::atomic<int>   g_buildOriginZ{ 0 };
std::atomic<int>   g_buildRotation{ 0 };
std::atomic<bool>  g_buildActive{ false };
std::atomic<bool>  g_buildAnchorLocked{ false };
std::atomic<int>   g_buildShowMode{ (int)BuildShow::WholeBuild };
std::atomic<int>   g_buildGhostsNeeded{ 0 };
std::atomic<bool>  g_buildAutoCapacity{ true };
std::atomic<int>   g_buildGhostCount{ 24 };
std::atomic<bool>  g_buildLayerOnly{ false };
// One block, not half of one. The matrix diagonal turned out to be the cube's
// side length rather than a half-extent: at 0.5 a solid floor came out as a
// grid of separated cubes with a half-block gap between each.
std::atomic<float> g_buildGhostScale{ 1.0f };
std::atomic<bool>  g_buildFadeOtherLayers{ true };
std::atomic<float> g_buildOpacityNear{ 0.55f };
std::atomic<float> g_buildOpacityFar{ 0.15f };
std::atomic<bool>  g_buildShowDone{ false };
std::atomic<bool>  g_buildColourByType{ true };
std::atomic<int>   g_buildGhostsDrawn{ 0 };
std::atomic<float> g_buildNudgeX{ 0.5f };
std::atomic<float> g_buildNudgeY{ 0.5f };
std::atomic<float> g_buildNudgeZ{ 0.5f };
std::atomic<int>   g_buildOrderMode{ (int)BuildOrder::ByMaterial };
std::atomic<bool>  g_buildCheckMaterial{ true };

namespace {

// -----------------------------------------------------------------------------
// Everything about the build in progress, in one place, under one lock.
//
// The old version kept these as a dozen separate globals and recomputed its
// totals by walking every block whenever anyone asked - which the UI did four
// times a frame. Now every change goes through SetStateLocked, which keeps the
// counts in step, so asking "how much is left" costs nothing.
// -----------------------------------------------------------------------------
struct BuildCore {
    std::shared_ptr<const Prefab> prefab;
    std::vector<uint8_t> state;          // one BuildBlockState per block
    std::vector<uint32_t> order;         // block indices, in the order to visit them
    std::vector<uint32_t> orderPos;      // block index -> its position in `order`
    int orderBuiltFor = -1;              // which BuildOrder `order` reflects
    size_t cursor = 0;                   // nothing before here in `order` is Todo
    int layer = 0;                       // prefab-local Y the cursor is on
    bool layerValid = false;

    size_t done = 0, skipped = 0;
    std::vector<int32_t> nameTodo;       // per material: blocks still to place
    std::map<int32_t, int32_t> layerTodo, layerTotal;

    std::vector<BuildHistoryEntry> history, redo;
    uint32_t nextGroup = 1;

    std::vector<BuildMistake> mistakes;

    // Your own materials. `effective` is the prefab's name list with every
    // swap applied, rebuilt whenever a swap changes, so the hot paths - the
    // ghost walk, the target line - never search the swap list.
    std::vector<std::pair<std::string, std::string>> swaps;   // wanted -> mine
    std::vector<std::string> effective;  // per nameId

    // What really went into each block, where the client said. Names are kept
    // once in a table; per block is an index into it, -1 when unknown.
    std::vector<int32_t> placedAs;
    std::vector<std::string> placedNames;
    std::unordered_map<std::string, int32_t> placedNameIdx;

    int64_t activeMs = 0;
    uint64_t placed = 0;
    int64_t lastPlaceUnixMs = 0;
};

std::mutex g_buildMtx;
BuildCore g_core;                        // guarded by g_buildMtx
// Outlives any one build: "the block I last placed" is about your hand, not
// about which prefab happens to be open.
std::string g_lastPlacedName;            // guarded by g_buildMtx
std::atomic<uint64_t> g_buildRev{ 1 };

// Big enough that a whole evening of building, or a bulk mark of a big layer,
// stays undoable. 24 bytes an entry, so the ceiling is a few megabytes.
constexpr size_t kHistoryCap = 300000;

void Touch() { g_buildRev.fetch_add(1, std::memory_order_relaxed); }

// What is on screen right now, keyed by the cell it belongs to.
//
// Placing a block changes exactly two shapes: the one just placed stops being
// outlined, and the next one turns white. Keeping this means the usual case is
// one removal and one recolour rather than a full clear and refill.
enum GhostKind : uint8_t { GhostTodo = 0, GhostTarget = 1, GhostDone = 2, GhostWrong = 3 };
struct DrawnGhost { int32_t nameId; int32_t layer; uint8_t kind; float opacity; };
std::unordered_map<uint64_t, DrawnGhost> g_drawnGhosts;   // guarded by g_buildMtx

// Anyone may ask for the ghost bookkeeping to be thrown away; the frame tick is
// the one that does it, because the game-thread state lives there.
std::atomic<bool> g_ghostResetPending{ false };

// A ceiling that comes down when draws stop completing. The redraw loop ran
// away once and took the game with it; rather than trust that the diagnosis
// was complete, the outline shrinks whenever draws keep failing.
std::atomic<int> g_ghostAdaptiveLimit{ kBuildMaxGhosts };
std::atomic<int> g_ghostFailStreak{ 0 };

std::mutex g_ghostMtx;
std::string g_ghostStatusText;
std::atomic<uint64_t> g_ghostSignature{ 0 };

void SetGhostStatus(const std::string& s) {
    std::lock_guard lock(g_ghostMtx);
    g_ghostStatusText = s;
}

int64_t NowUnixMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

void RotateXZ(int rot, int32_t x, int32_t z, int32_t& ox, int32_t& oz) {
    switch (rot & 3) {
    case 1:  ox = -z; oz = x;  break;
    case 2:  ox = -x; oz = -z; break;
    case 3:  ox = z;  oz = -x; break;
    default: ox = x;  oz = z;  break;
    }
}

void WorldPos(const PrefabBlock& b, int rot, int ox, int oy, int oz,
    int32_t& wx, int32_t& wy, int32_t& wz) {
    int32_t rx = 0, rz = 0;
    RotateXZ(rot, b.x, b.z, rx, rz);
    wx = ox + rx;
    wy = oy + b.y;
    wz = oz + rz;
}

// A world cell back to the prefab's own coordinates: undo the origin, then turn
// the other way. Three quarter turns undo one.
void LocalFromWorld(int wx, int wy, int wz, int32_t& lx, int32_t& ly, int32_t& lz) {
    const int rot = g_buildRotation.load();
    RotateXZ((4 - (rot & 3)) & 3, wx - g_buildOriginX.load(), wz - g_buildOriginZ.load(), lx, lz);
    ly = wy - g_buildOriginY.load();
}

const std::string& NameOfLocked(const PrefabBlock& b) {
    static const std::string kNone;
    const Prefab& p = *g_core.prefab;
    if (b.nameId < 0 || (size_t)b.nameId >= p.names.size()) return kNone;
    return p.names[(size_t)b.nameId];
}

// What to actually place for this block: your swap for its material, if any.
const std::string& EffectiveOfLocked(const PrefabBlock& b) {
    const std::vector<std::string>& e = g_core.effective;
    if (b.nameId < 0 || (size_t)b.nameId >= e.size()) return NameOfLocked(b);
    return e[(size_t)b.nameId];
}

void RebuildEffectiveLocked() {
    BuildCore& c = g_core;
    c.effective.clear();
    if (!c.prefab) return;
    c.effective = c.prefab->names;
    for (std::string& e : c.effective)
        for (const auto& sw : c.swaps)
            if (sw.first == e) { e = sw.second; break; }
}

int32_t PlacedNameIndexLocked(const std::string& n) {
    if (n.empty()) return -1;
    BuildCore& c = g_core;
    const auto it = c.placedNameIdx.find(n);
    if (it != c.placedNameIdx.end()) return it->second;
    const int32_t idx = (int32_t)c.placedNames.size();
    c.placedNames.push_back(n);
    c.placedNameIdx.emplace(n, idx);
    return idx;
}

void NotePlacedAsLocked(uint32_t idx, const std::string& placedName) {
    BuildCore& c = g_core;
    if (placedName.empty() || idx >= c.placedAs.size()) return;
    const int32_t v = PlacedNameIndexLocked(placedName);
    if (c.placedAs[idx] != v) { c.placedAs[idx] = v; Touch(); }
}

// Recomputes every derived count from the state vector. Only on install and
// import - every other change keeps them in step one block at a time.
void RecountLocked() {
    BuildCore& c = g_core;
    c.done = c.skipped = 0;
    c.layerTodo.clear();
    c.layerTotal.clear();
    c.nameTodo.assign(c.prefab ? c.prefab->names.size() : 0, 0);
    c.cursor = 0;
    if (!c.prefab) return;

    const std::vector<PrefabBlock>& blocks = c.prefab->blocks;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const PrefabBlock& b = blocks[i];
        ++c.layerTotal[b.y];
        const uint8_t s = c.state[i];
        if (s == (uint8_t)BuildBlockState::Done) ++c.done;
        else if (s == (uint8_t)BuildBlockState::Skipped) ++c.skipped;
        else {
            ++c.layerTodo[b.y];
            if (b.nameId >= 0 && (size_t)b.nameId < c.nameTodo.size()) ++c.nameTodo[(size_t)b.nameId];
        }
    }
    // Every layer gets an entry, so "is this layer finished" is one lookup.
    for (const auto& kv : c.layerTotal) c.layerTodo.emplace(kv.first, 0);
}

// Rebuilds the visiting order. The blocks never move, so progress recorded
// against them survives a change of mind about the order.
void RebuildOrderLocked() {
    BuildCore& c = g_core;
    const int mode = g_buildOrderMode.load();
    const size_t n = c.prefab ? c.prefab->blocks.size() : 0;
    c.order.resize(n);
    for (size_t i = 0; i < n; ++i) c.order[i] = (uint32_t)i;

    if (n) {
        const PrefabBlock* b = c.prefab->blocks.data();
        if (mode == (int)BuildOrder::ByMaterial) {
            std::stable_sort(c.order.begin(), c.order.end(),
                [b](uint32_t l, uint32_t r) {
                    if (b[l].y != b[r].y) return b[l].y < b[r].y;
                    if (b[l].nameId != b[r].nameId) return b[l].nameId < b[r].nameId;
                    if (b[l].z != b[r].z) return b[l].z < b[r].z;
                    return b[l].x < b[r].x;
                });
        }
        else if (mode == (int)BuildOrder::MaterialFirst) {
            std::stable_sort(c.order.begin(), c.order.end(),
                [b](uint32_t l, uint32_t r) {
                    if (b[l].nameId != b[r].nameId) return b[l].nameId < b[r].nameId;
                    if (b[l].y != b[r].y) return b[l].y < b[r].y;
                    if (b[l].z != b[r].z) return b[l].z < b[r].z;
                    return b[l].x < b[r].x;
                });
        }
        // RowByRow needs no sort: the prefab is already stored in that order.
    }

    c.orderPos.assign(n, 0);
    for (size_t k = 0; k < n; ++k) c.orderPos[c.order[k]] = (uint32_t)k;
    c.orderBuiltFor = mode;
    c.cursor = 0;
}

void EnsureOrderLocked() {
    BuildCore& c = g_core;
    if (!c.prefab) return;
    if (c.order.size() != c.prefab->blocks.size() ||
        c.orderBuiltFor != g_buildOrderMode.load())
        RebuildOrderLocked();
}

// Drops the oldest entries, never splitting a group: half an undo step is
// worse than none.
void TrimHistoryLocked(std::vector<BuildHistoryEntry>& h) {
    if (h.size() <= kHistoryCap) return;
    size_t cut = h.size() - kHistoryCap / 2;
    const uint32_t g = h[cut - 1].group;
    while (g && cut < h.size() && h[cut].group == g) ++cut;
    h.erase(h.begin(), h.begin() + (ptrdiff_t)cut);
}

// The ONLY place a block's state changes. Every count that depends on it is
// adjusted here, so they can never drift from the state vector.
bool SetStateLocked(uint32_t idx, BuildBlockState toState, BuildSource src,
    uint32_t group, bool record, int64_t nowMs) {
    BuildCore& c = g_core;
    if (!c.prefab || idx >= c.state.size()) return false;
    const uint8_t from = c.state[idx];
    const uint8_t to = (uint8_t)toState;
    if (from == to) return false;

    const PrefabBlock& b = c.prefab->blocks[idx];
    const bool named = b.nameId >= 0 && (size_t)b.nameId < c.nameTodo.size();

    if (from == (uint8_t)BuildBlockState::Done)         --c.done;
    else if (from == (uint8_t)BuildBlockState::Skipped) --c.skipped;
    else { --c.layerTodo[b.y]; if (named) --c.nameTodo[(size_t)b.nameId]; }

    if (to == (uint8_t)BuildBlockState::Done)           ++c.done;
    else if (to == (uint8_t)BuildBlockState::Skipped)   ++c.skipped;
    else {
        ++c.layerTodo[b.y];
        if (named) ++c.nameTodo[(size_t)b.nameId];
        if (idx < c.placedAs.size()) c.placedAs[idx] = -1;
        // Something behind the cursor is open again, so the cursor goes back.
        if (idx < c.orderPos.size() && c.orderPos[idx] < c.cursor) c.cursor = c.orderPos[idx];
    }
    c.state[idx] = to;

    if (record) {
        c.history.push_back(BuildHistoryEntry{ idx, from, to, (uint8_t)src, group, nowMs });
        TrimHistoryLocked(c.history);
        c.redo.clear();
    }
    Touch();
    return true;
}

// Index of the next block to place, honouring the layer filter. SIZE_MAX when
// there is nothing left. Starts from the cursor rather than the beginning, so
// a nearly finished build does not rescan everything it has already done.
size_t NextIndexLocked(bool layerOnly, int layer) {
    EnsureOrderLocked();
    BuildCore& c = g_core;
    if (!c.prefab) return SIZE_MAX;
    while (c.cursor < c.order.size() &&
           c.state[c.order[c.cursor]] != (uint8_t)BuildBlockState::Todo) ++c.cursor;
    for (size_t k = c.cursor; k < c.order.size(); ++k) {
        const uint32_t idx = c.order[k];
        if (c.state[idx] != (uint8_t)BuildBlockState::Todo) continue;
        if (layerOnly && c.prefab->blocks[idx].y != layer) continue;
        return idx;
    }
    return SIZE_MAX;
}

bool FirstUnfinishedLayerLocked(int& outLayer) {
    for (const auto& kv : g_core.layerTodo)
        if (kv.second > 0) { outLayer = kv.first; return true; }
    return false;
}

// Keeps the layer cursor somewhere useful after something changed. A finished
// layer is left for the next thing to do: with "stay on this layer" that is the
// lowest unfinished layer, otherwise it is wherever the next target is.
//
// Only called after changes and when the layer is not yet known - never every
// frame - so jumping to a finished layer to look at it stays put until you
// next place something.
void FixLayerLocked() {
    BuildCore& c = g_core;
    if (!c.prefab) return;
    bool need = !c.layerValid;
    if (!need) {
        const auto it = c.layerTodo.find(c.layer);
        need = (it == c.layerTodo.end() || it->second <= 0);
    }
    if (!need) return;

    if (g_buildLayerOnly.load()) {
        if (FirstUnfinishedLayerLocked(c.layer)) c.layerValid = true;
    }
    else {
        const size_t idx = NextIndexLocked(false, 0);
        if (idx != SIZE_MAX) { c.layer = c.prefab->blocks[idx].y; c.layerValid = true; }
        else if (!c.layerValid && !c.layerTotal.empty()) {
            c.layer = c.layerTotal.begin()->first;
            c.layerValid = true;
        }
    }
}

// The block being asked for: the white one.
size_t TargetIndexLocked() {
    BuildCore& c = g_core;
    if (!c.prefab) return SIZE_MAX;
    if (!c.layerValid) FixLayerLocked();
    const bool layerOnly = g_buildLayerOnly.load();
    size_t idx = NextIndexLocked(layerOnly, c.layer);
    if (idx == SIZE_MAX && layerOnly && FirstUnfinishedLayerLocked(c.layer)) {
        c.layerValid = true;
        idx = NextIndexLocked(true, c.layer);
    }
    return idx;
}

// The block that belongs at these world coordinates, or SIZE_MAX. One hash
// lookup - this used to walk every block, on the network thread, per placement.
size_t FindIndexAtWorldLocked(int wx, int wy, int wz) {
    const BuildCore& c = g_core;
    if (!c.prefab) return SIZE_MAX;
    int32_t lx = 0, ly = 0, lz = 0;
    LocalFromWorld(wx, wy, wz, lx, ly, lz);
    const auto it = c.prefab->cells.find(PrefabCellKey(lx, ly, lz));
    return it == c.prefab->cells.end() ? SIZE_MAX : (size_t)it->second;
}

// Two names for the same block, as far as building is concerned. The '*' marks
// a state variant, and a state definition suffix names one of a block's forms
// ("_State_Definitions_Bottom") - the client reports placing the block, the
// prefab names the form it settled into, and both are right.
std::string BlockFamily(const std::string& n) {
    std::string s = n;
    if (!s.empty() && s[0] == '*') s.erase(0, 1);
    s = ToLower(s);
    const size_t st = s.find("_state_definitions");
    if (st != std::string::npos) s.resize(st);
    return s;
}

// Either the block the prefab names or your swap for it counts. The original
// is still accepted after a swap: choosing oak over stone does not make a stone
// block you had lying about wrong.
bool AcceptableLocked(const std::string& wanted, const std::string& placed) {
    if (placed.empty() || wanted.empty()) return true;   // cannot tell, so no complaint
    const std::string fam = BlockFamily(placed);
    if (BlockFamily(wanted) == fam) return true;
    for (const auto& sw : g_core.swaps)
        if (sw.first == wanted && BlockFamily(sw.second) == fam) return true;
    return false;
}

void RemoveMistakeLocked(uint32_t idx) {
    auto& m = g_core.mistakes;
    const size_t before = m.size();
    m.erase(std::remove_if(m.begin(), m.end(),
        [idx](const BuildMistake& x) { return x.index == idx; }), m.end());
    if (m.size() != before) Touch();
}

// Time spent building is the gaps between placements, as long as the gaps are
// short enough that you were plausibly still at it. Walking off for tea does
// not count.
void NotePlacedLocked(int64_t nowMs) {
    BuildCore& c = g_core;
    if (c.lastPlaceUnixMs && nowMs > c.lastPlaceUnixMs && nowMs - c.lastPlaceUnixMs < 90000)
        c.activeMs += nowMs - c.lastPlaceUnixMs;
    c.lastPlaceUnixMs = nowMs;
    ++c.placed;
}

// Marks the target and moves on.
bool AdvanceLocked(BuildBlockState to, BuildSource src, uint32_t group, int64_t nowMs) {
    const size_t idx = TargetIndexLocked();
    if (idx == SIZE_MAX) return false;
    SetStateLocked((uint32_t)idx, to, src, group, true, nowMs);
    if (to == BuildBlockState::Done) {
        RemoveMistakeLocked((uint32_t)idx);
        NotePlacedLocked(nowMs);
    }
    FixLayerLocked();
    return true;
}

void AdvanceFrom(BuildBlockState to, BuildSource src, int count) {
    if (count < 1) return;
    std::lock_guard lock(g_buildMtx);
    if (!g_core.prefab) return;
    const int64_t now = NowUnixMs();
    // More than one is a single step in the history.
    const uint32_t group = count > 1 ? g_core.nextGroup++ : 0;
    for (int i = 0; i < count; ++i)
        if (!AdvanceLocked(to, src, group, now)) break;
}

bool UndoOneLocked() {
    BuildCore& c = g_core;
    if (c.history.empty()) return false;
    const uint32_t g = c.history.back().group;
    do {
        const BuildHistoryEntry e = c.history.back();
        c.history.pop_back();
        SetStateLocked(e.index, (BuildBlockState)e.from, BuildSource::Manual, 0, false, 0);
        c.redo.push_back(e);
        if (e.index < c.prefab->blocks.size()) {
            c.layer = c.prefab->blocks[e.index].y;
            c.layerValid = true;
        }
    } while (g && !c.history.empty() && c.history.back().group == g);
    Touch();
    return true;
}

bool RedoOneLocked() {
    BuildCore& c = g_core;
    if (c.redo.empty()) return false;
    const uint32_t g = c.redo.back().group;
    do {
        const BuildHistoryEntry e = c.redo.back();
        c.redo.pop_back();
        SetStateLocked(e.index, (BuildBlockState)e.to, BuildSource::Manual, 0, false, 0);
        c.history.push_back(e);
        if (e.index < c.prefab->blocks.size()) {
            c.layer = c.prefab->blocks[e.index].y;
            c.layerValid = true;
        }
    } while (g && !c.redo.empty() && c.redo.back().group == g);
    Touch();
    return true;
}

// Current player position, from whichever source is working.
bool PlayerPos(double& x, double& y, double& z) {
    const GsdkPlayerInfo me = GsdkGetPlayerInfo();
    if (me.valid) { x = me.x; y = me.y; z = me.z; return true; }
    if (NowSteadyMs() - g_posLastUpdateMs.load() < 3000) {
        x = g_posX.load(); y = g_posY.load(); z = g_posZ.load();
        return true;
    }
    return false;
}

std::string ShortTimeAgo(int64_t unixMs) {
    const int64_t s = (NowUnixMs() - unixMs) / 1000;
    char buf[48];
    if (s < 5)           return "just now";
    if (s < 90)          snprintf(buf, sizeof(buf), "%llds ago", (long long)s);
    else if (s < 5400)   snprintf(buf, sizeof(buf), "%lld min ago", (long long)(s / 60));
    else if (s < 172800) snprintf(buf, sizeof(buf), "%lld h ago", (long long)(s / 3600));
    else                 snprintf(buf, sizeof(buf), "%lld days ago", (long long)(s / 86400));
    return buf;
}

} // namespace

int64_t BuildUnixMs() { return NowUnixMs(); }
uint64_t BuildRevision() { return g_buildRev.load(std::memory_order_relaxed); }

std::string BuildGhostStatus() {
    std::lock_guard lock(g_ghostMtx);
    return g_ghostStatusText;
}

std::shared_ptr<const Prefab> PrefabCurrent() {
    std::lock_guard lock(g_buildMtx);
    return g_core.prefab;
}

void BuildInstallPrefab(std::shared_ptr<const Prefab> p) {
    {
        std::lock_guard lock(g_buildMtx);
        g_core = BuildCore{};
        g_core.prefab = std::move(p);
        if (g_core.prefab) {
            g_core.state.assign(g_core.prefab->blocks.size(), (uint8_t)BuildBlockState::Todo);
            g_core.placedAs.assign(g_core.prefab->blocks.size(), -1);
        }
        RecountLocked();
        RebuildEffectiveLocked();
        g_drawnGhosts.clear();
    }
    Touch();

    // Everything the previous build left behind. Without the queue clear, the
    // tail of the last build keeps spawning on top of the new one.
    DbgClearSpawnQueue();
    DbgRequestClearAll();
    DbgForgetTags();
    // The wanted capacity is only ever RAISED while a build is shown, so a
    // huge prefab left every later one asking for a list the client would not
    // build. Installing a build is the moment it is allowed back down.
    g_dbgWantedCapacity.store(64);
    g_ghostAdaptiveLimit.store(kBuildMaxGhosts);
    g_ghostFailStreak.store(0);
    g_ghostSignature.store(0);
    g_ghostResetPending.store(true);
    g_buildGhostsDrawn.store(0);
    g_buildGhostsNeeded.store(0);
    SetGhostStatus("");
}

bool BuildMoveAnchor(int dx, int dy, int dz) {
    if (g_buildAnchorLocked.load()) return false;
    if (dx) g_buildOriginX.fetch_add(dx);
    if (dy) g_buildOriginY.fetch_add(dy);
    if (dz) g_buildOriginZ.fetch_add(dz);
    Touch();
    return true;
}

bool BuildRotateAnchor(int quarterTurns) {
    if (g_buildAnchorLocked.load()) return false;
    const int now = g_buildRotation.load();
    g_buildRotation.store(((now + quarterTurns) % 4 + 4) % 4);
    Touch();
    return true;
}

bool BuildNudgeRelative(int forward, int right, int up) {
    // Move it the way the player is looking, so "forward" pushes the build
    // away from you whichever way you happen to be facing.
    const GsdkPlayerInfo me = GsdkGetPlayerInfo();
    int dx = 0, dz = 0;
    if (me.valid && me.haveRotation) {
        // Snap the yaw to the nearest quarter turn: a build nudged along a
        // diagonal is never what anyone wanted.
        const float yaw = me.yaw;
        int q = (int)floorf((yaw + 45.0f) / 90.0f) & 3;
        //  0 = north (-Z), 1 = east (+X), 2 = south (+Z), 3 = west (-X)
        const int fx[4] = { 0, 1, 0, -1 };
        const int fz[4] = { -1, 0, 1, 0 };
        dx = fx[q] * forward + fx[(q + 1) & 3] * right;
        dz = fz[q] * forward + fz[(q + 1) & 3] * right;
    }
    else {
        dx = right;
        dz = -forward;
    }
    return BuildMoveAnchor(dx, up, dz);
}

void BuildColourForName(const std::string& name, float& r, float& g, float& b) {
    // A stable colour per block type, so two materials next to each other do
    // not read as one shape. Hue from the name, high value, so nothing comes
    // out muddy against the world.
    uint32_t h = 2166136261u;
    for (char ch : name) { h ^= (uint8_t)ch; h *= 16777619u; }

    const float hue = (float)(h % 360u);
    const float sat = 0.75f, val = 1.0f;
    const float c = val * sat;
    const float x = c * (1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f));
    const float m = val - c;

    float rr = 0.0f, gg = 0.0f, bb = 0.0f;
    if (hue < 60.0f)       { rr = c; gg = x; }
    else if (hue < 120.0f) { rr = x; gg = c; }
    else if (hue < 180.0f) { gg = c; bb = x; }
    else if (hue < 240.0f) { gg = x; bb = c; }
    else if (hue < 300.0f) { rr = x; bb = c; }
    else                   { rr = c; bb = x; }

    r = rr + m; g = gg + m; b = bb + m;
}

bool BuildSetOriginToPlayer() {
    if (g_buildAnchorLocked.load()) return false;
    double x = 0, y = 0, z = 0;
    if (!PlayerPos(x, y, z)) {
        x = g_posX.load(); y = g_posY.load(); z = g_posZ.load();
    }
    g_buildOriginX.store((int)std::floor(x));
    g_buildOriginY.store((int)std::floor(y));
    g_buildOriginZ.store((int)std::floor(z));
    Touch();
    return true;
}

void BuildStart() {
    g_buildActive.store(true);
    // Ghosts are ordinary debug shapes and expire the same way anything else
    // does, so a build is the one case where keeping them alive is not a choice.
    g_dbgKeepAlive.store(true);
    g_ghostResetPending.store(true);
    Touch();
}

void BuildStop() {
    const bool wasActive = g_buildActive.exchange(false);
    if (wasActive) {
        DbgClearSpawnQueue();
        DbgRequestClearAll();
    }
    {
        std::lock_guard lock(g_buildMtx);
        g_drawnGhosts.clear();
    }
    DbgForgetTags();
    g_ghostSignature.store(0);
    SetGhostStatus("");
    Touch();
}

void BuildReset() {
    BuildBulkSet(BuildBulk::Everything, 0, BuildBlockState::Todo);
    std::lock_guard lock(g_buildMtx);
    g_core.layerValid = false;
    FixLayerLocked();
}

void BuildMarkDone(int count) { AdvanceFrom(BuildBlockState::Done, BuildSource::Manual, count); }
void BuildSkip(int count)     { AdvanceFrom(BuildBlockState::Skipped, BuildSource::Manual, count); }

bool BuildUndo(int count) {
    std::lock_guard lock(g_buildMtx);
    if (!g_core.prefab) return false;
    bool any = false;
    for (int i = 0; i < count; ++i) { if (!UndoOneLocked()) break; any = true; }
    return any;
}

bool BuildRedo(int count) {
    std::lock_guard lock(g_buildMtx);
    if (!g_core.prefab) return false;
    bool any = false;
    for (int i = 0; i < count; ++i) { if (!RedoOneLocked()) break; any = true; }
    return any;
}

size_t BuildUndoDepth() { std::lock_guard lock(g_buildMtx); return g_core.history.size(); }
size_t BuildRedoDepth() { std::lock_guard lock(g_buildMtx); return g_core.redo.size(); }

std::string BuildLastActionText() {
    std::lock_guard lock(g_buildMtx);
    const BuildCore& c = g_core;
    if (!c.prefab || c.history.empty()) return std::string();
    const BuildHistoryEntry& e = c.history.back();

    // A group is one step, so describe it as one.
    size_t n = 1;
    if (e.group)
        for (size_t k = c.history.size() - 1; k-- > 0 && c.history[k].group == e.group; ) ++n;

    static const char* kVerb[3] = { "reopened", "placed", "skipped" };
    static const char* kSrc[4] = { "", " (seen)", " (right-click)", " (in bulk)" };
    const char* verb = kVerb[e.to < 3 ? e.to : 0];
    const char* src = kSrc[e.source < 4 ? e.source : 0];

    char buf[256];
    if (n > 1) {
        snprintf(buf, sizeof(buf), "%s %zu blocks%s, %s", verb, n, src,
            ShortTimeAgo(e.unixMs).c_str());
    }
    else {
        std::string nm = "a block";
        int32_t wx = 0, wy = 0, wz = 0;
        if (e.index < c.prefab->blocks.size()) {
            const PrefabBlock& b = c.prefab->blocks[e.index];
            nm = EffectiveOfLocked(b);
            WorldPos(b, g_buildRotation.load(), g_buildOriginX.load(),
                g_buildOriginY.load(), g_buildOriginZ.load(), wx, wy, wz);
        }
        snprintf(buf, sizeof(buf), "%s %s at %d, %d, %d%s, %s", verb, nm.c_str(),
            wx, wy, wz, src, ShortTimeAgo(e.unixMs).c_str());
    }
    return buf;
}

int BuildChangedInLastMinutes(int minutes) {
    if (minutes < 1) return 0;
    const int64_t cutoff = NowUnixMs() - (int64_t)minutes * 60000;
    std::lock_guard lock(g_buildMtx);
    int n = 0;
    for (size_t k = g_core.history.size(); k-- > 0; ) {
        if (g_core.history[k].unixMs < cutoff) break;
        ++n;
    }
    return n;
}

int BuildRewindMinutes(int minutes) {
    if (minutes < 1) return 0;
    const int64_t cutoff = NowUnixMs() - (int64_t)minutes * 60000;

    std::lock_guard lock(g_buildMtx);
    BuildCore& c = g_core;
    if (!c.prefab) return 0;

    // Undo, back to the cutoff, as ONE step: the entries move to the redo list
    // under a single fresh group, so one Redo brings the whole lot back.
    const uint32_t g = c.nextGroup++;
    int n = 0;
    while (!c.history.empty() && c.history.back().unixMs >= cutoff) {
        BuildHistoryEntry e = c.history.back();
        c.history.pop_back();
        SetStateLocked(e.index, (BuildBlockState)e.from, BuildSource::Manual, 0, false, 0);
        e.group = g;
        c.redo.push_back(e);
        ++n;
    }
    if (n) {
        c.layerValid = false;
        FixLayerLocked();
        Touch();
    }
    return n;
}

void BuildJumpToLayer(int layer) {
    std::lock_guard lock(g_buildMtx);
    g_core.layer = layer;
    g_core.layerValid = true;
    Touch();
}

void BuildStepLayer(int direction) {
    std::lock_guard lock(g_buildMtx);
    BuildCore& c = g_core;
    if (!c.prefab || c.layerTotal.empty()) return;
    if (!c.layerValid) FixLayerLocked();
    if (direction > 0) {
        const auto it = c.layerTotal.upper_bound(c.layer);
        if (it == c.layerTotal.end()) return;
        c.layer = it->first;
    }
    else {
        auto it = c.layerTotal.lower_bound(c.layer);
        if (it == c.layerTotal.begin()) return;
        --it;
        c.layer = it->first;
    }
    c.layerValid = true;
    Touch();
}

int BuildCurrentLayer() {
    std::lock_guard lock(g_buildMtx);
    if (!g_core.prefab) return 0;
    if (!g_core.layerValid) FixLayerLocked();
    return g_core.layer;
}

int BuildLayerRemaining() {
    std::lock_guard lock(g_buildMtx);
    if (!g_core.prefab) return 0;
    if (!g_core.layerValid) FixLayerLocked();
    const auto it = g_core.layerTodo.find(g_core.layer);
    return it == g_core.layerTodo.end() ? 0 : it->second;
}

BuildLayerInfo BuildGetLayerInfo() {
    BuildLayerInfo info;
    std::lock_guard lock(g_buildMtx);
    BuildCore& c = g_core;
    if (!c.prefab || c.layerTotal.empty()) return info;
    if (!c.layerValid) FixLayerLocked();

    info.valid = true;
    info.current = c.layer;
    info.count = (int)c.layerTotal.size();
    int pos = 0;
    for (const auto& kv : c.layerTotal) {
        ++pos;
        if (kv.first == c.layer) { info.position = pos; info.totalHere = kv.second; break; }
    }
    const auto t = c.layerTodo.find(c.layer);
    info.todoHere = (t == c.layerTodo.end()) ? 0 : t->second;
    info.anyUnfinished = FirstUnfinishedLayerLocked(info.lowestUnfinished);
    return info;
}

int BuildBulkSet(BuildBulk scope, int radius, BuildBlockState to) {
    double px = 0, py = 0, pz = 0;
    const bool havePos = PlayerPos(px, py, pz);
    if (scope == BuildBulk::NearMe && !havePos) return 0;

    std::lock_guard lock(g_buildMtx);
    BuildCore& c = g_core;
    if (!c.prefab) return 0;
    if (!c.layerValid) FixLayerLocked();

    const int rot = g_buildRotation.load();
    const int ox = g_buildOriginX.load(), oy = g_buildOriginY.load(), oz = g_buildOriginZ.load();
    const double r2 = (double)radius * (double)radius;
    const int layer = c.layer;
    const int64_t now = NowUnixMs();
    const uint32_t group = c.nextGroup++;
    EnsureOrderLocked();

    int n = 0;
    const std::vector<PrefabBlock>& blocks = c.prefab->blocks;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const PrefabBlock& b = blocks[i];
        bool in = false;
        switch (scope) {
        case BuildBulk::ThisLayer:  in = b.y == layer; break;
        case BuildBulk::BelowLayer: in = b.y < layer;  break;
        case BuildBulk::Everything: in = true;         break;
        case BuildBulk::NearMe: {
            int32_t wx = 0, wy = 0, wz = 0;
            WorldPos(b, rot, ox, oy, oz, wx, wy, wz);
            const double dx = wx + 0.5 - px, dy = wy + 0.5 - py, dz = wz + 0.5 - pz;
            in = dx * dx + dy * dy + dz * dz <= r2;
            break;
        }
        }
        if (!in) continue;
        if (SetStateLocked((uint32_t)i, to, BuildSource::Bulk, group, true, now)) ++n;
    }
    if (to != BuildBlockState::Todo) {
        // Nothing marked done should still be complaining about its material.
        auto& m = c.mistakes;
        m.erase(std::remove_if(m.begin(), m.end(), [&c](const BuildMistake& x) {
            return x.index < c.state.size() &&
                   c.state[x.index] != (uint8_t)BuildBlockState::Todo; }), m.end());
    }
    FixLayerLocked();
    Touch();
    return n;
}

void BuildProgress(size_t& done, size_t& skipped, size_t& total) {
    std::lock_guard lock(g_buildMtx);
    done = g_core.done;
    skipped = g_core.skipped;
    total = g_core.state.size();
}

BuildStats BuildGetStats() {
    BuildStats s;
    std::lock_guard lock(g_buildMtx);
    const BuildCore& c = g_core;
    s.done = c.done;
    s.skipped = c.skipped;
    s.total = c.state.size();
    s.activeMs = c.activeMs;
    s.placed = c.placed;
    // Two minutes and twenty blocks before quoting a rate: the first handful of
    // placements says nothing about how fast a whole build goes.
    if (c.activeMs >= 120000 && c.placed >= 20) {
        s.perHour = (double)c.placed / ((double)c.activeMs / 3600000.0);
        const size_t left = s.total - s.done - s.skipped;
        if (s.perHour > 0.0) s.etaHours = (double)left / s.perHour;
    }
    return s;
}

BuildTarget BuildGetTarget() {
    BuildTarget t;
    {
        std::lock_guard lock(g_buildMtx);
        if (!g_core.prefab) return t;
        const size_t idx = TargetIndexLocked();
        if (idx == SIZE_MAX) return t;

        const PrefabBlock& b = g_core.prefab->blocks[idx];
        WorldPos(b, g_buildRotation.load(),
            g_buildOriginX.load(), g_buildOriginY.load(), g_buildOriginZ.load(),
            t.wx, t.wy, t.wz);
        t.valid = true;
        t.index = idx;
        t.rotation = b.rotation;
        t.name = EffectiveOfLocked(b);
        t.wanted = NameOfLocked(b);
    }

    const double px = g_posX.load(), py = g_posY.load(), pz = g_posZ.load();
    t.dx = (t.wx + g_buildNudgeX.load()) - px;
    t.dy = (t.wy + g_buildNudgeY.load()) - py;
    t.dz = (t.wz + g_buildNudgeZ.load()) - pz;
    t.distance = std::sqrt(t.dx * t.dx + t.dy * t.dy + t.dz * t.dz);
    return t;
}

std::vector<BuildMaterialRow> BuildMaterialRows(bool thisLayer) {
    std::vector<BuildMaterialRow> out;
    std::lock_guard lock(g_buildMtx);
    const BuildCore& c = g_core;
    if (!c.prefab) return out;
    const Prefab& p = *c.prefab;

    auto row = [&c, &p](size_t i, int left) {
        BuildMaterialRow r;
        r.wanted = p.names[i];
        r.mine = i < c.effective.size() ? c.effective[i] : p.names[i];
        r.left = left;
        return r;
    };

    if (!thisLayer) {
        for (size_t i = 0; i < c.nameTodo.size(); ++i)
            if (c.nameTodo[i] > 0) out.push_back(row(i, c.nameTodo[i]));
    }
    else {
        // Once per change rather than once per frame: this is a walk over the
        // whole build, and the UI asks every frame.
        static uint64_t cachedRev = 0;
        static const Prefab* cachedFor = nullptr;
        static std::vector<int> counts;
        const uint64_t rev = g_buildRev.load();
        if (rev != cachedRev || cachedFor != &p) {
            counts.assign(p.names.size(), 0);
            for (size_t i = 0; i < p.blocks.size(); ++i) {
                if (c.state[i] != (uint8_t)BuildBlockState::Todo) continue;
                if (p.blocks[i].y != c.layer) continue;
                const int32_t id = p.blocks[i].nameId;
                if (id >= 0 && (size_t)id < counts.size()) ++counts[(size_t)id];
            }
            cachedRev = rev;
            cachedFor = &p;
        }
        for (size_t i = 0; i < counts.size() && i < p.names.size(); ++i)
            if (counts[i] > 0) out.push_back(row(i, counts[i]));
    }

    std::sort(out.begin(), out.end(), [](const BuildMaterialRow& a, const BuildMaterialRow& b) {
        if (a.left != b.left) return a.left > b.left;
        return a.wanted < b.wanted;
    });
    return out;
}

std::string BuildDirectionText(double dx, double dy, double dz) {
    char buf[96];
    std::string out;
    const double flat = std::sqrt(dx * dx + dz * dz);
    if (flat >= 0.75) {
        // North is -Z, east is +X - the convention the anchor pad and the
        // compass calibration both use.
        double ang = std::atan2(dx, -dz) * 180.0 / 3.14159265358979;
        if (ang < 0) ang += 360.0;
        static const char* kDir[8] = { "north", "north-east", "east", "south-east",
                                       "south", "south-west", "west", "north-west" };
        const int sector = (int)std::floor((ang + 22.5) / 45.0) & 7;
        snprintf(buf, sizeof(buf), "%.0f %s", flat, kDir[sector]);
        out = buf;
    }
    if (std::fabs(dy) >= 0.75) {
        snprintf(buf, sizeof(buf), "%s%.0f %s", out.empty() ? "" : ", ",
            std::fabs(dy), dy > 0 ? "up" : "down");
        out += buf;
    }
    if (out.empty()) out = "right here";
    return out;
}

// =============================================================================
// Hearing about placements
// =============================================================================
// Progress can arrive as a world coordinate - from the client's own placement
// packet, or from the learner. Taking a coordinate rather than "the next one"
// is what makes it robust: you can build in any order, go back and fill a gap,
// or place something three layers up, and it all lands in the right place.
BuildPlaceResult BuildOnPlacement(int wx, int wy, int wz, const std::string& placedName,
    BuildSource source) {
    std::lock_guard lock(g_buildMtx);
    if (!placedName.empty()) g_lastPlacedName = placedName;
    BuildCore& c = g_core;
    if (!c.prefab) return BuildPlaceResult::NotInBuild;

    const size_t found = FindIndexAtWorldLocked(wx, wy, wz);
    if (found == SIZE_MAX) return BuildPlaceResult::NotInBuild;
    const uint32_t idx = (uint32_t)found;
    const PrefabBlock& b = c.prefab->blocks[idx];
    const std::string& wanted = NameOfLocked(b);
    const bool right = !g_buildCheckMaterial.load() || AcceptableLocked(wanted, placedName);
    const int64_t now = NowUnixMs();
    EnsureOrderLocked();

    if (!right) {
        // Keep it as a mistake, not progress. If it lands on a block already
        // marked done, the right one must have been broken to make room, so
        // that block is not done any more either.
        if (c.state[idx] != (uint8_t)BuildBlockState::Todo)
            SetStateLocked(idx, BuildBlockState::Todo, source, 0, true, now);

        bool updated = false;
        for (BuildMistake& m : c.mistakes)
            if (m.index == idx) { m.placedName = placedName; m.unixMs = now; updated = true; }
        if (!updated) {
            BuildMistake m;
            m.index = idx;
            m.placedName = placedName;
            m.unixMs = now;
            c.mistakes.push_back(m);
            // Bounded, because a mistake list nobody is reading is not a log.
            if (c.mistakes.size() > 256) c.mistakes.erase(c.mistakes.begin());
        }
        Touch();
        return BuildPlaceResult::WrongBlock;
    }

    RemoveMistakeLocked(idx);
    if (c.state[idx] != (uint8_t)BuildBlockState::Todo) {
        NotePlacedAsLocked(idx, placedName);      // replaced, or re-placed after a rollback
        return BuildPlaceResult::AlreadyDone;
    }

    SetStateLocked(idx, BuildBlockState::Done, source, 0, true, now);
    NotePlacedAsLocked(idx, placedName);
    NotePlacedLocked(now);

    // Follow the work rather than dragging it back: if you placed something on
    // a different layer, that is the layer you are on now.
    c.layer = b.y;
    c.layerValid = true;
    FixLayerLocked();

    // The client has now confirmed a block where the build says one goes, so
    // the build is where it belongs. Moving it after this would pull every
    // finished block out from under the guide.
    if (source == BuildSource::Follow) g_buildAnchorLocked.store(true);
    return BuildPlaceResult::Placed;
}

bool BuildMarkPlacedAt(int wx, int wy, int wz) {
    // No name, so no material check - the learner and the burst sender only
    // know where a block went.
    return BuildOnPlacement(wx, wy, wz, std::string(), BuildSource::Follow) ==
        BuildPlaceResult::Placed;
}

std::string BuildWantsAt(int wx, int wy, int wz) {
    std::lock_guard lock(g_buildMtx);
    const size_t idx = FindIndexAtWorldLocked(wx, wy, wz);
    if (idx == SIZE_MAX) return std::string();
    return EffectiveOfLocked(g_core.prefab->blocks[idx]);
}

bool BuildHasTodoAt(int wx, int wy, int wz) {
    std::lock_guard lock(g_buildMtx);
    const size_t idx = FindIndexAtWorldLocked(wx, wy, wz);
    return idx != SIZE_MAX && g_core.state[idx] == (uint8_t)BuildBlockState::Todo;
}

// =============================================================================
// The wrong block
// =============================================================================
std::vector<BuildMistake> BuildGetMistakes() {
    std::lock_guard lock(g_buildMtx);
    std::vector<BuildMistake> out = g_core.mistakes;
    if (!g_core.prefab) return out;
    // Coordinates from the block, not from when it was seen, so they are right
    // even if the anchor has moved since.
    const int rot = g_buildRotation.load();
    const int ox = g_buildOriginX.load(), oy = g_buildOriginY.load(), oz = g_buildOriginZ.load();
    for (BuildMistake& m : out)
        if (m.index < g_core.prefab->blocks.size())
            WorldPos(g_core.prefab->blocks[m.index], rot, ox, oy, oz, m.wx, m.wy, m.wz);
    return out;
}

namespace {

// Settles every flagged block that now counts - after a swap, or after
// switching to "any block counts". The block is already in the world, so it is
// done, and done as whatever was put there.
void SettleMistakesLocked(bool all) {
    BuildCore& c = g_core;
    if (!c.prefab || c.mistakes.empty()) return;
    const int64_t now = NowUnixMs();
    const uint32_t group = c.nextGroup++;
    EnsureOrderLocked();
    std::vector<BuildMistake> keep;
    for (const BuildMistake& m : c.mistakes) {
        if (m.index >= c.state.size()) continue;
        const std::string& wanted = NameOfLocked(c.prefab->blocks[m.index]);
        if (!all && !AcceptableLocked(wanted, m.placedName)) { keep.push_back(m); continue; }
        if (c.state[m.index] == (uint8_t)BuildBlockState::Todo)
            SetStateLocked(m.index, BuildBlockState::Done, BuildSource::Manual, group, true, now);
        NotePlacedAsLocked(m.index, m.placedName);
    }
    c.mistakes.swap(keep);
    FixLayerLocked();
    Touch();
}

void SetSwapLocked(const std::string& wanted, const std::string& mine) {
    BuildCore& c = g_core;
    c.swaps.erase(std::remove_if(c.swaps.begin(), c.swaps.end(),
        [&wanted](const std::pair<std::string, std::string>& sw) { return sw.first == wanted; }),
        c.swaps.end());
    if (!mine.empty() && mine != wanted) c.swaps.emplace_back(wanted, mine);
    RebuildEffectiveLocked();
    SettleMistakesLocked(false);
    Touch();
}

} // namespace

void BuildAcceptMistake(size_t i, bool always) {
    std::lock_guard lock(g_buildMtx);
    BuildCore& c = g_core;
    if (!c.prefab || i >= c.mistakes.size()) return;
    const BuildMistake m = c.mistakes[i];
    c.mistakes.erase(c.mistakes.begin() + (ptrdiff_t)i);

    if (m.index < c.state.size() && c.state[m.index] == (uint8_t)BuildBlockState::Todo) {
        const int64_t now = NowUnixMs();
        EnsureOrderLocked();
        SetStateLocked(m.index, BuildBlockState::Done, BuildSource::Manual, 0, true, now);
        NotePlacedLocked(now);
    }
    NotePlacedAsLocked(m.index, m.placedName);

    // "Use it for all": a swap, so the ghost, the lists and the check all
    // follow - and any other block already flagged for the same thing settles.
    if (always && m.index < c.prefab->blocks.size() && !m.placedName.empty())
        SetSwapLocked(NameOfLocked(c.prefab->blocks[m.index]), m.placedName);
    FixLayerLocked();
    Touch();
}

void BuildDismissMistake(size_t i) {
    std::lock_guard lock(g_buildMtx);
    if (i >= g_core.mistakes.size()) return;
    g_core.mistakes.erase(g_core.mistakes.begin() + (ptrdiff_t)i);
    Touch();
}

void BuildClearMistakes() {
    std::lock_guard lock(g_buildMtx);
    g_core.mistakes.clear();
    Touch();
}

void BuildSetAnyBlock(bool anyBlockCounts) {
    g_buildCheckMaterial.store(!anyBlockCounts);
    std::lock_guard lock(g_buildMtx);
    // Anything already flagged was only wrong by the old rule. It is in the
    // world, so it counts now.
    if (anyBlockCounts) SettleMistakesLocked(true);
    Touch();
}

void BuildSetSwap(const std::string& wanted, const std::string& mine) {
    if (wanted.empty()) return;
    std::lock_guard lock(g_buildMtx);
    if (!g_core.prefab) return;
    SetSwapLocked(wanted, mine);
}

std::vector<std::pair<std::string, std::string>> BuildGetSwaps() {
    std::lock_guard lock(g_buildMtx);
    return g_core.swaps;
}

std::string BuildEffectiveName(const std::string& wanted) {
    std::lock_guard lock(g_buildMtx);
    for (const auto& sw : g_core.swaps)
        if (sw.first == wanted) return sw.second;
    return wanted;
}

std::string BuildLastPlacedName() {
    std::lock_guard lock(g_buildMtx);
    return g_lastPlacedName;
}

bool BuildSaveMyVersion(const std::string& name, std::string& err, size_t& placedKnown) {
    placedKnown = 0;
    std::string stem;
    for (char ch : name) {
        if (isalnum((unsigned char)ch) || ch == '_' || ch == '-') stem += ch;
        else if (ch == ' ') stem += '_';
    }
    if (stem.empty()) { err = "give it a name"; return false; }

    Prefab q;
    {
        std::lock_guard lock(g_buildMtx);
        const BuildCore& c = g_core;
        if (!c.prefab) { err = "no build is open"; return false; }
        const Prefab& p = *c.prefab;
        q.name = stem;
        q.blocks = p.blocks;
        std::unordered_map<std::string, int32_t> ids;
        for (size_t i = 0; i < p.blocks.size(); ++i) {
            const PrefabBlock& b = p.blocks[i];
            const std::string& wanted = NameOfLocked(b);
            std::string use = EffectiveOfLocked(b);
            const int32_t pa = i < c.placedAs.size() ? c.placedAs[i] : -1;
            if (pa >= 0 && (size_t)pa < c.placedNames.size()) {
                const std::string& real = c.placedNames[(size_t)pa];
                // Same block as asked for: keep the prefab's own name, which
                // carries the state variant the client does not report.
                use = (BlockFamily(real) == BlockFamily(wanted)) ? wanted : real;
                ++placedKnown;
            }
            auto it = ids.find(use);
            if (it == ids.end()) {
                it = ids.emplace(use, (int32_t)q.names.size()).first;
                q.names.push_back(use);
            }
            q.blocks[i].nameId = it->second;
        }
    }

    const std::wstring dir = GetThisModuleDirectory() + L"prefabs";
    CreateDirectoryW(dir.c_str(), nullptr);
    // Never over an existing file - that might be the prefab this came from.
    std::wstring path = dir + L"\\" + Utf8ToWide(stem) + L".json";
    for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && n < 1000; ++n)
        path = dir + L"\\" + Utf8ToWide(stem) + L"_" + std::to_wstring(n) + L".json";
    if (!PrefabWriteJson(q, path, err)) return false;
    err = "saved as " + WideToUtf8Path(path.substr(dir.size() + 1));
    PrefabScanFolder();
    return true;
}

// =============================================================================
// Saving and restoring
// =============================================================================
void BuildExportState(BuildSnapshot& s) {
    s = BuildSnapshot{};
    s.originX = g_buildOriginX.load();
    s.originY = g_buildOriginY.load();
    s.originZ = g_buildOriginZ.load();
    s.rotation = g_buildRotation.load();
    s.locked = g_buildAnchorLocked.load();
    s.shown = g_buildActive.load();
    s.orderMode = g_buildOrderMode.load();
    s.layerOnly = g_buildLayerOnly.load();
    s.showMode = g_buildShowMode.load();
    s.ghostCount = g_buildGhostCount.load();
    s.scale = g_buildGhostScale.load();
    s.nudgeX = g_buildNudgeX.load();
    s.nudgeY = g_buildNudgeY.load();
    s.nudgeZ = g_buildNudgeZ.load();
    s.opNear = g_buildOpacityNear.load();
    s.opFar = g_buildOpacityFar.load();
    s.fade = g_buildFadeOtherLayers.load();
    s.byType = g_buildColourByType.load();
    s.showDone = g_buildShowDone.load();
    s.checkMaterial = g_buildCheckMaterial.load();

    std::lock_guard lock(g_buildMtx);
    const BuildCore& c = g_core;
    s.layer = c.layer;
    s.layerValid = c.layerValid;
    s.state = c.state;
    s.history = c.history;
    s.redo = c.redo;
    s.mistakes = c.mistakes;
    s.swaps = c.swaps;
    s.placedAs = c.placedAs;
    s.placedNames = c.placedNames;
    s.activeMs = c.activeMs;
    s.placed = c.placed;
}

void BuildImportState(const BuildSnapshot& s, std::string& note) {
    note.clear();
    g_buildOriginX.store(s.originX);
    g_buildOriginY.store(s.originY);
    g_buildOriginZ.store(s.originZ);
    g_buildRotation.store(s.rotation & 3);
    g_buildAnchorLocked.store(s.locked);
    g_buildOrderMode.store((s.orderMode >= 0 && s.orderMode <= 2) ? s.orderMode
                                                                  : (int)BuildOrder::ByMaterial);
    g_buildLayerOnly.store(s.layerOnly);
    g_buildShowMode.store((s.showMode >= 0 && s.showMode <= 2) ? s.showMode : 0);
    g_buildGhostCount.store((std::max)(1, (std::min)(64, s.ghostCount)));
    g_buildGhostScale.store((std::max)(0.2f, (std::min)(1.5f, s.scale)));
    g_buildNudgeX.store(s.nudgeX);
    g_buildNudgeY.store(s.nudgeY);
    g_buildNudgeZ.store(s.nudgeZ);
    g_buildOpacityNear.store(s.opNear);
    g_buildOpacityFar.store(s.opFar);
    g_buildFadeOtherLayers.store(s.fade);
    g_buildColourByType.store(s.byType);
    g_buildShowDone.store(s.showDone);
    g_buildCheckMaterial.store(s.checkMaterial);

    {
        std::lock_guard lock(g_buildMtx);
        BuildCore& c = g_core;
        const size_t n = c.state.size();

        if (s.state.size() == n) {
            c.state = s.state;
            for (uint8_t& v : c.state) if (v > 2) v = (uint8_t)BuildBlockState::Todo;
        }
        else if (!s.state.empty()) {
            note = "saved progress was for " + std::to_string(s.state.size()) +
                   " blocks and this build has " + std::to_string(n) +
                   ", so it starts from the beginning";
        }
        RecountLocked();

        // Only entries that still name a real block. A history pointing past
        // the end would undo something that does not exist.
        auto fit = [n](const std::vector<BuildHistoryEntry>& in) {
            std::vector<BuildHistoryEntry> out;
            out.reserve(in.size());
            for (const BuildHistoryEntry& e : in)
                if (e.index < n && e.from <= 2 && e.to <= 2) out.push_back(e);
            return out;
        };
        const bool stateTaken = s.state.size() == n;
        c.history = stateTaken ? fit(s.history) : std::vector<BuildHistoryEntry>{};
        c.redo = stateTaken ? fit(s.redo) : std::vector<BuildHistoryEntry>{};
        uint32_t maxGroup = 0;
        for (const BuildHistoryEntry& e : c.history) maxGroup = (std::max)(maxGroup, e.group);
        for (const BuildHistoryEntry& e : c.redo) maxGroup = (std::max)(maxGroup, e.group);
        c.nextGroup = maxGroup + 1;

        c.mistakes.clear();
        if (stateTaken)
            for (const BuildMistake& m : s.mistakes)
                if (m.index < n) c.mistakes.push_back(m);
        c.swaps = s.swaps;
        RebuildEffectiveLocked();
        c.placedNames = s.placedNames;
        c.placedNameIdx.clear();
        for (size_t k = 0; k < c.placedNames.size(); ++k)
            c.placedNameIdx.emplace(c.placedNames[k], (int32_t)k);
        c.placedAs.assign(n, -1);
        if (stateTaken && s.placedAs.size() == n)
            for (size_t k = 0; k < n; ++k) {
                const int32_t v = s.placedAs[k];
                if (v >= 0 && (size_t)v < c.placedNames.size() &&
                    c.state[k] != (uint8_t)BuildBlockState::Todo)
                    c.placedAs[k] = v;
            }
        c.activeMs = s.activeMs;
        c.placed = s.placed;
        c.lastPlaceUnixMs = 0;

        c.layer = s.layer;
        c.layerValid = s.layerValid && c.layerTotal.count(s.layer) != 0;
        c.orderBuiltFor = -1;
        FixLayerLocked();
        g_drawnGhosts.clear();
    }
    g_ghostResetPending.store(true);
    Touch();
}

// =============================================================================
// Drawing the ghosts
// =============================================================================
namespace {

// Game-thread state for the frame tick. Nothing else touches these.
int  g_ghostAnchorX = 0, g_ghostAnchorY = 0, g_ghostAnchorZ = 0;
bool g_ghostAnchorSet = false;
long long g_ghostLastDrawMs = 0;
uint64_t g_lastInputSig = 0;
bool g_lastDrawComplete = false;
bool g_walkedFresh = false;
long long g_nextVerifyMs = 0;
long long g_nextRecoverAllowedMs = 0;
long long g_recoverBackoffMs = 5000;
std::atomic<uint64_t> g_ghostRecoveries{ 0 };

struct Ghost {
    int32_t wx, wy, wz;
    int32_t nameId;
    int32_t layer;
    uint8_t kind;
    int64_t dist2;
};
std::vector<Ghost> g_wantGhosts;      // the last walk's answer
std::vector<std::string> g_wantNames; // per nameId, swaps applied, as of that walk
int g_wantNeeded = 0;
int g_wantLayer = 0;

// Throws away everything we believe is on screen and asks for a clean list.
// The next frame redraws from scratch.
void ForgetDrawnGhosts() {
    { std::lock_guard lock(g_buildMtx); g_drawnGhosts.clear(); }
    DbgClearSpawnQueue();
    DbgForgetTags();
    DbgRequestClearAll();
    g_ghostSignature.store(0);
    g_lastInputSig = 0;
    g_lastDrawComplete = false;
    g_ghostAnchorSet = false;
}

// Is what we drew still there?
//
// A relog, a server restart, joining another world, or the selection box
// borrowing the list all replace or empty the client's shape list behind our
// back. The bookkeeping above still says "drawn", so nothing was ever redrawn
// and the ghost simply vanished until you hid and showed it again. Asking the
// list itself - does the slot we wrote still hold one of our entries - is the
// only thing that can tell.
//
// Sampled, throttled, and backed off: a check that fails every time must not
// turn into a clear-and-refill every second, which is the death spiral that
// once took the game down.
void VerifyDrawnGhosts(long long now) {
    if (now < g_nextVerifyMs) return;
    g_nextVerifyMs = now + 1000;
    if (!g_lastDrawComplete || DbgSpawnQueueDepth() > 0) return;

    std::vector<uint64_t> sample;
    {
        std::lock_guard lock(g_buildMtx);
        const size_t n = g_drawnGhosts.size();
        if (!n) return;
        const size_t want = (std::min)((size_t)12, n);
        const size_t step = (std::max)((size_t)1, n / want);
        size_t k = 0;
        for (const auto& kv : g_drawnGhosts) {
            if (k++ % step == 0) sample.push_back(kv.first);
            if (sample.size() >= want) break;
        }
    }

    int missing = 0;
    const int alive = DbgCountTagsAlive(sample, missing);
    if (alive < 0) return;                        // cannot tell right now
    if (alive * 2 >= (int)sample.size()) {        // most of it is still there
        g_recoverBackoffMs = 5000;
        return;
    }
    if (now < g_nextRecoverAllowedMs) return;
    g_nextRecoverAllowedMs = now + g_recoverBackoffMs;
    g_recoverBackoffMs = (std::min)(g_recoverBackoffMs * 2, 60000LL);

    ForgetDrawnGhosts();
    g_ghostRecoveries.fetch_add(1);
    SetGhostStatus("the outline was lost (relog or world change) - drawing it again");
}

} // namespace

// Shapes come from a fixed pool, so this does not always outline the whole
// prefab - it outlines what is nearest you, up to what the list holds. The set
// is only recomputed when something that decides it has changed, and only
// redrawn when the answer differs from what is on screen.
void BuildFrameTick() {
    if (!g_buildActive.load()) return;

    std::shared_ptr<const Prefab> pp;
    { std::lock_guard lock(g_buildMtx); pp = g_core.prefab; }
    if (!pp) { SetGhostStatus("no build loaded"); return; }
    const Prefab& p = *pp;

    if (g_ghostResetPending.exchange(false)) {
        g_ghostAnchorSet = false;
        g_lastInputSig = 0;
        g_lastDrawComplete = false;
        g_ghostSignature.store(0);
    }

    const long long now = NowSteadyMs();
    const bool ready = DbgSpawnReady();
    {
        // The list just came (back) up - after a relog it is a different list,
        // so check what survived at once rather than in a second's time.
        static bool wasReady = false;
        if (ready && !wasReady) g_nextVerifyMs = 0;
        wasReady = ready;
    }

    // Where the player is, with hysteresis. A build too big to outline all at
    // once gets the part NEAREST YOU; the selection point only moves when you
    // have gone a long way from where it was last made, because each
    // re-selection is a full clear and refill.
    int px = 0, py = 0, pz = 0;
    {
        const GsdkPlayerInfo me = GsdkGetPlayerInfo();
        if (me.valid) {
            const int nx = (int)me.x, ny = (int)me.y, nz = (int)me.z;
            const long long ddx = nx - g_ghostAnchorX, ddy = ny - g_ghostAnchorY,
                            ddz = nz - g_ghostAnchorZ;
            if (!g_ghostAnchorSet || ddx * ddx + ddy * ddy + ddz * ddz > 48LL * 48LL) {
                g_ghostAnchorX = nx; g_ghostAnchorY = ny; g_ghostAnchorZ = nz;
                g_ghostAnchorSet = true;
            }
            px = g_ghostAnchorX; py = g_ghostAnchorY; pz = g_ghostAnchorZ;
        }
    }

    const int mode = g_buildShowMode.load();
    const int rot = g_buildRotation.load();
    const int ox = g_buildOriginX.load();
    const int oy = g_buildOriginY.load();
    const int oz = g_buildOriginZ.load();
    const bool layerOnly = g_buildLayerOnly.load();
    const bool showDone = g_buildShowDone.load();
    // -1 means the list could not be read this frame, which happens. Holding the
    // last good reading stops a flicker in that from re-walking the build.
    static int s_lastCapacity = -1;
    int capacity = ready ? DbgListCapacity() : -1;
    if (capacity > 0) s_lastCapacity = capacity;
    else if (ready)   capacity = s_lastCapacity;

    int limit = g_ghostAdaptiveLimit.load();
    if (limit < 64) limit = 64;
    if (limit > kBuildMaxGhosts) limit = kBuildMaxGhosts;
    if (mode == (int)BuildShow::NextFew) limit = (std::max)(1, g_buildGhostCount.load());

    // ---- has anything that decides the outline changed? -------------------
    // Cheap to compute, and the usual answer is no - in which case there is no
    // need to walk fifty thousand blocks to find out the outline is the same.
    uint64_t in = 1469598103934665603ull;
    auto mixIn = [&in](uint64_t v) { in ^= v; in *= 1099511628211ull; };
    mixIn(BuildRevision());
    mixIn((uint64_t)(uintptr_t)&p);
    mixIn((uint32_t)ox); mixIn((uint32_t)oy); mixIn((uint32_t)oz); mixIn((uint32_t)rot);
    mixIn((uint32_t)mode); mixIn((uint32_t)limit);
    mixIn(layerOnly ? 3 : 5); mixIn(showDone ? 7 : 11);
    mixIn((uint32_t)g_buildOrderMode.load());
    mixIn((uint32_t)px); mixIn((uint32_t)py); mixIn((uint32_t)pz);
    // How it looks, and how much room there is, decide what goes on screen
    // just as much - a slider dragged or a list that just grew has to redraw.
    mixIn((uint32_t)capacity);
    mixIn((uint64_t)(int64_t)(g_buildGhostScale.load() * 1000.0f));
    mixIn((uint64_t)(int64_t)(g_buildNudgeX.load() * 1000.0f));
    mixIn((uint64_t)(int64_t)(g_buildNudgeY.load() * 1000.0f));
    mixIn((uint64_t)(int64_t)(g_buildNudgeZ.load() * 1000.0f));
    mixIn((uint64_t)(int64_t)(g_buildOpacityNear.load() * 1000.0f));
    mixIn((uint64_t)(int64_t)(g_buildOpacityFar.load() * 1000.0f));
    mixIn(g_buildColourByType.load() ? 13 : 17);
    mixIn(g_buildFadeOtherLayers.load() ? 19 : 23);

    if (in != g_lastInputSig) {
        g_lastInputSig = in;
        g_walkedFresh = true;

        // Farthest at the front, so the heap gives up its worst candidate first.
        const auto farther = [](const Ghost& a, const Ghost& b) { return a.dist2 < b.dist2; };
        g_wantGhosts.clear();
        g_wantGhosts.reserve((size_t)limit);
        int needed = 0;

        std::lock_guard lock(g_buildMtx);
        BuildCore& c = g_core;
        const size_t targetIdx = TargetIndexLocked();
        g_wantLayer = c.layer;
        g_wantNames = c.effective;

        std::unordered_set<uint32_t> wrong;
        for (const BuildMistake& m : c.mistakes) wrong.insert(m.index);

        EnsureOrderLocked();
        for (uint32_t i : c.order) {
            const uint8_t s = c.state[i];
            const PrefabBlock& b = p.blocks[i];
            uint8_t kind;
            if (s == (uint8_t)BuildBlockState::Todo) {
                if (mode == (int)BuildShow::ThisLayer && b.y != c.layer) continue;
                if (mode == (int)BuildShow::NextFew && layerOnly && b.y != c.layer) continue;
                kind = wrong.count(i) ? GhostWrong : (i == targetIdx ? GhostTarget : GhostTodo);
                ++needed;
            }
            else if (s == (uint8_t)BuildBlockState::Done && showDone &&
                     mode != (int)BuildShow::NextFew) {
                if (mode == (int)BuildShow::ThisLayer && b.y != c.layer) continue;
                kind = GhostDone;
            }
            else continue;

            Ghost g{};
            WorldPos(b, rot, ox, oy, oz, g.wx, g.wy, g.wz);
            g.nameId = b.nameId;
            g.layer = b.y;
            g.kind = kind;
            const int64_t dx = g.wx - px, dy = g.wy - py, dz = g.wz - pz;
            // The block being asked for, and anything placed wrong, are nearer
            // than anything - they can never be the ones left out.
            g.dist2 = (kind == GhostTarget || kind == GhostWrong) ? -1
                                                                  : (dx * dx + dy * dy + dz * dz);

            // A bounded heap of the nearest, rather than every candidate
            // followed by a sort - collecting them all was a 24 MB vector on
            // the frame thread for the dragon statue.
            if ((int)g_wantGhosts.size() < limit) {
                g_wantGhosts.push_back(g);
                std::push_heap(g_wantGhosts.begin(), g_wantGhosts.end(), farther);
            }
            else if (g.dist2 < g_wantGhosts.front().dist2) {
                std::pop_heap(g_wantGhosts.begin(), g_wantGhosts.end(), farther);
                g_wantGhosts.back() = g;
                std::push_heap(g_wantGhosts.begin(), g_wantGhosts.end(), farther);
            }
        }
        g_wantNeeded = needed;
        g_buildGhostsNeeded.store(needed);

        // Size the list to the outline rather than the outline to the list.
        // Only ever raised here; a prefab load is where it comes back down.
        if (g_buildAutoCapacity.load()) {
            int want = (int)g_wantGhosts.size() + 16;
            if (want < 64) want = 64;
            if (want > kDbgMaxCapacity) want = kDbgMaxCapacity;
            if (g_dbgWantedCapacity.load() < want) g_dbgWantedCapacity.store(want);
        }
    }

    if (!ready) {
        SetGhostStatus(DbgAutoStatus());
        return;
    }

    VerifyDrawnGhosts(now);

    if (g_wantGhosts.empty()) {
        // Nothing to draw, but something may still be on screen from before -
        // the last block of a build, or everything after "start over" was
        // undone. Clear it once.
        bool anyDrawn = false;
        { std::lock_guard lock(g_buildMtx); anyDrawn = !g_drawnGhosts.empty(); }
        if (anyDrawn) ForgetDrawnGhosts();
        g_lastDrawComplete = true;
        g_walkedFresh = false;
        SetGhostStatus("nothing left to place - it is finished");
        return;
    }

    // Nothing changed since a draw that went through: nothing to do.
    if (!g_walkedFresh && g_lastDrawComplete) return;

    std::vector<Ghost> ghosts = g_wantGhosts;
    // Whatever the list actually holds is the real ceiling. Queueing spawns
    // that cannot land just burns the failure counter. The nearest survive.
    if (capacity > 0 && (int)ghosts.size() > capacity) {
        std::sort(ghosts.begin(), ghosts.end(),
            [](const Ghost& a, const Ghost& b) { return a.dist2 < b.dist2; });
        ghosts.resize((size_t)capacity);
    }

    // ---- is the answer different from what is on screen? ------------------
    const float scale = g_buildGhostScale.load();
    const float nx = g_buildNudgeX.load();
    const float ny = g_buildNudgeY.load();
    const float nz = g_buildNudgeZ.load();
    const bool byType = g_buildColourByType.load();
    const bool fade = g_buildFadeOtherLayers.load();
    const float opNear = g_buildOpacityNear.load();
    const float opFar = g_buildOpacityFar.load();
    const int cursorLayer = g_wantLayer;

    uint64_t sig = 1469598103934665603ull;
    auto mix = [&sig](uint64_t v) { sig ^= v; sig *= 1099511628211ull; };
    for (const Ghost& g : ghosts) {
        mix((uint64_t)(uint32_t)g.wx);
        mix((uint64_t)(uint32_t)g.wy);
        mix((uint64_t)(uint32_t)g.wz);
        mix((uint64_t)(uint32_t)g.nameId);
        mix((uint64_t)g.kind);
    }
    mix((uint64_t)(int64_t)(scale * 1000.0f));
    mix((uint64_t)(int64_t)(nx * 1000.0f));
    mix((uint64_t)(int64_t)(ny * 1000.0f));
    mix((uint64_t)(int64_t)(nz * 1000.0f));
    mix((uint64_t)(int64_t)(opNear * 1000.0f));
    mix((uint64_t)(int64_t)(opFar * 1000.0f));
    mix(byType ? 1ull : 0ull);
    mix(fade ? 1ull : 0ull);
    mix((uint64_t)(uint32_t)cursorLayer);
    mix((uint64_t)(uint32_t)capacity);

    if (sig == g_ghostSignature.load()) {
        g_walkedFresh = false;
        g_lastDrawComplete = true;
        return;
    }

    // Never start a draw while the last one is still filling in. A redraw that
    // re-queued the whole outline every 150ms while the queue was still
    // draining is what made the ghost flash and then took the game down.
    if (DbgSpawnQueueDepth() > 0) return;
    if (now - g_ghostLastDrawMs < 120) return;
    g_ghostLastDrawMs = now;

    auto makeRequest = [&](const Ghost& g, DbgRequest& req) {
        req = DbgRequest{};
        req.shape = DbgShape::Cube;
        req.x = (float)g.wx + nx;
        req.y = (float)g.wy + ny;
        req.z = (float)g.wz + nz;
        req.scaleX = req.scaleY = req.scaleZ = scale;
        req.time = 600.0f;
        req.flags = DBG_FLAG_NONE;
        req.tag = PrefabCellKey(g.wx, g.wy, g.wz);

        switch (g.kind) {
        case GhostTarget:
            // The one you are being asked to place. White, opaque and a touch
            // proud of the rest, so it reads as "this one" without looking.
            req.r = 1.0f; req.g = 1.0f; req.b = 1.0f;
            req.opacity = 0.95f;
            req.scaleX = req.scaleY = req.scaleZ = scale * 1.04f;
            break;
        case GhostWrong:
            // Something is in this cell that the build did not ask for.
            req.r = 1.0f; req.g = 0.12f; req.b = 0.10f;
            req.opacity = 0.9f;
            req.scaleX = req.scaleY = req.scaleZ = scale * 1.08f;
            break;
        case GhostDone:
            // Smaller than a block on purpose: it sits INSIDE the block you
            // placed, so where the block is there you see nothing, and where
            // the server lost it you see green. That is the whole check.
            req.r = 0.30f; req.g = 1.0f; req.b = 0.45f;
            req.opacity = 0.45f;
            req.scaleX = req.scaleY = req.scaleZ = scale * 0.7f;
            break;
        default:
            // Coloured by what you are placing, so a swap recolours the ghost.
            if (byType && g.nameId >= 0 && (size_t)g.nameId < g_wantNames.size())
                BuildColourForName(g_wantNames[(size_t)g.nameId], req.r, req.g, req.b);
            else { req.r = 0.35f; req.g = 0.75f; req.b = 1.0f; }
            // Layers you are not on are drawn faint, so you can see the course
            // you are laying rather than one opaque mass.
            req.opacity = (fade && g.layer != cursorLayer) ? opFar : opNear;
            break;
        }
    };

    // ---- draw the difference ----------------------------------------------
    // Shapes no longer wanted are hidden, new ones take freed slots or are
    // spawned, and ones whose look changed are written over in place.
    std::unordered_map<uint64_t, DrawnGhost> drawnWas;
    { std::lock_guard lock(g_buildMtx); drawnWas = g_drawnGhosts; }

    int accepted = 0, added = 0, changed = 0, removed = 0;
    std::unordered_map<uint64_t, DrawnGhost> wanted;
    wanted.reserve(ghosts.size());

    for (const Ghost& g : ghosts) {
        DbgRequest req;
        makeRequest(g, req);
        const uint64_t key = req.tag;

        const DrawnGhost nowG{ g.nameId, g.layer, g.kind, req.opacity };
        wanted.emplace(key, nowG);

        const auto had = drawnWas.find(key);
        const bool tagged = DbgHasTag(key);
        if (had != drawnWas.end() && tagged) {
            const DrawnGhost& was = had->second;
            if (was.nameId == nowG.nameId && was.kind == nowG.kind &&
                was.layer == nowG.layer && was.opacity == nowG.opacity) {
                ++accepted;
                continue;                       // already exactly right
            }
        }

        if (tagged) {
            if (DbgUpdateTagged(key, req)) { ++accepted; ++changed; continue; }
            DbgRemoveTagged(key);
        }
        // A freed slot before a new allocation. Building frees one slot per
        // block placed and needs none, so after the first fill this almost
        // never allocates.
        if (DbgReuseSlot(key, req)) { ++accepted; ++changed; }
        else if (DbgQueueSpawn(req)) { ++accepted; ++added; }
    }

    // Anything drawn that is no longer wanted - the block you just placed,
    // most of the time.
    for (const auto& kv : drawnWas)
        if (wanted.find(kv.first) == wanted.end()) { DbgRemoveTagged(kv.first); ++removed; }

    { std::lock_guard lock(g_buildMtx); g_drawnGhosts.swap(wanted); }

    // Only now is it "drawn". Storing the signature before queueing meant a
    // draw refused halfway was remembered as complete and never retried.
    g_walkedFresh = false;
    if (accepted == (int)ghosts.size()) {
        g_ghostSignature.store(sig);
        g_ghostFailStreak.store(0);
        g_lastDrawComplete = true;
    }
    else {
        g_lastDrawComplete = false;
        // Three failures in a row and the outline halves. It settles at
        // whatever this client can actually hold.
        const int streak = g_ghostFailStreak.fetch_add(1) + 1;
        if (streak >= 3) {
            const int cur = g_ghostAdaptiveLimit.load();
            g_ghostAdaptiveLimit.store(cur > 128 ? cur / 2 : 64);
            g_ghostFailStreak.store(0);
        }
    }

    g_buildGhostsDrawn.store(accepted);

    char buf[220];
    const size_t waiting = DbgSpawnQueueDepth();
    if (waiting)
        snprintf(buf, sizeof(buf), "%d outlined, %zu still drawing...", accepted, waiting);
    else if ((int)ghosts.size() < g_wantNeeded)
        snprintf(buf, sizeof(buf),
            "%zu of %d outlined - the nearest to you; the list holds %d",
            ghosts.size(), g_wantNeeded, capacity);
    else
        snprintf(buf, sizeof(buf), "%d outlined  (+%d  ~%d  -%d)", accepted, added,
            changed, removed);
    const uint64_t rec = g_ghostRecoveries.load();
    if (rec) {
        const size_t len = strlen(buf);
        snprintf(buf + len, sizeof(buf) - len, "  - redrawn after %llu world change(s)",
            (unsigned long long)rec);
    }
    SetGhostStatus(buf);
}

// =============================================================================
// Which slot holds what
// =============================================================================
// Written down rather than read out of the client. Reading the inventory means
// finding it in the object graph and then working out an item's identity with
// no type names to go on, which is a project of its own; typing "3" once per
// material answers the same question today.
namespace {
std::mutex g_matSlotMtx;
std::unordered_map<std::string, int> g_matSlots;

std::wstring MatSlotFilePath() {
    return GetThisModuleDirectory() + L"prefab_slots.txt";
}
} // namespace

void BuildSetMaterialSlot(const std::string& material, int slot) {
    if (material.empty()) return;
    {
        std::lock_guard lock(g_matSlotMtx);
        if (slot <= 0) g_matSlots.erase(material);
        else           g_matSlots[material] = slot;
    }
    BuildSaveMaterialSlots();
}

int BuildGetMaterialSlot(const std::string& material) {
    std::lock_guard lock(g_matSlotMtx);
    auto it = g_matSlots.find(material);
    return (it == g_matSlots.end()) ? 0 : it->second;
}

bool BuildSaveMaterialSlots() {
    std::string out =
        "# hotbar slot for each block, one per line: <slot><tab><block name>\n";
    {
        std::lock_guard lock(g_matSlotMtx);
        for (const auto& kv : g_matSlots) {
            out += std::to_string(kv.second);
            out += '\t';
            out += kv.first;
            out += '\n';
        }
    }

    HANDLE h = CreateFileW(MatSlotFilePath().c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &wrote, nullptr);
    CloseHandle(h);
    return ok && wrote == out.size();
}

bool BuildLoadMaterialSlots() {
    std::string text, err;
    if (!ReadWholeFile(WideToUtf8Path(MatSlotFilePath()), text, err)) return false;

    std::unordered_map<std::string, int> loaded;
    size_t i = 0;
    while (i < text.size()) {
        size_t eol = text.find('\n', i);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(i, eol - i);
        i = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const int slot = atoi(line.substr(0, tab).c_str());
        const std::string name = line.substr(tab + 1);
        if (slot > 0 && !name.empty()) loaded[name] = slot;
    }

    std::lock_guard lock(g_matSlotMtx);
    g_matSlots.swap(loaded);
    return true;
}

// =============================================================================
// Advancing without leaving the game
// =============================================================================
// The overlay is its own always-on-top window, so every press of "Placed it"
// costs a trip out of the game and back. Polling the keyboard directly is what
// the rest of the overlay already does for its own hotkeys, and it works while
// Hytale has focus, which is the only time it matters here.
//
// The right-mouse option is the interesting one: right-click IS "place a
// block", so treating it as "I placed that" needs no new mechanism at all. It
// is wrong whenever you right-click at something other than the target, which
// is why it is off by default, why it only counts when the block being asked
// for is within arm's reach, and why Undo is on a key of its own.
std::atomic<bool>  g_buildHotkeys{ true };
std::atomic<bool> g_buildNudgeMode{ false };
std::atomic<int>  g_buildNudgeStep{ 1 };

std::atomic<int>   g_buildKeyAdvance{ VK_OEM_6 };   // ]
std::atomic<int>   g_buildKeyUndo{ VK_OEM_4 };      // [
std::atomic<int>   g_buildKeySkip{ VK_OEM_5 };      // backslash
std::atomic<bool>  g_buildRightClickAdvance{ false };
std::atomic<float> g_buildReachBlocks{ 8.0f };
std::atomic<uint64_t> g_buildHotkeyAdvances{ 0 };
std::atomic<uint64_t> g_buildClickAdvances{ 0 };
std::atomic<uint64_t> g_buildClickIgnored{ 0 };

namespace {

// Edge detection, one flag per action. GetAsyncKeyState's own "was pressed
// since last call" bit is shared process-wide and unreliable when more than
// one thing polls, so this keeps its own.
bool EdgeDown(int vk, bool& was) {
    if (vk <= 0) { was = false; return false; }
    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fired = now && !was;
    was = now;
    return fired;
}

} // namespace

void BuildPollHotkeys(void* overlayHwnd) {
    if (!g_buildHotkeys.load()) return;

    static bool wasAdvance = false, wasUndo = false, wasSkip = false, wasClick = false;

    // Keys work whichever window has focus - they are deliberate, and nothing
    // else in the game is listening for them.
    if (EdgeDown(g_buildKeyAdvance.load(), wasAdvance)) {
        if (g_buildActive.load()) {
            BuildMarkDone(1);
            g_buildHotkeyAdvances.fetch_add(1);
        }
    }
    if (EdgeDown(g_buildKeyUndo.load(), wasUndo)) {
        if (g_buildActive.load()) BuildUndo(1);
    }
    if (EdgeDown(g_buildKeySkip.load(), wasSkip)) {
        if (g_buildActive.load()) BuildSkip(1);
    }

    // Arrow keys move the ghost, but only while nudge mode is on - the rest of
    // the time they belong to the game.
    if (g_buildNudgeMode.load() && g_buildActive.load()) {
        static bool wasL = false, wasR = false, wasU = false, wasD = false;
        static bool wasPgU = false, wasPgD = false, wasRot = false;
        const int step = (std::max)(1, g_buildNudgeStep.load());

        if (EdgeDown(VK_UP, wasU))      BuildNudgeRelative(step, 0, 0);
        if (EdgeDown(VK_DOWN, wasD))    BuildNudgeRelative(-step, 0, 0);
        if (EdgeDown(VK_RIGHT, wasR))   BuildNudgeRelative(0, step, 0);
        if (EdgeDown(VK_LEFT, wasL))    BuildNudgeRelative(0, -step, 0);
        if (EdgeDown(VK_PRIOR, wasPgU)) BuildNudgeRelative(0, 0, step);
        if (EdgeDown(VK_NEXT, wasPgD))  BuildNudgeRelative(0, 0, -step);
        if (EdgeDown(VK_HOME, wasRot))  BuildRotateAnchor(1);
    }

    // The mouse is different: a right-click means "place a block" only while
    // the game has focus, and only tells us anything about the target if the
    // target is somewhere you could have been clicking at.
    const bool clickNow = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool clickEdge = clickNow && !wasClick;
    wasClick = clickNow;

    if (!clickEdge || !g_buildRightClickAdvance.load() || !g_buildActive.load()) return;

    // If the client's own placement packet is being watched, it is a better
    // witness than a mouse button, and counting both would advance twice.
    if (PlaceIsWatching()) return;

    // And while the learner is still working it out, the cursor is being moved
    // by the learner itself after each sample - advancing it from here as well
    // would walk past the block it is about to ask for.
    if (g_placeLearning.load()) return;

    // The overlay is the one window we can be sure is not the game, and a
    // right-click in it is someone using the UI, not placing a block.
    const HWND fg = GetForegroundWindow();
    if (!fg || fg == (HWND)overlayHwnd) return;

    const BuildTarget t = BuildGetTarget();
    if (!t.valid) return;
    if (t.distance > (double)g_buildReachBlocks.load()) {
        g_buildClickIgnored.fetch_add(1);
        return;
    }

    AdvanceFrom(BuildBlockState::Done, BuildSource::Click, 1);
    g_buildClickAdvances.fetch_add(1);
}

// =============================================================================
// Can the camera be aimed?
// =============================================================================
namespace {

// No SafeWrite is exported, and a __try cannot share a frame with anything that
// unwinds, so this gets a frame of its own - the same split hooks.cpp uses.
bool RawWrite(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

bool BuildNudgeYaw(float degrees) {
    GsdkFacingPath f;
    { std::lock_guard lock(g_gsdkFacingMtx); f = g_gsdkFacing; }
    if (!f.found) return false;

    const uintptr_t root = GsdkCurrentRoot();
    if (!root) return false;
    const uintptr_t obj = GsdkFollow(root, f.derefs);
    if (!obj) return false;

    float yaw = 0.0f;
    if (!SafeRead((const void*)(obj + f.yawOffset), &yaw, 4)) return false;
    if (!std::isfinite(yaw)) return false;

    // Whatever units the finder decided this field is in. Guessing degrees at a
    // radian field would turn the view by three thousand degrees and look
    // exactly like "the write did not stick".
    const float step = f.radians ? degrees * 3.14159265358979f / 180.0f : degrees;
    float next = yaw + step;

    // Keep it in the range the field was already sitting in, so a wrap does not
    // get mistaken for the write being rejected.
    const float full = f.radians ? 6.28318530717959f : 360.0f;
    while (next >= full) next -= full;
    while (next < 0.0f) next += full;

    return RawWrite((void*)(obj + f.yawOffset), &next, 4);
}

// =============================================================================
// Which number is which block
// =============================================================================
namespace {

std::mutex g_blockIdMtx;
std::unordered_map<std::string, uint32_t> g_nameToId;
std::unordered_map<uint32_t, std::string> g_idToName;
std::string g_blockIdConflict;

std::wstring BlockIdFilePath() {
    return GetThisModuleDirectory() + L"block_ids.txt";
}

} // namespace

void BuildNoteBlockId(const std::string& name, uint32_t id) {
    if (name.empty() || !id) return;

    // A recorded build writes blocks it could not name as "Unknown_<id>", and
    // building from that prefab would otherwise teach the table that the block
    // really is called that. It then counts as a known name, and the registry
    // search checks candidate arrays against it and rules out the right one for
    // disagreeing. A placeholder is not a fact.
    if (name.rfind("Unknown_", 0) == 0) return;

    bool changed = false;
    {
        std::lock_guard lock(g_blockIdMtx);
        auto it = g_nameToId.find(name);
        if (it == g_nameToId.end()) {
            g_nameToId[name] = id;
            g_idToName[id] = name;
            changed = true;
        }
        else if (it->second != id) {
            // Not a conflict to resolve - a sign that the field being read is
            // not the block id. Averaging it or taking the newest would bury
            // exactly the fact worth knowing.
            char buf[256];
            snprintf(buf, sizeof(buf),
                "%s read as %u before and %u now - the field being used for the "
                "block id is probably not the block id",
                name.c_str(), it->second, id);
            g_blockIdConflict = buf;
        }
    }
    if (changed) BuildSaveBlockIds();
}

uint32_t BuildGetBlockId(const std::string& name) {
    std::lock_guard lock(g_blockIdMtx);
    auto it = g_nameToId.find(name);
    return (it == g_nameToId.end()) ? 0u : it->second;
}

std::string BuildGetBlockName(uint32_t id) {
    {
        std::lock_guard lock(g_blockIdMtx);
        auto it = g_idToName.find(id);
        if (it != g_idToName.end()) return it->second;
    }
    // Nothing watched you place this one, so ask the client's own table. That
    // is the whole point of finding it: names for blocks you have never
    // touched, which is every block in somebody else's building.
    return BlockRegLookup(id);
}

size_t BuildKnownBlockIds() {
    std::lock_guard lock(g_blockIdMtx);
    return g_nameToId.size();
}

std::vector<std::string> BuildBlockNameList() {
    // Rebuilt only when the table grows. It is five thousand names, asked for
    // every frame while the picker is open.
    static std::vector<std::string> cached;
    static size_t cachedFor = (size_t)-1;
    std::lock_guard lock(g_blockIdMtx);
    if (cachedFor != g_nameToId.size()) {
        cached.clear();
        for (const auto& kv : g_nameToId) {
            const std::string& n = kv.first;
            // State variants are what a block BECOMES, not what you hold.
            if (n.empty() || n[0] == '*' || n.rfind("Unknown_", 0) == 0 || n == "Empty") continue;
            cached.push_back(n);
        }
        std::sort(cached.begin(), cached.end());
        cachedFor = g_nameToId.size();
    }
    return cached;
}

std::string BuildBlockIdConflict() {
    std::lock_guard lock(g_blockIdMtx);
    return g_blockIdConflict;
}

int BuildPurgeUnknownNames() {
    int removed = 0;
    {
        std::lock_guard lock(g_blockIdMtx);
        for (auto it = g_nameToId.begin(); it != g_nameToId.end(); ) {
            if (it->first.rfind("Unknown_", 0) == 0) {
                auto back = g_idToName.find(it->second);
                if (back != g_idToName.end() && back->second == it->first)
                    g_idToName.erase(back);
                it = g_nameToId.erase(it);
                ++removed;
            }
            else ++it;
        }
    }
    if (removed) BuildSaveBlockIds();
    return removed;
}

std::vector<std::pair<std::string, uint32_t>> BuildKnownBlockIdPairs(size_t limit) {
    // Placeholders are worse than nothing here: they are wrong on purpose, and
    // a wrong check discards the right answer.
    BuildPurgeUnknownNames();

    std::vector<std::pair<std::string, uint32_t>> out;
    {
        std::lock_guard lock(g_blockIdMtx);
        out.assign(g_nameToId.begin(), g_nameToId.end());
    }
    // Longest first: a long name is far less likely to turn up by accident in
    // several gigabytes of memory than a short one.
    std::sort(out.begin(), out.end(),
        [](const std::pair<std::string, uint32_t>& a,
           const std::pair<std::string, uint32_t>& b) {
            if (a.first.size() != b.first.size()) return a.first.size() > b.first.size();
            return a.first < b.first;
        });
    if (out.size() > limit) out.resize(limit);
    return out;
}

namespace {
std::mutex g_probeMtx;
std::vector<BlockProbe> g_probes;
}

std::vector<BlockProbe> BuildGetProbes() {
    std::lock_guard lock(g_probeMtx);
    return g_probes;
}

void BuildAddProbe(int x, int y, int z, uint32_t id) {
    if (!id) return;
    std::lock_guard lock(g_probeMtx);
    for (BlockProbe& p : g_probes)
        if (p.x == x && p.y == y && p.z == z) { p.id = id; return; }
    if (g_probes.size() < 4096) g_probes.push_back(BlockProbe{ x, y, z, id });
}

void BuildClearProbes() {
    std::lock_guard lock(g_probeMtx);
    g_probes.clear();
}

int BuildLearnFromBlockGet(const std::string& text) {
    int learned = 0;
    size_t i = 0;
    while (i < text.size()) {
        size_t eol = text.find('\n', i);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(i, eol - i);
        i = eol + 1;

        // "Block at (x, y, z) is <Name> (<id>, support=N)". The id is printed
        // with thousands separators, which is why it is read a character at a
        // time rather than handed straight to a number parser.
        const size_t isAt = line.find(" is ");
        if (isAt == std::string::npos) continue;
        const size_t nameFrom = isAt + 4;
        const size_t paren = line.find(" (", nameFrom);
        if (paren == std::string::npos) continue;

        const std::string name = line.substr(nameFrom, paren - nameFrom);
        if (name.empty()) continue;

        std::string digits;
        for (size_t k = paren + 2; k < line.size(); ++k) {
            const char ch = line[k];
            if (isdigit((unsigned char)ch)) digits += ch;
            else if (ch == ',') continue;          // thousands separator
            else break;
        }
        if (digits.empty()) continue;

        const uint32_t id = (uint32_t)strtoul(digits.c_str(), nullptr, 10);
        // Zero is a real id - Empty - so it is worth keeping, but it is also
        // what a failed parse produces, so it only counts with a real name.
        if (name == "Empty" || id != 0) {
            const bool isNew = (BuildGetBlockId(name) == 0) && (id != 0);
            // The game said so, which beats anything we worked out by watching.
            // A value learned off a packet field can be wrong; this cannot.
            {
                std::lock_guard lock(g_blockIdMtx);
                if (name.rfind("Unknown_", 0) != 0 && id != 0) {
                    g_nameToId[name] = id;
                    g_idToName[id] = name;
                }
            }
            if (isNew) ++learned;
        }

        // The coordinates matter as much as the id. "Block 1238 exists" is
        // nothing to go on; "block 1238 is at 167, 82, -195" is a value at a
        // known place, which is the only kind of clue that has ever worked here.
        const size_t atOpen = line.find('(');
        if (atOpen != std::string::npos && atOpen < isAt) {
            int cx = 0, cy = 0, cz = 0;
            if (sscanf_s(line.c_str() + atOpen, "(%d, %d, %d)", &cx, &cy, &cz) == 3) {
                std::lock_guard lock(g_probeMtx);
                bool dup = false;
                for (BlockProbe& p : g_probes)
                    if (p.x == cx && p.y == cy && p.z == cz) { p.id = id; dup = true; break; }
                if (!dup && g_probes.size() < 4096)
                    g_probes.push_back(BlockProbe{ cx, cy, cz, id });
            }
        }
    }
    return learned;
}

bool BuildAnyKnownBlockId(std::string& name, uint32_t& id) {
    std::lock_guard lock(g_blockIdMtx);
    if (g_nameToId.empty()) return false;
    // Whichever has the longest name. A long name is far less likely to turn up
    // by accident in memory than a short one, and this is about to go looking
    // for it in several gigabytes.
    const std::pair<const std::string, uint32_t>* best = nullptr;
    for (const auto& kv : g_nameToId)
        if (!best || kv.first.size() > best->first.size()) best = &kv;
    name = best->first;
    id = best->second;
    return true;
}

bool BuildSaveBlockIds() {
    std::string out = "# block id for each block name, learned by watching you build\n";
    {
        std::lock_guard lock(g_blockIdMtx);
        for (const auto& kv : g_nameToId) {
            out += std::to_string(kv.second);
            out += '\t';
            out += kv.first;
            out += '\n';
        }
    }

    HANDLE h = CreateFileW(BlockIdFilePath().c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &wrote, nullptr);
    CloseHandle(h);
    return ok && wrote == out.size();
}

bool BuildLoadBlockIds() {
    std::string text, err;
    if (!ReadWholeFile(WideToUtf8Path(BlockIdFilePath()), text, err)) return false;

    std::lock_guard lock(g_blockIdMtx);
    g_nameToId.clear();
    g_idToName.clear();

    size_t i = 0;
    while (i < text.size()) {
        size_t eol = text.find('\n', i);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(i, eol - i);
        i = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const uint32_t id = (uint32_t)strtoul(line.substr(0, tab).c_str(), nullptr, 10);
        const std::string name = line.substr(tab + 1);
        if (id && !name.empty()) {
            g_nameToId[name] = id;
            g_idToName[id] = name;
        }
    }
    return true;
}

// =============================================================================
// Recording what you build
// =============================================================================
std::atomic<bool> g_buildRecording{ false };

namespace {

std::mutex g_recMtx;
struct RecordedBlock { int32_t x, y, z; uint32_t id; };
std::vector<RecordedBlock> g_recorded;

// Same coordinate twice means you replaced something. The later one wins, and
// the earlier is dropped rather than both being written - a prefab with two
// blocks in one place is not a prefab.
void RecordAddLocked(int wx, int wy, int wz, uint32_t id) {
    for (RecordedBlock& b : g_recorded) {
        if (b.x == wx && b.y == wy && b.z == wz) { b.id = id; return; }
    }
    if (g_recorded.size() >= 200000) return;
    g_recorded.push_back(RecordedBlock{ wx, wy, wz, id });
}

} // namespace

void BuildRecordStart() { g_buildRecording.store(true); }
void BuildRecordStop() { g_buildRecording.store(false); }

void BuildRecordClear() {
    std::lock_guard lock(g_recMtx);
    g_recorded.clear();
}

void BuildRecordAdd(int wx, int wy, int wz, uint32_t id) {
    if (!g_buildRecording.load() || !id) return;
    std::lock_guard lock(g_recMtx);
    RecordAddLocked(wx, wy, wz, id);
}

size_t BuildRecordCount() {
    std::lock_guard lock(g_recMtx);
    return g_recorded.size();
}

bool BuildRecordBounds(int& minX, int& minY, int& minZ,
    int& maxX, int& maxY, int& maxZ) {
    std::lock_guard lock(g_recMtx);
    if (g_recorded.empty()) return false;
    minX = maxX = g_recorded[0].x;
    minY = maxY = g_recorded[0].y;
    minZ = maxZ = g_recorded[0].z;
    for (const RecordedBlock& b : g_recorded) {
        if (b.x < minX) minX = b.x;  if (b.x > maxX) maxX = b.x;
        if (b.y < minY) minY = b.y;  if (b.y > maxY) maxY = b.y;
        if (b.z < minZ) minZ = b.z;  if (b.z > maxZ) maxZ = b.z;
    }
    return true;
}

std::string BuildRecordSummary() {
    int a, b, c, d, e, f;
    const size_t n = BuildRecordCount();
    if (!n) return "nothing recorded yet";
    if (!BuildRecordBounds(a, b, c, d, e, f)) return std::to_string(n) + " blocks";

    char buf[160];
    snprintf(buf, sizeof(buf), "%zu blocks, %d x %d x %d",
        n, d - a + 1, e - b + 1, f - c + 1);
    return buf;
}

bool BuildRecordSavePrefab(const std::string& name, std::string& err) {
    std::vector<RecordedBlock> blocks;
    {
        std::lock_guard lock(g_recMtx);
        blocks = g_recorded;
    }
    if (blocks.empty()) { err = "nothing has been recorded"; return false; }

    // A name that is a file name. Anything else is asking for a path to be
    // written somewhere it was not meant to go.
    std::string stem;
    for (char ch : name) {
        if (isalnum((unsigned char)ch) || ch == '_' || ch == '-') stem += ch;
        else if (ch == ' ') stem += '_';
    }
    if (stem.empty()) { err = "give it a name"; return false; }

    // Relative to the lowest corner, so the anchor is the block you would stand
    // on to place it again.
    int minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    BuildRecordBounds(minX, minY, minZ, maxX, maxY, maxZ);

    std::string json;
    json.reserve(blocks.size() * 80);
    json += "{\n  \"version\": 8,\n  \"blockIdVersion\": 10,\n";
    json += "  \"anchorX\": 0,\n  \"anchorY\": 0,\n  \"anchorZ\": 0,\n";
    json += "  \"blocks\": [\n";

    size_t unknown = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const RecordedBlock& b = blocks[i];
        std::string blockName = BuildGetBlockName(b.id);
        if (blockName.empty()) {
            blockName = "Unknown_" + std::to_string(b.id);
            ++unknown;
        }
        char head[160];
        snprintf(head, sizeof(head),
            "    { \"x\": %d, \"y\": %d, \"z\": %d, \"name\": \"",
            b.x - minX, b.y - minY, b.z - minZ);
        json += head;
        json += JsonEscape(blockName);
        json += "\" }";
        json += (i + 1 < blocks.size()) ? ",\n" : "\n";
    }
    json += "  ],\n  \"fluids\": []\n}\n";

    const std::wstring dir = GetThisModuleDirectory() + L"prefabs";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\" + Utf8ToWide(stem) + L".json";

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "could not create the file"; return false; }
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, json.data(), (DWORD)json.size(), &wrote, nullptr);
    CloseHandle(h);
    if (!ok || wrote != json.size()) { err = "the write did not complete"; return false; }

    if (unknown)
        err = "saved, but " + std::to_string(unknown) + " block(s) had no name yet "
              "and were written as Unknown_<id> - build one of each from a prefab "
              "to teach it the names";
    PrefabScanFolder();
    return true;
}

// =============================================================================
// Every block name at once
// =============================================================================
// First attempt looked for a flat array of names indexed by block id. It found
// the text twenty-nine times over and no such array, which is an answer rather
// than a failure: the names are not stored that way. Almost certainly each
// block is a definition object that HOLDS a name, and the array is of those.
//
// So this goes one level further, and stops assuming the index:
//
//   1. Find the name as a real string object.
//   2. Find every field that points at it. Each such field sits inside some
//      object - the definition - and the distance back to the object's start
//      is unknown, so every plausible start is tried and checked against the
//      object's own declared size.
//   3. Find every field that points at one of those definitions, and look
//      backwards from it for an array header that would put it on an element
//      boundary.
//
// What it does NOT do is insist the element index equals the block id. It
// reports the index it found, because if those differ the difference is the
// interesting part - a dictionary keyed by hash, or an id space with an offset.
// Deciding in advance what the answer has to look like is what made the first
// attempt come back empty.
std::atomic<bool> g_blockRegScanning{ false };

namespace {

// .NET string: MethodTable at +0, char count at +8, UTF-16 from +0xC.
constexpr uint32_t kStrLenOffset = 8;
constexpr uint32_t kStrCharOffset = 12;

// How far back from a reference to look for the array that contains it. At
// eight bytes an element that covers sixteen thousand entries, which is far
// more blocks than any game has.
constexpr size_t kLookBack = 128u * 1024u;

struct RegLayout {
    uintptr_t arr = 0;
    int64_t len = 0;
    uint32_t stride = 8;       // bytes per element
    bool viaDef = false;       // the element points at a definition object
    uint32_t nameOffset = 0;   // where the name sits inside that definition
    int64_t index = 0;         // where the block we searched with turned up
};

std::mutex g_regMtx;
RegLayout g_reg;
bool g_regFound = false;
std::string g_regStatusText = "not found yet";
std::string g_regReportText;

void SetRegStatus(const std::string& s) {
    std::lock_guard lock(g_regMtx);
    g_regStatusText = s;
}

// Walks committed, readable, private memory in chunks. Image and mapped
// regions are skipped: a registry built at runtime lives on the heap.
void ScanCommitted(size_t chunkBytes,
    const std::function<bool(uintptr_t base, const uint8_t* buf, size_t len)>& fn) {

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t at = (uintptr_t)si.lpMinimumApplicationAddress;
    const uintptr_t end = (uintptr_t)si.lpMaximumApplicationAddress;

    std::vector<uint8_t> buf(chunkBytes);

    while (at < end) {
        if (g_shuttingDown.load()) return;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((LPCVOID)at, &mbi, sizeof(mbi))) break;

        const uintptr_t regionBase = (uintptr_t)mbi.BaseAddress;
        const size_t regionSize = mbi.RegionSize;

        const bool usable =
            mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));

        if (usable && regionSize) {
            for (size_t off = 0; off < regionSize; off += chunkBytes) {
                size_t want = regionSize - off;
                if (want > chunkBytes) want = chunkBytes;
                if (!SafeRead((const void*)(regionBase + off), buf.data(), want)) continue;
                if (!fn(regionBase + off, buf.data(), want)) return;
            }
        }

        if (regionSize == 0) break;
        at = regionBase + regionSize;
    }
}

// Reads the object's declared size out of its MethodTable, which is how we can
// tell "this field is inside this object" from "this address happens to be a
// few bytes after something".
bool ObjectSizeAt(uintptr_t obj, uint32_t& outSize) {
    uint64_t mt = 0;
    if (!SafeRead((const void*)obj, &mt, 8)) return false;
    if (!mt || (mt & 7) || !IsPlausiblePtr((const void*)mt)) return false;
    uint32_t base = 0;
    if (!SafeRead((const void*)((uintptr_t)mt + 4), &base, 4)) return false;
    if (base < 16 || base > 0x10000) return false;
    outSize = base;
    return true;
}

// Given a field that holds a pointer we care about, find an array whose element
// boundary that field lands on. Reads one window and scans it in memory: doing
// it read by read is a hundred thousand guarded calls per candidate.
bool FindArrayAround(uintptr_t fieldAddr, uintptr_t& outArr, int64_t& outLen,
    uint32_t& outStride, int64_t& outIndex) {

    const size_t back = (fieldAddr > kLookBack) ? kLookBack : (size_t)fieldAddr;
    if (back < 0x18) return false;
    const uintptr_t winBase = fieldAddr - back;

    std::vector<uint8_t> win(back);
    // Tolerate holes: read in pages and leave anything unreadable as zeroes,
    // which simply fails the header checks below.
    const size_t kPage = 4096;
    for (size_t off = 0; off < back; off += kPage) {
        size_t want = back - off;
        if (want > kPage) want = kPage;
        SafeRead((const void*)(winBase + off), win.data() + off, want);
    }

    static const uint32_t kStrides[] = { 8, 16, 24, 32, 40, 48 };
    for (uint32_t stride : kStrides) {
        // The header sits 0x10 + index*stride before the field.
        for (size_t dist = 0x10; dist + 16 <= back; dist += stride) {
            const size_t at = back - dist;
            uint64_t mt = 0;
            memcpy(&mt, win.data() + at, 8);
            if (!mt || (mt & 7) || !IsPlausiblePtr((const void*)mt)) continue;
            int64_t len = 0;
            memcpy(&len, win.data() + at + 8, 8);
            if (len <= 0 || len > 262144) continue;

            const int64_t idx = (int64_t)((dist - 0x10) / stride);
            if (idx >= len) continue;

            outArr = winBase + at;
            outLen = len;
            outStride = stride;
            outIndex = idx;
            return true;
        }
    }
    return false;
}

} // namespace

std::string BlockRegStatus() {
    std::lock_guard lock(g_regMtx);
    return g_regStatusText;
}

std::string BlockRegReport() {
    std::lock_guard lock(g_regMtx);
    return g_regReportText;
}

bool BlockRegFound() {
    std::lock_guard lock(g_regMtx);
    return g_regFound;
}

size_t BlockRegSize() {
    std::lock_guard lock(g_regMtx);
    return (size_t)(g_regFound && g_reg.len > 0 ? g_reg.len : 0);
}

namespace {

// Decodes the string a layout points at for a given index. Used both to answer
// lookups and, during the search, to check a candidate against what we already
// know - which is the only thing that turns a candidate into an answer.
std::string ReadNameVia(const RegLayout& L, int64_t index) {
    if (!L.arr || index < 0 || index >= L.len) return std::string();

    const uintptr_t elem = L.arr + 0x10 + (uint64_t)index * L.stride;
    uint64_t strObj = 0;

    if (L.viaDef) {
        uint64_t def = 0;
        if (!SafeRead((const void*)elem, &def, 8)) return std::string();
        if (!def || (def & 7) || !IsPlausiblePtr((const void*)def)) return std::string();
        if (!SafeRead((const void*)((uintptr_t)def + L.nameOffset), &strObj, 8))
            return std::string();
    }
    else {
        if (!SafeRead((const void*)elem, &strObj, 8)) return std::string();
    }

    if (!strObj || (strObj & 7) || !IsPlausiblePtr((const void*)strObj)) return std::string();

    int32_t chars = 0;
    if (!SafeRead((const void*)((uintptr_t)strObj + kStrLenOffset), &chars, 4))
        return std::string();
    if (chars <= 0 || chars > 200) return std::string();

    std::vector<wchar_t> w((size_t)chars + 1, 0);
    if (!SafeRead((const void*)((uintptr_t)strObj + kStrCharOffset), w.data(),
        (size_t)chars * 2)) return std::string();
    return WideToUtf8(w.data(), chars);
}

} // namespace

std::string BlockRegLookup(uint32_t id) {
    RegLayout L;
    {
        std::lock_guard lock(g_regMtx);
        if (!g_regFound) return std::string();
        L = g_reg;
    }
    return ReadNameVia(L, (int64_t)id);
}

size_t BlockRegSeedNames() {
    RegLayout L;
    {
        std::lock_guard lock(g_regMtx);
        if (!g_regFound) return 0;
        L = g_reg;
    }

    size_t taken = 0;
    {
        std::lock_guard lock(g_blockIdMtx);
        for (int64_t i = 0; i < L.len; ++i) {
            const std::string nm = ReadNameVia(L, i);
            if (nm.empty()) continue;
            g_idToName[(uint32_t)i] = nm;
            // Several ids can share a name where a block has state variants, so
            // the reverse direction keeps the first and does not fight over it.
            if (g_nameToId.find(nm) == g_nameToId.end())
                g_nameToId[nm] = (uint32_t)i;
            ++taken;
        }
    }
    BuildSaveBlockIds();
    return taken;
}

void BlockRegFind() {
    if (g_blockRegScanning.exchange(true)) return;

    StartWorker([]() {
        WorkerGuard guard;

        // Two known blocks, not one. Searching with a single name means every
        // array that happens to hold it anywhere is a candidate, and the last
        // run turned up sixty-four of them with the real one crowded out. An
        // array that holds TWO named blocks, each at the index its own id
        // names, is not something that happens by accident.
        const auto pairs = BuildKnownBlockIdPairs(6);
        if (pairs.size() < 2) {
            SetRegStatus("needs two known blocks, and only " +
                std::to_string(pairs.size()) + " is known. Run /block get on a "
                "couple of blocks and paste the chat lines in below - that is "
                "the quickest way to get some.");
            g_blockRegScanning.store(false);
            return;
        }

        const long long startedMs = NowSteadyMs();
        std::string report;
        char line[400];

        const std::string nameA = pairs[0].first;
        const uint32_t idA = pairs[0].second;
        snprintf(line, sizeof(line), "anchor: %s = %u\nchecking against:",
            nameA.c_str(), idA);
        report = line;
        for (size_t i = 1; i < pairs.size(); ++i) {
            snprintf(line, sizeof(line), " %s=%u", pairs[i].first.c_str(), pairs[i].second);
            report += line;
        }
        report += "\n";

        // ---- 1. the anchor name, as a real string object ------------------
        SetRegStatus("finding \"" + nameA + "\" in memory...");
        const std::wstring wantW = Utf8ToWide(nameA);
        const int32_t wantLen = (int32_t)wantW.size();
        const size_t wantBytes = wantW.size() * 2;

        std::unordered_set<uint64_t> strObjects;
        ScanCommitted(1u << 20, [&](uintptr_t base, const uint8_t* buf, size_t len) {
            if (len < wantBytes + kStrCharOffset) return true;
            for (size_t i = kStrCharOffset; i + wantBytes <= len; i += 2) {
                if (memcmp(buf + i, wantW.data(), wantBytes) != 0) continue;
                int32_t gotLen = 0;
                memcpy(&gotLen, buf + i - (kStrCharOffset - kStrLenOffset), 4);
                if (gotLen != wantLen) continue;
                strObjects.insert((uint64_t)(base + i - kStrCharOffset));
                if (strObjects.size() >= 128) return false;
            }
            return true;
            });

        snprintf(line, sizeof(line), "%zu string object(s)\n", strObjects.size());
        report += line;
        if (strObjects.empty()) {
            std::lock_guard lock(g_regMtx);
            g_regReportText = report;
            SetRegStatus("that name is not in memory as a string object");
            g_blockRegScanning.store(false);
            return;
        }

        // ---- 2. what holds it ---------------------------------------------
        SetRegStatus("finding what holds it...");
        std::vector<uintptr_t> directFields;
        std::unordered_map<uint64_t, std::vector<uint32_t>> defOffsets;  // object -> offsets

        ScanCommitted(1u << 20, [&](uintptr_t base, const uint8_t* buf, size_t len) {
            for (size_t i = 0; i + 8 <= len; i += 8) {
                uint64_t v = 0;
                memcpy(&v, buf + i, 8);
                if (!v || strObjects.find(v) == strObjects.end()) continue;

                const uintptr_t field = base + i;
                directFields.push_back(field);
                for (uint32_t k = 8; k <= 256 && k <= field; k += 8) {
                    const uintptr_t obj = field - k;
                    uint32_t objSize = 0;
                    if (!ObjectSizeAt(obj, objSize)) continue;
                    if (k + 8 > objSize) continue;
                    defOffsets[(uint64_t)obj].push_back(k);
                }
                if (directFields.size() >= 512) return false;
            }
            return true;
            });

        snprintf(line, sizeof(line), "%zu field(s) point at it, %zu holder(s)\n",
            directFields.size(), defOffsets.size());
        report += line;

        // ---- 3. arrays, tested rather than guessed ------------------------
        // The array is no longer found by walking backwards hoping to trip over
        // a header. If a field is element `idA`, the array starts at a known
        // place - so that is computed directly and then checked, which is both
        // sharper and far cheaper.
        SetRegStatus("testing arrays...");

        struct Cand { RegLayout layout; int agree; std::string atA; };
        std::vector<Cand> cands;

        const int wantAgree = (int)pairs.size() - 1;

        auto consider = [&](uintptr_t arr, uint32_t stride, bool viaDef, uint32_t nameOff) {
            // One entry per ARRAY, not per candidate offset. Keeping the offset
            // in this key is what let a single array appear eight times over and
            // fill the whole list while the right one never got in.
            Cand* existing = nullptr;
            for (Cand& c : cands)
                if (c.layout.arr == arr && c.layout.stride == stride &&
                    c.layout.viaDef == viaDef) { existing = &c; break; }
            if (existing && existing->agree >= wantAgree) return;   // already perfect

            uint64_t mt = 0;
            if (!SafeRead((const void*)arr, &mt, 8)) return;
            if (!mt || (mt & 7) || !IsPlausiblePtr((const void*)mt)) return;
            int64_t alen = 0;
            if (!SafeRead((const void*)(arr + 8), &alen, 8)) return;
            if (alen <= (int64_t)idA || alen > 262144) return;

            RegLayout L;
            L.arr = arr; L.len = alen; L.stride = stride;
            L.viaDef = viaDef; L.nameOffset = nameOff; L.index = (int64_t)idA;

            const std::string atA = ReadNameVia(L, (int64_t)idA);
            if (atA != nameA) return;

            // Every other block we know has to land correctly too.
            int agree = 0;
            for (size_t i = 1; i < pairs.size(); ++i) {
                if ((int64_t)pairs[i].second >= alen) continue;
                if (ReadNameVia(L, (int64_t)pairs[i].second) == pairs[i].first) ++agree;
            }

            if (existing) {
                // Same array, a different guess at where the name sits inside
                // the definition. Whichever explains more of the known blocks
                // wins; the rest are noise from the same pointer.
                if (agree > existing->agree) {
                    existing->layout = L;
                    existing->agree = agree;
                    existing->atA = atA;
                }
                return;
            }
            cands.push_back(Cand{ L, agree, atA });
            };

        static const uint32_t kStrides[] = { 8, 16, 24, 32 };

        // The flat case: the array element is the name itself.
        for (uintptr_t f : directFields)
            for (uint32_t s : kStrides) {
                const uint64_t head = 0x10 + (uint64_t)idA * s;
                if (f > head) consider(f - (uintptr_t)head, s, false, 0);
            }

        // The definition case: the element points at an object holding the name.
        if (!defOffsets.empty()) {
            ScanCommitted(1u << 20, [&](uintptr_t base, const uint8_t* buf, size_t len) {
                for (size_t i = 0; i + 8 <= len; i += 8) {
                    uint64_t v = 0;
                    memcpy(&v, buf + i, 8);
                    if (!v) continue;
                    auto it = defOffsets.find(v);
                    if (it == defOffsets.end()) continue;

                    const uintptr_t field = base + i;
                    for (uint32_t s : kStrides) {
                        const uint64_t head = 0x10 + (uint64_t)idA * s;
                        if (field <= head) continue;
                        for (uint32_t k : it->second)
                            consider(field - (uintptr_t)head, s, true, k);
                    }
                    // No early stop on count any more. With one entry per
                    // array the list stays short by itself, and stopping early
                    // is exactly what kept discarding the answer.
                    if (cands.size() >= 512) return false;
                }
                return true;
                });
        }

        snprintf(line, sizeof(line), "%zu array(s) hold %s at index %u\n\n",
            cands.size(), nameA.c_str(), idA);
        report += line;

        // ---- 4. the one that agrees with everything -----------------------
        RegLayout best;
        int bestAgree = -1;
        for (const Cand& c : cands) {
            snprintf(line, sizeof(line),
                "  arr 0x%llX  len %lld  stride %u  %s  agrees with %d of %zu others\n"
                "    [0]=\"%s\"  [%lld]=\"%s\"\n",
                (unsigned long long)c.layout.arr, (long long)c.layout.len,
                c.layout.stride, c.layout.viaDef ? "via a definition" : "name directly",
                c.agree, pairs.size() - 1,
                ReadNameVia(c.layout, 0).c_str(),
                (long long)(c.layout.len / 2),
                ReadNameVia(c.layout, c.layout.len / 2).c_str());
            report += line;

            if (c.agree > bestAgree) { bestAgree = c.agree; best = c.layout; }
        }

        const bool haveBest = bestAgree > 0;
        {
            std::lock_guard lock(g_regMtx);
            g_reg = haveBest ? best : RegLayout{};
            g_regFound = haveBest;
            g_regReportText = report;
        }

        const long long tookMs = NowSteadyMs() - startedMs;
        char msg[260];
        if (haveBest) {
            const size_t taken = BlockRegSeedNames();
            snprintf(msg, sizeof(msg),
                "found it - %zu names copied out and saved, so this never has to "
                "run again (%lld entries, %lld ms)", taken,
                (long long)best.len, (long long)tookMs);
            SetRegStatus(msg);
        }
        else if (!cands.empty()) {
            SetRegStatus("found arrays holding the anchor at its own index, but "
                "none of them place the other known blocks correctly. The report "
                "shows what each one holds.");
        }
        else {
            SetRegStatus("no array holds that block at the index its id names");
        }

        g_blockRegScanning.store(false);
        });
}
