// Tests for buildscript.cpp: the script -> prefab expansion, the checks on block
// names, the rotation tables, and that the Build tab's own parser reads what is
// written. The rotation tables are checked against a prefab exported by the game
// itself (x64/Release/prefabs/test_house.json) when it is present.
#include "pch.h"
#include "buildscript.hpp"
#include "prefab.hpp"
#include "common.hpp"

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

using namespace BuildScript;

extern std::wstring g_testDir;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const std::string& what) {
    if (ok) { ++g_pass; return; }
    ++g_fail;
    std::printf("FAIL: %s\n", what.c_str());
}
template <class A, class B>
static void Eq(const A& got, const B& want, const std::string& what) {
    if (got == want) { ++g_pass; return; }
    ++g_fail;
    std::ostringstream os;
    os << "FAIL: " << what << "\n   got:  " << got << "\n   want: " << want << "\n";
    std::printf("%s", os.str().c_str());
}

static NameBook MakeBook() {
    NameBook b;
    for (const char* n : { "Rock_Stone_Brick", "Rock_Stone_Brick_Stairs", "Rock_Stone_Cobble", "Wood_Darkwood_Planks",
                           "Wood_Darkwood_Roof", "Wood_Darkwood_Roof_Flat", "Wood_Darkwood_Beam",
                           "Furniture_Village_Door", "Furniture_Village_Window", "Plant_Flower_Common_Pink",
                           "Rock_Stone", "Rock_Stone_Mossy", "Wood_Softwood_Planks", "Wood_Village_Wall_White_Full",
                           "*Wood_Oak_Trunk_State_Definitions_Stripped" })
        b.Add(n);
    return b;
}

static Result Run(const std::string& script, const NameBook& book) {
    Result r;
    Options o;
    Expand(script, book, o, r);
    return r;
}

static const Block* Find(const Result& r, int x, int y, int z) {
    for (const Block& b : r.blocks) if (b.x == x && b.y == y && b.z == z) return &b;
    return nullptr;
}
static int RotOf(const Block* b) { return b ? (b->rotation < 0 ? 0 : b->rotation) : -99; }

static std::string Script(const std::string& ops, const std::string& palette = "{}") {
    return "{\"name\":\"t\",\"palette\":" + palette + ",\"ops\":[" + ops + "]}";
}

static void TestParsing() {
    const NameBook book = MakeBook();
    // Comments, trailing commas, markdown fences and chatter around it all work.
    Result r = Run("Sure! Here you go:\n```json\n{ // my build\n \"name\": \"x\",\n \"ops\": [\n"
                   "  {\"op\":\"set\",\"at\":[0,0,0],\"block\":\"Rock_Stone\",},\n ],\n}\n```\nEnjoy!", book);
    Check(r.Ok(), "tolerant parse of a fenced, commented reply");
    Eq(r.blocks.size(), (size_t)1, "one block");
    Eq(r.name, std::string("x"), "name read");

    r = Run("{ this is not json", book);
    Check(!r.errors.empty() && r.blocks.empty(), "garbage is an error, not a crash");
    r = Run("{\"name\":\"a\"}", book);
    Check(!r.errors.empty(), "no ops is an error");
    r = Run(Script("{\"op\":\"nope\"}"), book);
    Check(!r.errors.empty(), "unknown op is an error");

    // An example fragment first and the real script second: the one with ops wins.
    const std::string both = "Example: {\"x\": 1} and here it is:\n" + Script("{\"op\":\"set\",\"at\":[1,2,3],\"block\":\"Rock_Stone\"}");
    Eq(ExtractJson(both).find("\"ops\"") != std::string::npos, true, "ExtractJson prefers the object with ops");
}

static void TestBoxModes() {
    const NameBook book = MakeBook();
    auto count = [&](const std::string& mode) {
        return Run(Script("{\"op\":\"box\",\"from\":[0,0,0],\"to\":[2,2,2],\"block\":\"Rock_Stone\",\"mode\":\"" + mode + "\"}"), book).blocks.size();
    };
    Eq(count("fill"), (size_t)27, "fill 3x3x3");
    Eq(count("shell"), (size_t)26, "shell 3x3x3");
    Eq(count("walls"), (size_t)24, "walls 3x3x3");
    Eq(count("frame"), (size_t)20, "frame 3x3x3");
    Eq(count("floor"), (size_t)9, "floor");
    Eq(count("ceiling"), (size_t)9, "ceiling");

    // Reversed corners are the same box; air carves.
    Result r = Run(Script("{\"op\":\"box\",\"from\":[2,2,2],\"to\":[0,0,0],\"block\":\"Rock_Stone\"},"
                          "{\"op\":\"box\",\"from\":[1,0,1],\"to\":[1,2,1],\"block\":\"air\"}"), book);
    Eq(r.blocks.size(), (size_t)24, "a doorway carved out of a solid box");
    Eq(r.erased, (size_t)3, "erased count");

    // Palette keys, lists and rotation on palette entries.
    r = Run(Script("{\"op\":\"set\",\"at\":[0,0,0],\"block\":\"w\"},{\"op\":\"set\",\"at\":[1,0,0],\"block\":\"r\"}",
                   "{\"w\":\"Rock_Stone_Brick\",\"r\":{\"block\":\"Rock_Stone_Brick_Stairs\",\"facing\":\"e\"}}"), book);
    Eq(Find(r, 0, 0, 0)->name, std::string("Rock_Stone_Brick"), "palette key resolves");
    Eq(RotOf(Find(r, 1, 0, 0)), 3, "palette entry carries its facing (east = 3)");
}

