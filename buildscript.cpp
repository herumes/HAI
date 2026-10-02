#include "pch.h"
#include "buildscript.hpp"

#include <cmath>
#include <cstdio>

namespace BuildScript {
namespace {

// =============================================================================
// A small tolerant JSON reader
// =============================================================================
// AI replies are *almost* JSON: a trailing comma, a // note to itself. Refusing
// the whole build over one of those is a bad trade, so both are accepted.
struct JVal {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;

    const JVal* Get(const char* key) const {
        if (t != T::Obj) return nullptr;
        for (const auto& kv : o) if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

class JParser {
public:
    explicit JParser(const std::string& text) : s_(text) {}

    bool Parse(JVal& out, std::string& err) {
        Ws();
        if (!Value(out, 0)) { err = err_; return false; }
        Ws();
        if (i_ < s_.size()) { Fail("unexpected text after the closing brace"); err = err_; return false; }
        return true;
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    std::string err_;

    bool Fail(const std::string& what) {
        if (!err_.empty()) return false;
        int line = 1, col = 1;
        for (size_t k = 0; k < i_ && k < s_.size(); ++k) {
            if (s_[k] == '\n') { ++line; col = 1; } else ++col;
        }
        err_ = "line " + std::to_string(line) + ", column " + std::to_string(col) + ": " + what;
        return false;
    }

    void Ws() {
        for (;;) {
            while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n' ||
                                      (unsigned char)s_[i_] == 0xEF || (unsigned char)s_[i_] == 0xBB ||
                                      (unsigned char)s_[i_] == 0xBF)) ++i_;
            if (i_ + 1 < s_.size() && s_[i_] == '/' && s_[i_ + 1] == '/') {
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
                continue;
            }
            if (i_ + 1 < s_.size() && s_[i_] == '/' && s_[i_ + 1] == '*') {
                i_ += 2;
                while (i_ + 1 < s_.size() && !(s_[i_] == '*' && s_[i_ + 1] == '/')) ++i_;
                i_ = (std::min)(s_.size(), i_ + 2);
                continue;
            }
            break;
        }
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else {
            out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool String(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return Fail("expected a string");
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) break;
            const char e = s_[i_++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                uint32_t cp = 0;
                for (int k = 0; k < 4 && i_ < s_.size(); ++k) {
                    const char h = s_[i_++];
                    cp = cp * 16 + (uint32_t)(isdigit((unsigned char)h) ? h - '0' : (tolower(h) - 'a' + 10));
                }
                AppendUtf8(out, cp);
                break;
            }
            default: out += e; break;
            }
        }
        return Fail("a string is never closed");
    }

