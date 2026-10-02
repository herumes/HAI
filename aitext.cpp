// Plain C++ with no pch.h, so tests/aitest can build it on its own (the project builds it without
// the precompiled header).
#include "aitext.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

namespace AiText {
namespace {

// -----------------------------------------------------------------------------
// UTF-8
// -----------------------------------------------------------------------------
// Bad bytes decode as U+FFFD, length 1, so they can never look like a real character.
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

// Longest prefix of `s` that is at most `maxChars` characters, never cutting a character.
size_t PrefixBytes(const std::string& s, size_t maxChars) {
    size_t i = 0, n = 0;
    while (i < s.size() && n < maxChars) {
        size_t len;
        CpAt(s, i, len);
        i += len;
        ++n;
    }
    return i;
}

size_t CountChars(const std::string& s) {
    size_t i = 0, n = 0;
    while (i < s.size()) {
        size_t len;
        CpAt(s, i, len);
        i += len;
        ++n;
    }
    return n;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

bool IsEmojiCp(uint32_t cp) {
    return (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) ||
           (cp >= 0x231A && cp <= 0x23FF) || cp == 0x2B50 || cp == 0x2B55 || cp == 0x2B1B || cp == 0x2B1C ||
           cp == 0xFE0F || cp == 0x200D || cp == 0x20E3 || cp == 0x3030 || cp == 0x303D;
}

bool IsWordByte(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

} // namespace

// =============================================================================
// Variables
// =============================================================================
std::string ExpandVars(const std::string& text, const Vars& vars) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size() && text[i + 1] == '{') {
            out += '{';
            ++i;
            continue;
        }
        if (c == '{') {
            size_t j = i + 1;
            while (j < text.size() && j - i <= 25 && (std::isalnum((unsigned char)text[j]) || text[j] == '_')) ++j;
            if (j < text.size() && text[j] == '}' && j > i + 1) {
                const auto it = vars.find(text.substr(i + 1, j - i - 1));
                if (it != vars.end()) {
                    out += it->second;
                    i = j;
                    continue;
                }
            }
        }
        out += c;
    }
    return out;
}

int EstimateTokens(const std::string& text) {
    size_t ascii = 0, other = 0;
    for (size_t i = 0; i < text.size();) {
        size_t len;
        const uint32_t cp = CpAt(text, i, len);
        if (cp < 0x80) ++ascii; else ++other;
        i += len;
    }
    return (int)((ascii + 3) / 4 + other);
}

// =============================================================================
// Prompt stack
// =============================================================================
std::vector<StackEntry> ParseStack(const std::string& text) {
    std::vector<StackEntry> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = Trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty()) continue;
        StackEntry e;
        if (line[0] == '+' || line[0] == '-') {
            e.enabled = line[0] == '+';
            line = Trim(line.substr(1));
        }
        if (line.empty()) continue;
        e.name = line;
        bool dup = false;
        for (const StackEntry& o : out) if (Lower(o.name) == Lower(e.name)) dup = true;
        if (!dup) out.push_back(std::move(e));
    }
    return out;
}

std::string SerializeStack(const std::vector<StackEntry>& stack) {
    std::string s;
    for (const StackEntry& e : stack) {
        s += e.enabled ? '+' : '-';
        s += e.name;
        s += '\n';
    }
    return s;
}

std::vector<StackEntry> ReconcileStack(const std::vector<StackEntry>& stack,
    const std::vector<std::string>& available, bool newOnesEnabled) {
    std::vector<StackEntry> out;
    for (const StackEntry& e : stack) {
        for (const std::string& a : available) {
            if (Lower(a) == Lower(e.name)) {
                out.push_back({ a, e.enabled });
                break;
            }
        }
    }
    for (const std::string& a : available) {
        bool have = false;
        for (const StackEntry& e : out) if (Lower(e.name) == Lower(a)) have = true;
        if (!have) out.push_back({ a, newOnesEnabled });
    }
    return out;
}

std::string SafePresetName(const std::string& in) {
    std::string s;
    for (unsigned char c : in) {
        if (c < 0x20 || std::strchr("<>:\"/\\|?*", c)) continue;
        s += (char)c;
    }
    // Trim spaces and dots at both ends (Windows drops trailing ones, a leading dot hides the file).
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '.')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '.')) --e;
    s = s.substr(b, e - b);
    if (CountChars(s) > 48) s.resize(PrefixBytes(s, 48));
    // Names Windows reserves for devices.
    const std::string l = Lower(s);
    static const char* reserved[] = { "con", "prn", "aux", "nul", "com1", "com2", "com3", "lpt1", "lpt2", "lpt3" };
    for (const char* r : reserved) if (l == r) return s + "_";
    return s;
}