static void TestNames() {
    const NameBook book = MakeBook();
    Result r = Run(Script("{\"op\":\"set\",\"at\":[0,0,0],\"block\":\"Rock_Stone_Bricks\"}"), book);
    Check(!r.Ok() && r.unknown.size() == 1, "an invented name is reported");
    Check(!r.unknown.empty() && !r.unknown[0].suggestions.empty() && r.unknown[0].suggestions[0] == "Rock_Stone_Brick",
          "and the closest real name is suggested");
    Check(FixRequest(r).find("Rock_Stone_Brick") != std::string::npos, "the fix message names it");

    r = Run(Script("{\"op\":\"set\",\"at\":[0,0,0],\"block\":\"rock_stone_brick\"}"), book);
    Check(r.Ok() && r.blocks[0].name == "Rock_Stone_Brick", "wrong capitalisation is corrected");

    r = Run(Script("{\"op\":\"set\",\"at\":[0,0,0],\"block\":\"*Wood_Village_Wall_White_Full_State_Definitions_Middle\"}"), book);
    Check(r.Ok() && r.blocks[0].name == "Wood_Village_Wall_White_Full", "a state name becomes the plain block");

    // Wood_Oak_Trunk is only ever seen as a state's base in the registry.
    r = Run(Script("{\"op\":\"set\",\"at\":[0,0,0],\"block\":\"Wood_Oak_Trunk\"}"), book);
    Check(r.Ok(), "a block known only as a state base is accepted");

    // No registry: nothing to check against, and it says so.
    r = Run(Script("{\"op\":\"set\",\"at\":[0,0,0],\"block\":\"Made_Up\"}"), NameBook());
    Check(r.Ok() && !r.warnings.empty() && !r.checkedNames, "without a block list nothing is rejected");

    // Doors are two tall: a block above one is called out.
    r = Run(Script("{\"op\":\"set\",\"at\":[0,0,0],\"block\":\"Furniture_Village_Door\"},"
                   "{\"op\":\"set\",\"at\":[0,1,0],\"block\":\"Rock_Stone\"}"), book);
    Check(std::any_of(r.warnings.begin(), r.warnings.end(), [](const std::string& w) { return w.find("door") != std::string::npos; }),
          "a block above a door is warned about");
}