    bool Value(JVal& v, int depth) {
        if (depth > 64) return Fail("nested too deeply");
        Ws();
        if (i_ >= s_.size()) return Fail("the text ends too early");
        const char c = s_[i_];
        if (c == '{') {
            ++i_;
            v.t = JVal::T::Obj;
            for (;;) {
                Ws();
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                std::string key;
                if (!String(key)) return false;
                Ws();
                if (i_ >= s_.size() || s_[i_] != ':') return Fail("expected ':' after \"" + key + "\"");
                ++i_;
                JVal child;
                if (!Value(child, depth + 1)) return false;
                v.o.emplace_back(std::move(key), std::move(child));
                Ws();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++i_;
            v.t = JVal::T::Arr;
            for (;;) {
                Ws();
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                JVal child;
                if (!Value(child, depth + 1)) return false;
                v.a.push_back(std::move(child));
                Ws();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.t = JVal::T::Str; return String(v.s); }
        if (s_.compare(i_, 4, "true") == 0) { i_ += 4; v.t = JVal::T::Bool; v.b = true; return true; }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; v.t = JVal::T::Bool; v.b = false; return true; }
        if (s_.compare(i_, 4, "null") == 0) { i_ += 4; v.t = JVal::T::Null; return true; }
        if (c == '-' || c == '+' || c == '.' || isdigit((unsigned char)c)) {
            const char* begin = s_.c_str() + i_;
            char* end = nullptr;
            const double d = strtod(begin, &end);
            if (end == begin) return Fail("expected a number");
            i_ += (size_t)(end - begin);
            v.t = JVal::T::Num;
            v.n = d;
            return true;
        }
        return Fail(std::string("unexpected character '") + c + "'");
    }
};

// =============================================================================
// Names
// =============================================================================
std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool IsAirWord(const std::string& n) {
    const std::string l = Lower(n);
    return l.empty() || l == "air" || l == "empty" || l == "none" || l == "void" || l == "minecraft:air";
}

// "*Wood_Village_Wall_White_Full_State_Definitions_Middle" -> the plain block.
// The game picks the state itself when the block is placed.
std::string PlainName(const std::string& in, bool& wasState) {
    std::string n = in;
    while (!n.empty() && isspace((unsigned char)n.front())) n.erase(n.begin());
    while (!n.empty() && isspace((unsigned char)n.back())) n.pop_back();
    wasState = false;
    if (!n.empty() && n[0] == '*') { n.erase(n.begin()); wasState = true; }
    const size_t at = n.find("_State_Definitions_");
    if (at != std::string::npos) { n.resize(at); wasState = true; }
    return n;
}

bool Contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
bool EndsWith(const std::string& s, const char* tail) {
    const size_t n = strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// Pieces whose rotation 0..3 / 8..11 says which side is tall. Read from real
// exports for stairs and the plain, hollow roofs; the steep and shallow roofs
// are assumed to turn the same way. Flat roofs have no tall side.
bool IsStairLike(const std::string& n) {
    if (Contains(n, "Stairs")) return true;
    if (!Contains(n, "Roof")) return false;
    return !(Contains(n, "Roof_Flat") || Contains(n, "Vertical") || Contains(n, "Flap") ||
             Contains(n, "Topper"));
}
bool IsBeamLike(const std::string& n) {
    if (Contains(n, "Stairs") || Contains(n, "_Half")) return false;
    return Contains(n, "Beam") || EndsWith(n, "Trunk") || EndsWith(n, "Trunk_Full");
}
bool IsOpeningLike(const std::string& n) {
    return Contains(n, "Window") || Contains(n, "Trapdoor") ||
           (Contains(n, "_Door") && !Contains(n, "Trap"));
}

// Levenshtein, bounded: names are short and this runs a few thousand times.
int EditDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const int sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = (std::min)({ prev[j] + 1, cur[j - 1] + 1, sub });
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::vector<std::string> Tokens(const std::string& n) {
    std::vector<std::string> t;
    std::string cur;
    for (char c : n) {
        if (c == '_') { if (!cur.empty()) t.push_back(Lower(cur)); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) t.push_back(Lower(cur));
    return t;
}

// =============================================================================
// Rotation tables (all read from real prefab exports - see buildscript.hpp)
// =============================================================================
//   ring order n e s w; the tall side of an upright stair for rotation 0..3 is
//   n w s e, and for the upside-down 8..11 it is s e n w.
const char kUpright[4] = { 'n', 'w', 's', 'e' };
const char kInverted[4] = { 's', 'e', 'n', 'w' };

int RingIndex(char f) {
    switch (f) { case 'n': return 0; case 'e': return 1; case 's': return 2; case 'w': return 3; }
    return -1;
}
const char kRing[4] = { 'n', 'e', 's', 'w' };
const char* kRingName[4] = { "north", "east", "south", "west" };

char FacingOfRotation(int rot, bool& inverted, bool& ok) {
    ok = true;
    inverted = false;
    if (rot < 0) rot = 0;
    if (rot >= 0 && rot <= 3) return kUpright[rot];
    if (rot >= 8 && rot <= 11) { inverted = true; return kInverted[rot - 8]; }
    ok = false;
    return 'n';
}

// Mirroring a build flips which way pieces face. `axis` is the axis whose
// coordinate is reflected.
int MirrorRotation(int rot, char axis) {
    if (rot < 0) return rot;
    bool inv = false, ok = false;
    if ((rot >= 0 && rot <= 3) || (rot >= 8 && rot <= 11)) {
        const char f = FacingOfRotation(rot, inv, ok);
        char nf = f;
        if (axis == 'x') { if (f == 'e') nf = 'w'; else if (f == 'w') nf = 'e'; }
        else             { if (f == 'n') nf = 's'; else if (f == 's') nf = 'n'; }
        return StairRotation(nf, inv);
    }
    // Lying pieces: 4 and 6 run along Z, 5 and 7 along X; a reflection keeps the
    // axis, and swaps the two members of the pair on the reflected axis.
    if (rot >= 4 && rot <= 7) {
        if (axis == 'x' && (rot == 5 || rot == 7)) return rot == 5 ? 7 : 5;
        if (axis == 'z' && (rot == 4 || rot == 6)) return rot == 4 ? 6 : 4;
    }
    return rot;
}

// =============================================================================
// The model and the ops
// =============================================================================
struct Cell { int32_t name = -1; int32_t rot = -1; };

constexpr int kCoordLimit = 4000;

uint64_t Key(int32_t x, int32_t y, int32_t z) {
    const uint64_t off = 1u << 20;
    return (uint64_t)(x + off) | ((uint64_t)(y + off) << 21) | ((uint64_t)(z + off) << 42);
}

uint32_t Hash(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = 2166136261u ^ seed * 2654435761u;
    for (int32_t v : { x, y, z }) { h ^= (uint32_t)v; h *= 16777619u; h ^= h >> 15; }
    h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return h;
}
double Hash01(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    return (double)(Hash(x, y, z, seed) >> 8) / 16777216.0;
}

struct Choice { int id = -1; int rot = -1; };     // id -1: air (clears the cell)
struct Spec {
    std::vector<Choice> choices;
    std::vector<double> weights;    // empty: all equal
    bool valid = false;
};

struct PaletteEntry { std::vector<std::string> blocks; int rot = -1; bool hasRot = false; };

struct Offset { int x = 0, y = 0, z = 0; };

struct V3 { double x = 0, y = 0, z = 0; };

struct Engine {
    const NameBook& book;
    const Options& opt;
    Result& res;

    std::unordered_map<uint64_t, Cell> cells;
    std::vector<std::string> names;
    std::unordered_map<std::string, int> nameIndex;
    std::unordered_map<std::string, PaletteEntry> palette;
    std::unordered_map<std::string, bool> warnedName;
    size_t stateStripped = 0;
    size_t opBudget = 100000000;
    bool aborted = false;

    Engine(const NameBook& b, const Options& o, Result& r) : book(b), opt(o), res(r) {}

    void Err(const std::string& where, const std::string& msg) {
        if (res.errors.size() < 40) res.errors.push_back(where + ": " + msg);
        else if (res.errors.size() == 40) res.errors.push_back("...and more errors; fix these first");
    }
    void Warn(const std::string& msg) {
        if (res.warnings.size() < 40) res.warnings.push_back(msg);
    }

    int Intern(const std::string& n) {
        auto it = nameIndex.find(n);
        if (it != nameIndex.end()) return it->second;
        const int id = (int)names.size();
        names.push_back(n);
        nameIndex.emplace(n, id);
        return id;
    }

    // ---- reading fields -----------------------------------------------------
    static bool ReadNum(const JVal& v, double& out) {
        if (v.t == JVal::T::Num) { out = v.n; return true; }
        return false;
    }

    bool ReadV3(const JVal& op, const char* key, const Offset& off, V3& out, const std::string& where,
                bool needed = true) {
        const JVal* v = op.Get(key);
        if (!v) { if (needed) Err(where, std::string("missing \"") + key + "\""); return false; }
        if (v->t != JVal::T::Arr || v->a.size() != 3 || v->a[0].t != JVal::T::Num ||
            v->a[1].t != JVal::T::Num || v->a[2].t != JVal::T::Num) {
            Err(where, std::string("\"") + key + "\" must be three numbers [x, y, z]");
            return false;
        }
        out.x = v->a[0].n + off.x;
        out.y = v->a[1].n + off.y;
        out.z = v->a[2].n + off.z;
        if (std::fabs(out.x) > kCoordLimit || std::fabs(out.y) > kCoordLimit || std::fabs(out.z) > kCoordLimit) {
            Err(where, std::string("\"") + key + "\" is further than " + std::to_string(kCoordLimit) +
                       " blocks from the origin - a typo?");
            return false;
        }
        return true;
    }

    static int RoundInt(double d) { return (int)std::floor(d + 0.5); }

    static bool ReadStr(const JVal& op, const char* key, std::string& out) {
        const JVal* v = op.Get(key);
        if (!v || v->t != JVal::T::Str) return false;
        out = v->s;
        return true;
    }
    static double NumOr(const JVal& op, const char* key, double def) {
        const JVal* v = op.Get(key);
        double d = def;
        if (v && ReadNum(*v, d)) return d;
        return def;
    }
    static bool BoolOr(const JVal& op, const char* key, bool def) {
        const JVal* v = op.Get(key);
        if (v && v->t == JVal::T::Bool) return v->b;
        return def;
    }

    static char ParseDir(std::string s) {
        s = Lower(s);
        if (s == "n" || s == "north" || s == "-z") return 'n';
        if (s == "e" || s == "east" || s == "+x") return 'e';
        if (s == "s" || s == "south" || s == "+z") return 's';
        if (s == "w" || s == "west" || s == "-x") return 'w';
        return 0;
    }

    // rot / axis / facing+flip on an op or palette entry. hasRot is false when
    // none of them was given, so a palette's rotation is not overridden by nothing.
    bool ReadRotation(const JVal& op, const std::string& where, int& rot) {
        if (const JVal* v = op.Get("rot")) {
            double d = 0;
            if (!ReadNum(*v, d) || d < 0 || d > 40) { Err(where, "\"rot\" must be a whole number 0..40"); return false; }
            rot = RoundInt(d);
            return true;
        }
        std::string axis;
        if (ReadStr(op, "axis", axis)) {
            axis = Lower(axis);
            if (axis == "y") rot = 0;
            else if (axis == "x") rot = 5;
            else if (axis == "z") rot = 4;
            else { Err(where, "\"axis\" must be \"x\", \"y\" or \"z\""); return false; }
            return true;
        }
        std::string facing;
        if (ReadStr(op, "facing", facing)) {
            const char f = ParseDir(facing);
            if (!f) { Err(where, "\"facing\" must be n, e, s or w"); return false; }
            rot = StairRotation(f, BoolOr(op, "flip", false));
            return true;
        }
        return false;   // nothing given (not an error)
    }

    // ---- blocks -------------------------------------------------------------
    int ResolveName(const std::string& given, const std::string& where, bool& air, int depth = 0) {
        air = false;
        auto pal = palette.find(given);
        if (pal != palette.end() && depth < 3 && !pal->second.blocks.empty())
            return ResolveName(pal->second.blocks[0], where, air, depth + 1);

        bool wasState = false;
        std::string n = PlainName(given, wasState);
        if (wasState) {
            if (!opt.stripStates) { n = given; }
            else ++stateStripped;
        }
        if (IsAirWord(n)) { air = true; return -1; }
        if (!book.Empty() && !book.Known(n)) {
            const std::string fix = book.CaseFix(n);
            if (!fix.empty()) {
                if (!warnedName[n]) { warnedName[n] = true; Warn("\"" + n + "\" corrected to \"" + fix + "\" (capitalisation)"); }
                n = fix;
            }
        }
        return Intern(n);
    }

    bool ResolveSpec(const JVal& op, const char* key, const std::string& where, Spec& out) {
        out = Spec{};
        const JVal* v = op.Get(key);
        if (!v) { Err(where, std::string("missing \"") + key + "\""); return false; }
        std::vector<const JVal*> items;
        if (v->t == JVal::T::Str) items.push_back(v);
        else if (v->t == JVal::T::Arr && !v->a.empty()) for (const JVal& e : v->a) items.push_back(&e);
        else { Err(where, std::string("\"") + key + "\" must be a block name, a palette key or a list of them"); return false; }

        for (const JVal* item : items) {
            if (item->t != JVal::T::Str) { Err(where, std::string("\"") + key + "\" holds something that is not a name"); return false; }
            auto pal = palette.find(item->s);
            if (pal != palette.end() && !pal->second.blocks.empty()) {
                // A palette key: one choice per block it lists.
                for (const std::string& b : pal->second.blocks) {
                    Choice c;
                    bool air = false;
                    c.id = ResolveName(b, where, air, 1);
                    if (pal->second.hasRot) c.rot = pal->second.rot;
                    out.choices.push_back(c);
                }
                continue;
            }
            Choice c;
            bool air = false;
            c.id = ResolveName(item->s, where, air);
            out.choices.push_back(c);
        }
        if (const JVal* w = op.Get("weights")) {
            if (w->t == JVal::T::Arr && w->a.size() == out.choices.size()) {
                for (const JVal& e : w->a) out.weights.push_back(e.t == JVal::T::Num ? (std::max)(0.0, e.n) : 1.0);
            }
            else Warn(where + ": \"weights\" must have one number per block; ignored");
        }
        out.valid = true;
        return true;
    }

    const Choice& Pick(const Spec& s, int x, int y, int z, uint32_t seed) const {
        if (s.choices.size() == 1) return s.choices[0];
        const double u = Hash01(x, y, z, seed);
        if (s.weights.size() == s.choices.size()) {
            double total = 0;
            for (double w : s.weights) total += w;
            if (total > 0) {
                double acc = 0;
                for (size_t i = 0; i < s.weights.size(); ++i) {
                    acc += s.weights[i] / total;
                    if (u < acc) return s.choices[i];
                }
                return s.choices.back();
            }
        }
        return s.choices[(size_t)(u * (double)s.choices.size()) % s.choices.size()];
    }

    // ---- writing cells ------------------------------------------------------
    bool Put(int x, int y, int z, const Spec& s, int opRot, uint32_t seed, const std::string& where) {
        const Choice& c = Pick(s, x, y, z, seed);
        if (c.id < 0) {
            if (cells.erase(Key(x, y, z))) ++res.erased;
            return true;
        }
        Cell cell;
        cell.name = c.id;
        cell.rot = (opRot >= 0) ? opRot : c.rot;
        cells[Key(x, y, z)] = cell;
        if (cells.size() > opt.maxBlocks) {
            Err(where, "the build passed " + std::to_string(opt.maxBlocks) + " blocks - scale it down");
            aborted = true;
            return false;
        }
        return true;
    }
    bool PutFacing(int x, int y, int z, const Spec& s, char facing, bool flip, uint32_t seed,
                   const std::string& where) {
        return Put(x, y, z, s, StairRotation(facing, flip), seed, where);
    }
    const Cell* At(int x, int y, int z) const {
        auto it = cells.find(Key(x, y, z));
        return it == cells.end() ? nullptr : &it->second;
    }

    static bool VolumeOk(double dx, double dy, double dz) { return dx * dy * dz <= 12.0e6; }

    // ---- ops ----------------------------------------------------------------
    void OpBox(const JVal& op, const Offset& off, const std::string& where) {
        V3 a, b;
        Spec spec;
        if (!ReadV3(op, "from", off, a, where) || !ReadV3(op, "to", off, b, where) ||
            !ResolveSpec(op, "block", where, spec)) return;
        int rot = -1;
        ReadRotation(op, where, rot);
        const int x0 = RoundInt((std::min)(a.x, b.x)), x1 = RoundInt((std::max)(a.x, b.x));
        const int y0 = RoundInt((std::min)(a.y, b.y)), y1 = RoundInt((std::max)(a.y, b.y));
        const int z0 = RoundInt((std::min)(a.z, b.z)), z1 = RoundInt((std::max)(a.z, b.z));
        if (!VolumeOk(x1 - x0 + 1, y1 - y0 + 1, z1 - z0 + 1)) { Err(where, "that box is enormous - a typo?"); return; }
        std::string mode = "fill";
        ReadStr(op, "mode", mode);
        mode = Lower(mode);
        const uint32_t seed = (uint32_t)NumOr(op, "seed", 1);
        if (mode != "fill" && mode != "shell" && mode != "hollow" && mode != "walls" && mode != "floor" &&
            mode != "ceiling" && mode != "frame") {
            Err(where, "\"mode\" must be fill, shell, walls, floor, ceiling or frame");
            return;
        }
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) {
                    const bool ex = (x == x0 || x == x1), ey = (y == y0 || y == y1), ez = (z == z0 || z == z1);
                    bool in = true;
                    if (mode == "shell" || mode == "hollow") in = ex || ey || ez;
                    else if (mode == "walls") in = ex || ez;
                    else if (mode == "floor") in = (y == y0);
                    else if (mode == "ceiling") in = (y == y1);
                    else if (mode == "frame") in = (int)ex + (int)ey + (int)ez >= 2;
                    if (in && !Put(x, y, z, spec, rot, seed, where)) return;
                }
    }

    void OpLine(const JVal& op, const Offset& off, const std::string& where) {
        V3 a, b;
        Spec spec;
        if (!ReadV3(op, "from", off, a, where) || !ReadV3(op, "to", off, b, where) ||
            !ResolveSpec(op, "block", where, spec)) return;
        int rot = -1;
        ReadRotation(op, where, rot);
        const uint32_t seed = (uint32_t)NumOr(op, "seed", 1);
        const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        const int n = (int)(std::max)({ std::fabs(dx), std::fabs(dy), std::fabs(dz) });
        for (int i = 0; i <= n; ++i) {
            const double t = n ? (double)i / (double)n : 0.0;
            if (!Put(RoundInt(a.x + dx * t), RoundInt(a.y + dy * t), RoundInt(a.z + dz * t), spec, rot, seed, where)) return;
        }
    }

    void OpSet(const JVal& op, const Offset& off, const std::string& where) {
        Spec spec;
        if (!ResolveSpec(op, "block", where, spec)) return;
        int rot = -1;
        ReadRotation(op, where, rot);
        const uint32_t seed = (uint32_t)NumOr(op, "seed", 1);
        auto one = [&](const JVal& p) {
            if (p.t != JVal::T::Arr || p.a.size() != 3 || p.a[0].t != JVal::T::Num || p.a[1].t != JVal::T::Num ||
                p.a[2].t != JVal::T::Num) { Err(where, "a point must be [x, y, z]"); return false; }
            const double x = p.a[0].n + off.x, y = p.a[1].n + off.y, z = p.a[2].n + off.z;
            if (std::fabs(x) > kCoordLimit || std::fabs(y) > kCoordLimit || std::fabs(z) > kCoordLimit) {
                Err(where, "a point is further than " + std::to_string(kCoordLimit) + " blocks out - a typo?");
                return false;
            }
            return Put(RoundInt(x), RoundInt(y), RoundInt(z), spec, rot, seed, where);
        };
        if (const JVal* at = op.Get("at")) { one(*at); return; }
        if (const JVal* pts = op.Get("points")) {
            if (pts->t != JVal::T::Arr) { Err(where, "\"points\" must be a list of [x, y, z]"); return; }
            for (const JVal& p : pts->a) if (!one(p) || aborted) return;
            return;
        }
        Err(where, "needs \"at\": [x, y, z] or \"points\": [[x, y, z], ...]");
    }

    void OpStairs(const JVal& op, const Offset& off, const std::string& where) {
        V3 a;
        Spec spec, under;
        if (!ReadV3(op, "from", off, a, where) || !ResolveSpec(op, "block", where, spec)) return;
        std::string dirText;
        if (!ReadStr(op, "dir", dirText) || !ParseDir(dirText)) { Err(where, "\"dir\" must be n, e, s or w - the way the stairs climb"); return; }
        const char dir = ParseDir(dirText);
        const int steps = (int)NumOr(op, "steps", 0);
        const int width = (std::max)(1, (int)NumOr(op, "width", 1));
        if (steps < 1 || steps > 400) { Err(where, "\"steps\" must be 1..400"); return; }
        const bool hasUnder = op.Get("under") != nullptr && ResolveSpec(op, "under", where, under);
        const uint32_t seed = (uint32_t)NumOr(op, "seed", 1);
        const int x0 = RoundInt(a.x), y0 = RoundInt(a.y), z0 = RoundInt(a.z);
        const int underTo = (int)NumOr(op, "under_to", y0);
        const int dx = dir == 'e' ? 1 : dir == 'w' ? -1 : 0;
        const int dz = dir == 's' ? 1 : dir == 'n' ? -1 : 0;
        const int lat0 = -((width - 1) / 2);
        for (int i = 0; i < steps; ++i) {
            for (int w = 0; w < width; ++w) {
                const int lat = lat0 + w;
                const int x = x0 + dx * i + (dx == 0 ? lat : 0);
                const int z = z0 + dz * i + (dz == 0 ? lat : 0);
                const int y = y0 + i;
                if (hasUnder)
                    for (int yy = underTo; yy < y; ++yy) if (!Put(x, yy, z, under, -1, seed, where)) return;
                if (!PutFacing(x, y, z, spec, dir, false, seed, where)) return;
            }
        }
    }

    // A gable roof: two slopes of a single piece each, rising one block for every
    // block across, meeting at a ridge. From a real house: hollow roof pieces for
    // the slopes and a flat piece for the ridge.
    void OpGable(const JVal& op, const Offset& off, const std::string& where) {
        V3 a, b;
        Spec slope, cap, fill;
        if (!ReadV3(op, "from", off, a, where) || !ReadV3(op, "to", off, b, where) ||
            !ResolveSpec(op, "block", where, slope)) return;
        const bool hasCap = op.Get("cap") != nullptr && ResolveSpec(op, "cap", where, cap);
        const bool hasFill = op.Get("gable_fill") != nullptr && ResolveSpec(op, "gable_fill", where, fill);
        const int x0 = RoundInt((std::min)(a.x, b.x)), x1 = RoundInt((std::max)(a.x, b.x));
        const int z0 = RoundInt((std::min)(a.z, b.z)), z1 = RoundInt((std::max)(a.z, b.z));
        const int y0 = RoundInt((std::min)(a.y, b.y));
        std::string ridge;
        ReadStr(op, "ridge", ridge);
        ridge = Lower(ridge);
        if (ridge != "x" && ridge != "z") ridge = ((x1 - x0) >= (z1 - z0)) ? "x" : "z";
        const bool ridgeAlongX = ridge == "x";
        const int a0 = ridgeAlongX ? z0 : x0, a1 = ridgeAlongX ? z1 : x1;     // across
        const int r0 = ridgeAlongX ? x0 : z0, r1 = ridgeAlongX ? x1 : z1;     // along
        const int W = a1 - a0 + 1;
        const int over = (std::max)(0, (int)NumOr(op, "overhang", 0));
        const int endOver = (std::max)(0, (int)NumOr(op, "end_overhang", 0));
        const uint32_t seed = (uint32_t)NumOr(op, "seed", 1);
        if (W > 400 || (r1 - r0) > 2000) { Err(where, "that roof is enormous - a typo?"); return; }

        for (int j = -over; j <= W - 1 + over; ++j) {
            const int h = (std::min)(j, W - 1 - j);
            const bool centre = (j == W - 1 - j);
            const bool lowSide = j <= W - 1 - j;
            const bool peak = h == (W - 1) / 2 && (centre || (W % 2 == 0 && (j == W / 2 - 1 || j == W / 2)));
            // Which way the tall side points: toward the ridge.
            const char facing = ridgeAlongX ? (lowSide ? 's' : 'n') : (lowSide ? 'e' : 'w');
            for (int r = r0 - endOver; r <= r1 + endOver; ++r) {
                const int across = a0 + j;
                const int x = ridgeAlongX ? r : across;
                const int z = ridgeAlongX ? across : r;
                const int y = y0 + h;
                if (peak && hasCap) { if (!Put(x, y, z, cap, -1, seed, where)) return; }
                else if (!PutFacing(x, y, z, slope, facing, false, seed, where)) return;
            }
        }
        if (hasFill) {
            // The triangle under the slope at each end of the ridge.
            for (int c = 0; c < W; ++c) {
                const int top = y0 + (std::min)(c, W - 1 - c) - 1;
                for (int y = y0; y <= top; ++y)
                    for (int r : { r0, r1 }) {
                        const int across = a0 + c;
                        if (!Put(ridgeAlongX ? r : across, y, ridgeAlongX ? across : r, fill, -1, seed, where)) return;
                    }
            }
        }
    }

    // A hipped roof or a pyramid: a ring of stairs that shrinks by one each level.
    void OpHip(const JVal& op, const Offset& off, const std::string& where) {
        V3 a, b;
        Spec slope, cap;
        if (!ReadV3(op, "from", off, a, where) || !ReadV3(op, "to", off, b, where) ||
            !ResolveSpec(op, "block", where, slope)) return;
        const bool hasCap = op.Get("cap") != nullptr && ResolveSpec(op, "cap", where, cap);
        const int x0 = RoundInt((std::min)(a.x, b.x)), x1 = RoundInt((std::max)(a.x, b.x));
        const int z0 = RoundInt((std::min)(a.z, b.z)), z1 = RoundInt((std::max)(a.z, b.z));
        const int y0 = RoundInt((std::min)(a.y, b.y));
        const int over = (std::max)(0, (int)NumOr(op, "overhang", 0));
        const uint32_t seed = (uint32_t)NumOr(op, "seed", 1);
        if ((x1 - x0) > 400 || (z1 - z0) > 400) { Err(where, "that roof is enormous - a typo?"); return; }

        for (int i = -over; i < 400; ++i) {
            const int xa = x0 + i, xb = x1 - i, za = z0 + i, zb = z1 - i;
            if (xa > xb || za > zb) break;
            const bool last = (xa + 1 > xb - 1) || (za + 1 > zb - 1);
            const int y = y0 + i;
            for (int z = za; z <= zb; ++z)
                for (int x = xa; x <= xb; ++x) {
                    const bool edge = (x == xa || x == xb || z == za || z == zb);
                    if (!edge && !last) continue;
                    if (last && hasCap) { if (!Put(x, y, z, cap, -1, seed, where)) return; continue; }
                    // Toward the middle: the north edge climbs south, and so on.
                    // A corner takes the north/south direction; the game shapes it.
                    char f;
                    if (z == za) f = 's';
                    else if (z == zb) f = 'n';
                    else if (x == xa) f = 'e';
                    else f = 'w';
                    if (!PutFacing(x, y, z, slope, f, false, seed, where)) return;
                }
            if (last) break;
        }
    }

    static bool Inside(double d, double r) { return d <= r + 0.5; }

    void OpCylinder(const JVal& op, const Offset& off, const std::string& where) {
        V3 c;
        Spec spec;
        if (!ReadV3(op, "center", off, c, where) || !ResolveSpec(op, "block", where, spec)) return;
        const double r = NumOr(op, "radius", 0);
        const int height = (int)NumOr(op, "height", 1);
        if (r < 0.5 || r > 300 || height < 1 || height > 2000) { Err(where, "\"radius\" must be 0.5..300 and \"height\" 1..2000"); return; }
        std::string mode = "fill";
        ReadStr(op, "mode", mode);
        mode = Lower(mode);
        if (mode != "fill" && mode != "wall" && mode != "shell") { Err(where, "\"mode\" must be fill, wall or shell"); return; }
        int rot = -1;
        ReadRotation(op, where, rot);
        const uint32_t seed = (uint32_t)NumOr(op, "seed", 1);
        const int cx = RoundInt(c.x), cz = RoundInt(c.z), y0 = RoundInt(c.y);
        const int R = (int)std::ceil(r) + 1;
        for (int dy = 0; dy < height; ++dy)
            for (int dz = -R; dz <= R; ++dz)
                for (int dx = -R; dx <= R; ++dx) {
                    const double d = std::sqrt((double)(dx * dx + dz * dz));
                    if (!Inside(d, r)) continue;
                    const bool ring = !Inside(d, r - 1.0);
                    const bool cap = (dy == 0 || dy == height - 1);
                    bool in = true;
                    if (mode == "wall") in = ring;
                    else if (mode == "shell") in = ring || cap;
                    if (in && !Put(cx + dx, y0 + dy, cz + dz, spec, rot, seed, where)) return;
                }
    }

    void OpSphere(const JVal& op, const Offset& off, const std::string& where) {
        V3 c;
        Spec spec;
        if (!ReadV3(op, "center", off, c, where) || !ResolveSpec(op, "block", where, spec)) return;
        double rx = NumOr(op, "radius", 0), ry = rx, rz = rx;
        if (const JVal* radii = op.Get("radii")) {
            if (radii->t == JVal::T::Arr && radii->a.size() == 3 && radii->a[0].t == JVal::T::Num &&
                radii->a[1].t == JVal::T::Num && radii->a[2].t == JVal::T::Num) {
                rx = radii->a[0].n; ry = radii->a[1].n; rz = radii->a[2].n;
            }
            else { Err(where, "\"radii\" must be three numbers [rx, ry, rz]"); return; }
        }
        if (rx < 0.5 || ry < 0.5 || rz < 0.5 || rx > 300 || ry > 300 || rz > 300) { Err(where, "the radius must be 0.5..300"); return; }
        std::string mode = "fill", part = "all";
        ReadStr(op, "mode", mode);
        ReadStr(op, "part", part);
        mode = Lower(mode);
        part = Lower(part);
        if (mode != "fill" && mode != "shell") { Err(where, "\"mode\" must be fill or shell"); return; }
        if (part != "all" && part != "upper" && part != "lower") { Err(where, "\"part\" must be all, upper or lower"); return; }
        const uint32_t seed = (uint32_t)NumOr(op, "seed", 1);
        const int cx = RoundInt(c.x), cy = RoundInt(c.y), cz = RoundInt(c.z);
        const int Rx = (int)std::ceil(rx) + 1, Ry = (int)std::ceil(ry) + 1, Rz = (int)std::ceil(rz) + 1;
        auto norm = [](double dx, double dy, double dz, double ax, double ay, double az) {
            return (dx * dx) / (ax * ax) + (dy * dy) / (ay * ay) + (dz * dz) / (az * az);
        };
        for (int dy = -Ry; dy <= Ry; ++dy) {
            if (part == "upper" && dy < 0) continue;
            if (part == "lower" && dy > 0) continue;
            for (int dz = -Rz; dz <= Rz; ++dz)
                for (int dx = -Rx; dx <= Rx; ++dx) {
                    if (norm(dx, dy, dz, rx + 0.5, ry + 0.5, rz + 0.5) > 1.0) continue;
                    if (mode == "shell" &&
                        norm(dx, dy, dz, (std::max)(0.5, rx - 0.5), (std::max)(0.5, ry - 0.5), (std::max)(0.5, rz - 0.5)) <= 1.0)
                        continue;
                    if (!Put(cx + dx, cy + dy, cz + dz, spec, -1, seed, where)) return;
                }
        }
    }

    void OpMirror(const JVal& op, const std::string& where) {
        std::string axis;
        if (!ReadStr(op, "axis", axis)) { Err(where, "needs \"axis\": \"x\" or \"z\""); return; }
        axis = Lower(axis);
        if (axis != "x" && axis != "z") { Err(where, "\"axis\" must be \"x\" or \"z\""); return; }
        const JVal* atv = op.Get("at");
        double at = 0;
        if (!atv || !ReadNum(*atv, at)) { Err(where, "needs \"at\": the coordinate of the mirror plane, e.g. 10 or 10.5"); return; }
        const int twice = RoundInt(at * 2.0);
        std::string keep = "low";
        ReadStr(op, "keep", keep);
        keep = Lower(keep);
        const bool useLow = keep != "high";

        struct Item { int x, y, z; Cell c; };
        std::vector<Item> src;
        for (const auto& kv : cells) {
            const uint64_t k = kv.first;
            const int x = (int)(k & 0x1FFFFF) - (1 << 20);
            const int y = (int)((k >> 21) & 0x1FFFFF) - (1 << 20);
            const int z = (int)((k >> 42) & 0x1FFFFF) - (1 << 20);
            const int coord = axis == "x" ? x : z;
            if (useLow ? (coord * 2 < twice) : (coord * 2 > twice)) src.push_back({ x, y, z, kv.second });
        }
        for (const Item& it : src) {
            const int coord = axis == "x" ? it.x : it.z;
            const int dest = twice - coord;
            Cell c = it.c;
            const std::string& nm = names[(size_t)c.name];
            // A block that has no facing is left without one; one that does is
            // turned as its mirror image would be.
            if (c.rot >= 0) c.rot = MirrorRotation(c.rot, axis[0]);
            else if (IsStairLike(nm) || IsBeamLike(nm)) c.rot = MirrorRotation(0, axis[0]);
            if (c.rot == 0 && it.c.rot < 0) c.rot = -1;
            const int nx = axis == "x" ? dest : it.x;
            const int nz = axis == "z" ? dest : it.z;
            if (std::abs(nx) > kCoordLimit || std::abs(nz) > kCoordLimit) { Err(where, "the mirrored copy would be too far out"); return; }
            cells[Key(nx, it.y, nz)] = c;
            if (cells.size() > opt.maxBlocks) { Err(where, "the build passed the block limit"); aborted = true; return; }
        }
    }

    // ---- road: a ribbon of blocks along a smooth curve -----------------------
    // The op race tracks need: a long road with curves cannot sensibly be written as boxes. The points
    // are waypoints the centre line passes through (a centripetal Catmull-Rom spline rounds the
    // corners); the height follows the points' y. Everything is decided per cell from its nearest
    // spot on the centre line, so the road has no gaps on curves and keeps its width.
    struct RoadSample { double x, y, z, tx, tz, s; };

    // The centre line, sampled finely. `round`: straight between the waypoints, each corner turned into
    // a circular arc of (at most) `radius`; `spline`: a smooth curve through every waypoint; `sharp`:
    // plain straight lines. The height is interpolated between the waypoints along the line.
    enum class RoadCurveMode { Round, Spline, Sharp };

    static void RoadCurve(const std::vector<V3>& p, bool closed, RoadCurveMode mode, double radius,
                          std::vector<RoadSample>& out) {
        const int n = (int)p.size();
        std::vector<double> mark;               // arc length along the line at each waypoint
        std::vector<RoadSample> dense;
        std::vector<size_t> markIdx((size_t)n, 0);
        auto push = [&](double x, double z) {
            RoadSample r{};
            r.x = x; r.z = z;
            dense.push_back(r);
        };
        auto straight = [&](double ax, double az, double bx, double bz) {
            const double len = std::hypot(bx - ax, bz - az);
            const int steps = (std::max)(1, (int)std::ceil(len * 4.0));
            for (int k = 1; k <= steps; ++k) push(ax + (bx - ax) * k / steps, az + (bz - az) * k / steps);
        };

        if (mode == RoadCurveMode::Spline) {
            const int segs = closed ? n : n - 1;
            auto at = [&](int i) -> V3 {
                if (closed) return p[(size_t)(((i % n) + n) % n)];
                return p[(size_t)(std::max)(0, (std::min)(n - 1, i))];
            };
            for (int i = 0; i < segs; ++i) {
                V3 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
                if (!closed) {           // reflect the end points so the curve leaves them straight
                    if (i == 0) p0 = V3{ 2 * p1.x - p2.x, 0, 2 * p1.z - p2.z };
                    if (i == segs - 1) p3 = V3{ 2 * p2.x - p1.x, 0, 2 * p2.z - p1.z };
                }
                const double chord = std::hypot(p2.x - p1.x, p2.z - p1.z);
                const int steps = (std::max)(2, (int)std::ceil(chord * 8.0));
                // centripetal parameterisation: no loops or overshoot at sharp corners
                auto knot = [](const V3& a, const V3& b) { return std::sqrt((std::max)(1e-6, std::hypot(b.x - a.x, b.z - a.z))); };
                const double t0 = 0, t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
                auto lerp2 = [](double ax, double az, double bx, double bz, double ta, double tb, double t, double& ox, double& oz) {
                    const double f = (tb - ta) > 0 ? (t - ta) / (tb - ta) : 0;
                    ox = ax + (bx - ax) * f; oz = az + (bz - az) * f;
                };
                if (i == 0) markIdx[0] = 0;
                for (int k = (i == 0 ? 0 : 1); k <= steps; ++k) {
                    const double t = t1 + (t2 - t1) * ((double)k / steps);
                    double a1x, a1z, a2x, a2z, a3x, a3z, b1x, b1z, b2x, b2z, cx, cz;
                    lerp2(p0.x, p0.z, p1.x, p1.z, t0, t1, t, a1x, a1z);
                    lerp2(p1.x, p1.z, p2.x, p2.z, t1, t2, t, a2x, a2z);
                    lerp2(p2.x, p2.z, p3.x, p3.z, t2, t3, t, a3x, a3z);
                    lerp2(a1x, a1z, a2x, a2z, t0, t2, t, b1x, b1z);
                    lerp2(a2x, a2z, a3x, a3z, t1, t3, t, b2x, b2z);
                    lerp2(b1x, b1z, b2x, b2z, t1, t2, t, cx, cz);
                    push(cx, cz);
                }
                markIdx[(size_t)((i + 1) % n)] = dense.size() - 1;
            }
            if (closed) markIdx[0] = 0;
        } else {
            struct Corner { double inx, inz, outx, outz, cx, cz, a0, sweep, r; bool arc = false; };
            std::vector<Corner> cn((size_t)n);
            for (int i = 0; i < n; ++i) {
                Corner& c = cn[(size_t)i];
                c.inx = c.outx = p[(size_t)i].x;
                c.inz = c.outz = p[(size_t)i].z;
                const bool interior = closed || (i > 0 && i < n - 1);
                if (!interior || mode == RoadCurveMode::Sharp) continue;
                const V3& a = p[(size_t)((i + n - 1) % n)];
                const V3& v = p[(size_t)i];
                const V3& b = p[(size_t)((i + 1) % n)];
                double d1x = v.x - a.x, d1z = v.z - a.z, d2x = b.x - v.x, d2z = b.z - v.z;
                const double l1 = std::hypot(d1x, d1z), l2 = std::hypot(d2x, d2z);
                if (l1 < 1e-6 || l2 < 1e-6) continue;
                d1x /= l1; d1z /= l1; d2x /= l2; d2z /= l2;
                const double cr = d1x * d2z - d1z * d2x, dt = d1x * d2x + d1z * d2z;
                const double theta = std::atan2(std::fabs(cr), dt);        // how far the road turns here
                if (theta < 0.02 || theta > 3.1) continue;
                const double tn = std::tan(theta / 2.0);
                double t = radius * tn;
                t = (std::min)(t, 0.5 * (std::min)(l1, l2));
                const double rr = t / tn;
                c.inx = v.x - d1x * t; c.inz = v.z - d1z * t;
                c.outx = v.x + d2x * t; c.outz = v.z + d2z * t;
                const double sgn = cr > 0 ? 1.0 : -1.0;
                // centre lies to the side the road turns towards
                c.cx = c.inx - d1z * sgn * rr;
                c.cz = c.inz + d1x * sgn * rr;
                c.a0 = std::atan2(c.inz - c.cz, c.inx - c.cx);
                c.sweep = sgn * theta;
                c.r = rr;
                c.arc = true;
            }
            push(cn[0].inx, cn[0].inz);
            for (int i = 0; i < n; ++i) {
                const Corner& c = cn[(size_t)i];
                if (c.arc) {
                    const int steps = (std::max)(2, (int)std::ceil(c.r * std::fabs(c.sweep) * 4.0));
                    for (int k = 1; k <= steps; ++k) {
                        const double a = c.a0 + c.sweep * k / steps;
                        push(c.cx + std::cos(a) * c.r, c.cz + std::sin(a) * c.r);
                        if (k == steps / 2) markIdx[(size_t)i] = dense.size() - 1;
                    }
                } else {
                    markIdx[(size_t)i] = dense.size() - 1;
                }
                const int j = i + 1;
                if (j < n) straight(c.outx, c.outz, cn[(size_t)j].inx, cn[(size_t)j].inz);
                else if (closed) straight(c.outx, c.outz, cn[0].inx, cn[0].inz);
            }
        }

        // arc length, then the height from the waypoints
        double s = 0;
        for (size_t i = 0; i < dense.size(); ++i) {
            if (i) s += std::hypot(dense[i].x - dense[i - 1].x, dense[i].z - dense[i - 1].z);
            dense[i].s = s;
        }
        mark.resize((size_t)n);
        for (int i = 0; i < n; ++i) mark[(size_t)i] = dense[(std::min)(markIdx[(size_t)i], dense.size() - 1)].s;
        if (closed) mark.push_back(s);               // the last stretch runs back to the first waypoint
        const int ymax = closed ? n : n - 1;
        for (RoadSample& r : dense) {
            int seg = 0;
            while (seg + 1 < ymax && r.s > mark[(size_t)seg + 1]) ++seg;
            const double s0 = mark[(size_t)seg], s1 = mark[(size_t)seg + 1];
            const double u = s1 > s0 ? (std::min)(1.0, (std::max)(0.0, (r.s - s0) / (s1 - s0))) : 0.0;
            const double y0 = p[(size_t)seg].y, y1 = p[(size_t)((seg + 1) % n)].y;
            r.y = y0 + (y1 - y0) * u;
        }
        for (size_t i = 0; i < dense.size(); ++i) {          // direction of travel
            const RoadSample& a = dense[i > 0 ? i - 1 : 0];
            const RoadSample& b = dense[(std::min)(i + 1, dense.size() - 1)];
            double dx = b.x - a.x, dz = b.z - a.z;
            const double len = std::hypot(dx, dz);
            if (len > 1e-9) { dx /= len; dz /= len; } else { dx = 1; dz = 0; }
            dense[i].tx = dx; dense[i].tz = dz;
        }
        out = std::move(dense);
    }

    void OpRoad(const JVal& op, const Offset& off, const std::string& where) {
        const JVal* pts = op.Get("points");
        if (!pts || pts->t != JVal::T::Arr || pts->a.size() < 2) {
            Err(where, "needs \"points\": [[x, y, z], [x, y, z], ...] - at least two waypoints");
            return;
        }
        std::vector<V3> p;
        for (const JVal& e : pts->a) {
            if (e.t != JVal::T::Arr || e.a.size() != 3 || e.a[0].t != JVal::T::Num || e.a[1].t != JVal::T::Num ||
                e.a[2].t != JVal::T::Num) { Err(where, "each point must be [x, y, z]"); return; }
            V3 v{ e.a[0].n + off.x, e.a[1].n + off.y, e.a[2].n + off.z };
            if (std::fabs(v.x) > kCoordLimit || std::fabs(v.y) > kCoordLimit || std::fabs(v.z) > kCoordLimit) {
                Err(where, "a point is further than " + std::to_string(kCoordLimit) + " blocks out - a typo?");
                return;
            }
            p.push_back(v);
        }
        const double width = NumOr(op, "width", 7);
        if (width < 1 || width > 64) { Err(where, "\"width\" must be 1..64"); return; }
        const bool closed = BoolOr(op, "closed", false);
        if (closed && p.size() < 3) { Err(where, "a closed road needs at least three points"); return; }
        std::string curveName = "round";
        ReadStr(op, "curve", curveName);
        curveName = Lower(curveName);
        RoadCurveMode mode = RoadCurveMode::Round;
        if (curveName == "spline") mode = RoadCurveMode::Spline;
        else if (curveName == "sharp") mode = RoadCurveMode::Sharp;
        else if (curveName != "round") { Err(where, "\"curve\" must be round, spline or sharp"); return; }
        const double radius = (std::max)(1.0, NumOr(op, "radius", (std::max)(3.0, width)));

        Spec surface, edge, line, barrier, verge, under;
        if (!ResolveSpec(op, "block", where, surface)) return;
        const bool hasEdge = op.Get("edge") && ResolveSpec(op, "edge", where, edge);
        const bool hasLine = op.Get("line") && ResolveSpec(op, "line", where, line);
        const bool hasBarrier = op.Get("barrier") && ResolveSpec(op, "barrier", where, barrier);
        const bool hasVerge = op.Get("verge") && ResolveSpec(op, "verge", where, verge);
        const bool hasUnder = op.Get("under") && ResolveSpec(op, "under", where, under);
        if ((op.Get("edge") && !hasEdge) || (op.Get("line") && !hasLine) || (op.Get("barrier") && !hasBarrier) ||
            (op.Get("verge") && !hasVerge) || (op.Get("under") && !hasUnder)) return;

        const double edgeWidth = (std::max)(0.0, NumOr(op, "edge_width", hasEdge ? 1 : 0));
        const double stripe = (std::max)(1.0, NumOr(op, "stripe", 3));      // cells per colour along the road
        const double lineWidth = (std::max)(1.0, NumOr(op, "line_width", 1));
        double dashOn = 0, dashOff = 0;
        if (const JVal* d = op.Get("dash")) {
            if (d->t == JVal::T::Arr && d->a.size() == 2 && d->a[0].t == JVal::T::Num && d->a[1].t == JVal::T::Num &&
                d->a[0].n >= 1 && d->a[1].n >= 0) { dashOn = d->a[0].n; dashOff = d->a[1].n; }
            else { Err(where, "\"dash\" must be [cells drawn, cells skipped]"); return; }
        }
        const double gap = (std::max)(0.0, NumOr(op, "barrier_gap", 0));
        std::string sides = "both";
        ReadStr(op, "barrier_sides", sides);
        sides = Lower(sides);
        if (sides != "both" && sides != "left" && sides != "right") { Err(where, "\"barrier_sides\" must be both, left or right"); return; }
        const int depth = (int)(std::max)(0.0, (std::min)(64.0, NumOr(op, "depth", 1)));
        std::string edgeMode = "top";
        ReadStr(op, "edge_mode", edgeMode);
        edgeMode = Lower(edgeMode);
        if (edgeMode != "top" && edgeMode != "same") { Err(where, "\"edge_mode\" must be top (on the road) or same (in the road's own layer)"); return; }
        const bool autoRot = BoolOr(op, "auto_rot", true);      // turn rails, curbs and lines to follow the road
        const uint32_t seed = (uint32_t)NumOr(op, "seed", 1);

        std::vector<RoadSample> path;
        RoadCurve(p, closed, mode, radius, path);
        if (path.size() > 400000) { Err(where, "that road is far too long"); return; }

        const double half = width / 2.0;
        const double reach = half + (hasBarrier || hasVerge ? gap + 1.0 : 0.0) + 0.75;
        struct Hit { float d; int idx; };
        std::unordered_map<uint64_t, Hit> hit;
        const int r = (int)std::ceil(reach);
        for (size_t i = 0; i < path.size(); ++i) {
            const RoadSample& s = path[i];
            const int cx = RoundInt(s.x), cz = RoundInt(s.z);
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    const int x = cx + dx, z = cz + dz;
                    const double d = std::hypot(x - s.x, z - s.z);
                    if (d > reach) continue;
                    const uint64_t k = Key(x, 0, z);
                    auto it = hit.find(k);
                    if (it == hit.end()) hit.emplace(k, Hit{ (float)d, (int)i });
                    else if (d < it->second.d) it->second = Hit{ (float)d, (int)i };
                }
            if (hit.size() > opt.maxBlocks) { Err(where, "that road covers more than " + std::to_string(opt.maxBlocks) + " cells - scale it down"); aborted = true; return; }
        }

        // The cells, in a fixed order so a repeated build is identical.
        struct Item { int x, z; Hit h; };
        std::vector<Item> items;
        items.reserve(hit.size());
        const uint64_t base = 1u << 20;
        for (const auto& kv : hit) {
            const int x = (int)((kv.first & 0x1FFFFF) - base), z = (int)(((kv.first >> 42) & 0x1FFFFF) - base);
            items.push_back({ x, z, kv.second });
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });

        const double eps = 1e-6;
        auto one = [&](const Choice& c) { Spec s; s.choices.push_back(c); s.valid = true; return s; };
        // rot 0 is the way thin pieces run east-west; pictures that run north-south need the other one
        auto alongRot = [&](const RoadSample& s) { return !autoRot ? -1 : (std::fabs(s.tx) >= std::fabs(s.tz) ? 0 : 1); };

        for (const Item& it : items) {
            const RoadSample& s = path[(size_t)it.h.idx];
            const int ySurf = RoundInt(s.y);
            const double d = it.h.d;
            const double cross = s.tx * (it.z - s.z) - s.tz * (it.x - s.x);      // < 0: left of the direction of travel
            const bool onRoad = d <= half + eps;
            if (onRoad) {
                if (!Put(it.x, ySurf, it.z, surface, -1, seed, where)) return;
                if (hasUnder)
                    for (int k = 1; k <= depth; ++k) if (!Put(it.x, ySurf - k, it.z, under, -1, seed, where)) return;
                const bool isEdge = hasEdge && edgeWidth > 0 && d > half - edgeWidth + eps;
                if (isEdge) {
                    const size_t n = edge.choices.size();
                    const Choice& c = edge.choices[(size_t)std::floor(s.s / stripe) % n];
                    const int yy = edgeMode == "top" ? ySurf + 1 : ySurf;
                    if (!Put(it.x, yy, it.z, one(c), c.rot >= 0 ? c.rot : alongRot(s), seed, where)) return;
                }
                if (hasLine && d <= lineWidth / 2.0 + eps &&
                    (dashOn <= 0 || std::fmod(s.s, dashOn + dashOff) < dashOn)) {
                    const Choice& c = line.choices[0];
                    if (!Put(it.x, ySurf + 1, it.z, one(c), c.rot >= 0 ? c.rot : (autoRot ? 1 - alongRot(s) : -1), seed, where)) return;
                }
            } else if (hasBarrier || hasVerge) {
                const bool sideOk = sides == "both" || (sides == "left" && cross < 0) || (sides == "right" && cross >= 0);
                if (!sideOk) continue;
                if (d > half + gap + 1.0 + eps) continue;
                if (hasVerge && !Put(it.x, ySurf, it.z, verge, -1, seed, where)) return;
                if (hasBarrier && d > half + gap + eps) {
                    const Choice& c = Pick(barrier, it.x, ySurf + 1, it.z, seed);
                    if (!Put(it.x, ySurf + 1, it.z, one(c), c.rot >= 0 ? c.rot : alongRot(s), seed, where)) return;
                }
            }
        }
    }

