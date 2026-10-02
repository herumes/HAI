#include "pch.h"
#include "chatprefix.hpp"

#include <cstring>

namespace ChatPrefix {
namespace {

// =============================================================================
// UTF-8 and character classes
// =============================================================================
// A stray or truncated byte decodes as U+FFFD with length 1, which is in no class
// below, so bad input can never look like a separator.
uint32_t CpAt(const std::string& s, size_t i, size_t& len) {
    const unsigned char c = (unsigned char)s[i];
    len = 1;
    if (c < 0x80) return c;
    const size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (n == 1 || i + n > s.size()) return 0xFFFD;
    uint32_t cp = c & (0xFFu >> (n + 1));
    for (size_t k = 1; k < n; ++k) {
        const unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
    }
    len = n;
    return cp;
}

// Start of the character that ends at byte `i` (i > lo).
size_t PrevStart(const std::string& s, size_t i, size_t lo) {
    size_t j = i - 1;
    while (j > lo && ((unsigned char)s[j] & 0xC0) == 0x80) --j;
    return j;
}

bool IsSpaceCp(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0xA0 || cp == 0x3000 || cp == 0x202F ||
           (cp >= 0x2000 && cp <= 0x200A);
}

bool IsBracketCp(uint32_t cp) {
    return cp == '[' || cp == ']' || cp == '(' || cp == ')' || cp == '{' || cp == '}' || cp == '<' || cp == '>';
}

// Symbols a server uses between a title and the name. ASCII punctuation counts
// too, but only as a token of its own (see the title check), because '-' and '_'
// turn up inside names.
bool IsSepCp(uint32_t cp) {
    if (cp < 0x80)
        return ispunct((int)cp) && !IsBracketCp(cp) && cp != ':';
    return (cp >= 0xA1 && cp <= 0xBF && cp != 0xAA && cp != 0xB5 && cp != 0xBA) ||
           (cp >= 0x2010 && cp <= 0x2064) ||      // dashes, bullets, quotes
           (cp >= 0x2190 && cp <= 0x2BFF) ||      // arrows, boxes, stars, dingbats
           (cp >= 0x2E00 && cp <= 0x2E7F) ||      // supplemental punctuation (the long dashes)
           (cp >= 0x3001 && cp <= 0x303F) ||
           (cp >= 0xE000 && cp <= 0xF8FF) ||      // private use: icon fonts
           (cp >= 0xFE30 && cp <= 0xFE6F) ||
           (cp >= 0xFF01 && cp <= 0xFF0F) ||
           (cp >= 0x1F000 && cp <= 0x1FAFF);      // emoji
}

// Non-ASCII separators end a name outright ("Warden ⸺Name"); ASCII ones can't.
bool IsWideSepCp(uint32_t cp) { return cp >= 0x80 && IsSepCp(cp); }

size_t SkipSpace(const std::string& s, size_t i, size_t lim) {
    while (i < lim) {
        size_t len;
        if (!IsSpaceCp(CpAt(s, i, len))) break;
        i += len;
    }
    return i;
}

// `hi` moved back over trailing whitespace, not past `lo`.
size_t TrimEnd(const std::string& s, size_t lo, size_t hi) {
    while (hi > lo) {
        const size_t b = PrevStart(s, hi, lo);
        size_t len;
        if (!IsSpaceCp(CpAt(s, b, len))) break;
        hi = b;
    }
    return hi;
}

bool SpaceBefore(const std::string& s, size_t i, size_t lo) {
    if (i <= lo) return true;
    size_t len;
    return IsSpaceCp(CpAt(s, PrevStart(s, i, lo), len));
}

// Same choice the rest of the app makes for "where does the speaker end".
size_t ChatColon(const std::string& s) {
    size_t p = s.find(": ");
    if (p != std::string::npos) return p;
    p = s.find(':');
    if (p != std::string::npos) return p;
    return s.find("\xEF\xBC\x9A");
}

char CloseFor(char open) {
    switch (open) {
    case '[': return ']';
    case '(': return ')';
    case '{': return '}';
    case '<': return '>';
    default: return 0;
    }
}

std::string LowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

std::string Trimmed(const std::string& s, size_t b, size_t e) {
    b = SkipSpace(s, b, e);
    e = TrimEnd(s, b, e);
    return s.substr(b, e - b);
}

// =============================================================================
// Parsing
// =============================================================================
struct Part { size_t b, e; int kind; };   // trimmed range inside a level tag; 0 prestige, 1 level

struct Tag {
    size_t open = 0, wsEnd = 0;           // text[open, wsEnd): brackets, contents and the space after
    size_t innerB = 0, innerE = 0;
    bool level = false;                   // "P. 69 · Lvl. 7415"
    bool symbol = false;                  // "[!]": no letters or digits, never touched
    std::vector<Part> parts;              // level tags only
};

struct Parsed {
    std::vector<Tag> tags;
    bool hasTitle = false;
    size_t titleB = 0, nameB = 0, nameE = 0; // text[titleB, nameB) is "Warden — "
};

// "P. 69", "Lvl. 7415", "Level 12", "Lv12", "Lvl 7,415", "P 3k" -> 0 prestige, 1 level, -1 neither.
int LevelPartKind(const std::string& t) {
    size_t i = 0;
    std::string w;
    while (i < t.size() && isalpha((unsigned char)t[i])) w += (char)tolower((unsigned char)t[i++]);
    if (w.empty()) return -1;
    if (i < t.size() && t[i] == '.') ++i;
    while (i < t.size() && t[i] == ' ') ++i;
    const size_t d = i;
    if (d >= t.size() || !isdigit((unsigned char)t[d])) return -1;
    while (i < t.size() && (isdigit((unsigned char)t[i]) || t[i] == ',' || t[i] == '.')) ++i;
    if (i < t.size() && strchr("kKmMbB", t[i])) ++i;
    if (i != t.size()) return -1;
    if (w == "p" || w == "pr" || w == "pres" || w == "prest" || w == "prestige") return 0;
    if (w == "l" || w == "lv" || w == "lvl" || w == "level") return 1;
    return -1;
}

bool IsLevelJoiner(uint32_t cp) {
    return cp == '|' || cp == '/' || cp == 0xB7 || cp == 0x2022 || cp == 0x2219 || cp == 0x22C5 || cp == 0x30FB;
}

bool HasWordChar(const std::string& s, size_t b, size_t e) {
    for (size_t i = b; i < e;) {
        size_t len;
        const uint32_t cp = CpAt(s, i, len);
        if (cp < 0x80 ? isalnum((int)cp) != 0 : !IsSepCp(cp) && !IsSpaceCp(cp)) return true;
        i += len;
    }
    return false;
}

void ClassifyTag(const std::string& s, Tag& t) {
    t.symbol = !HasWordChar(s, t.innerB, t.innerE);
    if (t.symbol) return;

    std::vector<Part> parts;
    size_t start = t.innerB;
    auto flush = [&](size_t end) {
        const size_t b = SkipSpace(s, start, end);
        const size_t e = TrimEnd(s, b, end);
        parts.push_back({ b, e, e > b ? LevelPartKind(s.substr(b, e - b)) : -1 });
    };
    for (size_t i = t.innerB; i < t.innerE;) {
        size_t len;
        const uint32_t cp = CpAt(s, i, len);
        if (IsLevelJoiner(cp)) { flush(i); start = i + len; }
        i += len;
    }
    flush(t.innerE);

    for (const Part& p : parts) if (p.kind < 0) return;   // something that isn't a level or prestige number
    t.level = !parts.empty();
    if (t.level) t.parts = std::move(parts);
}

bool Parse(const std::string& s, Parsed& out) {
    out = Parsed{};
    const size_t colon = ChatColon(s);
    if (colon == std::string::npos || colon == 0 || colon > 200) return false;

    size_t pos = SkipSpace(s, 0, colon);

    // Leading [tags].
    while (pos < colon) {
        const char close = CloseFor(s[pos]);
        if (!close) break;
        const size_t e = s.find(close, pos + 1);
        if (e == std::string::npos || e >= colon || e - pos > 64) break;
        Tag t;
        t.open = pos;
        t.innerB = pos + 1;
        t.innerE = e;
        ClassifyTag(s, t);
        t.wsEnd = SkipSpace(s, e + 1, colon);
        pos = t.wsEnd;
        out.tags.push_back(std::move(t));
        if (out.tags.size() > 6) return false;
    }

    // The name is the last run of name characters before the colon.
    const size_t end = TrimEnd(s, pos, colon);
    if (end <= pos) return false;
    size_t nb = end;
    while (nb > pos) {
        const size_t b = PrevStart(s, nb, pos);
        size_t len;
        const uint32_t cp = CpAt(s, b, len);
        if (IsSpaceCp(cp) || IsBracketCp(cp) || IsWideSepCp(cp)) break;
        nb = b;
    }
    if (nb == end || end - nb > 40 || !HasWordChar(s, nb, end)) return false;
    out.nameB = nb;
    out.nameE = end;

    // Anything between the tags and the name has to be a title ending in a separator.
    const size_t tb = TrimEnd(s, pos, nb);
    if (tb > pos) {
        size_t sb = tb;
        int n = 0;
        bool wide = false;
        while (sb > pos && n < 4) {
            const size_t b = PrevStart(s, sb, pos);
            size_t len;
            const uint32_t cp = CpAt(s, b, len);
            if (!IsSepCp(cp)) break;
            wide = wide || cp >= 0x80;
            sb = b;
            ++n;
        }
        if (n == 0) return false;
        if (!SpaceBefore(s, sb, pos) && !wide) return false;   // "Warden-" is not a title, "Warden⸺" is

        const size_t te = TrimEnd(s, pos, sb);                 // title text is s[pos, te)
        if (te == pos) return false;
        size_t words = 1;
        for (size_t i = pos; i < te;) {
            size_t len;
            const bool sp = IsSpaceCp(CpAt(s, i, len));
            i += len;
            if (sp && i < te && !IsSpaceCp(CpAt(s, i, len))) ++words;
        }
        const size_t bytes = te - pos;
        // With a level or rank tag in front this is certainly a player line; without
        // one, only a short title is trusted, so "Server restarts soon - Note: x"
        // is not mistaken for a title and a name.
        const bool strong = !out.tags.empty();
        if (bytes > 48 || words > 6) return false;
        if (!strong && (bytes > 24 || words > 2)) return false;
        out.hasTitle = true;
        out.titleB = pos;
    }
    return true;
}

// =============================================================================
// Deciding what goes
// =============================================================================
bool RankListed(const Options& o, const std::string& name) {
    const std::string key = LowerAscii(name);
    for (const std::string& r : o.ranks) if (r == key) return true;
    return false;
}

bool HideRank(const Options& o, const std::string& name) {
    switch (o.rankMode) {
    case 1: return true;
    case 2: return !RankListed(o, name);
    case 3: return RankListed(o, name);
    default: return false;
    }
}

std::string StripEdgePunct(const std::string& in) {
    size_t b = 0, e = in.size();
    while (b < e && (unsigned char)in[b] < 0x80 && ispunct((unsigned char)in[b])) ++b;
    while (e > b && (unsigned char)in[e - 1] < 0x80 && ispunct((unsigned char)in[e - 1])) --e;
    return LowerAscii(in.substr(b, e - b));
}

} // namespace

std::vector<std::string> SplitRankList(const std::string& list) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        std::string t = Trimmed(cur, 0, cur.size());
        cur.clear();
        // "[Titan]" and "Titan" mean the same thing.
        while (!t.empty() && (t.front() == '[' || t.front() == '(' || t.front() == '{' || t.front() == '<')) t.erase(0, 1);
        while (!t.empty() && (t.back() == ']' || t.back() == ')' || t.back() == '}' || t.back() == '>')) t.pop_back();
        t = Trimmed(t, 0, t.size());
        if (!t.empty()) out.push_back(LowerAscii(t));
    };
    for (char c : list) {
        if (c == ',' || c == ';' || c == '\n' || c == '\r') flush();
        else cur += c;
    }
    flush();
    return out;
}