static void TestStairsAndRoofs() {
    const NameBook book = MakeBook();
    // Stairs climbing east: each step one up, tall side east (rotation 3).
    Result r = Run(Script("{\"op\":\"stairs\",\"from\":[0,0,0],\"dir\":\"e\",\"steps\":3,\"block\":\"Rock_Stone_Brick_Stairs\"}"), book);
    Eq(r.blocks.size(), (size_t)3, "three steps");
    for (int i = 0; i < 3; ++i) {
        const Block* b = Find(r, i, i, 0);
        Check(b != nullptr, "step " + std::to_string(i) + " exists");
        Eq(RotOf(b), 3, "step faces east");
    }
    // Width is centred on the start; under fills down to the start height.
    r = Run(Script("{\"op\":\"stairs\",\"from\":[5,0,5],\"dir\":\"n\",\"steps\":2,\"width\":3,\"block\":\"Rock_Stone_Brick_Stairs\","
                   "\"under\":\"Rock_Stone_Brick\"}"), book);
    Eq(r.blocks.size(), (size_t)(6 + 3), "3-wide 2-step stairs plus support under the top step");
    Check(Find(r, 4, 1, 4) && Find(r, 6, 1, 4) && !Find(r, 3, 1, 4), "width is centred");
    Eq(RotOf(Find(r, 5, 0, 5)), 0, "climbing north is rotation 0");

    // A gable over a 5-wide footprint, ridge along X: the north slope climbs
    // south (2), the south slope climbs north (0), the ridge is the cap. This
    // is exactly what the real house does.
    r = Run(Script("{\"op\":\"gable\",\"from\":[0,10,0],\"to\":[9,10,4],\"ridge\":\"x\",\"block\":\"Wood_Darkwood_Roof\","
                   "\"cap\":\"Wood_Darkwood_Roof_Flat\"}"), book);
    Eq(r.blocks.size(), (size_t)(10 * 5), "one piece per column, per slope row");
    Eq(RotOf(Find(r, 3, 10, 0)), 2, "north eave faces south");
    Eq(RotOf(Find(r, 3, 11, 1)), 2, "north slope one up");
    Eq(Find(r, 3, 12, 2)->name, std::string("Wood_Darkwood_Roof_Flat"), "ridge cap at the middle column");
    Eq(RotOf(Find(r, 3, 11, 3)), 0, "south slope faces north");
    Eq(RotOf(Find(r, 3, 10, 4)), 0, "south eave");
    Check(Find(r, 3, 10, 2) == nullptr, "hollow underneath");

    // Ridge along Z: the west slope climbs east (3), the east slope climbs west (1).
    r = Run(Script("{\"op\":\"gable\",\"from\":[0,0,0],\"to\":[4,0,9],\"block\":\"Wood_Darkwood_Roof\"}"), book);
    Eq(RotOf(Find(r, 0, 0, 3)), 3, "west eave faces east (ridge picked along the long side)");
    Eq(RotOf(Find(r, 4, 0, 3)), 1, "east eave faces west");

    // Overhang extends the slope down and out; gable_fill closes the triangle.
    r = Run(Script("{\"op\":\"gable\",\"from\":[0,10,0],\"to\":[4,10,4],\"ridge\":\"x\",\"block\":\"Wood_Darkwood_Roof\","
                   "\"overhang\":1,\"end_overhang\":1,\"gable_fill\":\"Rock_Stone_Brick\"}"), book);
    Check(Find(r, 0, 9, -1) != nullptr, "overhang steps one lower and one further out");
    Check(Find(r, -1, 10, 0) != nullptr && Find(r, 5, 10, 0) != nullptr, "end overhang runs past both ends of the footprint");
    Check(Find(r, 0, 10, 2) != nullptr && Find(r, 0, 11, 2) != nullptr, "gable end filled under the ridge");
    Check(Find(r, 2, 10, 2) == nullptr, "but not the inside of the roof");

    // A hip roof on a 5x5 footprint is 5x5 ring, 3x3 ring, one cap block.
    r = Run(Script("{\"op\":\"hip\",\"from\":[0,0,0],\"to\":[4,0,4],\"block\":\"Wood_Darkwood_Roof\",\"cap\":\"Wood_Darkwood_Roof_Flat\"}"), book);
    Eq(r.blocks.size(), (size_t)(16 + 8 + 1), "hip roof rings");
    Eq(Find(r, 2, 2, 2)->name, std::string("Wood_Darkwood_Roof_Flat"), "hip cap");
    Eq(RotOf(Find(r, 2, 0, 0)), 2, "north edge climbs south");
    Eq(RotOf(Find(r, 0, 0, 2)), 3, "west edge climbs east");
    Eq(RotOf(Find(r, 4, 0, 2)), 1, "east edge climbs west");
    Eq(RotOf(Find(r, 2, 0, 4)), 0, "south edge climbs north");
}

