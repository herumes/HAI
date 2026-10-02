// Tests for chatprefix.cpp: which tags of a player line go, and that nothing
// else is ever touched.
#include "pch.h"
#include "chatprefix.hpp"

#include <cstdio>
#include <random>

using namespace ChatPrefix;

static int g_fail = 0, g_pass = 0;

static void Check(bool ok, const std::string& what) {
    if (ok) { ++g_pass; return; }
    ++g_fail;
    std::printf("FAIL: %s\n", what.c_str());
}

static void Eq(const std::string& got, const std::string& want, const std::string& what) {
    if (got == want) { ++g_pass; return; }
    ++g_fail;
    std::printf("FAIL: %s\n   got:  [%s]\n   want: [%s]\n", what.c_str(), got.c_str(), want.c_str());
}

static const ImVec4 kWhite(1, 1, 1, 1);

static ChatLine MakeLine(const std::string& text, const std::string& speaker) {
    ChatLine L;
    L.segs.push_back({ text, kWhite });
    L.plain = text;
    L.speaker = speaker;
    return L;
}

// What the compact chat would show.
static std::string Show(const std::string& text, const std::string& speaker, const Options& o) {
    ChatLine L = MakeLine(text, speaker);
    std::vector<ChatSeg> out;
    return Simplify(L, o, out) ? Join(out) : text;
}

static Options Opts(bool prestige, bool level, int rank, const std::string& list, bool title) {
    Options o;
    o.showPrestige = prestige;
    o.showLevel = level;
    o.rankMode = rank;
    o.ranks = SplitRankList(list);
    o.showTitle = title;
    return o;
}

#define DOT "\xC2\xB7"
#define LONGDASH "\xE2\xB8\xBA"   // U+2E3A
#define EMDASH "\xE2\x80\x94"     // U+2014
#define STAR "\xE2\x98\x85"       // U+2605

