#include "aitext.hpp"
#include "aicalc.hpp"

#include <cmath>
#include <cstdio>
#include <random>

using namespace AiText;

static int g_pass = 0, g_fail = 0;

static void Eq(const std::string& got, const std::string& want, const std::string& what) {
    if (got == want) { ++g_pass; return; }
    ++g_fail;
    std::printf("FAIL: %s\n   got:  [%s]\n   want: [%s]\n", what.c_str(), got.c_str(), want.c_str());
}
static void Check(bool ok, const std::string& what) {
    if (ok) { ++g_pass; return; }
    ++g_fail;
    std::printf("FAIL: %s\n", what.c_str());
}

static std::string Calc(const std::string& e) {
    double v; std::string err;
    if (!AiCalc::Eval(e, v, err)) return "ERR:" + err;
    return AiCalc::FormatNumber(v);
}

int main() {
    // ---- variables ----
    {
        const Vars v = { { "me", "chris" }, { "bot", "HyAI" }, { "time", "12:30" } };
        Eq(ExpandVars("hi {me}, i am {bot} at {time}", v), "hi chris, i am HyAI at 12:30", "vars basic");
        Eq(ExpandVars("json {\"a\": 1} and {unknown} and {}", v), "json {\"a\": 1} and {unknown} and {}", "unknown braces stay");
        Eq(ExpandVars("\\{me} is literal, {me} is not", v), "{me} is literal, chris is not", "escaped brace");
        Eq(ExpandVars("{me}{me}", v), "chrischris", "adjacent");
        Eq(ExpandVars("unterminated {me", v), "unterminated {me", "unterminated");
        Eq(ExpandVars("", v), "", "empty");
        Eq(ExpandVars("{ME}", v), "{ME}", "names are case sensitive");
        Check(EstimateTokens("") == 0, "tokens of nothing");
        Check(EstimateTokens("abcdefgh") == 2, "tokens ascii");
        Check(EstimateTokens("\xE4\xBD\xA0\xE5\xA5\xBD") == 2, "tokens cjk");
    }

    // ---- prompt stack ----
    {
        auto st = ParseStack("+CONTEXT.md\n-Pirate\n\n  +  Short  \nPirate\n+context.md\n-\n");
        Check(st.size() == 3, "stack parsed size");
        if (st.size() == 3) {
            Check(st[0].name == "CONTEXT.md" && st[0].enabled, "stack 0");
            Check(st[1].name == "Pirate" && !st[1].enabled, "stack 1 (duplicate dropped, first wins)");
            Check(st[2].name == "Short" && st[2].enabled, "stack 2 trimmed");
        }
        Eq(SerializeStack(st), "+CONTEXT.md\n-Pirate\n+Short\n", "stack round trip");
        Check(ParseStack("").empty(), "empty stack");
        auto rec = ReconcileStack(st, { "short", "Zebra", "CONTEXT.md" });
        Check(rec.size() == 3, "reconcile drops missing, appends new");
        if (rec.size() == 3) {
            Check(rec[0].name == "CONTEXT.md" && rec[0].enabled, "reconcile keeps order");
            Check(rec[1].name == "short" && rec[1].enabled, "reconcile takes the file's spelling");
            Check(rec[2].name == "Zebra" && !rec[2].enabled, "new file arrives switched off");
        }
        Eq(SafePresetName("  ..My: Pre/set?* "), "My Preset", "safe name");
        Eq(SafePresetName("con"), "con_", "reserved name");
        Eq(SafePresetName("///"), "", "nothing left");
        Check(SafePresetName(std::string(200, 'a')).size() == 48, "safe name capped");
    }

    // ---- chat context ----
    {
        std::vector<CtxEntry> e;
        auto add = [&](const char* sp, const char* body, long long t, bool bot = false, bool hidden = false) {
            CtxEntry c; c.speaker = sp; c.body = body; c.plain = std::string(sp[0] ? std::string(sp) + ": " : "") + body;
            c.unixMs = t; c.fromBot = bot; c.hidden = hidden; e.push_back(c);
        };
        const long long now = 1790000000000LL;
        add("Steve", "hello", now - 600000);
        add("", "[!] Server ad", now - 500000);
        add("HyAI", "hi steve", now - 400000, true);
        add("Amelie", "salut", now - 300000);
        add("Bob", "secret", now - 200000, false, true);
        add("Steve", "what is a kweebec", now - 100000);

        CtxOptions o;
        o.maxLines = 20;
        o.nowMs = now;
        Eq(JoinLines(BuildChatLines(e, o)),
           "Steve: hello\n[!] Server ad\nHyAI: hi steve\nAmelie: salut\nSteve: what is a kweebec\n",
           "default context matches the old wording, hidden line left out");
        o.maxLines = 2;
        Eq(JoinLines(BuildChatLines(e, o)), "Amelie: salut\nSteve: what is a kweebec\n", "maxLines counts from the newest and skips hidden");
        o.maxLines = 20; o.includeBot = false;
        Check(JoinLines(BuildChatLines(e, o)).find("HyAI") == std::string::npos, "bot lines off");
        o.includeBot = true; o.includeSystem = false;
        Check(JoinLines(BuildChatLines(e, o)).find("Server ad") == std::string::npos, "system lines off");
        o.includeSystem = true; o.maxAgeMin = 9;
        Eq(JoinLines(BuildChatLines(e, o)), "[!] Server ad\nHyAI: hi steve\nAmelie: salut\nSteve: what is a kweebec\n", "age filter");
        o.maxAgeMin = 0; o.lineFormat = "[{time}] <{speaker}> {body}"; o.maxLines = 1;
        auto one = BuildChatLines(e, o);
        Check(one.size() == 1 && one[0].index == 5, "index points into the entries");
        Check(one[0].text.find("<Steve> what is a kweebec") != std::string::npos && one[0].text[0] == '[', "custom format: " + one[0].text);
        o.lineFormat = "{speaker}: {body}"; o.maxLineChars = 5; o.maxLines = 20;
        Check(BuildChatLines(e, o).back().text == "Steve...", "line cap: " + BuildChatLines(e, o).back().text);
        o.maxLineChars = 0; o.lineFormat = "";
        Check(!BuildChatLines(e, o).empty(), "empty format falls back to the default");
        CtxOptions none; none.maxLines = 0;
        Check(BuildChatLines(e, none).empty(), "zero lines");
        Check(BuildChatLines({}, o).empty(), "no entries");
    }

    // ---- reply clean-up ----
    {
        PostOptions o;
        Eq(PostProcess("Hello THERE.", o).text, "Hello THERE.", "post: nothing on by default");
        o.lowercase = true;
        Eq(PostProcess("Hello THERE.", o).text, "hello there.", "lowercase");
        o = PostOptions{}; o.stripEmoji = true;
        Eq(PostProcess("nice \xF0\x9F\x8E\x89 one \xE2\x9D\xA4\xEF\xB8\x8F ok", o).text, "nice one ok", "emoji stripped, spaces collapsed");
        Eq(PostProcess("caf\xC3\xA9 \xE4\xBD\xA0\xE5\xA5\xBD", o).text, "caf\xC3\xA9 \xE4\xBD\xA0\xE5\xA5\xBD", "letters are not emoji");
        o = PostOptions{}; o.stripMarkdown = true;
        Eq(PostProcess("**bold** and `code` and ~~gone~~", o).text, "bold and code and gone", "markdown marks");
        Eq(PostProcess("# Title\n- one\n* two\nplain", o).text, "Title\none\ntwo\nplain", "headings and bullets");
        Eq(PostProcess("see [the wiki](https://x.io/a) now", o).text, "see the wiki (https://x.io/a) now", "links");
        Eq(PostProcess("2*3 and a * b and *italic*", o).text, "2*3 and a * b and italic", "asterisks");
        Eq(PostProcess("death_the_kid__ said hi", o).text, "death_the_kid__ said hi", "underscore names survive");
        o = PostOptions{}; o.maxChars = 20;
        Eq(PostProcess("short one", o).text, "short one", "under the cap");
        Eq(PostProcess("First sentence here. Second sentence goes on and on", o).text, "First sentence here.", "cut at a sentence");
        Eq(PostProcess("wordwordword wordwordword wordwordword", o).text, "wordwordword", "cut at a word: " + PostProcess("wordwordword wordwordword wordwordword", o).text);
        o.maxChars = 3;
        Eq(PostProcess("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C", o).text, "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96", "cut counts characters, keeps them whole");
        o = PostOptions{}; o.blocklist = "darn, Bad Word";
        Eq(PostProcess("well darn it, a bad word!", o).text, "well **** it, a ********!", "mask");
        Eq(PostProcess("darned and darning", o).text, "darned and darning", "whole words only");
        o.blockAction = 1;
        Check(PostProcess("oh DARN", o).blocked, "block action");
        Check(!PostProcess("all fine", o).blocked, "clean reply not blocked");
        o = PostOptions{}; o.prefix = ">> "; o.suffix = " <<";
        Eq(PostProcess("hi", o).text, ">> hi <<", "prefix and suffix");
        Eq(PostProcess("   ", o).text, "", "no prefix on an empty reply");
        o = PostOptions{}; o.lowercase = true; o.stripEmoji = true; o.stripMarkdown = true; o.maxChars = 30;
        auto r = PostProcess("**Hey Steve!** \xF0\x9F\x98\x80 Check the FORGE at spawn for more stuff", o);
        Eq(r.text, "hey steve! check the forge at", "all together: " + r.text);
        Check(r.notes.size() >= 3, "notes explain what changed");
    }

    // ---- tool rules ----
    {
        auto r = ParseToolRules("Lookup_Mob=auto; set_reminder = ASK\nweb_search=off;bogus;x=maybe");
        Check(r.byName.size() == 3, "tool rules parsed");
        Check(ResolveToolMode(r, 0, "lookup_mob", false) == ToolMode::Auto, "rule auto");
        Check(ResolveToolMode(r, 2, "lookup_mob", false) == ToolMode::Auto, "rule beats policy");
        Check(ResolveToolMode(r, 0, "set_reminder", true) == ToolMode::Ask, "rule ask");
        Check(ResolveToolMode(r, 0, "web_search", true) == ToolMode::Off, "rule off");
        Check(ResolveToolMode(r, 0, "get_time", false) == ToolMode::Auto, "policy 0 runs all");
        Check(ResolveToolMode(r, 1, "get_time", false) == ToolMode::Auto, "policy 1 ignores harmless tools");
        Check(ResolveToolMode(r, 1, "cancel_reminder", true) == ToolMode::Ask, "policy 1 asks for side effects");
        Check(ResolveToolMode(r, 2, "get_time", false) == ToolMode::Ask, "policy 2 asks for everything");
        Eq(SerializeToolRules(ParseToolRules(SerializeToolRules(r))), SerializeToolRules(r), "tool rules round trip");
    }

    // ---- wake words at the start ----
    {
        const std::vector<std::string> w = { "hyai", "bot" };
        Check(WakeAtStart("hyai what is a kweebec", w), "wake at start");
        Check(WakeAtStart("HyAI, what is a kweebec", w), "wake with comma, any case");
        Check(WakeAtStart("@hyai hello", w), "wake with @");
        Check(WakeAtStart("hey hyai hello", w), "greeting then wake");
        Check(WakeAtStart("yo, bot help", w), "greeting comma second wake");
        Check(WakeAtStart("hyai", w), "wake alone");
        Check(!WakeAtStart("what is a kweebec hyai", w), "wake at the end does not count");
        Check(!WakeAtStart("hyaiiii hello", w), "longer word is not the wake word");
        Check(!WakeAtStart("hey there hyai", w), "two words in front");
        Check(!WakeAtStart("heyhyai", w), "glued greeting");
        Check(!WakeAtStart("", w), "empty message");
        Check(!WakeAtStart("hyai hello", {}), "no wake words");
        Check(!WakeAtStart("hyai hello", { "", " " }), "blank wake words");
    }

    // ---- rate limiter ----
    {
        RateLimiter rl;
        std::string why;
        const long long t0 = 1000000;
        Check(rl.Allow(t0, "Steve", 10, 30, 3, &why), "first answer allowed");
        rl.Record(t0, "Steve");
        Check(!rl.Allow(t0 + 5000, "Amelie", 10, 0, 0, &why) && why.find("cooldown") != std::string::npos, "global cooldown: " + why);
        Check(rl.Allow(t0 + 11000, "Amelie", 10, 0, 0), "global cooldown over");
        Check(!rl.Allow(t0 + 11000, "steve", 0, 30, 0, &why) && why.find("steve") != std::string::npos, "per-player cooldown is case-insensitive: " + why);
        Check(rl.Allow(t0 + 31000, "Steve", 0, 30, 0), "per-player cooldown over");
        rl.Record(t0 + 12000, "A"); rl.Record(t0 + 13000, "B");
        Check(!rl.Allow(t0 + 14000, "C", 0, 0, 3, &why) && why.find("per minute") != std::string::npos, "per-minute limit: " + why);
        Check(rl.Allow(t0 + 61000, "C", 0, 0, 3), "per-minute window slides");
        Check(rl.Allow(t0 + 14000, "C", 0, 0, 0), "limits off");
        rl.Clear();
        Check(rl.Allow(t0 + 14000, "Steve", 10, 30, 1), "cleared");
    }

    // ---- calculator ----
    {
        Eq(Calc("1+2*3"), "7", "precedence");
        Eq(Calc("(1+2)*3"), "9", "parens");
        Eq(Calc("2^3^2"), "512", "power is right-associative");
        Eq(Calc("-2^2"), "4", "unary minus binds first here: " + Calc("-2^2"));
        Eq(Calc("10/4"), "2.5", "division");
        Eq(Calc("10 % 4"), "2", "modulo");
        Eq(Calc("sqrt(16)+abs(-3)"), "7", "functions");
        Eq(Calc("max(2, 9) - min(4, 1)"), "8", "two-argument functions");
        Eq(Calc("2 x 3"), "6", "x as times");
        Eq(Calc("round(pi*100)/100"), "3.14", "constants");
        Eq(Calc("1e3+1"), "1001", "exponent literal");
        Eq(Calc("0.1+0.2"), "0.3", "float formatting");
        Check(Calc("1/0").rfind("ERR:", 0) == 0, "divide by zero");
        Check(Calc("sqrt(-1)").rfind("ERR:", 0) == 0, "sqrt of negative");
        Check(Calc("2+").rfind("ERR:", 0) == 0, "dangling operator");
        Check(Calc("2+2)").rfind("ERR:", 0) == 0, "stray paren");
        Check(Calc("foo(2)").rfind("ERR:", 0) == 0, "unknown function");
        Check(Calc("system(1)").rfind("ERR:", 0) == 0, "no way out");
        Check(Calc(std::string(300, '1')).rfind("ERR:", 0) == 0, "too long");
        Check(Calc(std::string(100, '(') + "1" + std::string(100, ')')).rfind("ERR:", 0) == 0, "nesting limit");
        Check(Calc("9^9^9").rfind("ERR:", 0) == 0, "overflow is an error");
        Check(Calc("").rfind("ERR:", 0) == 0, "empty");
    }

    // ---- dice ----
    {
        unsigned n = 0;
        auto seq = [&](unsigned sides) { return (unsigned)(++n % sides) + 1; };
        Eq(AiCalc::RollDice("2d6+3", seq), "2d6+3: 2 + 3 + 3 = 8", "dice with bonus");
        n = 0;
        Eq(AiCalc::RollDice(" D20 ", seq), "1d20: 2 = 2", "d20");
        n = 0;
        Eq(AiCalc::RollDice("1d6-2", seq), "1d6-2: 2 - 2 = 0", "negative bonus");
        Check(AiCalc::RollDice("101d6", seq).rfind("error", 0) == 0, "too many dice");
        Check(AiCalc::RollDice("1d1", seq).rfind("error", 0) == 0, "one-sided die");
        Check(AiCalc::RollDice("1d5000", seq).rfind("error", 0) == 0, "too many sides");
        Check(AiCalc::RollDice("hello", seq).rfind("error", 0) == 0, "not dice");
        Check(AiCalc::RollDice("", seq).rfind("error", 0) == 0, "empty");
        Check(AiCalc::RollDice("2d6+", seq).rfind("error", 0) == 0, "dangling bonus");
    }

    // ---- fuzz: nothing here may crash or run away ----
    {
        std::mt19937 rng(7);
        const char* bits[] = { "{", "}", "\\", "{me}", "{x", "a", " ", "\n", "**", "`", "[", "](", ")", "#", "- ", "\xF0\x9F\x98\x80", "\xE4\xBD\xA0", "\xFF", "+", "-", "(", "^", "1", "d", "sqrt" };
        const int nb = (int)(sizeof(bits) / sizeof(bits[0]));
        Vars v = { { "me", "x" } };
        for (int it = 0; it < 60000; ++it) {
            std::string s;
            const int n = (int)(rng() % 16);
            for (int i = 0; i < n; ++i) s += bits[rng() % nb];
            ExpandVars(s, v);
            EstimateTokens(s);
            ParseStack(s);
            SafePresetName(s);
            PostOptions o; o.lowercase = o.stripEmoji = o.stripMarkdown = true; o.maxChars = (int)(rng() % 12); o.blocklist = "a, x";
            PostResult r = PostProcess(s, o);
            if (o.maxChars > 0) {
                size_t chars = 0;
                for (unsigned char c : r.text) if ((c & 0xC0) != 0x80) ++chars;
                if (chars > (size_t)o.maxChars + 0) { Check(false, "fuzz: cap exceeded for [" + s + "]"); break; }
            }
            double d; std::string err;
            AiCalc::Eval(s, d, err);
            AiCalc::RollDice(s, [](unsigned k) { return k; });
            ParseToolRules(s);
        }
        Check(true, "fuzz finished");
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
