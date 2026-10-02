#include "pch.h"
#include "prefab.hpp"
#include "buildsession.hpp"
#include "common.hpp"
#include <cstdio>
#include <unordered_set>

extern long long g_testNowMs;
extern std::wstring g_testDir;
extern double g_testPX, g_testPY, g_testPZ;
extern std::unordered_set<uint64_t> g_testTags, g_testInList;
extern int g_testSpawns, g_testUpdates, g_testRemoves, g_testClears;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; \
    printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static void WriteText(const std::wstring& path, const std::string& s) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0; WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr); CloseHandle(h);
}
static std::string ReadAll(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string s(1 << 22, '\0'); DWORD r = 0; ReadFile(h, s.data(), (DWORD)s.size(), &r, nullptr);
    CloseHandle(h); s.resize(r); return s;
}

// World coordinates of block i under the current anchor, via the public API only:
// the target after marking everything before it would be slow, so recompute here.
static void WorldOf(const Prefab& p, size_t i, int& wx, int& wy, int& wz) {
    const PrefabBlock& b = p.blocks[i];
    int rx = b.x, rz = b.z;
    switch (g_buildRotation.load() & 3) {
    case 1: rx = -b.z; rz = b.x; break;
    case 2: rx = -b.x; rz = -b.z; break;
    case 3: rx = b.z; rz = -b.x; break;
    }
    wx = g_buildOriginX.load() + rx; wy = g_buildOriginY.load() + b.y; wz = g_buildOriginZ.load() + rz;
}