    void OpScatter(const JVal& op, const Offset& off, const std::string& where) {
        V3 a, b;
        Spec spec, onlySpec;
        if (!ReadV3(op, "from", off, a, where) || !ReadV3(op, "to", off, b, where) ||
            !ResolveSpec(op, "block", where, spec)) return;
        std::string mode = "replace";
        ReadStr(op, "where", mode);
        mode = Lower(mode);
        if (mode != "replace" && mode != "top" && mode != "under" && mode != "empty") {
            Err(where, "\"where\" must be replace, top, under or empty");
            return;
        }
        const bool hasOnly = op.Get("only") != nullptr && ResolveSpec(op, "only", where, onlySpec);
        const double chance = (std::min)(1.0, (std::max)(0.0, NumOr(op, "chance", 0.1)));
        const uint32_t seed = (uint32_t)NumOr(op, "seed", 1);
        int rot = -1;
        ReadRotation(op, where, rot);
        const int x0 = RoundInt((std::min)(a.x, b.x)), x1 = RoundInt((std::max)(a.x, b.x));
        const int y0 = RoundInt((std::min)(a.y, b.y)), y1 = RoundInt((std::max)(a.y, b.y));
        const int z0 = RoundInt((std::min)(a.z, b.z)), z1 = RoundInt((std::max)(a.z, b.z));
        if (!VolumeOk(x1 - x0 + 1, y1 - y0 + 1, z1 - z0 + 1)) { Err(where, "that region is enormous - a typo?"); return; }
        std::vector<int> onlyIds;
        if (hasOnly) for (const Choice& c : onlySpec.choices) onlyIds.push_back(c.id);
        // Decide from the model as it stands, then write: otherwise a flower placed
        // on the floor makes the cell above it "the top of something" and they stack.
        struct Spot { int x, y, z; };
        std::vector<Spot> chosen;
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) {
                    const Cell* here = At(x, y, z);
                    bool ok = false;
                    if (mode == "replace") {
                        ok = here != nullptr;
                        if (ok && hasOnly) ok = std::find(onlyIds.begin(), onlyIds.end(), here->name) != onlyIds.end();
                    }
                    else if (mode == "top") ok = !here && At(x, y - 1, z) && !At(x, y + 1, z);
                    else if (mode == "under") ok = !here && At(x, y + 1, z);
                    else ok = !here;
                    if (!ok) continue;
                    if (Hash01(x, y, z, seed + 977u) >= chance) continue;
                    chosen.push_back({ x, y, z });
                }
        for (const Spot& s : chosen)
            if (!Put(s.x, s.y, s.z, spec, rot, seed, where)) return;
    }