int main() {
    const Options all = Opts(true, true, 0, "", true);
    const Options none = Opts(false, false, 1, "", false);

    struct Line { const char* text; const char* speaker; const char* bare; };
    const Line lines[] = {
        { "[P. 69 " DOT " Lvl. 7415] [Head Builder] Beaverplzr: wb death o/", "Beaverplzr", "Beaverplzr: wb death o/" },
        { "[P. 25 " DOT " Lvl. 1] [Head Mod] death_the_kid__: thank you", "death_the_kid", "death_the_kid__: thank you" },
        { "[P. 6 " DOT " Lvl. 841] [Titan] Ro4: u made ur own sun ???", "Ro4", "Ro4: u made ur own sun ???" },
        { "[P. 20 " DOT " Lvl. 6449] [Mythic] RhythmicEnergy: yes but beaverplzr helped me", "RhythmicEnergy",
          "RhythmicEnergy: yes but beaverplzr helped me" },
        { "[P. 130 " DOT " Lvl. 3905] [Divine] Warden " LONGDASH " HitShift_MC: a", "HitShift_MC", "HitShift_MC: a" },
        { "[P. 130 " DOT " Lvl. 3905] [Divine] Warden " EMDASH " HitShift_MC: a", "HitShift_MC", "HitShift_MC: a" },
        { "[Lvl. 12] Newbie: how do i get a pet?", "Newbie", "Newbie: how do i get a pet?" },
        { "[P. 3 " DOT " Lvl. 99] Steve: no rank here", "Steve", "Steve: no rank here" },
    };
    for (const Line& l : lines) {
        Eq(Show(l.text, l.speaker, all), l.text, std::string("all shown: ") + l.text);
        Eq(Show(l.text, l.speaker, none), l.bare, std::string("just names: ") + l.text);
    }

    // Each switch on its own, on the line that has everything.
    const std::string full = "[P. 130 " DOT " Lvl. 3905] [Divine] Warden " LONGDASH " HitShift_MC: a";
    Eq(Show(full, "HitShift_MC", Opts(false, true, 0, "", true)),
       "[Lvl. 3905] [Divine] Warden " LONGDASH " HitShift_MC: a", "prestige off");
    Eq(Show(full, "HitShift_MC", Opts(true, false, 0, "", true)),
       "[P. 130] [Divine] Warden " LONGDASH " HitShift_MC: a", "level off");
    Eq(Show(full, "HitShift_MC", Opts(false, false, 0, "", true)),
       "[Divine] Warden " LONGDASH " HitShift_MC: a", "both parts off drops the whole island tag");
    Eq(Show(full, "HitShift_MC", Opts(true, true, 1, "", true)),
       "[P. 130 " DOT " Lvl. 3905] Warden " LONGDASH " HitShift_MC: a", "rank off");
    Eq(Show(full, "HitShift_MC", Opts(true, true, 0, "", false)),
       "[P. 130 " DOT " Lvl. 3905] [Divine] HitShift_MC: a", "title off");

    // Rank lists.
    const std::string staff = "[P. 25 " DOT " Lvl. 1] [Head Mod] death_the_kid__: hi";
    const std::string fan = "[P. 6 " DOT " Lvl. 841] [Titan] Ro4: hi";
    Eq(Show(staff, "death_the_kid", Opts(true, true, 2, "Head Mod, Head Builder", true)), staff, "except list keeps a listed rank");
    Eq(Show(fan, "Ro4", Opts(true, true, 2, "Head Mod, Head Builder", true)),
       "[P. 6 " DOT " Lvl. 841] Ro4: hi", "except list hides an unlisted rank");
    Eq(Show(fan, "Ro4", Opts(true, true, 2, "", true)), "[P. 6 " DOT " Lvl. 841] Ro4: hi", "empty except list hides every rank");
    Eq(Show(fan, "Ro4", Opts(true, true, 3, "titan", true)), "[P. 6 " DOT " Lvl. 841] Ro4: hi", "only-list hides a listed rank (any case)");
    Eq(Show(staff, "death_the_kid", Opts(true, true, 3, "[Titan]", true)), staff, "only-list keeps an unlisted rank");
    Eq(Show(fan, "Ro4", Opts(true, true, 3, "  [TITAN] ; vip", true)), "[P. 6 " DOT " Lvl. 841] Ro4: hi", "list separators and brackets");
    Eq(Show("[VIP] [Titan] Ro4: hi", "Ro4", Opts(true, true, 3, "vip", true)), "[Titan] Ro4: hi", "two rank tags, one hidden");

    // Lines that are not a player talking stay exactly as they are.
    const char* untouched[][2] = {
        { "[!] No one answered correctly!", "" },
        { "[!] Players sometimes sell items cheaper than the shop. Check /ah to see what's for sale.", "" },
        { "[Pet] feran_burrower #2 leveled up to 319 (Farming). Unspent points: 0", "points" },
        { "[Pet] Rex leveled up: 5", "up" },
        { "Server restarts soon - Note: back in 5", "Note" },
        { "[!] Tip: use /ah", "Tip" },
        { "You whisper to Steve: hi", "Steve" },
        { "[Titan] Ro4: hi", "SomeoneElse" },           // parsed name disagrees with the app's speaker
        { "Steve: hi", "Steve" },
        { "", "" },
        { ": hi", "x" },
        { "[P. 6 " DOT " Lvl. 841] [Titan] : nobody", "x" },
    };
    for (auto& u : untouched) Eq(Show(u[0], u[1], none), u[0], std::string("untouched: ") + u[0]);

    // Titles: trusted after a tag, or when short.
    Eq(Show("Warden " EMDASH " Steve: hi", "Steve", none), "Steve: hi", "short title, no tags");
    Eq(Show("[Titan] Warden of the Deep " EMDASH " Steve: hi", "Steve", none), "Steve: hi", "long title after a tag");
    Eq(Show("Warden of the Deep Seas " EMDASH " Steve: hi", "Steve", none), "Warden of the Deep Seas " EMDASH " Steve: hi",
       "long title without a tag is not trusted");
    Eq(Show("[Titan] Warden" EMDASH "Steve: hi", "Steve", none), "Steve: hi", "attached wide dash");
    Eq(Show("[Titan] Warden - Steve: hi", "Steve", none), "Steve: hi", "spaced hyphen");
    Eq(Show("[Titan] Some-Name: hi", "Some-Name", none), "Some-Name: hi", "hyphen inside a name is not a title");
    Eq(Show("[Titan] Warden-Steve: hi", "Warden-Steve", none), "Warden-Steve: hi", "hyphenated name");

    // Level tag spellings.
    Eq(Show("[Lvl 12] Newbie: hi", "Newbie", Opts(true, false, 0, "", true)), "Newbie: hi", "Lvl 12");
    Eq(Show("[Level 7,415] Newbie: hi", "Newbie", Opts(true, false, 0, "", true)), "Newbie: hi", "Level 7,415");
    Eq(Show("[P.3|Lvl.4] Newbie: hi", "Newbie", Opts(false, true, 0, "", true)), "[Lvl.4] Newbie: hi", "pipe joiner");
    Eq(Show("[Lvl. 12] Newbie: hi", "Newbie", Opts(false, true, 0, "", true)), "[Lvl. 12] Newbie: hi", "level-only tag with prestige off");
    Eq(Show("[VIP2] Newbie: hi", "Newbie", Opts(false, false, 0, "", true)), "[VIP2] Newbie: hi", "VIP2 is a rank, not a level");
    Eq(Show("[VIP2] Newbie: hi", "Newbie", Opts(true, true, 1, "", true)), "Newbie: hi", "VIP2 hidden as a rank");
    Eq(Show("[P. 9 " DOT " Lvl. 8] [" STAR "] Newbie: hi", "Newbie", Opts(true, true, 1, "", true)),
       "[P. 9 " DOT " Lvl. 8] [" STAR "] Newbie: hi", "a star-only tag has no letters, so it is left alone");

    // Names.
    const std::string ivan = "\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD";
    Eq(Show("[Titan] " + ivan + ": hi", ivan, none), ivan + ": hi", "non-latin name");
    Eq(Show("[Titan] Steve: a: b: c", "Steve", none), "Steve: a: b: c", "colons in the message");
    Eq(Show("[Titan]Steve: hi", "Steve", none), "Steve: hi", "no space after the tag");
    Eq(Show("  [Titan]   Steve  : hi", "Steve", none), "  Steve  : hi", "stray spaces");

    // Our own lines are never touched.
    {
        ChatLine L = MakeLine("[P. 6 " DOT " Lvl. 841] [Titan] Ro4: hi", "Ro4");
        std::vector<ChatSeg> out;
        L.local = true;
        Check(!Simplify(L, none, out), "local line untouched");
        L.local = false; L.outgoing = true;
        Check(!Simplify(L, none, out), "outgoing line untouched");
        L.outgoing = false; L.speaker.clear();
        Check(!Simplify(L, none, out), "line without a speaker untouched");
        L.speaker = "Ro4";
        Check(!Simplify(L, all, out), "nothing to hide, nothing changed");
        Check(Simplify(L, none, out), "sanity: the same line does change");
    }

    // Colours of what stays survive, wherever the cuts fall (gradient names, split tags).
    {
        const ImVec4 a(1, 0, 0, 1), b(0, 1, 0, 1), c(0, 0, 1, 1), d(1, 1, 0, 1);
        ChatLine L;
        L.speaker = "HitShift_MC";
        L.segs = { { "[P. 130 " DOT " Lvl. ", a }, { "3905] [Div", b }, { "ine] Warden " LONGDASH " ", c }, { "Hit", d },
                   { "Shift_MC", a }, { ": a", b } };
        std::vector<ChatSeg> out;
        Check(Simplify(L, none, out), "gradient line simplified");
        Eq(Join(out), "HitShift_MC: a", "gradient text");
        const bool ok = out.size() == 3 && out[0].text == "Hit" && out[0].color.y == 1 && out[0].color.x == 1 &&
                        out[1].text == "Shift_MC" && out[1].color.x == 1 && out[1].color.y == 0 &&
                        out[2].text == ": a" && out[2].color.y == 1 && out[2].color.x == 0;
        Check(ok, "gradient colours kept");
        for (const ChatSeg& s : out) Check(!s.text.empty(), "no empty segments");

        // Only the prestige goes: the leftover pieces keep their own colours.
        Check(Simplify(L, Opts(false, true, 0, "", true), out), "prestige-only cut");
        Eq(Join(out), "[Lvl. 3905] [Divine] Warden " LONGDASH " HitShift_MC: a", "prestige cut across two segments");
        Check(out.front().color.x == 1 && out.front().color.y == 0 && out.front().text == "[Lvl. ", "cut keeps the segment's colour");
    }

    // The original line is never modified.
    {
        ChatLine L = MakeLine(full, "HitShift_MC");
        std::vector<ChatSeg> out;
        Simplify(L, none, out);
        Eq(L.segs[0].text, full, "source segments unchanged");
    }

    // Fuzz: whatever the text, the tail from the colon on survives byte for byte, the
    // result only ever loses text, and nothing crashes or loops.
    {
        std::mt19937 rng(12345);
        const char* bits[] = { "[", "]", "(", ")", "<", ">", " ", " ", ":", ": ", "P. 6", "Lvl. 9", DOT, LONGDASH, EMDASH, "-", "_",
                               "Titan", "Warden", "Steve", "x", "\xF0\x9F\x91\x91", "\xC2\xA0", "!", STAR, "1", "k" };
        const int nbits = (int)(sizeof(bits) / sizeof(bits[0]));
        const Options modes[] = { none, Opts(false, true, 1, "", true), Opts(true, false, 2, "titan", false), Opts(true, true, 3, "x", true) };
        int changed = 0;
        bool reported = false;
        for (int it = 0; it < 300000 && !reported; ++it) {
            std::string t;
            const int n = 1 + (int)(rng() % 14);
            for (int i = 0; i < n; ++i) t += bits[rng() % nbits];
            if (rng() % 9 == 0 && !t.empty()) t[rng() % t.size()] = (char)(rng() & 0xFF);   // corrupt a byte
            std::string speaker = "Steve";
            if (rng() % 3 == 0) speaker = "x";
            if (rng() % 5 == 0) speaker.clear();
            for (const Options& o : modes) {
                ChatLine L = MakeLine(t, speaker);
                std::vector<ChatSeg> out;
                if (!Simplify(L, o, out)) continue;
                ++changed;
                const std::string res = Join(out);
                size_t colon = t.find(": ");
                if (colon == std::string::npos) colon = t.find(':');
                if (colon == std::string::npos) colon = t.find("\xEF\xBC\x9A");
                const std::string tail = colon == std::string::npos ? "" : t.substr(colon);
                bool bad = colon == std::string::npos || res.size() < tail.size() ||
                           res.compare(res.size() - tail.size(), tail.size(), tail) != 0 || res.size() >= t.size();
                // Removed parts are whole ranges, so the result is a subsequence of the input.
                size_t j = 0;
                for (char ch : res) { while (j < t.size() && t[j] != ch) ++j; if (j == t.size()) { bad = true; break; } ++j; }
                if (bad) {
                    Check(false, "fuzz: tail lost, nothing removed, or not a subsequence for [" + t + "]");
                    reported = true;
                    break;
                }
            }
        }
        Check(changed > 1000, "fuzz actually exercised the cutter (" + std::to_string(changed) + " lines changed)");
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
