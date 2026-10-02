#include "pch.h"
#include "autoreply.hpp"
#include "common.hpp"
#include "chat.hpp"
#include "settings.hpp"
#include "shared.hpp"
#include "extras.hpp"

#include <regex>
#include <fstream>

namespace {

struct Compiled {
    AutoReplyRule rule;
    std::regex re;
    bool reOk = false;
    long long lastFireMs = -1000000000LL;
};

struct Pending {
    std::string plain, speaker, body;
    bool fromLocal = false;
    bool fromBot = false;
};

std::mutex g_rulesMtx;
std::vector<Compiled> g_rules;
std::atomic<uint64_t> g_generation{ 1 };

std::mutex g_queueMtx;
std::deque<Pending> g_queue;

// Floor between any two auto replies, whatever the rules say, so a burst of
// matching lines (a join wave) can't flood chat.
long long g_lastAnyReplyMs = 0;
const long long kGlobalGapMs = 1500;

const char* kFileHeader = "# HyAI auto replies v1: enabled<TAB>match<TAB>case<TAB>skipown<TAB>cooldown<TAB>pattern<TAB>reply";

std::wstring RulesPath() { return GetThisModuleDirectory() + L"auto_replies.txt"; }

std::string Esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\t') o += "\\t";
        else if (c == '\n') o += "\\n";
        else if (c != '\r') o += c;
    }
    return o;
}

std::string Unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            o += n == 't' ? '\t' : n == 'n' ? '\n' : n;
        }
        else o += s[i];
    }
    return o;
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t t = line.find('\t', start);
        parts.push_back(line.substr(start, t == std::string::npos ? std::string::npos : t - start));
        if (t == std::string::npos) break;
        start = t + 1;
    }
    return parts;
}

void Compile(Compiled& c) {
    c.rule.error.clear();
    c.reOk = false;
    if (c.rule.match != AutoReplyRule::Regex) return;
    try {
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (!c.rule.caseSensitive) flags |= std::regex::icase;
        c.re = std::regex(c.rule.pattern, flags);
        c.reOk = true;
    }
    catch (const std::regex_error& e) {
        c.rule.error = e.what();
    }
}

void SaveLocked() {
    const std::wstring path = RulesPath();
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open()) return;
        f << kFileHeader << "\n";
        for (const Compiled& c : g_rules) {
            const AutoReplyRule& r = c.rule;
            f << (r.enabled ? 1 : 0) << '\t' << r.match << '\t' << (r.caseSensitive ? 1 : 0) << '\t'
              << (r.skipOwn ? 1 : 0) << '\t' << r.cooldownSec << '\t'
              << Esc(r.pattern) << '\t' << Esc(r.reply) << "\n";
        }
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// The three reactions this replaces, as rules. Enabled only if the old toggle
// was on, so upgrading changes nothing.
std::vector<AutoReplyRule> LegacyDefaults() {
    bool wb = false, wel = false, gg = false;
    {
        std::lock_guard lock(g_settingsMtx);
        wb = g_autoWelcomeBack;
        wel = g_autoWelcome;
        gg = g_autoGg;
    }
    std::vector<AutoReplyRule> out;
    AutoReplyRule r;
    r.enabled = wb;  r.match = AutoReplyRule::StartsWith; r.pattern = "[+]"; r.reply = "wb";
    out.push_back(r);
    r = {};
    r.enabled = wel; r.match = AutoReplyRule::Contains; r.pattern = "has joined Histatu for the first time!"; r.reply = "welcome";
    out.push_back(r);
    r = {};
    r.enabled = gg;  r.match = AutoReplyRule::Regex; r.pattern = "^\\[!\\].*reached Ascension"; r.reply = "gg";
    out.push_back(r);
    return out;
}

// Does `rule` match? Fills `groups` for regex rules ([0] is the whole match).
bool Matches(Compiled& c, const std::string& text, std::vector<std::string>& groups) {
    groups.clear();
    const AutoReplyRule& r = c.rule;
    if (r.pattern.empty()) return false;

    if (r.match == AutoReplyRule::Regex) {
        if (!c.reOk) return false;
        try {
            std::smatch m;
            if (!std::regex_search(text, m, c.re)) return false;
            for (size_t i = 0; i < m.size() && i < 10; ++i) groups.push_back(m[i].str());
            return true;
        }
        catch (const std::regex_error&) {
            // MSVC throws error_complexity / error_stack instead of running away
            // on a pathological pattern; treat that as no match.
            return false;
        }
    }

    const std::string hay = r.caseSensitive ? text : ToLower(text);
    const std::string pat = r.caseSensitive ? r.pattern : ToLower(r.pattern);
    switch (r.match) {
    case AutoReplyRule::StartsWith: return hay.compare(0, pat.size(), pat) == 0;
    case AutoReplyRule::Exact:      return hay == pat;
    default:                        return hay.find(pat) != std::string::npos;
    }
}

std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}

std::string Expand(const AutoReplyRule& r, const Pending& line, const std::vector<std::string>& groups) {
    std::string me, bot;
    {
        std::lock_guard lock(g_settingsMtx);
        me = g_localName;
        bot = g_botName;
    }
    std::string out = r.reply;
    out = ReplaceAll(out, "{speaker}", NormalizeSpeaker(line.speaker));
    out = ReplaceAll(out, "{me}", me);
    out = ReplaceAll(out, "{bot}", bot);
    out = ReplaceAll(out, "{message}", line.body.empty() ? line.plain : line.body);
    for (int i = 1; i <= 9; ++i) {
        const std::string key = "{" + std::to_string(i) + "}";
        out = ReplaceAll(out, key, (size_t)i < groups.size() ? groups[(size_t)i] : std::string());
    }
    out = TrimCopy(Extras::ExpandEchoPlaceholders(out));

    // A reply that turned into a slash command only because someone else's
    // words were pasted into it ({message}, {1}...) would let any player run
    // commands as you. Only a reply you wrote as a command may be one.
    if (!out.empty() && out[0] == '/' && (r.reply.empty() || TrimCopy(r.reply)[0] != '/'))
        out.insert(out.begin(), '>');
    return out;
}

} // namespace