// =============================================================================
// Chat context
// =============================================================================
namespace {
std::string FormatClockUtc(long long unixMs) {
    if (unixMs <= 0) return "";
    const __time64_t t = (__time64_t)(unixMs / 1000);
    struct tm tmv {};
    if (_gmtime64_s(&tmv, &t) != 0) return "";
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", &tmv);
    return buf;
}

std::string CapChars(const std::string& s, int maxChars) {
    if (maxChars <= 0 || CountChars(s) <= (size_t)maxChars) return s;
    return s.substr(0, PrefixBytes(s, (size_t)maxChars)) + "...";
}
} // namespace

std::vector<CtxLine> BuildChatLines(const std::vector<CtxEntry>& entries, const CtxOptions& o) {
    std::vector<CtxLine> rev;
    const long long maxAgeMs = o.maxAgeMin > 0 ? (long long)o.maxAgeMin * 60000LL : 0;
    for (int i = (int)entries.size() - 1; i >= 0 && (int)rev.size() < o.maxLines; --i) {
        const CtxEntry& e = entries[(size_t)i];
        if (e.hidden) continue;
        if (e.fromBot && !o.includeBot) continue;
        if (e.speaker.empty() && !o.includeSystem) continue;
        if (maxAgeMs > 0 && e.unixMs > 0 && o.nowMs > 0 && o.nowMs - e.unixMs > maxAgeMs) continue;

        std::string text;
        if (e.speaker.empty()) {
            text = e.plain;
        }
        else {
            const Vars v = { { "speaker", e.speaker }, { "body", e.body }, { "plain", e.plain },
                             { "time", o.timeFn ? o.timeFn(e.unixMs) : FormatClockUtc(e.unixMs) } };
            text = ExpandVars(o.lineFormat.empty() ? std::string("{speaker}: {body}") : o.lineFormat, v);
        }
        text = CapChars(text, o.maxLineChars);
        if (Trim(text).empty()) continue;
        rev.push_back({ i, std::move(text) });
    }
    std::reverse(rev.begin(), rev.end());
    return rev;
}

std::string JoinLines(const std::vector<CtxLine>& lines) {
    std::string s;
    for (const CtxLine& l : lines) { s += l.text; s += '\n'; }
    return s;
}

// =============================================================================
// Reply clean-up
// =============================================================================
std::vector<std::string> SplitList(const std::string& list) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        const std::string t = Trim(cur);
        cur.clear();
        if (!t.empty()) out.push_back(t);
    };
    for (char c : list) {
        if (c == ',' || c == ';' || c == '\n' || c == '\r') flush();
        else cur += c;
    }
    flush();
    return out;
}

namespace {

std::string StripMarkdown(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    // Line starts: "# heading", "- item", "* item", "1. item" keep their words only.
    bool lineStart = true;
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (lineStart) {
            size_t j = i;
            while (j < in.size() && (in[j] == ' ' || in[j] == '\t')) ++j;
            if (j < in.size() && in[j] == '#') {
                while (j < in.size() && in[j] == '#') ++j;
                while (j < in.size() && in[j] == ' ') ++j;
                i = j - 1;
                lineStart = false;
                continue;
            }
            if (j + 1 < in.size() && (in[j] == '-' || in[j] == '*' || in[j] == '+') && in[j + 1] == ' ') {
                i = j + 1;
                lineStart = false;
                continue;
            }
        }
        lineStart = c == '\n';
        // [text](url) -> text (url)
        if (c == '[') {
            const size_t close = in.find(']', i + 1);
            if (close != std::string::npos && close + 1 < in.size() && in[close + 1] == '(') {
                const size_t end = in.find(')', close + 2);
                if (end != std::string::npos && close > i + 1) {
                    s += in.substr(i + 1, close - i - 1);
                    s += " (" + in.substr(close + 2, end - close - 2) + ")";
                    i = end;
                    continue;
                }
            }
        }
        if (c == '`') continue;
        if ((c == '*' || c == '~') && i + 1 < in.size() && in[i + 1] == c) { ++i; continue; }   // ** ~~  (not __: player names have them)
        if (c == '*') {
            // *italic*: drop it unless it sits between digits (2*3) or spaces (a * b).
            const bool prevDigit = !s.empty() && std::isdigit((unsigned char)s.back());
            const bool nextDigit = i + 1 < in.size() && std::isdigit((unsigned char)in[i + 1]);
            const bool prevSpace = s.empty() || s.back() == ' ';
            const bool nextSpace = i + 1 >= in.size() || in[i + 1] == ' ';
            if ((prevDigit && nextDigit) || (prevSpace && nextSpace)) { s += c; continue; }
            continue;
        }
        s += c;
    }
    return s;
}