static void TestShapesAndMirror() {
    const NameBook book = MakeBook();
    Result r = Run(Script("{\"op\":\"cylinder\",\"center\":[0,0,0],\"radius\":3,\"height\":2,\"block\":\"Rock_Stone\",\"mode\":\"wall\"}"), book);
    Check(!r.blocks.empty() && Find(r, 3, 0, 0) && Find(r, -3, 1, 0) && Find(r, 0, 0, 3) && !Find(r, 0, 0, 0),
          "a hollow tower wall");
    Eq(r.SizeX(), 7, "radius 3 is 7 wide");
    r = Run(Script("{\"op\":\"cylinder\",\"center\":[0,0,0],\"radius\":3,\"height\":2,\"block\":\"Rock_Stone\",\"mode\":\"shell\"}"), book);
    Check(Find(r, 0, 0, 0) && Find(r, 0, 1, 0), "shell has a floor and a roof");

    r = Run(Script("{\"op\":\"sphere\",\"center\":[0,0,0],\"radius\":4,\"block\":\"Rock_Stone\",\"mode\":\"shell\",\"part\":\"upper\"}"), book);
    Check(Find(r, 0, 4, 0) && !Find(r, 0, 0, 0) && !Find(r, 0, -1, 0) && Find(r, 4, 0, 0), "a dome: shell, upper half only");

    // Mirror: a stair facing east at x=1 becomes one facing west on the far side.
    r = Run(Script("{\"op\":\"set\",\"at\":[1,0,0],\"block\":\"Rock_Stone_Brick_Stairs\",\"facing\":\"e\"},"
                   "{\"op\":\"set\",\"at\":[2,0,3],\"block\":\"Rock_Stone_Brick_Stairs\",\"facing\":\"n\"},"
                   "{\"op\":\"mirror\",\"axis\":\"x\",\"at\":5}"), book);
    Eq(r.blocks.size(), (size_t)4, "mirror doubles it");
    Eq(RotOf(Find(r, 9, 0, 0)), 1, "east-facing mirrors to west-facing");
    Eq(RotOf(Find(r, 8, 0, 3)), 0, "north-facing stays north across an x mirror");
    // Across z, north and south swap.
    r = Run(Script("{\"op\":\"set\",\"at\":[0,0,0],\"block\":\"Rock_Stone_Brick_Stairs\",\"facing\":\"n\"},"
                   "{\"op\":\"mirror\",\"axis\":\"z\",\"at\":2.5}"), book);
    Eq(RotOf(Find(r, 0, 0, 5)), 2, "north mirrors to south across z (half-block plane)");
    // A cube with no facing gains none.
    r = Run(Script("{\"op\":\"set\",\"at\":[0,0,0],\"block\":\"Rock_Stone\"},{\"op\":\"mirror\",\"axis\":\"z\",\"at\":2.5}"), book);
    Eq(Find(r, 0, 0, 5)->rotation, -1, "no rotation invented for a cube");

    // Repeat with a step, nested offsets.
    r = Run(Script("{\"op\":\"repeat\",\"count\":4,\"step\":[3,0,0],\"ops\":[{\"op\":\"box\",\"from\":[0,0,0],\"to\":[0,2,0],\"block\":\"Wood_Darkwood_Beam\",\"axis\":\"y\"}]}"), book);
    Eq(r.blocks.size(), (size_t)12, "four pillars");
    Check(Find(r, 9, 2, 0) != nullptr && Find(r, 4, 0, 0) == nullptr, "pillars are three apart");

    // Scatter: chance 0 and 1 are exact, the same seed is the same result.
    const std::string base = "{\"op\":\"box\",\"from\":[0,0,0],\"to\":[9,0,9],\"block\":\"Rock_Stone_Brick\"},";
    r = Run(Script(base + "{\"op\":\"scatter\",\"from\":[0,0,0],\"to\":[9,0,9],\"block\":\"Rock_Stone_Mossy\",\"chance\":0}"), book);
    Eq(r.materials.size(), (size_t)1, "chance 0 changes nothing");
    r = Run(Script(base + "{\"op\":\"scatter\",\"from\":[0,0,0],\"to\":[9,0,9],\"block\":\"Rock_Stone_Mossy\",\"chance\":1}"), book);
    Eq(r.materials.size(), (size_t)1, "chance 1 replaces everything");
    const std::string sc = Script(base + "{\"op\":\"scatter\",\"from\":[0,0,0],\"to\":[9,0,9],\"block\":\"Rock_Stone_Mossy\",\"chance\":0.3,\"seed\":5}");
    const Result a = Run(sc, book), b = Run(sc, book);
    Check(a.blocks.size() == b.blocks.size() && ToPrefabJson(a) == ToPrefabJson(b), "scatter is deterministic");
    Check(a.materials.size() == 2, "scatter at 0.3 mixes the two");
    // 'top' puts things only on top of something.
    r = Run(Script("{\"op\":\"box\",\"from\":[0,0,0],\"to\":[4,0,4],\"block\":\"Rock_Stone\"},"
                   "{\"op\":\"scatter\",\"from\":[0,-1,0],\"to\":[4,3,4],\"block\":\"Plant_Flower_Common_Pink\",\"where\":\"top\",\"chance\":1}"), book);
    Eq(r.blocks.size(), (size_t)50, "flowers on every top of a 5x5 floor");
    Check(Find(r, 2, 1, 2) && !Find(r, 2, 2, 2), "and only on the floor");
}

static void TestGuards() {
    const NameBook book = MakeBook();
    Result r = Run(Script("{\"op\":\"box\",\"from\":[0,0,0],\"to\":[99999,1,1],\"block\":\"Rock_Stone\"}"), book);
    Check(!r.errors.empty(), "an absurd coordinate is refused");
    Options tiny; tiny.maxBlocks = 100;
    Result big;
    Expand(Script("{\"op\":\"box\",\"from\":[0,0,0],\"to\":[20,20,20],\"block\":\"Rock_Stone\"}"), book, tiny, big);
    Check(!big.errors.empty(), "the block limit is enforced");
    r = Run(Script("{\"op\":\"set\",\"at\":[0,0],\"block\":\"Rock_Stone\"},{\"op\":\"set\",\"at\":[0,0,0],\"block\":\"Rock_Stone\"}"), book);
    Check(r.errors.size() == 1 && r.blocks.size() == 1, "one bad op does not stop the rest, and is reported once");
}

static NameBook RaceBook() {
    NameBook b;
    for (const char* n : { "Kart_Asphalt", "Kart_Curb_Red", "Kart_Curb_Yellow", "Kart_Painted_Line", "Kart_Armco",
                           "Kart_Tire_Wall", "Kart_Grass", "Rock_Stone", "Kart_Concrete" })
        b.Add(n);
    return b;
}

static std::set<std::tuple<int, int, int>> CellsOf(const Result& r, const std::string& name) {
    std::set<std::tuple<int, int, int>> out;
    for (const Block& b : r.blocks) if (b.name == name) out.insert({ b.x, b.y, b.z });
    return out;
}

