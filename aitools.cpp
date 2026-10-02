#include "pch.h"
#include "aitools.hpp"
#include "aicalc.hpp"
#include "chat.hpp"
#include "common.hpp"
#include "extras.hpp"
#include "knowledge.hpp"
#include "position.hpp"
#include "settings.hpp"
#include "shared.hpp"

#include <random>

namespace AiTools {
namespace {

// ---- arguments ---------------------------------------------------------------
std::string ArgString(const std::string& args, const char* key) {
    std::string v;
    ExtractJsonStringField(args, key, v);
    return v;
}

int ArgInt(const std::string& args, const char* key, int def = 0) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = args.find(pat);
    if (pos == std::string::npos) return def;
    pos = args.find(':', pos + pat.size());
    if (pos == std::string::npos) return def;
    ++pos;
    while (pos < args.size() && (args[pos] == ' ' || args[pos] == '\t')) ++pos;
    if (pos >= args.size()) return def;
    if (args[pos] == '"') ++pos;               // some models quote their numbers
    size_t end = pos;
    while (end < args.size() && (isdigit((unsigned char)args[end]) || args[end] == '-' || args[end] == '+')) ++end;
    if (end == pos) return def;
    try { return std::stoi(args.substr(pos, end - pos)); }
    catch (...) { return def; }
}

// ---- the tools ---------------------------------------------------------------
// Schemas are split one per tool so each can be switched off on its own.
const std::vector<Def>& Table() {
    static const std::vector<Def> defs = {
        { "lookup_mob", "Look up a mob", "Health, traits and drops of one mob.", false,
          R"J({"type":"function","function":{"name":"lookup_mob","description":"Get accurate health, traits and drops for one specific Hytale mob by name or in-game ID. Always call this for questions about a named mob.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Mob display name or in-game ID e.g. Wild Pig, Pig_Wild, Scarak Broodmother, Eye_Void"}},"required":["query"]}}})J" },
        { "search_mobs", "Search mobs", "Find mobs by text, drop, trait or health.", false,
          R"J({"type":"function","function":{"name":"search_mobs","description":"Search or filter mobs by free text, by an item they drop, by trait, or by health range. Use for questions like what drops X, timid animals, high health mobs, etc.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Free-text keywords"},"drops_item":{"type":"string","description":"Item name or ID the mob must drop e.g. Void Essence, Raw Pork"},"trait":{"type":"string","description":"timid | mountable | spawns_at_night | attack_when_startled"},"min_health":{"type":"integer"},"max_health":{"type":"integer"},"limit":{"type":"integer","description":"Max results default 8"}}}}})J" },
        { "lookup_item", "Look up an item", "Quality, description and crafting recipe of one item.", false,
          R"J({"type":"function","function":{"name":"lookup_item","description":"Get accurate info for one Hytale item: quality, level, description, categories, crafting recipe. Always call for questions about a named item or how to craft something.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Item display name or in-game ID e.g. Void Essence, Raw Pork, Farmer's Workbench"}},"required":["query"]}}})J" },
        { "search_items", "Search items", "Find items by text, quality or category.", false,
          R"J({"type":"function","function":{"name":"search_items","description":"Search items by free text, quality, or category.","parameters":{"type":"object","properties":{"query":{"type":"string"},"quality":{"type":"string","description":"Common, Uncommon, Rare, Epic, etc."},"category":{"type":"string","description":"Weapons, Food, etc."},"limit":{"type":"integer","description":"Max results default 8"}}}}})J" },
        { "get_time", "Time", "The time in any place or timezone.", false,
          R"J({"type":"function","function":{"name":"get_time","description":"Get the current date/time for a place, timezone, or abbreviation. Use for questions like what time is it in Germany, what time is GMT, CST, Tokyo, London, etc. Empty query = local time on the bot host.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Country, city, timezone name, or abbr e.g. Germany, Berlin, UTC, GMT, CST, PST, EST, CET, Tokyo, London. Empty for local."}}}}})J" },
        { "set_reminder", "Set a reminder", "Says something to a player later.", true,
          R"J({"type":"function","function":{"name":"set_reminder","description":"Set reminders for users. Convert minutes to seconds if needed (2 minutes=120). user=player name, in_seconds=delay, message=what to say later.","parameters":{"type":"object","properties":{"user":{"type":"string","description":"Player name to address in the reminder"},"in_seconds":{"type":"integer","description":"Delay in seconds (min 5, max 604800)"},"message":{"type":"string","description":"What to say when the reminder fires"}},"required":["user","in_seconds","message"]}}})J" },
        { "list_reminders", "List reminders", "Shows the active reminders.", false,
          R"J({"type":"function","function":{"name":"list_reminders","description":"List active reminders, optionally filtered by player name.","parameters":{"type":"object","properties":{"user":{"type":"string","description":"Optional player name filter"}}}}})J" },
        { "cancel_reminder", "Cancel a reminder", "Removes a reminder by its number.", true,
          R"J({"type":"function","function":{"name":"cancel_reminder","description":"Cancel an active reminder by its numeric id.","parameters":{"type":"object","properties":{"id":{"type":"integer","description":"Reminder id from set_reminder / list_reminders"}},"required":["id"]}}})J" },
        { "web_search", "Web search", "Live web search (needs the LangSearch key).", true,
          R"J({"type":"function","function":{"name":"web_search","description":"Live web search for current facts, news, prices, or info not covered by game knowledge/tools. Prefer lookup_mob/lookup_item for Hytale. Use short keyword queries.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Focused keywords, not a full sentence"},"freshness":{"type":"string","description":"oneDay|oneWeek|oneMonth|oneYear|noLimit"}},"required":["query"]}}})J" },
        { "calculate", "Calculator", "Exact arithmetic instead of guessing.", false,
          R"J({"type":"function","function":{"name":"calculate","description":"Evaluate a math expression exactly. Supports + - * / % ^, parentheses, sqrt abs floor ceil round ln log sin cos tan min max, and pi e. Use it for any arithmetic instead of computing in your head.","parameters":{"type":"object","properties":{"expression":{"type":"string","description":"e.g. (12*7+3)/5"}},"required":["expression"]}}})J" },
        { "roll_dice", "Dice", "Rolls dice like 2d6+3.", false,
          R"J({"type":"function","function":{"name":"roll_dice","description":"Roll dice for a game or a decision. Format NdM with an optional +K or -K, e.g. d20, 2d6+3.","parameters":{"type":"object","properties":{"dice":{"type":"string","description":"e.g. d20 or 2d6+3"}},"required":["dice"]}}})J" },
        { "search_chat", "Search chat history", "Finds what people said earlier.", false,
          R"J({"type":"function","function":{"name":"search_chat","description":"Search the server chat this bot has seen (a few hundred recent lines) for some text, to recall what someone said earlier. Returns the newest matches first.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Text to look for, e.g. a name or a word"},"limit":{"type":"integer","description":"Max lines, default 8"}},"required":["query"]}}})J" },
        { "list_players", "Recent players", "Players seen in chat lately.", false,
          R"J({"type":"function","function":{"name":"list_players","description":"List the players who spoke in chat recently, newest first, with how long ago. Use to see who is around.","parameters":{"type":"object","properties":{}}}})J" },
        { "get_my_position", "Bot position", "The bot player's own X Y Z, when known.", false,
          R"J({"type":"function","function":{"name":"get_my_position","description":"Get the coordinates (X Y Z) of the player this bot is running as, if known. Use when asked where you are.","parameters":{"type":"object","properties":{}}}})J" },
    };
    return defs;
}

AiText::ToolRules CurrentRules() {
    std::string s;
    { std::lock_guard lock(g_settingsMtx); s = g_prefs.aiToolRules; }
    return AiText::ParseToolRules(s);
}

std::mt19937& Rng() {
    static std::mt19937 rng{ std::random_device{}() };
    return rng;
}

std::string AgoText(long long ms) {
    const long long s = ms / 1000;
    if (s < 90) return std::to_string(s) + "s ago";
    if (s < 5400) return std::to_string(s / 60) + "m ago";
    return std::to_string(s / 3600) + "h ago";
}

std::string SearchChat(const std::string& query, int limit) {
    if (query.empty()) return "error: give some text to search for";
    if (limit < 1) limit = 8;
    if (limit > 20) limit = 20;
    const std::string needle = ToLower(query);
    std::string out;
    int found = 0;
    std::lock_guard lock(g_mtx);
    for (auto it = g_log.rbegin(); it != g_log.rend() && found < limit; ++it) {
        if (it->local || it->outgoing) continue;
        if (ToLower(it->plain).find(needle) == std::string::npos) continue;
        const std::string when = FormatChatTime(it->unixMs);
        out += (when.empty() ? "" : when + " ") + it->plain + "\n";
        ++found;
        if (out.size() > 1500) break;
    }
    return found ? out : "No chat lines contain \"" + query + "\".";
}

std::string ListPlayers() {
    const long long now = NowSteadyMs();
    std::string out;
    int n = 0;
    std::lock_guard lock(g_mtx);
    for (auto it = g_seenPlayers.rbegin(); it != g_seenPlayers.rend() && n < 20; ++it, ++n)
        out += it->display + " (" + AgoText(now - it->lastSeenMs) + ")\n";
    return n ? out : "No players have spoken since the bot started.";
}

} // namespace

const std::vector<Def>& All() { return Table(); }

const Def* Find(const std::string& name) {
    for (const Def& d : Table()) if (name == d.name) return &d;
    return nullptr;
}

AiText::ToolMode ModeFor(const std::string& name) {
    const Def* d = Find(name);
    if (!d) return AiText::ToolMode::Off;
    if (name == "web_search") {
        bool on;
        { std::lock_guard lock(g_settingsMtx); on = g_webSearchEnabled; }
        if (!on) return AiText::ToolMode::Off;
    }
    int policy;
    { std::lock_guard lock(g_settingsMtx); policy = g_prefs.aiToolPolicy; }
    return AiText::ResolveToolMode(CurrentRules(), policy, name, d->sideEffect);
}

void SetRule(const std::string& name, AiText::ToolMode mode, bool clear) {
    std::lock_guard lock(g_settingsMtx);
    AiText::ToolRules r = AiText::ParseToolRules(g_prefs.aiToolRules);
    if (clear) r.byName.erase(ToLower(name));
    else r.byName[ToLower(name)] = mode;
    g_prefs.aiToolRules = AiText::SerializeToolRules(r);
    MarkPrefsChanged();
}

std::string ToolsJson() {
    std::string out = "[";
    for (const Def& d : Table()) {
        if (ModeFor(d.name) == AiText::ToolMode::Off) continue;
        if (out.size() > 1) out += ",";
        out += d.json;
    }
    out += "]";
    return out;
}

std::string Execute(const std::string& name, const std::string& args) {
    if (name == "lookup_mob") return Knowledge::ToolLookupMob(ArgString(args, "query"));
    if (name == "search_mobs")
        return Knowledge::ToolSearchMobs(ArgString(args, "query"), ArgString(args, "drops_item"), ArgString(args, "trait"),
            ArgInt(args, "min_health", 0), ArgInt(args, "max_health", 0), ArgInt(args, "limit", 8));
    if (name == "lookup_item") return Knowledge::ToolLookupItem(ArgString(args, "query"));
    if (name == "search_items")
        return Knowledge::ToolSearchItems(ArgString(args, "query"), ArgString(args, "quality"), ArgString(args, "category"),
            ArgInt(args, "limit", 8));
    if (name == "get_time") return Extras::ToolGetTime(ArgString(args, "query"));
    if (name == "set_reminder")
        return Extras::ToolSetReminder(ArgString(args, "user"), ArgInt(args, "in_seconds", 0), ArgString(args, "message"));
    if (name == "list_reminders") return Extras::ToolListReminders(ArgString(args, "user"));
    if (name == "cancel_reminder") return Extras::ToolCancelReminder((uint64_t)ArgInt(args, "id", 0));
    if (name == "web_search") return Extras::ToolWebSearch(ArgString(args, "query"), ArgString(args, "freshness"));
    if (name == "calculate") {
        double v;
        std::string err;
        if (!AiCalc::Eval(ArgString(args, "expression"), v, err)) return "error: " + err;
        return AiCalc::FormatNumber(v);
    }
    if (name == "roll_dice")
        return AiCalc::RollDice(ArgString(args, "dice"), [](unsigned sides) {
            return (unsigned)(std::uniform_int_distribution<unsigned>(1, sides)(Rng()));
        });
    if (name == "search_chat") return SearchChat(ArgString(args, "query"), ArgInt(args, "limit", 8));
    if (name == "list_players") return ListPlayers();
    if (name == "get_my_position") {
        if (!g_posLocked.load()) return "The bot's position is not known (the Position tab has not found it).";
        char buf[96];
        snprintf(buf, sizeof(buf), "X %.1f  Y %.1f  Z %.1f", g_posX.load(), g_posY.load(), g_posZ.load());
        return buf;
    }
    return "Unknown tool: " + name;
}

} // namespace AiTools
