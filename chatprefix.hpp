#pragma once
// Hiding the tags a server puts in front of player names in the compact chat.
//
// A line on the server this was written for looks like
//
//     [P. 69 · Lvl. 7415] [Head Builder] Warden — Beaverplzr: wb death o/
//      ^ island tag         ^ rank tag     ^ title   ^ name
//
// Nothing is stored differently: the log keeps the full line and this works out,
// at paint time, which character ranges of it to leave out. Cutting ranges out of
// the coloured segments (instead of re-parsing colours) keeps every gradient and
// every colour of the parts that stay.
//
// The parser is deliberately strict. A line only counts as "a player said this"
// when the part before the first colon is: optional [tags], an optional title
// that ends in a separator (a dash or a symbol), then one name. Anything else -
// announcements, "Rex leveled up: 5", "[!] Tip: ..." - is left exactly as it was.
#include "pch.h"
#include "chat.hpp"
#include "settings.hpp"
#include <string>
#include <vector>

namespace ChatPrefix {

struct Options {
    bool showPrestige = true;   // the "P. 69" half of [P. 69 · Lvl. 7415]
    bool showLevel = true;      // the "Lvl. 7415" half
    int  rankMode = 0;          // [Titan]: 0 show all, 1 hide all, 2 hide all except `ranks`, 3 hide only `ranks`
    std::vector<std::string> ranks; // lower-case rank names, see SplitRankList
    bool showTitle = true;      // "Warden —"

    bool Active() const { return !showPrestige || !showLevel || rankMode != 0 || !showTitle; }
};

// "VIP, [Titan]; Head Mod" -> { "vip", "titan", "head mod" }
std::vector<std::string> SplitRankList(const std::string& list);

inline Options FromPrefs(const ChatPrefs& p) {
    Options o;
    o.showPrestige = p.compactShowPrestige;
    o.showLevel = p.compactShowLevel;
    o.rankMode = p.compactRankMode;
    o.showTitle = p.compactShowTitle;
    if (o.rankMode >= 2) o.ranks = SplitRankList(p.compactRankList);
    return o;
}

// A byte range of the line's text to leave out.
struct Range { size_t b, e; };

// The pure part, on plain text: which ranges of `text` go. `speaker` is the name
// the app itself extracted for the line; a line whose parsed name disagrees with
// it is left alone. Returns false (and no ranges) when nothing should change.
bool HiddenRanges(const std::string& text, const std::string& speaker, const Options& o,
    std::vector<Range>& out);

// The segments to draw for `line`. Returns false when the line is shown as it is
// (nothing to hide, not a player line, our own line, ...); `out` is then unused.
bool Simplify(const ChatLine& line, const Options& o, std::vector<ChatSeg>& out);

// Plain text of segments, for copy and for the tests.
std::string Join(const std::vector<ChatSeg>& segs);

} // namespace ChatPrefix