static void TestRoad() {
    const NameBook book = RaceBook();
    // A straight road 5 wide along x.
    Result r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[30,0,0]],\"width\":5,\"block\":\"Kart_Asphalt\"}"), book);
    Check(r.Ok(), "a straight road expands");
    const auto road = CellsOf(r, "Kart_Asphalt");
    bool full = true, tight = true;
    for (int x = 4; x <= 26; ++x) {
        for (int z = -2; z <= 2; ++z) if (!road.count({ x, 0, z })) full = false;
        if (road.count({ x, 0, 3 }) || road.count({ x, 0, -3 })) tight = false;
    }
    Check(full, "the road is 5 wide: z -2..2 at every x");
    Check(tight, "and no wider");

    // An even width needs the centre line on a half block.
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0.5],[30,0,0.5]],\"width\":4,\"block\":\"Kart_Asphalt\"}"), book);
    const auto r4 = CellsOf(r, "Kart_Asphalt");
    Check(r4.count({ 15, 0, 0 }) && r4.count({ 15, 0, 1 }) && r4.count({ 15, 0, -1 }) && r4.count({ 15, 0, 2 }) &&
          !r4.count({ 15, 0, -2 }) && !r4.count({ 15, 0, 3 }),
          "width 4 on a .5 centre line covers z -1..2");

    // Edge, centre line, barrier.
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[30,0,0]],\"width\":7,\"block\":\"Kart_Asphalt\","
                   "\"edge\":[\"Kart_Curb_Red\",\"Kart_Curb_Yellow\"],\"stripe\":2,\"line\":\"Kart_Painted_Line\","
                   "\"barrier\":\"Kart_Armco\",\"barrier_gap\":1}"), book);
    Check(r.Ok(), "road with edge, line and barrier expands");
    const auto red = CellsOf(r, "Kart_Curb_Red"), yel = CellsOf(r, "Kart_Curb_Yellow");
    Check(red.count({ 15, 1, 3 }) || yel.count({ 15, 1, 3 }), "curbs sit on the outer road cell, one layer up");
    Check(!red.count({ 15, 1, 2 }) && !yel.count({ 15, 1, 2 }), "and only on the outer cell");
    bool alternates = false;
    for (int x = 4; x < 25; ++x) if (red.count({ x, 1, 3 }) && yel.count({ x + 2, 1, 3 })) alternates = true;
    Check(alternates, "curb colours alternate along the road every stripe cells");
    const auto ln = CellsOf(r, "Kart_Painted_Line");
    Check(ln.count({ 15, 1, 0 }) && !ln.count({ 15, 1, 1 }), "centre line one cell wide, one layer up");
    Eq(RotOf(Find(r, 15, 1, 0)), 1, "a north-south picture turned for a road that runs east-west");
    const auto rail = CellsOf(r, "Kart_Armco");
    Check(rail.count({ 15, 1, 5 }) && rail.count({ 15, 1, -5 }) && !rail.count({ 15, 1, 4 }) && !rail.count({ 15, 1, 6 }),
          "barrier ring one cell wide, after a gap of one");
    Eq(RotOf(Find(r, 15, 1, 5)), 0, "rails along an east-west road keep rotation 0");

    // Only the left side (heading east, left is north).
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[30,0,0]],\"width\":5,\"block\":\"Kart_Asphalt\","
                   "\"barrier\":\"Kart_Armco\",\"barrier_sides\":\"left\"}"), book);
    const auto left = CellsOf(r, "Kart_Armco");
    Check(left.count({ 15, 1, -3 }) && !left.count({ 15, 1, 3 }), "barrier_sides left: north side only when heading east");
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[0,0,30]],\"width\":5,\"block\":\"Kart_Asphalt\","
                   "\"barrier\":\"Kart_Armco\"}"), book);
    Eq(RotOf(Find(r, 3, 1, 15)), 1, "rails along a north-south road turn to rotation 1");
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[30,0,0]],\"width\":5,\"block\":\"Kart_Asphalt\","
                   "\"barrier\":\"Kart_Tire_Wall\",\"auto_rot\":false}"), book);
    Check(r.Ok() && Find(r, 15, 1, 3) && Find(r, 15, 1, 3)->rotation < 0, "auto_rot false leaves the blocks unturned");

    // Dashes.
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[40,0,0]],\"width\":5,\"block\":\"Kart_Asphalt\","
                   "\"line\":\"Kart_Painted_Line\",\"dash\":[3,2]}"), book);
    const auto dash = CellsOf(r, "Kart_Painted_Line");
    Check(dash.size() > 15 && dash.size() < 30, "a dashed line is about 3/5 filled");

    // Supports under the road.
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,5,0],[10,5,0]],\"width\":3,\"block\":\"Kart_Asphalt\","
                   "\"under\":\"Kart_Concrete\",\"depth\":3}"), book);
    Check(Find(r, 5, 5, 0) && Find(r, 5, 4, 0) && Find(r, 5, 2, 0) && !Find(r, 5, 1, 0), "under fills 3 layers below the surface");

    // A closed loop has no gaps: its road cells form one connected ring.
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[40,0,0],[40,0,30],[0,0,30]],\"closed\":true,\"width\":7,"
                   "\"block\":\"Kart_Asphalt\"}"), book);
    Check(r.Ok(), "a closed loop expands");
    const auto ring = CellsOf(r, "Kart_Asphalt");
    std::set<std::tuple<int, int, int>> seen;
    std::vector<std::tuple<int, int, int>> todo{ *ring.begin() };
    seen.insert(*ring.begin());
    while (!todo.empty()) {
        const auto c = todo.back();
        todo.pop_back();
        for (int dx = -1; dx <= 1; ++dx) for (int dz = -1; dz <= 1; ++dz) {
            const std::tuple<int, int, int> n{ std::get<0>(c) + dx, 0, std::get<2>(c) + dz };
            if (ring.count(n) && !seen.count(n)) { seen.insert(n); todo.push_back(n); }
        }
    }
    Eq(seen.size(), ring.size(), "the loop is one connected piece");
    Check(!ring.count({ 20, 0, 15 }), "the middle of the loop is left alone");
    Check(ring.count({ 20, 0, 0 }) && ring.count({ 20, 0, 30 }) && ring.count({ 0, 0, 15 }) && ring.count({ 40, 0, 15 }),
          "the four sides are there");

    // Corners: straight between the waypoints, rounded at the corner, sharp when asked.
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[30,0,0],[30,0,30]],\"width\":5,\"radius\":8,\"block\":\"Kart_Asphalt\"}"), book);
    const auto ell = CellsOf(r, "Kart_Asphalt");
    Check(ell.count({ 10, 0, 2 }) && ell.count({ 10, 0, -2 }) && !ell.count({ 10, 0, 3 }) && !ell.count({ 10, 0, -3 }),
          "the straight before a corner stays exactly 5 wide");
    Check(!ell.count({ 30, 0, 0 }) && ell.count({ 28, 0, 0 }) && ell.count({ 30, 0, 2 }),
          "the corner point itself is cut off by the arc, the road goes round inside it");
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[30,0,0],[30,0,30]],\"width\":5,\"curve\":\"sharp\",\"block\":\"Kart_Asphalt\"}"), book);
    Check(CellsOf(r, "Kart_Asphalt").count({ 30, 0, 0 }), "curve sharp keeps the corner point");
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[30,0,0],[30,0,30]],\"width\":5,\"curve\":\"spline\",\"block\":\"Kart_Asphalt\"}"), book);
    Check(r.Ok() && r.blocks.size() > 200, "curve spline works");
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[30,0,0],[30,0,30]],\"width\":5,\"curve\":\"wavy\",\"block\":\"Kart_Asphalt\"}"), book);
    Check(!r.errors.empty(), "an unknown curve is an error");

    // Slopes follow the waypoints' heights.
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[40,8,0]],\"width\":3,\"block\":\"Kart_Asphalt\"}"), book);
    int lo = 99, hi = -99;
    for (const Block& b : r.blocks) { lo = (std::min)(lo, b.y); hi = (std::max)(hi, b.y); }
    Check(lo == 0 && hi == 8, "a road rises from y 0 to y 8 between its endpoints");
    Check(Find(r, 20, 4, 0) != nullptr, "and is at half height halfway");

    // Mistakes are explained, not crashed on.
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0]],\"block\":\"Kart_Asphalt\"}"), book);
    Check(!r.errors.empty(), "one point is an error");
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[5,0,0]],\"block\":\"Kart_Nope\"}"), book);
    Check(!r.Ok(), "an unknown block in a road is reported");
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[5,0,0]],\"width\":0,\"block\":\"Kart_Asphalt\"}"), book);
    Check(!r.errors.empty(), "width 0 is an error");
    r = Run(Script("{\"op\":\"road\",\"points\":[[0,0,0],[5,0,0]],\"block\":\"Kart_Asphalt\",\"edge_mode\":\"sideways\"}"), book);
    Check(!r.errors.empty(), "a bad edge_mode is an error");
    // A huge road stops at the block limit instead of eating memory.
    Result big;
    Options o;
    o.maxBlocks = 5000;
    Expand(Script("{\"op\":\"road\",\"points\":[[0,0,0],[1000,0,0]],\"width\":20,\"block\":\"Kart_Asphalt\"}"), book, o, big);
    Check(!big.errors.empty(), "a road over the block limit is refused");
}