bool HiddenRanges(const std::string& text, const std::string& speaker, const Options& o,
    std::vector<Range>& out) {
    out.clear();
    if (!o.Active() || speaker.empty()) return false;

    Parsed p;
    if (!Parse(text, p)) return false;
    if (p.tags.empty() && !p.hasTitle) return false;

    // The app decided who spoke when the line arrived; if the two disagree this
    // is not a shape we understand, so change nothing.
    if (StripEdgePunct(text.substr(p.nameB, p.nameE - p.nameB)) != StripEdgePunct(speaker))
        return false;

    for (const Tag& t : p.tags) {
        if (t.symbol) continue;
        if (t.level) {
            const size_t n = t.parts.size();
            std::vector<bool> keep(n);
            size_t kept = 0, lastKept = 0;
            for (size_t i = 0; i < n; ++i) {
                keep[i] = t.parts[i].kind == 0 ? o.showPrestige : o.showLevel;
                if (keep[i]) { ++kept; lastKept = i; }
            }
            if (kept == n) continue;
            if (kept == 0) { out.push_back({ t.open, t.wsEnd }); continue; }
            // Some parts stay: drop each removed part with the joiner after it,
            // and everything after the last kept part with the joiner before it.
            for (size_t i = 0; i < lastKept; ++i)
                if (!keep[i]) out.push_back({ t.parts[i].b, t.parts[i + 1].b });
            if (lastKept + 1 < n)
                out.push_back({ t.parts[lastKept].e, t.parts[n - 1].e });
        }
        else if (HideRank(o, Trimmed(text, t.innerB, t.innerE))) {
            out.push_back({ t.open, t.wsEnd });
        }
    }
    if (p.hasTitle && !o.showTitle)
        out.push_back({ p.titleB, p.nameB });

    std::sort(out.begin(), out.end(), [](const Range& a, const Range& b) { return a.b < b.b; });
    return !out.empty();
}