    void RunOps(const JVal& list, const Offset& off, int depth, const std::string& path) {
        if (list.t != JVal::T::Arr) { Err(path, "\"ops\" must be a list"); return; }
        if (depth > 4) { Err(path, "repeat blocks are nested too deeply"); return; }
        int index = 0;
        for (const JVal& op : list.a) {
            ++index;
            if (aborted) return;
            std::string opName;
            const std::string where = path + "#" + std::to_string(index);
            if (op.t != JVal::T::Obj || !ReadStr(op, "op", opName)) { Err(where, "each op needs an \"op\" name"); continue; }
            opName = Lower(opName);
            const std::string label = where + " (" + opName + ")";
            if (opBudget == 0) { Err(label, "too many operations"); aborted = true; return; }
            --opBudget;
            ++res.opsRun;
            if (opName == "box" || opName == "fill") OpBox(op, off, label);
            else if (opName == "line") OpLine(op, off, label);
            else if (opName == "set" || opName == "point" || opName == "points") OpSet(op, off, label);
            else if (opName == "stairs") OpStairs(op, off, label);
            else if (opName == "gable") OpGable(op, off, label);
            else if (opName == "hip" || opName == "pyramid") OpHip(op, off, label);
            else if (opName == "cylinder" || opName == "cyl") OpCylinder(op, off, label);
            else if (opName == "sphere" || opName == "dome") OpSphere(op, off, label);
            else if (opName == "mirror") OpMirror(op, label);
            else if (opName == "scatter") OpScatter(op, off, label);
            else if (opName == "road" || opName == "track") OpRoad(op, off, label);
            else if (opName == "repeat") {
                const int count = (int)NumOr(op, "count", 0);
                if (count < 1 || count > 2000) { Err(label, "\"count\" must be 1..2000"); continue; }
                V3 step{ 1, 0, 0 };
                if (op.Get("step")) { if (!ReadV3(op, "step", Offset{}, step, label)) continue; }
                const JVal* inner = op.Get("ops");
                if (!inner) { Err(label, "needs \"ops\": [...] to repeat"); continue; }
                for (int i = 0; i < count && !aborted; ++i) {
                    Offset o = off;
                    o.x += (int)std::floor(step.x * i + 0.5);
                    o.y += (int)std::floor(step.y * i + 0.5);
                    o.z += (int)std::floor(step.z * i + 0.5);
                    RunOps(*inner, o, depth + 1, label + ">");
                }
            }
            else Err(label, "unknown op \"" + opName + "\" (box, line, set, stairs, gable, hip, cylinder, sphere, mirror, scatter, repeat, road)");
        }
    }