namespace AutoReply {

void Load() {
    std::vector<AutoReplyRule> rules;
    std::ifstream f(RulesPath());
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const auto p = SplitTabs(line);
            if (p.size() < 7) continue;
            AutoReplyRule r;
            try {
                r.enabled = p[0] == "1";
                r.match = (std::max)(0, (std::min)(3, std::stoi(p[1])));
                r.caseSensitive = p[2] == "1";
                r.skipOwn = p[3] != "0";
                r.cooldownSec = (std::max)(0, (std::min)(86400, std::stoi(p[4])));
            }
            catch (...) { continue; }
            r.pattern = Unesc(p[5]);
            r.reply = Unesc(p[6]);
            rules.push_back(std::move(r));
        }
        std::lock_guard lock(g_rulesMtx);
        g_rules.clear();
        for (auto& r : rules) {
            Compiled c;
            c.rule = std::move(r);
            Compile(c);
            g_rules.push_back(std::move(c));
        }
        g_generation.fetch_add(1);
        return;
    }

    SetRules(LegacyDefaults());
}

std::vector<AutoReplyRule> GetRules() {
    std::lock_guard lock(g_rulesMtx);
    std::vector<AutoReplyRule> out;
    out.reserve(g_rules.size());
    for (const Compiled& c : g_rules) out.push_back(c.rule);
    return out;
}

void SetRules(std::vector<AutoReplyRule> rules) {
    std::lock_guard lock(g_rulesMtx);
    // Keep each surviving rule's cooldown clock and fire count by position, so
    // editing a reply doesn't let it fire again immediately.
    std::vector<Compiled> next;
    next.reserve(rules.size());
    for (size_t i = 0; i < rules.size(); ++i) {
        Compiled c;
        c.rule = std::move(rules[i]);
        if (i < g_rules.size()) {
            c.lastFireMs = g_rules[i].lastFireMs;
            c.rule.fired = g_rules[i].rule.fired;
        }
        Compile(c);
        next.push_back(std::move(c));
    }
    g_rules.swap(next);
    g_generation.fetch_add(1);
    SaveLocked();
}

uint64_t Generation() { return g_generation.load(); }

void Offer(const std::string& plain, const std::string& speaker,
    const std::string& body, bool fromLocal, bool fromBot) {
    if (fromBot || plain.empty()) return;
    {
        std::lock_guard lock(g_rulesMtx);
        bool any = false;
        for (const Compiled& c : g_rules) if (c.rule.enabled) { any = true; break; }
        if (!any) return;
    }
    Pending p;
    p.plain = plain.size() > 1024 ? plain.substr(0, 1024) : plain;
    p.speaker = speaker;
    p.body = body.size() > 1024 ? body.substr(0, 1024) : body;
    p.fromLocal = fromLocal;
    std::lock_guard lock(g_queueMtx);
    if (g_queue.size() >= 64) g_queue.pop_front();
    g_queue.push_back(std::move(p));
}

void Tick() {
    std::deque<Pending> work;
    {
        std::lock_guard lock(g_queueMtx);
        if (g_queue.empty()) return;
        work.swap(g_queue);
    }
    {
        std::lock_guard lock(g_settingsMtx);
        if (g_botMuted) return;
    }

    const long long now = NowSteadyMs();
    std::vector<std::string> toSend;
    {
        std::lock_guard lock(g_rulesMtx);
        std::vector<std::string> groups;
        for (const Pending& line : work) {
            for (Compiled& c : g_rules) {
                const AutoReplyRule& r = c.rule;
                if (!r.enabled || r.reply.empty()) continue;
                if (r.skipOwn && line.fromLocal) continue;
                if (now - c.lastFireMs < (long long)r.cooldownSec * 1000) continue;
                if (now - g_lastAnyReplyMs < kGlobalGapMs) continue;
                if (!Matches(c, line.plain, groups)) continue;

                const std::string reply = Expand(r, line, groups);
                if (reply.empty()) continue;
                c.lastFireMs = now;
                c.rule.fired++;
                g_lastAnyReplyMs = now;
                toSend.push_back(reply);
                LogToFile("AutoReply fired: [" + r.pattern + "] -> " + reply);
                break; // first matching rule wins for a given line
            }
        }
    }
    for (const std::string& s : toSend)
        QueueSend({ s, s, "", "", "" });
}

std::string Test(const std::string& sample, const std::string& speaker) {
    if (sample.empty()) return "Type a sample chat line to test against.";
    Pending p;
    p.plain = sample;
    p.speaker = speaker;
    p.body = sample;
    const size_t colon = sample.find(": ");
    if (speaker.empty() && colon != std::string::npos && colon < 64) {
        p.speaker = sample.substr(0, colon);
        p.body = sample.substr(colon + 2);
    }

    std::lock_guard lock(g_rulesMtx);
    std::vector<std::string> groups;
    for (size_t i = 0; i < g_rules.size(); ++i) {
        Compiled& c = g_rules[i];
        if (!c.rule.enabled) continue;
        if (!Matches(c, p.plain, groups)) continue;
        return "Rule " + std::to_string(i + 1) + " matches and would send:  " + Expand(c.rule, p, groups);
    }
    return "No enabled rule matches.";
}

} // namespace AutoReply