static void TestHints() {
    Eq(RotationHint("Rock_Stone_Brick_Stairs", 3, 0), std::string("tall side toward the east"), "stairs 3 = east");
    Eq(RotationHint("Rock_Stone_Brick_Stairs", 3, 1), std::string("tall side toward the south"), "turning the build 90 clockwise moves east to south");
    Eq(RotationHint("Rock_Stone_Brick_Stairs", -1, 0), std::string("tall side toward the north"), "no rotation is north");
    Eq(RotationHint("Rock_Stone_Brick_Stairs", 10, 0), std::string("upside down, tall side toward the north"), "10 is upside down north");
    Eq(RotationHint("Wood_Softwood_Roof_Hollow", 2, 2), std::string("tall side toward the north"), "roofs too, and two turns flips it");
    Eq(RotationHint("Wood_Softwood_Roof_Flat", 3, 0), std::string(""), "flat roofs have no tall side");
    Eq(RotationHint("Wood_Oak_Trunk", 4, 0), std::string("lying north-south"), "trunk 4 runs along Z");
    Eq(RotationHint("Wood_Oak_Trunk", 5, 0), std::string("lying east-west"), "trunk 5 runs along X");
    Eq(RotationHint("Wood_Oak_Trunk", 5, 1), std::string("lying north-south"), "and turns with the build");
    Eq(RotationHint("Wood_Hardwood_Beam", 0, 0), std::string("standing upright"), "beam 0 is upright");
    Eq(RotationHint("Furniture_Village_Window", 0, 0), std::string("in an east-west wall"), "window 0 is in a wall running along X");
    Eq(RotationHint("Furniture_Village_Window", 1, 0), std::string("in a north-south wall"), "window 1 along Z");
    Eq(RotationHint("Rock_Stone", 3, 0), std::string(""), "plain blocks say nothing");
    Eq(RotationHint("*Wood_Softwood_Roof_State_Definitions_Inverted_Corner_Left", 3, 0), std::string("tall side toward the east"), "state names are read as their base");
    Eq(StairRotation('n', false), 0, "n");  Eq(StairRotation('w', false), 1, "w");
    Eq(StairRotation('s', false), 2, "s");  Eq(StairRotation('e', false), 3, "e");
    Eq(StairRotation('s', true), 8, "upside down s");  Eq(StairRotation('e', true), 9, "upside down e");
    Eq(StairRotation('n', true), 10, "upside down n"); Eq(StairRotation('w', true), 11, "upside down w");
}