    bool LoadPalette(const JVal& root) {
        const JVal* pal = root.Get("palette");
        if (!pal) return true;
        if (pal->t != JVal::T::Obj) { Err("palette", "must be an object of name: block"); return false; }
        for (const auto& kv : pal->o) {
            PaletteEntry e;
            auto takeList = [&](const JVal& list) {
                for (const JVal& item : list.a)
                    if (item.t == JVal::T::Str) e.blocks.push_back(item.s);
                    else Err("palette." + kv.first, "a list must hold block names");
            };
            if (kv.second.t == JVal::T::Str) e.blocks.push_back(kv.second.s);
            else if (kv.second.t == JVal::T::Arr) takeList(kv.second);
            else if (kv.second.t == JVal::T::Obj) {
                const JVal* b = kv.second.Get("block");
                if (b && b->t == JVal::T::Str) e.blocks.push_back(b->s);
                else if (b && b->t == JVal::T::Arr) takeList(*b);
                else { Err("palette." + kv.first, "needs a \"block\""); continue; }
                int rot = -1;
                if (ReadRotation(kv.second, "palette." + kv.first, rot)) { e.rot = rot; e.hasRot = true; }
            }
            else { Err("palette." + kv.first, "must be a block name, a list of them, or {\"block\": ..., \"rot\": ...}"); continue; }
            if (e.blocks.empty()) { Err("palette." + kv.first, "is empty"); continue; }
            palette[kv.first] = e;
        }
        return true;
    }
};

} // namespace

