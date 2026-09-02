#include "pch.h"
#include "knowledge.hpp"
#include "common.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <vector>

namespace Knowledge {

    // ---------------------------------------------------------------------------
    // Shared helpers
    // ---------------------------------------------------------------------------
    static std::string NormalizeKey(std::string s) {
        s = ToLower(TrimCopy(s));
        std::string out;
        out.reserve(s.size());
        bool lastSpace = false;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '_' || c == '-') {
                out += (char)c;
                lastSpace = false;
            }
            else if (std::isspace(c) && !lastSpace) {
                out += ' ';
                lastSpace = true;
            }
        }
        return TrimCopy(out);
    }

    // Underscores/hyphens -> spaces so "Ingredient_Void_Essence" ~ "ingredient void essence"
    static std::string FlattenKey(std::string s) {
        s = NormalizeKey(std::move(s));
        for (char& c : s) {
            if (c == '_' || c == '-') c = ' ';
        }
        // collapse double spaces
        std::string out;
        out.reserve(s.size());
        bool lastSpace = false;
        for (char c : s) {
            if (c == ' ') {
                if (!lastSpace) out += ' ';
                lastSpace = true;
            }
            else {
                out += c;
                lastSpace = false;
            }
        }
        return TrimCopy(out);
    }

    static std::vector<std::string> TokenizeKey(const std::string& flat) {
        std::vector<std::string> toks;
        std::string cur;
        for (char c : flat) {
            if (c == ' ') {
                if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
            }
            else cur += c;
        }
        if (!cur.empty()) toks.push_back(cur);
        return toks;
    }

    static bool IsStopToken(const std::string& t) {
        return t == "of" || t == "the" || t == "a" || t == "an" || t == "and" || t == "or";
    }

    // True if every significant query token appears somewhere in the haystack tokens
    // or the flattened hay string contains the query as a contiguous phrase.
    static bool ItemKeyMatches(const std::string& hayRaw, const std::string& queryRaw) {
        if (queryRaw.empty()) return false;
        const std::string hay = FlattenKey(hayRaw);
        const std::string q = FlattenKey(queryRaw);
        if (hay.empty() || q.empty()) return false;

        // Contiguous phrase (handles "void essence" inside "essence of the void" after token check fails order)
        if (hay.find(q) != std::string::npos) return true;

        auto qToks = TokenizeKey(q);
        auto hToks = TokenizeKey(hay);
        if (qToks.empty()) return false;

        // Drop stop-words from query for matching
        std::vector<std::string> significant;
        for (const auto& t : qToks)
            if (!IsStopToken(t) && t.size() >= 2)
                significant.push_back(t);
        if (significant.empty())
            significant = qToks; // fall back if query was only stop words

        // Every significant query token must appear as a full token OR as a substring of a hay token
        // (so "void" matches "void", "essence" matches "essence")
        for (const auto& qt : significant) {
            bool found = false;
            for (const auto& ht : hToks) {
                if (ht == qt || ht.find(qt) != std::string::npos || qt.find(ht) != std::string::npos) {
                    // avoid very short false positives (e.g. "a" in everything) — already filtered size
                    if (ht.size() >= 2 || ht == qt) { found = true; break; }
                }
            }
            if (!found) return false;
        }
        return true;
    }

    static int SimpleEditDistance(const std::string& a, const std::string& b) {
        const size_t n = a.size(), m = b.size();
        if (n == 0) return (int)m;
        if (m == 0) return (int)n;
        if (n > 48 || m > 48) return 999;
        std::vector<int> prev(m + 1), cur(m + 1);
        for (size_t j = 0; j <= m; ++j) prev[j] = (int)j;
        for (size_t i = 1; i <= n; ++i) {
            cur[0] = (int)i;
            for (size_t j = 1; j <= m; ++j) {
                int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                cur[j] = (std::min)({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
            }
            prev.swap(cur);
        }
        return prev[m];
    }

    static bool JsonExtractObjectString(const std::string& obj, const char* key, std::string& out) {
        out.clear();
        return ExtractJsonStringField(obj, key, out);
    }

    static int JsonExtractInt(const std::string& obj, const char* key, int def = 0) {
        std::string pat = std::string("\"") + key + "\"";
        size_t pos = obj.find(pat);
        if (pos == std::string::npos) return def;
        pos = obj.find(':', pos + pat.size());
        if (pos == std::string::npos) return def;
        ++pos;
        while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == '\t')) ++pos;
        if (pos >= obj.size()) return def;
        if (obj[pos] == 'n') return def;
        try {
            size_t end = pos;
            while (end < obj.size() && (isdigit((unsigned char)obj[end]) || obj[end] == '-' || obj[end] == '+')) ++end;
            if (end == pos) return def;
            return std::stoi(obj.substr(pos, end - pos));
        }
        catch (...) { return def; }
    }

    static float JsonExtractFloat(const std::string& obj, const char* key, float def = 0.f) {
        std::string pat = std::string("\"") + key + "\"";
        size_t pos = obj.find(pat);
        if (pos == std::string::npos) return def;
        pos = obj.find(':', pos + pat.size());
        if (pos == std::string::npos) return def;
        ++pos;
        while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == '\t')) ++pos;
        if (pos >= obj.size() || obj[pos] == 'n') return def;
        try {
            size_t end = pos;
            while (end < obj.size() && (isdigit((unsigned char)obj[end]) || obj[end] == '-' || obj[end] == '+' || obj[end] == '.' || obj[end] == 'e' || obj[end] == 'E')) ++end;
            if (end == pos) return def;
            return std::stof(obj.substr(pos, end - pos));
        }
        catch (...) { return def; }
    }

    static bool JsonExtractBool(const std::string& obj, const char* key, bool def = false) {
        std::string pat = std::string("\"") + key + "\"";
        size_t pos = obj.find(pat);
        if (pos == std::string::npos) return def;
        pos = obj.find(':', pos + pat.size());
        if (pos == std::string::npos) return def;
        ++pos;
        while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == '\t')) ++pos;
        if (pos + 4 <= obj.size() && obj.compare(pos, 4, "true") == 0) return true;
        if (pos + 5 <= obj.size() && obj.compare(pos, 5, "false") == 0) return false;
        return def;
    }

    static bool ExtractBalanced(const std::string& s, size_t pos, std::string& out) {
        out.clear();
        if (pos >= s.size()) return false;
        char open = s[pos];
        char close = (open == '{') ? '}' : (open == '[') ? ']' : 0;
        if (!close) return false;
        int depth = 0;
        bool inStr = false, esc = false;
        for (size_t i = pos; i < s.size(); ++i) {
            char c = s[i];
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == open) ++depth;
            else if (c == close) {
                --depth;
                if (depth == 0) {
                    out = s.substr(pos, i - pos + 1);
                    return true;
                }
            }
        }
        return false;
    }

    // ---------------------------------------------------------------------------
    // Mobs
    // ---------------------------------------------------------------------------
    struct MobDrop {
        std::string item_name;
        std::string item_id;
        int qty_min = 0;
        int qty_max = 0;
        int chance_pct = 100;
    };

    struct MobInfo {
        std::string id;
        std::string name;
        std::string summary;
        int max_health = 0;
        int view_range = 0;
        int hearing_range = 0;
        float speed = 0.f;
        bool timid = false;
        bool mountable = false;
        bool spawns_at_night = false;
        bool attack_when_startled = false;
        bool saved_as_memory = false;
        std::vector<MobDrop> drops;
    };

    static std::unordered_map<std::string, MobInfo> g_mobsById;
    static std::unordered_map<std::string, std::string> g_mobNameToId;
    static std::unordered_map<std::string, std::vector<std::string>> g_itemToMobs;
    static std::vector<std::pair<std::string, std::string>> g_allMobNames;
    static std::atomic<bool> g_mobsLoaded{ false };
    static std::string g_mobsStatus = "not loaded";
    static std::mutex g_mobsMtx;

    // ---------------------------------------------------------------------------
    // Items
    // ---------------------------------------------------------------------------
    struct ItemRecipeIng {
        int quantity = 0;
        std::string name;
        std::string id;
    };

    struct ItemRecipe {
        int duration_s = 0;
        std::vector<ItemRecipeIng> ingredients;
        std::vector<std::string> benches; // names
    };

    struct ItemInfo {
        std::string id;
        std::string name;
        std::string description;
        std::string summary;
        std::string quality;
        int item_level = 0;
        std::vector<std::string> categories;
        bool is_bench = false;
        std::vector<ItemRecipe> recipes;
    };

    static std::unordered_map<std::string, ItemInfo> g_itemsById;
    static std::unordered_map<std::string, std::string> g_itemNameToId;
    static std::vector<std::pair<std::string, std::string>> g_allItemNames;
    static std::atomic<bool> g_itemsLoaded{ false };
    static std::string g_itemsStatus = "not loaded";
    static std::mutex g_itemsMtx;

    static bool LoadMobs(bool force) {
        if (g_mobsLoaded.load() && !force) return true;
        std::lock_guard lock(g_mobsMtx);
        if (g_mobsLoaded.load() && !force) return true;

        const std::wstring wpath = GetThisModuleDirectory() + L"hytale_mobs.json";
        std::ifstream file(wpath, std::ios::binary);
        if (!file.is_open()) {
            g_mobsStatus = "hytale_mobs.json not found next to DLL";
            LogToFile(g_mobsStatus);
            return false;
        }
        std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        size_t mobsKey = raw.find("\"mobs\"");
        if (mobsKey == std::string::npos) {
            g_mobsStatus = "no \"mobs\" key in JSON";
            LogToFile(g_mobsStatus);
            return false;
        }
        size_t arrStart = raw.find('[', mobsKey);
        if (arrStart == std::string::npos) {
            g_mobsStatus = "mobs array not found";
            return false;
        }

        g_mobsById.clear();
        g_mobNameToId.clear();
        g_itemToMobs.clear();
        g_allMobNames.clear();

        size_t i = arrStart + 1;
        int loaded = 0;
        while (i < raw.size()) {
            while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\n' || raw[i] == '\r' || raw[i] == '\t' || raw[i] == ',')) ++i;
            if (i >= raw.size() || raw[i] == ']') break;
            if (raw[i] != '{') { ++i; continue; }
            std::string obj;
            if (!ExtractBalanced(raw, i, obj)) break;
            i += obj.size();

            MobInfo info;
            if (!JsonExtractObjectString(obj, "in_game_identifier", info.id) || info.id.empty())
                JsonExtractObjectString(obj, "id", info.id);
            JsonExtractObjectString(obj, "name", info.name);
            if (info.name.empty()) info.name = info.id;
            JsonExtractObjectString(obj, "summary", info.summary);

            size_t propPos = obj.find("\"properties\"");
            if (propPos != std::string::npos) {
                size_t brace = obj.find('{', propPos);
                std::string props;
                if (brace != std::string::npos && ExtractBalanced(obj, brace, props)) {
                    info.max_health = JsonExtractInt(props, "max_health");
                    info.view_range = JsonExtractInt(props, "view_range_blocks");
                    info.hearing_range = JsonExtractInt(props, "hearing_range");
                    info.speed = JsonExtractFloat(props, "speed_blocks_per_s");
                }
            }

            size_t traitPos = obj.find("\"traits\"");
            if (traitPos != std::string::npos) {
                size_t brace = obj.find('{', traitPos);
                std::string traits;
                if (brace != std::string::npos && ExtractBalanced(obj, brace, traits)) {
                    info.timid = JsonExtractBool(traits, "timid");
                    info.mountable = JsonExtractBool(traits, "mountable");
                    info.spawns_at_night = JsonExtractBool(traits, "spawns_at_night");
                    info.attack_when_startled = JsonExtractBool(traits, "attack_when_startled");
                    info.saved_as_memory = JsonExtractBool(traits, "saved_as_memory");
                }
            }

            size_t dropsPos = obj.find("\"drops\"");
            if (dropsPos != std::string::npos) {
                size_t bracket = obj.find('[', dropsPos);
                std::string dropsArr;
                if (bracket != std::string::npos && ExtractBalanced(obj, bracket, dropsArr)) {
                    size_t di = 1;
                    while (di < dropsArr.size()) {
                        while (di < dropsArr.size() && (dropsArr[di] == ' ' || dropsArr[di] == '\n' || dropsArr[di] == '\r' || dropsArr[di] == '\t' || dropsArr[di] == ',')) ++di;
                        if (di >= dropsArr.size() || dropsArr[di] == ']') break;
                        if (dropsArr[di] != '{') { ++di; continue; }
                        std::string dobj;
                        if (!ExtractBalanced(dropsArr, di, dobj)) break;
                        di += dobj.size();
                        MobDrop drop;
                        JsonExtractObjectString(dobj, "item_name", drop.item_name);
                        JsonExtractObjectString(dobj, "item_id", drop.item_id);
                        drop.qty_min = JsonExtractInt(dobj, "quantity_min");
                        drop.qty_max = JsonExtractInt(dobj, "quantity_max");
                        drop.chance_pct = JsonExtractInt(dobj, "chance_pct", 100);
                        info.drops.push_back(std::move(drop));
                    }
                }
            }

            if (info.id.empty()) continue;
            std::string idKey = NormalizeKey(info.id);
            std::string nameKey = NormalizeKey(info.name);
            g_mobsById[idKey] = std::move(info);
            const MobInfo& stored = g_mobsById[idKey];
            g_mobNameToId[nameKey] = idKey;
            g_mobNameToId[idKey] = idKey;
            g_allMobNames.emplace_back(nameKey, idKey);
            for (const auto& d : stored.drops) {
                auto indexDrop = [&](const std::string& raw) {
                    if (raw.empty()) return;
                    std::string k1 = NormalizeKey(raw);
                    std::string k2 = FlattenKey(raw);
                    if (!k1.empty()) g_itemToMobs[k1].push_back(idKey);
                    if (!k2.empty() && k2 != k1) g_itemToMobs[k2].push_back(idKey);
                    // also index without stop-words joined (e.g. "essence void")
                    auto toks = TokenizeKey(k2);
                    std::string nos;
                    for (const auto& t : toks) {
                        if (IsStopToken(t)) continue;
                        if (!nos.empty()) nos += ' ';
                        nos += t;
                    }
                    if (!nos.empty() && nos != k2)
                        g_itemToMobs[nos].push_back(idKey);
                    };
                indexDrop(d.item_name);
                indexDrop(d.item_id);
            }
            ++loaded;
        }

        for (auto& kv : g_itemToMobs) {
            auto& v = kv.second;
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }

        g_mobsLoaded = true;
        g_mobsStatus = "loaded " + std::to_string(loaded) + " mobs / "
            + std::to_string(g_itemToMobs.size()) + " drop items";
        LogToFile("Mob KB: " + g_mobsStatus);
        return loaded > 0;
    }

    static bool LoadItems(bool force) {
        if (g_itemsLoaded.load() && !force) return true;
        std::lock_guard lock(g_itemsMtx);
        if (g_itemsLoaded.load() && !force) return true;

        const std::wstring wpath = GetThisModuleDirectory() + L"hytale_items.json";
        std::ifstream file(wpath, std::ios::binary);
        if (!file.is_open()) {
            g_itemsStatus = "hytale_items.json not found next to DLL";
            LogToFile(g_itemsStatus);
            return false;
        }
        std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        size_t itemsKey = raw.find("\"items\"");
        if (itemsKey == std::string::npos) {
            g_itemsStatus = "no \"items\" key in JSON";
            LogToFile(g_itemsStatus);
            return false;
        }
        size_t arrStart = raw.find('[', itemsKey);
        if (arrStart == std::string::npos) {
            g_itemsStatus = "items array not found";
            return false;
        }

        g_itemsById.clear();
        g_itemNameToId.clear();
        g_allItemNames.clear();

        size_t i = arrStart + 1;
        int loaded = 0;
        while (i < raw.size()) {
            while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\n' || raw[i] == '\r' || raw[i] == '\t' || raw[i] == ',')) ++i;
            if (i >= raw.size() || raw[i] == ']') break;
            if (raw[i] != '{') { ++i; continue; }
            std::string obj;
            if (!ExtractBalanced(raw, i, obj)) break;
            i += obj.size();

            ItemInfo info;
            if (!JsonExtractObjectString(obj, "in_game_identifier", info.id) || info.id.empty())
                JsonExtractObjectString(obj, "id", info.id);
            JsonExtractObjectString(obj, "name", info.name);
            if (info.name.empty()) info.name = info.id;
            JsonExtractObjectString(obj, "description", info.description);
            JsonExtractObjectString(obj, "summary", info.summary);
            info.is_bench = JsonExtractBool(obj, "is_bench");

            // properties: quality.name, item_level
            size_t propPos = obj.find("\"properties\"");
            if (propPos != std::string::npos) {
                size_t brace = obj.find('{', propPos);
                std::string props;
                if (brace != std::string::npos && ExtractBalanced(obj, brace, props)) {
                    info.item_level = JsonExtractInt(props, "item_level");
                    size_t qpos = props.find("\"quality\"");
                    if (qpos != std::string::npos) {
                        size_t qb = props.find('{', qpos);
                        std::string qobj;
                        if (qb != std::string::npos && ExtractBalanced(props, qb, qobj))
                            JsonExtractObjectString(qobj, "name", info.quality);
                    }
                }
            }

            // categories array of objects with "name"
            size_t catPos = obj.find("\"categories\"");
            if (catPos != std::string::npos) {
                size_t bracket = obj.find('[', catPos);
                std::string carr;
                if (bracket != std::string::npos && ExtractBalanced(obj, bracket, carr)) {
                    size_t ci = 1;
                    while (ci < carr.size()) {
                        while (ci < carr.size() && (carr[ci] == ' ' || carr[ci] == '\n' || carr[ci] == '\r' || carr[ci] == '\t' || carr[ci] == ',')) ++ci;
                        if (ci >= carr.size() || carr[ci] == ']') break;
                        if (carr[ci] != '{') { ++ci; continue; }
                        std::string cobj;
                        if (!ExtractBalanced(carr, ci, cobj)) break;
                        ci += cobj.size();
                        std::string cname;
                        JsonExtractObjectString(cobj, "name", cname);
                        if (!cname.empty()) info.categories.push_back(cname);
                    }
                }
            }

            // recipes (keep short - first few)
            size_t recPos = obj.find("\"recipes\"");
            if (recPos != std::string::npos) {
                size_t bracket = obj.find('[', recPos);
                std::string rarr;
                if (bracket != std::string::npos && ExtractBalanced(obj, bracket, rarr)) {
                    size_t ri = 1;
                    int recCount = 0;
                    while (ri < rarr.size() && recCount < 3) {
                        while (ri < rarr.size() && (rarr[ri] == ' ' || rarr[ri] == '\n' || rarr[ri] == '\r' || rarr[ri] == '\t' || rarr[ri] == ',')) ++ri;
                        if (ri >= rarr.size() || rarr[ri] == ']') break;
                        if (rarr[ri] != '{') { ++ri; continue; }
                        std::string robj;
                        if (!ExtractBalanced(rarr, ri, robj)) break;
                        ri += robj.size();
                        ItemRecipe recipe;
                        recipe.duration_s = JsonExtractInt(robj, "duration_s");
                        // ingredients
                        size_t ingPos = robj.find("\"ingredients\"");
                        if (ingPos != std::string::npos) {
                            size_t ib = robj.find('[', ingPos);
                            std::string iarr;
                            if (ib != std::string::npos && ExtractBalanced(robj, ib, iarr)) {
                                size_t ii = 1;
                                while (ii < iarr.size()) {
                                    while (ii < iarr.size() && (iarr[ii] == ' ' || iarr[ii] == '\n' || iarr[ii] == '\r' || iarr[ii] == '\t' || iarr[ii] == ',')) ++ii;
                                    if (ii >= iarr.size() || iarr[ii] == ']') break;
                                    if (iarr[ii] != '{') { ++ii; continue; }
                                    std::string iobj;
                                    if (!ExtractBalanced(iarr, ii, iobj)) break;
                                    ii += iobj.size();
                                    ItemRecipeIng ing;
                                    ing.quantity = JsonExtractInt(iobj, "quantity");
                                    JsonExtractObjectString(iobj, "name", ing.name);
                                    JsonExtractObjectString(iobj, "id", ing.id);
                                    recipe.ingredients.push_back(std::move(ing));
                                }
                            }
                        }
                        // benches
                        size_t bPos = robj.find("\"benches\"");
                        if (bPos != std::string::npos) {
                            size_t bb = robj.find('[', bPos);
                            std::string barr;
                            if (bb != std::string::npos && ExtractBalanced(robj, bb, barr)) {
                                size_t bi = 1;
                                while (bi < barr.size()) {
                                    while (bi < barr.size() && (barr[bi] == ' ' || barr[bi] == '\n' || barr[bi] == '\r' || barr[bi] == '\t' || barr[bi] == ',')) ++bi;
                                    if (bi >= barr.size() || barr[bi] == ']') break;
                                    if (barr[bi] != '{') { ++bi; continue; }
                                    std::string bobj;
                                    if (!ExtractBalanced(barr, bi, bobj)) break;
                                    bi += bobj.size();
                                    std::string bname;
                                    JsonExtractObjectString(bobj, "name", bname);
                                    if (!bname.empty()) recipe.benches.push_back(bname);
                                }
                            }
                        }
                        info.recipes.push_back(std::move(recipe));
                        ++recCount;
                    }
                }
            }

            if (info.id.empty()) continue;
            std::string idKey = NormalizeKey(info.id);
            std::string nameKey = NormalizeKey(info.name);
            g_itemsById[idKey] = std::move(info);
            g_itemNameToId[nameKey] = idKey;
            g_itemNameToId[idKey] = idKey;
            g_allItemNames.emplace_back(nameKey, idKey);
            ++loaded;
        }

        g_itemsLoaded = true;
        g_itemsStatus = "loaded " + std::to_string(loaded) + " items";
        LogToFile("Item KB: " + g_itemsStatus);
        return loaded > 0;
    }

    bool Load(bool force) {
        bool m = LoadMobs(force);
        bool it = LoadItems(force);
        return m || it;
    }

    // ---- Mob find / format / tools ----
    static const MobInfo* FindMobExact(const std::string& query) {
        std::string q = NormalizeKey(query);
        auto it = g_mobsById.find(q);
        if (it != g_mobsById.end()) return &it->second;
        auto nit = g_mobNameToId.find(q);
        if (nit != g_mobNameToId.end()) {
            auto it2 = g_mobsById.find(nit->second);
            if (it2 != g_mobsById.end()) return &it2->second;
        }
        return nullptr;
    }

    static const MobInfo* FindMobFuzzy(const std::string& query, int maxDist = 3) {
        if (const MobInfo* exact = FindMobExact(query)) return exact;
        std::string q = NormalizeKey(query);
        if (q.size() < 2) return nullptr;
        const MobInfo* best = nullptr;
        int bestDist = maxDist + 1;
        for (const auto& pr : g_allMobNames) {
            const std::string& name = pr.first;
            const std::string& id = pr.second;
            if (name.find(q) != std::string::npos || q.find(name) != std::string::npos) {
                auto it = g_mobsById.find(id);
                if (it != g_mobsById.end()) return &it->second;
            }
            int d = SimpleEditDistance(q, name);
            if (d < bestDist) {
                bestDist = d;
                auto it = g_mobsById.find(id);
                if (it != g_mobsById.end()) best = &it->second;
            }
        }
        return best;
    }

    static std::string FormatMobCompact(const MobInfo& m) {
        std::ostringstream ss;
        ss << m.name << " (" << m.id << ")\n";
        if (m.max_health > 0) ss << "Health: " << m.max_health;
        if (m.view_range > 0) ss << " | View: " << m.view_range;
        if (m.hearing_range > 0) ss << " | Hearing: " << m.hearing_range;
        if (m.speed > 0.f) ss << " | Speed: " << m.speed;
        ss << "\n";
        std::vector<std::string> traits;
        if (m.timid) traits.push_back("timid");
        if (m.mountable) traits.push_back("mountable");
        if (m.spawns_at_night) traits.push_back("spawns at night");
        if (m.attack_when_startled) traits.push_back("attacks when startled");
        if (m.saved_as_memory) traits.push_back("saved as memory");
        if (!traits.empty()) {
            ss << "Traits: ";
            for (size_t i = 0; i < traits.size(); ++i) {
                if (i) ss << ", ";
                ss << traits[i];
            }
            ss << "\n";
        }
        if (!m.drops.empty()) {
            ss << "Drops:\n";
            for (const auto& d : m.drops) {
                ss << "- ";
                if (d.qty_min == d.qty_max && d.qty_min > 0)
                    ss << d.qty_min << "x ";
                else if (d.qty_max > 0)
                    ss << d.qty_min << "-" << d.qty_max << "x ";
                ss << (d.item_name.empty() ? d.item_id : d.item_name);
                if (d.chance_pct > 0 && d.chance_pct < 100)
                    ss << " (" << d.chance_pct << "%)";
                ss << "\n";
            }
        }
        else {
            ss << "Drops: none listed\n";
        }
        return ss.str();
    }

    std::string ToolLookupMob(const std::string& query) {
        LoadMobs(false);
        const MobInfo* m = FindMobFuzzy(query);
        if (!m) return "No mob found matching \"" + query + "\". Try another name or in-game ID.";
        return FormatMobCompact(*m);
    }

    std::string ToolSearchMobs(const std::string& query,
        const std::string& dropsItem,
        const std::string& trait,
        int minHealth, int maxHealth,
        int limit) {
        LoadMobs(false);
        if (limit <= 0 || limit > 20) limit = 8;
        std::vector<const MobInfo*> results;
        std::set<std::string> seen;

        auto add = [&](const MobInfo* m) {
            if (!m || seen.count(m->id)) return;
            if (minHealth > 0 && m->max_health < minHealth) return;
            if (maxHealth > 0 && m->max_health > maxHealth) return;
            if (!trait.empty()) {
                std::string t = NormalizeKey(trait);
                bool ok = false;
                if (t == "timid" && m->timid) ok = true;
                else if (t == "mountable" && m->mountable) ok = true;
                else if ((t == "spawns_at_night" || t == "night" || t == "spawns at night") && m->spawns_at_night) ok = true;
                else if ((t == "attack_when_startled" || t == "startled" || t == "attacks when startled") && m->attack_when_startled) ok = true;
                if (!ok) return;
            }
            seen.insert(m->id);
            results.push_back(m);
            };

        if (!dropsItem.empty()) {
            std::string key = NormalizeKey(dropsItem);
            std::string keyFlat = FlattenKey(dropsItem);

            // Exact / flattened key lookup first
            for (const std::string& tryKey : { key, keyFlat }) {
                if (tryKey.empty()) continue;
                auto it = g_itemToMobs.find(tryKey);
                if (it != g_itemToMobs.end()) {
                    for (const auto& id : it->second) {
                        auto mit = g_mobsById.find(id);
                        if (mit != g_mobsById.end()) add(&mit->second);
                    }
                }
            }

            // Fuzzy: token match against every known drop item key + also scan mob drops directly
            if (results.empty()) {
                for (const auto& kv : g_itemToMobs) {
                    if (ItemKeyMatches(kv.first, dropsItem)) {
                        for (const auto& id : kv.second) {
                            auto mit = g_mobsById.find(id);
                            if (mit != g_mobsById.end()) add(&mit->second);
                        }
                    }
                }
            }
            // Last resort: walk every mob's drop list (catches odd name forms)
            if (results.empty()) {
                for (const auto& kv : g_mobsById) {
                    const MobInfo& m = kv.second;
                    for (const auto& d : m.drops) {
                        if (ItemKeyMatches(d.item_name, dropsItem) || ItemKeyMatches(d.item_id, dropsItem)) {
                            add(&m);
                            break;
                        }
                    }
                }
            }
        }

        std::string q = NormalizeKey(query);
        bool needScan = dropsItem.empty() || !q.empty() || !trait.empty() || minHealth > 0 || maxHealth > 0;
        if (needScan && (dropsItem.empty() || results.empty())) {
            for (const auto& kv : g_mobsById) {
                if ((int)results.size() >= limit * 2) break;
                const MobInfo& m = kv.second;
                if (!q.empty()) {
                    std::string hay = NormalizeKey(m.name + " " + m.id + " " + m.summary);
                    if (hay.find(q) == std::string::npos) continue;
                }
                add(&m);
            }
        }

        if (results.empty())
            return "No mobs matched the search criteria.";
        if ((int)results.size() > limit)
            results.resize(limit);

        std::ostringstream ss;
        ss << "Found " << results.size() << " mob(s):\n\n";
        for (const MobInfo* m : results)
            ss << FormatMobCompact(*m) << "\n";
        return ss.str();
    }

    // ---- Item find / format / tools ----
    static const ItemInfo* FindItemExact(const std::string& query) {
        std::string q = NormalizeKey(query);
        auto it = g_itemsById.find(q);
        if (it != g_itemsById.end()) return &it->second;
        auto nit = g_itemNameToId.find(q);
        if (nit != g_itemNameToId.end()) {
            auto it2 = g_itemsById.find(nit->second);
            if (it2 != g_itemsById.end()) return &it2->second;
        }
        return nullptr;
    }

    static const ItemInfo* FindItemFuzzy(const std::string& query, int maxDist = 3) {
        if (const ItemInfo* exact = FindItemExact(query)) return exact;
        std::string q = NormalizeKey(query);
        if (q.size() < 2) return nullptr;
        const ItemInfo* best = nullptr;
        int bestDist = maxDist + 1;
        for (const auto& pr : g_allItemNames) {
            const std::string& name = pr.first;
            const std::string& id = pr.second;
            if (name.find(q) != std::string::npos) {
                auto it = g_itemsById.find(id);
                if (it != g_itemsById.end()) return &it->second;
            }
            int d = SimpleEditDistance(q, name);
            if (d < bestDist) {
                bestDist = d;
                auto it = g_itemsById.find(id);
                if (it != g_itemsById.end()) best = &it->second;
            }
        }
        return best;
    }

    static std::string FormatItemCompact(const ItemInfo& it) {
        std::ostringstream ss;
        ss << it.name << " (" << it.id << ")\n";
        if (!it.quality.empty()) ss << "Quality: " << it.quality;
        if (it.item_level > 0) ss << " | Level: " << it.item_level;
        if (it.is_bench) ss << " | Bench";
        ss << "\n";
        if (!it.categories.empty()) {
            ss << "Categories: ";
            for (size_t i = 0; i < it.categories.size(); ++i) {
                if (i) ss << ", ";
                ss << it.categories[i];
            }
            ss << "\n";
        }
        if (!it.description.empty())
            ss << it.description << "\n";
        if (!it.recipes.empty()) {
            ss << "Craft:\n";
            for (const auto& r : it.recipes) {
                if (!r.benches.empty()) {
                    ss << "  @ ";
                    for (size_t i = 0; i < r.benches.size(); ++i) {
                        if (i) ss << "/";
                        ss << r.benches[i];
                    }
                    ss << ": ";
                }
                else ss << "  ";
                for (size_t i = 0; i < r.ingredients.size(); ++i) {
                    if (i) ss << " + ";
                    const auto& ing = r.ingredients[i];
                    if (ing.quantity > 0) ss << ing.quantity << "x ";
                    ss << (ing.name.empty() ? ing.id : ing.name);
                }
                if (r.duration_s > 0) ss << " (" << r.duration_s << "s)";
                ss << "\n";
            }
        }
        return ss.str();
    }

    std::string ToolLookupItem(const std::string& query) {
        LoadItems(false);
        const ItemInfo* it = FindItemFuzzy(query);
        if (!it) return "No item found matching \"" + query + "\". Try another name or in-game ID.";
        return FormatItemCompact(*it);
    }

    std::string ToolSearchItems(const std::string& query,
        const std::string& quality,
        const std::string& category,
        int limit) {
        LoadItems(false);
        if (limit <= 0 || limit > 20) limit = 8;
        std::string q = NormalizeKey(query);
        std::string qu = NormalizeKey(quality);
        std::string cat = NormalizeKey(category);
        std::vector<const ItemInfo*> results;

        for (const auto& kv : g_itemsById) {
            if ((int)results.size() >= limit) break;
            const ItemInfo& it = kv.second;
            if (!qu.empty() && NormalizeKey(it.quality).find(qu) == std::string::npos)
                continue;
            if (!cat.empty()) {
                bool ok = false;
                for (const auto& c : it.categories)
                    if (NormalizeKey(c).find(cat) != std::string::npos) { ok = true; break; }
                if (!ok) continue;
            }
            if (!q.empty()) {
                std::string hay = NormalizeKey(it.name + " " + it.id + " " + it.description + " " + it.summary);
                if (hay.find(q) == std::string::npos) continue;
            }
            results.push_back(&it);
        }

        if (results.empty())
            return "No items matched the search criteria.";

        std::ostringstream ss;
        ss << "Found " << results.size() << " item(s):\n\n";
        for (const ItemInfo* it : results)
            ss << FormatItemCompact(*it) << "\n";
        return ss.str();
    }

    std::string Status() {
        std::string m, it;
        {
            std::lock_guard lock(g_mobsMtx);
            m = g_mobsStatus;
        }
        {
            std::lock_guard lock(g_itemsMtx);
            it = g_itemsStatus;
        }
        return "Mobs: " + m + " | Items: " + it;
    }

} // namespace Knowledge