// The files written must load in the Build tab's own parser, block for block.
static void TestRoundTrip() {
    const NameBook book = MakeBook();
    Result r = Run(Script("{\"op\":\"box\",\"from\":[-3,0,-2],\"to\":[3,3,2],\"block\":\"Rock_Stone_Brick\",\"mode\":\"walls\"},"
                          "{\"op\":\"gable\",\"from\":[-3,4,-2],\"to\":[3,4,2],\"block\":\"Wood_Darkwood_Roof\",\"cap\":\"Wood_Darkwood_Roof_Flat\"},"
                          "{\"op\":\"set\",\"at\":[0,0,-2],\"block\":\"Furniture_Village_Door\",\"rot\":0}"), book);
    Check(r.Ok(), "the round-trip build is valid");
    const std::wstring dir = g_testDir + L"scripttest\\";
    CreateDirectoryW(g_testDir.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring file = dir + L"roundtrip.json";
    {
        std::ofstream f(file, std::ios::binary);
        f << ToPrefabJson(r);
    }
    Prefab p;
    std::string err;
    const bool ok = PrefabParseFile(WideToUtf8Path(file), p, err);
    Check(ok, "the Build tab's parser accepts the file: " + err);
    if (ok) {
        Eq(p.blocks.size(), r.blocks.size(), "same number of blocks");
        Eq(p.SizeX(), r.SizeX(), "same width");
        Eq(p.SizeY(), r.SizeY(), "same height");
        // Same block in the same cell, with the same rotation.
        size_t same = 0;
        for (const Block& b : r.blocks) {
            auto it = p.cells.find(PrefabCellKey(b.x, b.y, b.z));
            if (it == p.cells.end()) continue;
            const PrefabBlock& pb = p.blocks[it->second];
            if (p.names[(size_t)pb.nameId] == b.name && pb.rotation == b.rotation) ++same;
        }
        Eq(same, r.blocks.size(), "every block round-trips exactly");
    }
}


// A palette entry may be a list (a random mix), a string, or an object.
static void TestPalette() {
    const NameBook book = MakeBook();
    Result r = Run(Script("{\"op\":\"box\",\"from\":[0,0,0],\"to\":[9,0,9],\"block\":\"mix\"}",
                          "{\"mix\":[\"Rock_Stone_Brick\",\"Rock_Stone_Cobble\"]}"), book);
    Check(r.Ok() && r.materials.size() == 2, "a palette list is a random mix of both");
    const size_t a = r.materials[0].second, b = r.materials[1].second;
    Check(a > 20 && b > 20, "and both really are used (" + std::to_string(a) + " / " + std::to_string(b) + ")");
    r = Run(Script("{\"op\":\"set\",\"at\":[0,0,0],\"block\":\"s\"}", "{\"s\":{\"block\":[\"Rock_Stone_Brick_Stairs\"],\"facing\":\"s\"}}"), book);
    Eq(RotOf(Find(r, 0, 0, 0)), 2, "an object entry with a list still carries its facing");
    r = Run(Script("{\"op\":\"set\",\"at\":[0,0,0],\"block\":\"nope\"}", "{\"nope\":[]}"), book);
    Check(!r.errors.empty(), "an empty palette list is an error");
}

// The two scripts shipped as examples must stay valid: one is embedded in the prompt.
static void TestShippedExamples() {
    NameBook book;
    std::ifstream names("tools\\ai_prefab_blocks.txt");
    std::string line;
    while (std::getline(names, line)) if (!line.empty() && line[0] != '#') book.Add(line);
    if (book.Empty()) { std::printf("(tools\\ai_prefab_blocks.txt not found - skipping the shipped examples)\n"); return; }
    struct Ex { const char* path; size_t minBlocks, maxBlocks; };
    for (const Ex& ex : { Ex{ "tools\\example_cottage.json", 300, 900 }, Ex{ "tools\\example_tavern.json", 1200, 4000 },
                        Ex{ "tools\\example_race.json", 3000, 5000 } }) {
        std::ifstream f(ex.path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        Result r = Run(ss.str(), book);
        Check(r.Ok(), std::string(ex.path) + " expands with no errors and only real block names" +
                      (r.errors.empty() ? "" : ": " + r.errors[0]) + (r.unknown.empty() ? "" : ": " + r.unknown[0].name));
        Check(r.blocks.size() >= ex.minBlocks && r.blocks.size() <= ex.maxBlocks,
              std::string(ex.path) + " has a sensible size (" + std::to_string(r.blocks.size()) + ")");
        Check(std::none_of(r.warnings.begin(), r.warnings.end(), [](const std::string& w) { return w.find("door") != std::string::npos; }),
              std::string(ex.path) + " leaves the cell above every door empty");
    }
}
// The direction tables come from real data. If the real prefab is here, test them
// against it: for a stair or roof piece the block one step up must be on its tall
// side. Ridge pieces legitimately fail (their tall-side neighbour is level).
static void TestAgainstRealHouse() {
    const char* candidates[] = { "x64\\Release\\prefabs\\test_house.json", "..\\..\\x64\\Release\\prefabs\\test_house.json",
                                 "..\\..\\..\\x64\\Release\\prefabs\\test_house.json" };
    Prefab p;
    std::string err;
    bool have = false;
    for (const char* c : candidates) if (PrefabParseFile(c, p, err)) { have = true; break; }
    if (!have) { std::printf("(real house prefab not found - skipping the data check)\n"); return; }

    size_t stairsTotal = 0, stairsHit = 0, roofTotal = 0, roofHit = 0;
    const int dx[4] = { 0, -1, 0, 1 }, dz[4] = { -1, 0, 1, 0 };    // tall side for rotation 0..3
    for (const PrefabBlock& b : p.blocks) {
        const std::string& n = p.names[(size_t)b.nameId];
        if (n.empty() || n[0] == '*') continue;
        const int rot = b.rotation < 0 ? 0 : b.rotation;
        if (rot > 3) continue;
        const bool stairs = n.find("Stairs") != std::string::npos;
        const bool roof = n.find("Roof") != std::string::npos && n.find("Flat") == std::string::npos;
        if (!stairs && !roof) continue;
        const bool hit = p.cells.count(PrefabCellKey(b.x + dx[rot], b.y + 1, b.z + dz[rot])) != 0;
        (stairs ? stairsTotal : roofTotal)++;
        if (hit) (stairs ? stairsHit : roofHit)++;
        // The hint must agree with the table used above.
        const std::string hint = RotationHint(n, b.rotation, 0);
        const char* words[4] = { "north", "west", "south", "east" };
        if (hint != std::string("tall side toward the ") + words[rot]) { Check(false, "hint disagrees for " + n); break; }
    }
    std::printf("real house: stairs %zu/%zu, roofs %zu/%zu have their next step up on the tall side\n",
        stairsHit, stairsTotal, roofHit, roofTotal);
    Check(stairsTotal > 10 && stairsHit == stairsTotal, "every real stair has its next step on its tall side");
    Check(roofTotal > 100 && roofHit * 100 >= roofTotal * 90, "at least 90% of real roof pieces do (the rest are ridge pieces)");
}

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_testDir = std::wstring(tmp) + L"hyai_scripttest\\";
    CreateDirectoryW(g_testDir.c_str(), nullptr);

    TestParsing();
    TestBoxModes();
    TestNames();
    TestStairsAndRoofs();
    TestShapesAndMirror();
    TestRoad();
    TestGuards();
    TestPalette();
    TestShippedExamples();
    TestHints();
    TestRoundTrip();
    TestAgainstRealHouse();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