// =============================================================================
// NameBook
// =============================================================================
void NameBook::Add(const std::string& raw) {
    bool wasState = false;
    const std::string n = PlainName(raw, wasState);
    if (n.empty() || names_.count(n)) return;
    names_.emplace(n, 1);
    lower_.emplace(Lower(n), n);
    list_.push_back(n);
}

std::string NameBook::CaseFix(const std::string& name) const {
    auto it = lower_.find(Lower(name));
    return it == lower_.end() ? std::string() : it->second;
}

std::vector<std::string> NameBook::Suggest(const std::string& name, size_t max) const {
    std::vector<std::pair<double, const std::string*>> scored;
    const std::vector<std::string> want = Tokens(name);
    const std::string lname = Lower(name);
    for (const std::string& cand : list_) {
        const std::vector<std::string> have = Tokens(cand);
        int shared = 0;
        for (const std::string& t : want)
            if (std::find(have.begin(), have.end(), t) != have.end()) ++shared;
        if (shared == 0 && want.size() > 1) continue;
        const int dist = EditDistance(lname, Lower(cand));
        const double score = shared * 3.0 - dist * 0.5 - (double)((std::max)(have.size(), want.size()) - (size_t)shared) * 0.5;
        scored.emplace_back(score, &cand);
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return *a.second < *b.second;
    });
    std::vector<std::string> out;
    for (size_t i = 0; i < scored.size() && out.size() < max; ++i) out.push_back(*scored[i].second);
    return out;
}