std::string StripEmoji(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        size_t len;
        const uint32_t cp = CpAt(in, i, len);
        if (!IsEmojiCp(cp)) s.append(in, i, len);
        i += len;
    }
    return s;
}

// Replaces whole-word hits of any listed word; returns how many.
int MaskWords(std::string& text, const std::vector<std::string>& words, bool doMask) {
    int hits = 0;
    const std::string lower = Lower(text);
    std::vector<std::pair<size_t, size_t>> spans;
    for (const std::string& w : words) {
        const std::string lw = Lower(w);
        if (lw.empty()) continue;
        size_t pos = 0;
        while ((pos = lower.find(lw, pos)) != std::string::npos) {
            const bool leftOk = pos == 0 || !IsWordByte((unsigned char)lower[pos - 1]);
            const size_t end = pos + lw.size();
            const bool rightOk = end >= lower.size() || !IsWordByte((unsigned char)lower[end]);
            if (leftOk && rightOk) { spans.push_back({ pos, end }); ++hits; }
            pos = end;
        }
    }
    if (doMask)
        for (const auto& sp : spans)
            for (size_t i = sp.first; i < sp.second; ++i) text[i] = '*';
    return hits;
}

std::string CollapseSpaces(const std::string& in) {
    std::string s;
    bool space = false;
    for (char c : in) {
        if (c == ' ' || c == '\t') {
            if (!space && !s.empty()) s += ' ';
            space = true;
        }
        else {
            s += c;
            space = c == '\n';
        }
    }
    return Trim(s);
}

std::string CutTo(const std::string& s, int maxChars) {
    if (maxChars <= 0 || CountChars(s) <= (size_t)maxChars) return s;
    std::string cut = s.substr(0, PrefixBytes(s, (size_t)maxChars));
    // Prefer ending on a sentence, if that keeps most of what was allowed.
    const size_t sentence = cut.find_last_of(".!?");
    if (sentence != std::string::npos && sentence + 1 >= cut.size() * 6 / 10)
        return Trim(cut.substr(0, sentence + 1));
    const size_t space = cut.find_last_of(' ');
    if (space != std::string::npos && space >= cut.size() / 2) cut.resize(space);
    return Trim(cut);
}

} // namespace

PostResult PostProcess(const std::string& in, const PostOptions& o) {
    PostResult r;
    std::string t = in;
    auto note = [&](const char* what) { r.notes.push_back(what); };

    if (o.stripMarkdown) {
        const std::string n = StripMarkdown(t);
        if (n != t) { t = n; note("removed markdown"); }
    }
    if (o.stripEmoji) {
        const std::string n = StripEmoji(t);
        if (n != t) { t = n; note("removed emoji"); }
    }
    if (!o.blocklist.empty()) {
        const std::vector<std::string> words = SplitList(o.blocklist);
        std::string masked = t;
        const int hits = MaskWords(masked, words, o.blockAction == 0);
        if (hits > 0) {
            if (o.blockAction == 0) { t = masked; note("masked a blocked word"); }
            else { r.blocked = true; note("contains a blocked word"); }
        }
    }
    if (o.lowercase) {
        const std::string n = Lower(t);
        if (n != t) { t = n; note("lower-cased"); }
    }
    {
        const std::string n = CollapseSpaces(t);
        if (n != t) t = n;
    }
    if (o.maxChars > 0) {
        const std::string n = CutTo(t, o.maxChars);
        if (n != t) { t = n; note("shortened"); }
    }
    if (!o.prefix.empty() || !o.suffix.empty()) {
        if (!t.empty()) t = o.prefix + t + o.suffix;
    }
    r.text = t;
    return r;
}

// =============================================================================
// Tools
// =============================================================================
ToolRules ParseToolRules(const std::string& s) {
    ToolRules r;
    for (const std::string& item : SplitList(s)) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = Lower(Trim(item.substr(0, eq)));
        const std::string mode = Lower(Trim(item.substr(eq + 1)));
        if (name.empty()) continue;
        if (mode == "off") r.byName[name] = ToolMode::Off;
        else if (mode == "ask") r.byName[name] = ToolMode::Ask;
        else if (mode == "auto") r.byName[name] = ToolMode::Auto;
    }
    return r;
}