int main() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    g_testDir = std::wstring(tmp) + L"hyai_enginetest_" + std::to_wstring(GetTickCount64()) + L"\\";
    CreateDirectoryW(g_testDir.c_str(), nullptr);
    CreateDirectoryW((g_testDir + L"prefabs").c_str(), nullptr);

    // ---- a prefab with everything awkward in it ----------------------------
    // anchor offset, air, a duplicate cell (last wins), state variant names,
    // rotations, two layers, negative coordinates.
    std::string json = "{ \"version\": 8, \"anchorX\": 1, \"anchorY\": 0, \"anchorZ\": -1,\n \"blocks\": [\n";
    const char* names[3] = { "Rock_Stone_Brick", "Wood_Oak_Planks",
                             "*Wood_Village_Wall_White_Full_State_Definitions_Bottom" };
    int n = 0;
    for (int y = 0; y < 2; ++y)
        for (int z = -2; z <= 2; ++z)
            for (int x = -2; x <= 2; ++x) {
                char b[256];
                snprintf(b, sizeof b, "%s{\"x\": %d, \"y\": %d, \"z\": %d, \"name\": \"%s\", \"rotation\": %d}\n",
                    n++ ? "," : "", x, y, z, names[(x + z + 10 + y) % 3], (x + 5) % 4);
                json += b;
            }
    json += ",{\"x\": 9, \"y\": 0, \"z\": 9, \"name\": \"Empty\"}\n";
    json += ",{\"x\": 0, \"y\": 0, \"z\": 0, \"name\": \"Rock_Stone_Brick\", \"rotation\": 3}\n";  // duplicate of an earlier cell
    json += "], \"fluids\": [] }\n";
    const std::wstring srcPath = g_testDir + L"prefabs\\house.json";
    WriteText(srcPath, json);

    Prefab p;
    std::string err;
    CHECK(PrefabParseFile(WideToUtf8Path(srcPath), p, err));
    CHECK(p.blocks.size() == 50);          // 51 real minus 1 duplicate; air dropped
    CHECK(p.airSkipped == 1);
    CHECK(p.cells.size() == 50);
    {
        // the duplicate: last one in the file wins (rotation 3)
        const auto it = p.cells.find(PrefabCellKey(0 - 1, 0, 0 + 1));
        CHECK(it != p.cells.end() && p.blocks[it->second].rotation == 3);
    }

    // ---- the session copy round-trips exactly ------------------------------
    {
        const std::wstring copyPath = g_testDir + L"copy.json";
        CHECK(PrefabWriteJson(p, copyPath, err));
        Prefab q;
        CHECK(PrefabParseFile(WideToUtf8Path(copyPath), q, err));
        CHECK(q.fingerprint == p.fingerprint);
        CHECK(q.blocks.size() == p.blocks.size());
        bool same = true;
        for (size_t i = 0; i < p.blocks.size() && i < q.blocks.size(); ++i) {
            const auto& a = p.blocks[i]; const auto& b = q.blocks[i];
            if (a.x != b.x || a.y != b.y || a.z != b.z || a.rotation != b.rotation ||
                p.names[a.nameId] != q.names[b.nameId]) same = false;
        }
        CHECK(same);
    }

    // ---- start a session: it lands at your feet and shows -------------------
    g_testPX = 100.4; g_testPY = 64.0; g_testPZ = -30.7;
    PrefabScanFolder();
    SessionStartNew(WideToUtf8Path(srcPath));
    CHECK(SessionHasActive());
    const std::string id = SessionActiveId();
    CHECK(!id.empty());
    CHECK(g_buildOriginX.load() == 100 && g_buildOriginY.load() == 64 && g_buildOriginZ.load() == -31);
    CHECK(g_buildActive.load());
    CHECK(GetFileAttributesW((g_testDir + L"build_sessions\\" + Utf8ToWide(id) + L"\\prefab.json").c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(ReadAll(g_testDir + L"build_sessions\\active.txt").find(id) != std::string::npos);

    auto cur = PrefabCurrent();
    CHECK(cur && cur->blocks.size() == 50);

    // ---- every rotation maps world -> block and back -----------------------
    for (int rot = 0; rot < 4; ++rot) {
        g_buildRotation.store(rot);
        bool ok = true;
        for (size_t i = 0; i < cur->blocks.size(); ++i) {
            int wx, wy, wz; WorldOf(*cur, i, wx, wy, wz);
            if (BuildWantsAt(wx, wy, wz) != cur->names[cur->blocks[i].nameId]) ok = false;
            if (!BuildHasTodoAt(wx, wy, wz)) ok = false;
        }
        CHECK(ok);
    }
    g_buildRotation.store(1);

    // ---- drawing: first fill, then no work when nothing changes ------------
    g_testSpawns = 0;
    BuildFrameTick();
    CHECK(g_testSpawns == 50);
    const int spawnsAfterFill = g_testSpawns;
    g_testNowMs += 200;
    BuildFrameTick();
    CHECK(g_testSpawns == spawnsAfterFill);

    // ---- follow along: right block, wrong block, state variant -------------
    size_t done = 0, skipped = 0, total = 0;
    int wx, wy, wz;
    WorldOf(*cur, 0, wx, wy, wz);
    const std::string want0 = cur->names[cur->blocks[0].nameId];
    CHECK(BuildOnPlacement(wx, wy, wz, want0, BuildSource::Follow) == BuildPlaceResult::Placed);
    CHECK(g_buildAnchorLocked.load());                  // first confirmed placement locks it
    CHECK(!BuildMoveAnchor(1, 0, 0));
    CHECK(BuildOnPlacement(wx, wy, wz, want0, BuildSource::Follow) == BuildPlaceResult::AlreadyDone);
    CHECK(BuildOnPlacement(wx + 50, wy, wz, want0, BuildSource::Follow) == BuildPlaceResult::NotInBuild);

    // find a block that wants the '*' state variant, place its base name
    size_t variant = SIZE_MAX, plain = SIZE_MAX;
    for (size_t i = 1; i < cur->blocks.size(); ++i) {
        const std::string& nm = cur->names[cur->blocks[i].nameId];
        if (nm[0] == '*' && variant == SIZE_MAX) variant = i;
        if (nm == "Rock_Stone_Brick" && plain == SIZE_MAX) plain = i;
    }
    WorldOf(*cur, variant, wx, wy, wz);
    CHECK(BuildOnPlacement(wx, wy, wz, "Wood_Village_Wall_White_Full", BuildSource::Follow) == BuildPlaceResult::Placed);

    WorldOf(*cur, plain, wx, wy, wz);
    CHECK(BuildOnPlacement(wx, wy, wz, "Soil_Dirt", BuildSource::Follow) == BuildPlaceResult::WrongBlock);
    CHECK(BuildGetMistakes().size() == 1);
    CHECK(BuildHasTodoAt(wx, wy, wz));
    // drawn red: a redraw happens
    g_testNowMs += 200; g_testUpdates = 0;
    BuildFrameTick();
    CHECK(g_testUpdates >= 1);
    // fixed by placing the right one
    CHECK(BuildOnPlacement(wx, wy, wz, "Rock_Stone_Brick", BuildSource::Follow) == BuildPlaceResult::Placed);
    CHECK(BuildGetMistakes().empty());
    BuildProgress(done, skipped, total);
    CHECK(done == 3 && total == 50);

    // a wrong block that is then accepted "always"
    size_t plain2 = SIZE_MAX;
    for (size_t i = plain + 1; i < cur->blocks.size(); ++i)
        if (cur->names[cur->blocks[i].nameId] == "Rock_Stone_Brick" &&
            [&]{ int a,b,c; WorldOf(*cur,i,a,b,c); return BuildHasTodoAt(a,b,c); }()) { plain2 = i; break; }
    WorldOf(*cur, plain2, wx, wy, wz);
    CHECK(BuildOnPlacement(wx, wy, wz, "Rock_Cobble", BuildSource::Follow) == BuildPlaceResult::WrongBlock);
    BuildAcceptMistake(0, true);
    CHECK(BuildGetSwaps().size() == 1);
    BuildProgress(done, skipped, total);
    CHECK(done == 4);

    // ---- your own materials: a swap changes names, rows, and the check ------
    CHECK(BuildEffectiveName("Rock_Stone_Brick") == "Rock_Cobble");
    CHECK(BuildLastPlacedName() == "Rock_Cobble");
    {
        bool rowOk = false;
        for (const BuildMaterialRow& r : BuildMaterialRows(false))
            if (r.wanted == "Rock_Stone_Brick" && r.mine == "Rock_Cobble" && r.left > 0) rowOk = true;
        CHECK(rowOk);
        // the next stone cell accepts the swap AND the original, and not dirt
        std::vector<size_t> stone;
        for (size_t i = 0; i < cur->blocks.size(); ++i) {
            int a, b2, c2; WorldOf(*cur, i, a, b2, c2);
            if (cur->names[cur->blocks[i].nameId] == "Rock_Stone_Brick" && BuildHasTodoAt(a, b2, c2))
                stone.push_back(i);
        }
        CHECK(stone.size() >= 3);
        WorldOf(*cur, stone[0], wx, wy, wz);
        CHECK(BuildWantsAt(wx, wy, wz) == "Rock_Cobble");
        CHECK(BuildOnPlacement(wx, wy, wz, "Rock_Cobble", BuildSource::Follow) == BuildPlaceResult::Placed);
        WorldOf(*cur, stone[1], wx, wy, wz);
        CHECK(BuildOnPlacement(wx, wy, wz, "Rock_Stone_Brick", BuildSource::Follow) == BuildPlaceResult::Placed);
        WorldOf(*cur, stone[2], wx, wy, wz);
        CHECK(BuildOnPlacement(wx, wy, wz, "Soil_Dirt", BuildSource::Follow) == BuildPlaceResult::WrongBlock);

        // "any block counts" settles what was flagged, and then anything goes
        BuildSetAnyBlock(true);
        CHECK(BuildGetMistakes().empty());
        CHECK(!BuildHasTodoAt(wx, wy, wz));
        size_t planks = SIZE_MAX;
        for (size_t i = 0; i < cur->blocks.size(); ++i) {
            int a, b2, c2; WorldOf(*cur, i, a, b2, c2);
            if (BuildHasTodoAt(a, b2, c2)) { planks = i; break; }
        }
        WorldOf(*cur, planks, wx, wy, wz);
        CHECK(BuildOnPlacement(wx, wy, wz, "Wool_Pink", BuildSource::Follow) == BuildPlaceResult::Placed);
        BuildSetAnyBlock(false);

        // undoing a swap puts the name back
        BuildSetSwap("Wood_Oak_Planks", "Wood_Birch_Planks");
        CHECK(BuildEffectiveName("Wood_Oak_Planks") == "Wood_Birch_Planks");
        BuildSetSwap("Wood_Oak_Planks", "");
        CHECK(BuildEffectiveName("Wood_Oak_Planks") == "Wood_Oak_Planks");
        CHECK(BuildGetSwaps().size() == 1);

        // save my version: what was really placed, swaps elsewhere
        size_t known = 0;
        std::string verr;
        CHECK(BuildSaveMyVersion("house mine", verr, known));
        CHECK(known >= 5);
        Prefab mine;
        CHECK(PrefabParseFile(WideToUtf8Path(g_testDir + L"prefabs\\house_mine.json"), mine, verr));
        CHECK(mine.blocks.size() == 50);
        bool hasDirt = false, hasPink = false, hasCobble = false, hasBrickOriginal = false;
        for (const auto& nm : mine.names) {
            if (nm == "Soil_Dirt") hasDirt = true;
            if (nm == "Wool_Pink") hasPink = true;
            if (nm == "Rock_Cobble") hasCobble = true;
            if (nm == "Rock_Stone_Brick") hasBrickOriginal = true;
        }
        CHECK(hasDirt && hasPink && hasCobble && hasBrickOriginal);
        // and never over an existing file
        CHECK(BuildSaveMyVersion("house mine", verr, known));
        CHECK(verr.find("house_mine_2.json") != std::string::npos);
    }
    BuildProgress(done, skipped, total);

    // ---- undo / redo are real history, groups are one step -----------------
    const size_t depth = BuildUndoDepth();
    const size_t doneAt = done;
    BuildMarkDone(5);                                   // one step
    BuildProgress(done, skipped, total);
    CHECK(done == doneAt + 5);
    CHECK(BuildUndoDepth() == depth + 5);
    CHECK(BuildUndo(1));
    BuildProgress(done, skipped, total);
    CHECK(done == doneAt);
    CHECK(BuildRedo(1));
    BuildProgress(done, skipped, total);
    CHECK(done == doneAt + 5);

    // ---- layers, bulk, and bulk undo ---------------------------------------
    BuildLayerInfo li = BuildGetLayerInfo();
    CHECK(li.valid && li.count == 2);
    const int layerTodo = li.todoHere;
    const int marked = BuildBulkSet(BuildBulk::ThisLayer, 0, BuildBlockState::Done);
    CHECK(marked == layerTodo);
    BuildUndo(1);
    li = BuildGetLayerInfo();
    CHECK(li.todoHere == layerTodo);

    // ---- rewind puts back recent work, redo restores it --------------------
    BuildProgress(done, skipped, total);
    const size_t before = done;
    const int changed = BuildChangedInLastMinutes(5);
    CHECK(changed > 0);
    const int rewound = BuildRewindMinutes(5);
    BuildProgress(done, skipped, total);
    CHECK(rewound == changed && done == 0);
    CHECK(BuildRedo(1));
    BuildProgress(done, skipped, total);
    CHECK(done == before);

    // ---- relog: the list is emptied behind our back -------------------------
    g_testNowMs += 2000;
    BuildFrameTick();                                   // settle
    g_testInList.clear();                               // the client dropped everything
    g_testSpawns = 0;
    const int clearsBefore = g_testClears;
    g_testNowMs += 1500;
    BuildFrameTick();                                   // notices
    CHECK(g_testClears == clearsBefore + 1);
    g_testNowMs += 200;
    BuildFrameTick();                                   // redraws
    BuildProgress(done, skipped, total);
    CHECK(g_testSpawns == (int)(total - done - skipped));

    // ---- save, close, reopen: exactly as it was -----------------------------
    SessionRename("Test house");
    SessionSetNote("hyforger spawn");
    g_buildShowDone.store(true);
    g_buildOrderMode.store(0);
    CHECK(SessionSaveNow());
    BuildProgress(done, skipped, total);
    const size_t doneBefore = done;
    const size_t undoBefore = BuildUndoDepth();
    size_t knownBefore = 0;
    { std::string verr; BuildSaveMyVersion("house before", verr, knownBefore); }
    const int ox = g_buildOriginX.load(), oy = g_buildOriginY.load(), oz = g_buildOriginZ.load();

    SessionClose();
    CHECK(!SessionHasActive());
    CHECK(!PrefabCurrent());
    g_buildShowDone.store(false);
    g_buildOriginX.store(0);

    SessionOpen(id);
    CHECK(SessionHasActive());
    CHECK(SessionActiveName() == "Test house");
    CHECK(SessionActiveNote() == "hyforger spawn");
    BuildProgress(done, skipped, total);
    CHECK(done == doneBefore);
    CHECK(BuildUndoDepth() == undoBefore);
    CHECK(g_buildOriginX.load() == ox && g_buildOriginY.load() == oy && g_buildOriginZ.load() == oz);
    CHECK(g_buildRotation.load() == 1);
    CHECK(g_buildAnchorLocked.load());
    CHECK(g_buildShowDone.load());
    CHECK(g_buildOrderMode.load() == 0);
    CHECK(BuildGetSwaps().size() == 1);
    CHECK(BuildEffectiveName("Rock_Stone_Brick") == "Rock_Cobble");
    {
        size_t knownAfter = 0;
        std::string verr;
        CHECK(BuildSaveMyVersion("house again", verr, knownAfter));
        CHECK(knownAfter == knownBefore);
    }
    CHECK(BuildRedo(0) == false);

    // ---- a crash: auto-resume from active.txt on a "new launch" -------------
    // (the tick's first call reads the pointer; simulate by closing the
    // in-memory side only, as a crash would, then re-reading via SessionOpen)
    const std::wstring stateFile = g_testDir + L"build_sessions\\" + Utf8ToWide(id) + L"\\session.txt";
    CHECK(ReadAll(stateFile).find("end=ok") != std::string::npos);

    // ---- autosave fires after a change --------------------------------------
    g_testNowMs += 5000;
    SessionTick();                                      // first tick: resume logic, list
    g_testNowMs += 5000;
    SessionTick();
    const std::string s1 = ReadAll(stateFile);
    BuildMarkDone(1);
    g_testNowMs += 100;  SessionTick();                 // too soon
    g_testNowMs += 700;  SessionTick();                 // quiet long enough: saves
    const std::string s2 = ReadAll(stateFile);
    CHECK(s1 != s2);
    CHECK(GetFileAttributesW((stateFile + L".bak").c_str()) != INVALID_FILE_ATTRIBUTES);

    // ---- a damaged newest save falls back to the backup ---------------------
    SessionClose();
    const std::string good = ReadAll(stateFile);
    WriteText(stateFile, good.substr(0, good.size() / 2));   // cut short, no end=ok
    SessionOpen(id);
    CHECK(SessionHasActive());
    CHECK(SessionStatus().find("damaged") != std::string::npos);
    CHECK(GetFileAttributesW((stateFile + L".damaged").c_str()) != INVALID_FILE_ATTRIBUTES);

    // ---- the session's copy was edited: progress is not misapplied ---------
    SessionClose();
    {
        const std::wstring copyPath = g_testDir + L"build_sessions\\" + Utf8ToWide(id) + L"\\prefab.json";
        std::string c = ReadAll(copyPath);
        const size_t at = c.find("Wood_Oak_Planks");
        c.replace(at, 15, "Wood_Oak_Trunks");
        WriteText(copyPath, c);
    }
    SessionOpen(id);
    CHECK(SessionHasActive());
    BuildProgress(done, skipped, total);
    CHECK(done == 0);
    CHECK(SessionStatus().find("not the one") != std::string::npos);

    // ---- list, duplicate, delete --------------------------------------------
    SessionRefreshList();
    CHECK(SessionList().size() == 1);
    SessionDuplicate(id, false);
    SessionRefreshList();
    const auto list = SessionList();
    CHECK(list.size() == 2);
    std::string copyId;
    for (const auto& s : list) if (!s.active) copyId = s.id;
    CHECK(!copyId.empty());
    CHECK(SessionDelete(copyId));
    SessionRefreshList();
    CHECK(SessionList().size() == 1);
    CHECK(!SessionDelete("..\\evil"));

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