// =============================================================================
// Rotation helpers
// =============================================================================
int StairRotation(char facing, bool upsideDown) {
    const char* table = upsideDown ? kInverted : kUpright;
    for (int i = 0; i < 4; ++i) if (table[i] == facing) return upsideDown ? 8 + i : i;
    return 0;
}

std::string RotationHint(const std::string& blockName, int rotation, int buildTurns) {
    bool wasState = false;
    const std::string n = PlainName(blockName, wasState);
    const int turns = ((buildTurns % 4) + 4) % 4;
    auto dirWord = [&](char f) {
        const int idx = RingIndex(f);
        return std::string(kRingName[(idx + turns) & 3]);
    };
    if (IsStairLike(n)) {
        bool inv = false, ok = false;
        const char f = FacingOfRotation(rotation, inv, ok);
        if (!ok) return "";
        return std::string(inv ? "upside down, " : "") + "tall side toward the " + dirWord(f);
    }
    if (IsBeamLike(n)) {
        const int r = rotation < 0 ? 0 : rotation;
        if (r == 0) return "standing upright";
        if (r >= 4 && r <= 7) {
            const bool alongZ = (r == 4 || r == 6);
            const bool northSouth = (alongZ != (turns & 1));
            return northSouth ? "lying north-south" : "lying east-west";
        }
        return "";
    }
    if (IsOpeningLike(n)) {
        const int r = rotation < 0 ? 0 : rotation;
        if (r > 3) return "";
        const bool wallAlongX = (r % 2 == 0);
        const bool eastWest = (wallAlongX != (turns & 1));
        return eastWest ? "in an east-west wall" : "in a north-south wall";
    }
    return "";
}