std::string Join(const std::vector<ChatSeg>& segs) {
    std::string s;
    for (const ChatSeg& g : segs) s += g.text;
    return s;
}

bool Simplify(const ChatLine& line, const Options& o, std::vector<ChatSeg>& out) {
    if (!o.Active() || line.local || line.outgoing || line.segs.empty() || line.speaker.empty())
        return false;

    const std::string text = Join(line.segs);
    std::vector<Range> cuts;
    if (!HiddenRanges(text, line.speaker, o, cuts)) return false;

    out.clear();
    size_t segStart = 0, ci = 0;
    for (const ChatSeg& g : line.segs) {
        const size_t segEnd = segStart + g.text.size();
        size_t at = segStart;
        while (at < segEnd) {
            while (ci < cuts.size() && cuts[ci].e <= at) ++ci;
            if (ci < cuts.size() && cuts[ci].b <= at) {          // inside a cut: skip to its end
                at = (std::min)(cuts[ci].e, segEnd);
                continue;
            }
            const size_t upTo = (ci < cuts.size()) ? (std::min)(cuts[ci].b, segEnd) : segEnd;
            const std::string piece = g.text.substr(at - segStart, upTo - at);
            // A cut can leave two pieces of one colour side by side ("[" + "Lvl. "); one run is drawn better.
            if (!out.empty() && memcmp(&out.back().color, &g.color, sizeof(ImVec4)) == 0)
                out.back().text += piece;
            else
                out.push_back({ piece, g.color });
            at = upTo;
        }
        segStart = segEnd;
    }
    return !out.empty();
}

} // namespace ChatPrefix