std::string SerializeToolRules(const ToolRules& r) {
    std::string s;
    for (const auto& kv : r.byName) {
        if (!s.empty()) s += ';';
        s += kv.first;
        s += kv.second == ToolMode::Off ? "=off" : kv.second == ToolMode::Ask ? "=ask" : "=auto";
    }
    return s;
}

ToolMode ResolveToolMode(const ToolRules& r, int policy, const std::string& name, bool sideEffect) {
    const auto it = r.byName.find(Lower(name));
    if (it != r.byName.end()) return it->second;
    if (policy >= 2) return ToolMode::Ask;
    if (policy == 1 && sideEffect) return ToolMode::Ask;
    return ToolMode::Auto;
}

// =============================================================================
// Wake words
// =============================================================================
bool WakeAtStart(const std::string& message, const std::vector<std::string>& wakes) {
    const std::string m = Lower(message);
    size_t i = 0;
    auto skipJunk = [&]() {
        while (i < m.size() && (m[i] == ' ' || m[i] == '\t' || m[i] == '@' || m[i] == '>' || m[i] == '"' ||
                                m[i] == '\'' || m[i] == '-' || m[i] == '.' || m[i] == ',' || m[i] == '!'))
            ++i;
    };
    skipJunk();
    auto matchAt = [&](size_t pos, const std::string& w) {
        if (w.empty() || m.compare(pos, w.size(), w) != 0) return false;
        const size_t end = pos + w.size();
        return end >= m.size() || !IsWordByte((unsigned char)m[end]);
    };
    for (const std::string& raw : wakes) {
        const std::string w = Lower(Trim(raw));
        if (matchAt(i, w)) return true;
    }
    // "hey hyai ...": one greeting word first.
    static const char* greetings[] = { "hey", "hi", "hello", "yo", "ok", "okay", "oi", "sup", "ay", "ayo" };
    for (const char* g : greetings) {
        const size_t n = std::strlen(g);
        if (m.compare(i, n, g) != 0) continue;
        if (i + n < m.size() && IsWordByte((unsigned char)m[i + n])) continue;
        size_t save = i;
        i += n;
        skipJunk();
        for (const std::string& raw : wakes)
            if (matchAt(i, Lower(Trim(raw)))) return true;
        i = save;
    }
    return false;
}

// =============================================================================
// Rate limiting
// =============================================================================
bool RateLimiter::Allow(long long nowMs, const std::string& who, int globalCooldownSec, int playerCooldownSec,
    int perMinute, std::string* why) const {
    auto deny = [&](const std::string& s) { if (why) *why = s; return false; };
    if (globalCooldownSec > 0 && lastAny_ > 0 && nowMs - lastAny_ < (long long)globalCooldownSec * 1000)
        return deny("cooldown: " + std::to_string((globalCooldownSec * 1000LL - (nowMs - lastAny_) + 999) / 1000) + "s left");
    if (playerCooldownSec > 0) {
        const auto it = lastByWho_.find(Lower(who));
        if (it != lastByWho_.end() && nowMs - it->second < (long long)playerCooldownSec * 1000)
            return deny(who + " must wait " + std::to_string((playerCooldownSec * 1000LL - (nowMs - it->second) + 999) / 1000) + "s");
    }
    if (perMinute > 0) {
        int n = 0;
        for (long long t : recent_) if (nowMs - t < 60000) ++n;
        if (n >= perMinute) return deny("limit of " + std::to_string(perMinute) + " per minute reached");
    }
    return true;
}

void RateLimiter::Record(long long nowMs, const std::string& who) {
    lastAny_ = nowMs;
    lastByWho_[Lower(who)] = nowMs;
    recent_.push_back(nowMs);
    while (!recent_.empty() && nowMs - recent_.front() >= 60000) recent_.pop_front();
    if (lastByWho_.size() > 400) {           // stale names don't need to live forever
        for (auto it = lastByWho_.begin(); it != lastByWho_.end();)
            it = nowMs - it->second > 3600000 ? lastByWho_.erase(it) : std::next(it);
    }
}

void RateLimiter::Clear() {
    recent_.clear();
    lastAny_ = 0;
    lastByWho_.clear();
}

} // namespace AiText