// =============================================================================
// Reading the reply, running it, writing the file
// =============================================================================
std::string ExtractJson(const std::string& reply) {
    // Candidates: every balanced {...} that is not inside an earlier one. The one
    // that contains "ops" is the script; a reply that also shows an example
    // fragment first would otherwise win.
    std::vector<std::string> found;
    size_t i = 0;
    while (i < reply.size()) {
        if (reply[i] != '{') { ++i; continue; }
        int depth = 0;
        bool inStr = false;
        size_t j = i;
        for (; j < reply.size(); ++j) {
            const char c = reply[j];
            if (inStr) {
                if (c == '\\') ++j;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') inStr = true;
            else if (c == '{') ++depth;
            else if (c == '}') { if (--depth == 0) break; }
        }
        if (depth == 0 && j < reply.size()) {
            found.push_back(reply.substr(i, j - i + 1));
            i = j + 1;
        }
        else break;   // unbalanced: whatever follows is inside it
    }
    for (const std::string& f : found) if (f.find("\"ops\"") != std::string::npos) return f;
    return found.empty() ? std::string() : found.front();
}

bool Expand(const std::string& scriptText, const NameBook& book, const Options& opt, Result& out) {
    out = Result{};
    std::string text = scriptText;
    {
        const std::string json = ExtractJson(text);
        if (!json.empty()) text = json;
    }
    JVal root;
    std::string perr;
    {
        JParser parser(text);
        if (!parser.Parse(root, perr)) {
            out.errors.push_back("The script is not valid JSON - " + perr);
            return false;
        }
    }
    if (root.t != JVal::T::Obj) { out.errors.push_back("The script must be one JSON object with an \"ops\" list."); return false; }

    Engine e(book, opt, out);
    if (const JVal* nm = root.Get("name"); nm && nm->t == JVal::T::Str) out.name = nm->s;
    e.LoadPalette(root);
    const JVal* ops = root.Get("ops");
    if (!ops) { out.errors.push_back("The script has no \"ops\" list."); return false; }
    e.RunOps(*ops, Offset{}, 0, "op");

    // ---- collect ------------------------------------------------------------
    std::unordered_map<int, size_t> counts;
    out.blocks.reserve(e.cells.size());
    for (const auto& kv : e.cells) {
        const uint64_t k = kv.first;
        Block b;
        b.x = (int32_t)(k & 0x1FFFFF) - (1 << 20);
        b.y = (int32_t)((k >> 21) & 0x1FFFFF) - (1 << 20);
        b.z = (int32_t)((k >> 42) & 0x1FFFFF) - (1 << 20);
        b.name = e.names[(size_t)kv.second.name];
        // Rotation 0 is the default; only write what says something.
        b.rotation = kv.second.rot > 0 ? kv.second.rot : -1;
        out.blocks.push_back(std::move(b));
        ++counts[kv.second.name];
    }
    std::sort(out.blocks.begin(), out.blocks.end(), [](const Block& a, const Block& b) {
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        return a.x < b.x;
    });
    if (!out.blocks.empty()) {
        out.minX = out.maxX = out.blocks[0].x;
        out.minY = out.maxY = out.blocks[0].y;
        out.minZ = out.maxZ = out.blocks[0].z;
        for (const Block& b : out.blocks) {
            out.minX = (std::min)(out.minX, b.x); out.maxX = (std::max)(out.maxX, b.x);
            out.minY = (std::min)(out.minY, b.y); out.maxY = (std::max)(out.maxY, b.y);
            out.minZ = (std::min)(out.minZ, b.z); out.maxZ = (std::max)(out.maxZ, b.z);
        }
    }
    for (const auto& kv : counts) out.materials.emplace_back(e.names[(size_t)kv.first], kv.second);
    std::sort(out.materials.begin(), out.materials.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });

    // ---- check names --------------------------------------------------------
    out.checkedNames = !book.Empty();
    if (out.checkedNames) {
        for (const auto& m : out.materials) {
            if (book.Known(m.first)) continue;
            Unknown u;
            u.name = m.first;
            u.count = m.second;
            u.suggestions = book.Suggest(m.first, 3);
            out.unknown.push_back(std::move(u));
        }
    }
    else {
        out.warnings.push_back("The game's block list is not loaded, so block names were not checked.");
    }

    // ---- things that look wrong but are not errors ---------------------------
    if (e.stateStripped)
        out.warnings.push_back(std::to_string(e.stateStripped) + " state-variant name(s) were reduced to the plain block; the game picks the state.");
    {
        size_t doorsBlocked = 0;
        std::string example;
        for (const Block& b : out.blocks) {
            if (!(Contains(b.name, "_Door") && !Contains(b.name, "Trap"))) continue;
            if (e.At(b.x, b.y + 1, b.z)) {
                ++doorsBlocked;
                if (example.empty()) example = "(" + std::to_string(b.x) + ", " + std::to_string(b.y + 1) + ", " + std::to_string(b.z) + ")";
            }
        }
        if (doorsBlocked)
            out.warnings.push_back(std::to_string(doorsBlocked) + " door(s) have a block in the cell above, e.g. at " + example +
                                   ". Doors are two tall: place one block and leave the cell above it empty.");
    }
    if (out.blocks.size() > 40000)
        out.warnings.push_back("That is " + std::to_string(out.blocks.size()) +
                               " blocks. The guide only outlines the nearest few thousand at a time, so it works, but it is a very large build.");
    if (out.blocks.empty() && out.errors.empty()) out.errors.push_back("The script ran but produced no blocks.");
    return true;
}

std::string ToPrefabJson(const Result& r) {
    std::string json;
    json.reserve(r.blocks.size() * 64 + 256);
    json += "{\n  \"version\": 8,\n  \"blockIdVersion\": 10,\n";
    json += "  \"anchorX\": 0,\n  \"anchorY\": 0,\n  \"anchorZ\": 0,\n";
    json += "  \"blocks\": [\n";
    for (size_t i = 0; i < r.blocks.size(); ++i) {
        const Block& b = r.blocks[i];
        char head[128];
        snprintf(head, sizeof(head), "    { \"x\": %d, \"y\": %d, \"z\": %d, \"name\": \"", b.x, b.y, b.z);
        json += head;
        for (char c : b.name) {
            if (c == '"' || c == '\\') json += '\\';
            json += c;
        }
        json += "\"";
        if (b.rotation > 0) {
            char rot[40];
            snprintf(rot, sizeof(rot), ", \"rotation\": %d", b.rotation);
            json += rot;
        }
        json += (i + 1 < r.blocks.size()) ? " },\n" : " }\n";
    }
    json += "  ],\n  \"fluids\": []\n}\n";
    return json;
}

std::string Summary(const Result& r) {
    std::string s;
    char buf[256];
    snprintf(buf, sizeof(buf), "%zu blocks, %d wide x %d tall x %d deep, %zu materials",
        r.blocks.size(), r.SizeX(), r.SizeY(), r.SizeZ(), r.materials.size());
    s += buf;
    for (const auto& m : r.materials) {
        snprintf(buf, sizeof(buf), "\n%7zu  %s", m.second, m.first.c_str());
        s += buf;
    }
    return s;
}

std::string FixRequest(const Result& r) {
    std::string s = "Your build script has problems. Fix every one and send the COMPLETE corrected script again "
                    "(one JSON object, nothing else). Keep the design unless a fix requires changing it.\n";
    if (!r.errors.empty()) {
        s += "\nErrors:\n";
        for (const std::string& e : r.errors) s += "- " + e + "\n";
    }
    if (!r.unknown.empty()) {
        s += "\nBlock names that do not exist in the game (use ONLY names from the block list I gave you):\n";
        for (const Unknown& u : r.unknown) {
            s += "- \"" + u.name + "\" (used " + std::to_string(u.count) + " times)";
            if (!u.suggestions.empty()) {
                s += " - closest real names: ";
                for (size_t i = 0; i < u.suggestions.size(); ++i) s += (i ? ", " : "") + u.suggestions[i];
            }
            s += "\n";
        }
    }
    if (!r.warnings.empty()) {
        s += "\nAlso worth fixing:\n";
        for (const std::string& w : r.warnings) s += "- " + w + "\n";
    }
    return s;
}

std::string Slug(const std::string& name) {
    std::string out;
    bool lastUnderscore = true;
    for (unsigned char c : name) {
        if (isalnum(c)) { out += (char)tolower(c); lastUnderscore = false; }
        else if (!lastUnderscore) { out += '_'; lastUnderscore = true; }
        if (out.size() >= 48) break;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "ai_build" : out;
}

} // namespace BuildScript
