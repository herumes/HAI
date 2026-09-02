#include "pch.h"
#include "ai.hpp"
#include "knowledge.hpp"
#include "common.hpp"
#include "chat.hpp"
#include "settings.hpp"
#include "extras.hpp"

// Chat / memory globals live in legacy_core.cpp (non-static).
extern std::deque<ChatMessage> g_memory;
extern std::deque<GlobalChatEntry> g_globalChat;
extern std::mutex g_mtx;

namespace AI {

    static std::atomic<bool> g_cancelRequested{ false };

    void RequestCancel() {
        g_cancelRequested.store(true);
        AbortHttp();
    }

    bool IsCancelRequested() {
        return g_cancelRequested.load();
    }

    // Base tools (always available). Web search is appended only when enabled.
    static const char* TOOLS_JSON_BASE =
        R"HYTOOLS([{"type":"function","function":{"name":"lookup_mob","description":"Get accurate health, traits and drops for one specific Hytale mob by name or in-game ID. Always call this for questions about a named mob.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Mob display name or in-game ID e.g. Wild Pig, Pig_Wild, Scarak Broodmother, Eye_Void"}},"required":["query"]}}},{"type":"function","function":{"name":"search_mobs","description":"Search or filter mobs by free text, by an item they drop, by trait, or by health range. Use for questions like what drops X, timid animals, high health mobs, etc.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Free-text keywords"},"drops_item":{"type":"string","description":"Item name or ID the mob must drop e.g. Void Essence, Raw Pork"},"trait":{"type":"string","description":"timid | mountable | spawns_at_night | attack_when_startled"},"min_health":{"type":"integer"},"max_health":{"type":"integer"},"limit":{"type":"integer","description":"Max results default 8"}}}}},{"type":"function","function":{"name":"lookup_item","description":"Get accurate info for one Hytale item: quality, level, description, categories, crafting recipe. Always call for questions about a named item or how to craft something.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Item display name or in-game ID e.g. Void Essence, Raw Pork, Farmer's Workbench"}},"required":["query"]}}},{"type":"function","function":{"name":"search_items","description":"Search items by free text, quality, or category.","parameters":{"type":"object","properties":{"query":{"type":"string"},"quality":{"type":"string","description":"Common, Uncommon, Rare, Epic, etc."},"category":{"type":"string","description":"Weapons, Food, etc."},"limit":{"type":"integer","description":"Max results default 8"}}}}},{"type":"function","function":{"name":"get_time","description":"Get the current date/time for a place, timezone, or abbreviation. Use for questions like what time is it in Germany, what time is GMT, CST, Tokyo, London, etc. Empty query = local time on the bot host.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Country, city, timezone name, or abbr e.g. Germany, Berlin, UTC, GMT, CST, PST, EST, CET, Tokyo, London. Empty for local."}}}}},{"type":"function","function":{"name":"set_reminder","description":"Set reminders for users. Convert minutes to seconds if needed (2 minutes=120). user=player name, in_seconds=delay, message=what to say later.","parameters":{"type":"object","properties":{"user":{"type":"string","description":"Player name to address in the reminder"},"in_seconds":{"type":"integer","description":"Delay in seconds (min 5, max 604800)"},"message":{"type":"string","description":"What to say when the reminder fires"}},"required":["user","in_seconds","message"]}}},{"type":"function","function":{"name":"list_reminders","description":"List active reminders, optionally filtered by player name.","parameters":{"type":"object","properties":{"user":{"type":"string","description":"Optional player name filter"}}}}},{"type":"function","function":{"name":"cancel_reminder","description":"Cancel an active reminder by its numeric id.","parameters":{"type":"object","properties":{"id":{"type":"integer","description":"Reminder id from set_reminder / list_reminders"}},"required":["id"]}}}]
)HYTOOLS";

    // Short schema: only present when web search is enabled (saves prompt tokens when off).
    static const char* WEB_SEARCH_TOOL_JSON =
        R"HYWS({"type":"function","function":{"name":"web_search","description":"Live web search for current facts, news, prices, or info not covered by game knowledge/tools. Prefer lookup_mob/lookup_item for Hytale. Use short keyword queries.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Focused keywords, not a full sentence"},"freshness":{"type":"string","description":"oneDay|oneWeek|oneMonth|oneYear|noLimit"}},"required":["query"]}}}
)HYWS";

    static std::string BuildToolsJson() {
        bool ws = false;
        {
            std::lock_guard lock(g_settingsMtx);
            ws = g_webSearchEnabled;
        }
        std::string base = TOOLS_JSON_BASE;
        // Raw string literals may end with "]\n" — trim so we can splice correctly.
        auto rtrim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r'
                || s.back() == ' ' || s.back() == '\t'))
                s.pop_back();
            };
        rtrim(base);
        if (!ws) return base;

        if (!base.empty() && base.back() == ']')
            base.pop_back();
        std::string web = WEB_SEARCH_TOOL_JSON;
        rtrim(web);
        base += ",";
        base += web;
        base += "]";
        return base;
    }

    void EnsureKnowledgeLoaded() {
        Knowledge::Load(false);
    }

    // Extract a balanced {...} or [...] starting at openIdx (which must be '{' or '[')
    static bool ExtractBalanced(const std::string& s, size_t openIdx, std::string& out) {
        out.clear();
        if (openIdx >= s.size()) return false;
        char open = s[openIdx];
        char close = (open == '{') ? '}' : (open == '[') ? ']' : 0;
        if (!close) return false;
        int depth = 0;
        bool inStr = false, esc = false;
        for (size_t i = openIdx; i < s.size(); ++i) {
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
                    out = s.substr(openIdx, i - openIdx + 1);
                    return true;
                }
            }
        }
        return false;
    }

    static std::string ExtractToolArgString(const std::string& argsJson, const char* key) {
        std::string val;
        ExtractJsonStringField(argsJson, key, val);
        return val;
    }

    static int ExtractToolArgInt(const std::string& argsJson, const char* key, int def = 0) {
        std::string pat = std::string("\"") + key + "\"";
        size_t pos = argsJson.find(pat);
        if (pos == std::string::npos) return def;
        pos = argsJson.find(':', pos + pat.size());
        if (pos == std::string::npos) return def;
        ++pos;
        while (pos < argsJson.size() && (argsJson[pos] == ' ' || argsJson[pos] == '\t')) ++pos;
        if (pos >= argsJson.size() || argsJson[pos] == 'n') return def;
        try {
            size_t end = pos;
            while (end < argsJson.size() && (isdigit((unsigned char)argsJson[end]) || argsJson[end] == '-' || argsJson[end] == '+')) ++end;
            if (end == pos) return def;
            return std::stoi(argsJson.substr(pos, end - pos));
        }
        catch (...) { return def; }
    }

    static std::string ExecuteTool(const std::string& name, const std::string& argsJson) {
        if (name == "lookup_mob") {
            return Knowledge::ToolLookupMob(ExtractToolArgString(argsJson, "query"));
        }
        if (name == "search_mobs") {
            return Knowledge::ToolSearchMobs(
                ExtractToolArgString(argsJson, "query"),
                ExtractToolArgString(argsJson, "drops_item"),
                ExtractToolArgString(argsJson, "trait"),
                ExtractToolArgInt(argsJson, "min_health", 0),
                ExtractToolArgInt(argsJson, "max_health", 0),
                ExtractToolArgInt(argsJson, "limit", 8)
            );
        }
        if (name == "lookup_item") {
            return Knowledge::ToolLookupItem(ExtractToolArgString(argsJson, "query"));
        }
        if (name == "search_items") {
            return Knowledge::ToolSearchItems(
                ExtractToolArgString(argsJson, "query"),
                ExtractToolArgString(argsJson, "quality"),
                ExtractToolArgString(argsJson, "category"),
                ExtractToolArgInt(argsJson, "limit", 8)
            );
        }
        if (name == "get_time") {
            return Extras::ToolGetTime(ExtractToolArgString(argsJson, "query"));
        }
        if (name == "set_reminder") {
            return Extras::ToolSetReminder(
                ExtractToolArgString(argsJson, "user"),
                ExtractToolArgInt(argsJson, "in_seconds", 0),
                ExtractToolArgString(argsJson, "message")
            );
        }
        if (name == "list_reminders") {
            return Extras::ToolListReminders(ExtractToolArgString(argsJson, "user"));
        }
        if (name == "cancel_reminder") {
            return Extras::ToolCancelReminder((uint64_t)ExtractToolArgInt(argsJson, "id", 0));
        }
        if (name == "web_search") {
            return Extras::ToolWebSearch(
                ExtractToolArgString(argsJson, "query"),
                ExtractToolArgString(argsJson, "freshness")
            );
        }
        return "Unknown tool: " + name;
    }

    // ---------------------------------------------------------------------------
    // URL parse for OpenAI base (scheme://host[:port])
    // ---------------------------------------------------------------------------
    struct ParsedUrl {
        bool https = false;
        std::wstring host;
        INTERNET_PORT port = 443;
        std::wstring path; // always /v1/chat/completions for us
    };

    static bool ParseOpenAiBase(const std::string& baseIn, ParsedUrl& out) {
        out = ParsedUrl{};
        out.path = L"/v1/chat/completions";
        std::string base = TrimCopy(baseIn);
        if (base.empty()) return false;

        // strip trailing slash
        while (!base.empty() && (base.back() == '/' || base.back() == '\\'))
            base.pop_back();

        // If user pasted full path ending in /v1 or /v1/chat/completions, strip it
        {
            std::string lower = ToLower(base);
            const char* suffixes[] = {
                "/v1/chat/completions", "/v1/chat", "/v1"
            };
            for (const char* suf : suffixes) {
                size_t n = strlen(suf);
                if (lower.size() >= n && lower.compare(lower.size() - n, n, suf) == 0) {
                    base.resize(base.size() - n);
                    while (!base.empty() && base.back() == '/') base.pop_back();
                    break;
                }
            }
        }

        bool https = false;
        std::string rest = base;
        if (StartsWithCI(rest, "https://")) {
            https = true;
            rest = rest.substr(8);
        }
        else if (StartsWithCI(rest, "http://")) {
            https = false;
            rest = rest.substr(7);
        }
        else {
            // assume https if no scheme
            https = true;
        }

        // host[:port][/path leftover]
        size_t slash = rest.find('/');
        std::string hostPort = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        if (slash != std::string::npos && slash + 1 < rest.size()) {
            // leftover path after host — keep as path prefix before /v1/...
            std::string extra = rest.substr(slash);
            while (!extra.empty() && extra.back() == '/') extra.pop_back();
            out.path = Utf8ToWide(extra + "/v1/chat/completions");
        }

        size_t colon = hostPort.rfind(':');
        // IPv6 not supported here; simple host:port
        std::string host = hostPort;
        int port = https ? 443 : 80;
        if (colon != std::string::npos && colon + 1 < hostPort.size()) {
            bool allDigit = true;
            for (size_t i = colon + 1; i < hostPort.size(); ++i) {
                if (!isdigit((unsigned char)hostPort[i])) { allDigit = false; break; }
            }
            if (allDigit) {
                host = hostPort.substr(0, colon);
                try { port = std::stoi(hostPort.substr(colon + 1)); }
                catch (...) { port = https ? 443 : 80; }
            }
        }
        if (host.empty()) return false;
        if (port <= 0 || port > 65535) port = https ? 443 : 80;

        out.https = https;
        out.host = Utf8ToWide(host);
        out.port = (INTERNET_PORT)port;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Parse assistant message (Ollama nests under "message"; OpenAI under choices[0].message)
    // ---------------------------------------------------------------------------
    static void ParseAssistantMessage(const std::string& accumulated, bool openai,
        std::string& content, std::string& toolCallsRaw, bool& hasTools)
    {
        content.clear();
        toolCallsRaw.clear();
        hasTools = false;

        std::string messageObj;
        if (openai) {
            // choices[0].message
            size_t chKey = accumulated.find("\"choices\"");
            if (chKey != std::string::npos) {
                size_t arr = accumulated.find('[', chKey);
                if (arr != std::string::npos) {
                    size_t brace = accumulated.find('{', arr);
                    if (brace != std::string::npos) {
                        std::string choiceObj;
                        if (ExtractBalanced(accumulated, brace, choiceObj)) {
                            size_t msgKey = choiceObj.find("\"message\"");
                            if (msgKey != std::string::npos) {
                                size_t mb = choiceObj.find('{', msgKey);
                                if (mb != std::string::npos)
                                    ExtractBalanced(choiceObj, mb, messageObj);
                            }
                        }
                    }
                }
            }
        }
        else {
            size_t msgKey = accumulated.find("\"message\"");
            if (msgKey != std::string::npos) {
                size_t brace = accumulated.find('{', msgKey);
                if (brace != std::string::npos)
                    ExtractBalanced(accumulated, brace, messageObj);
            }
        }

        const std::string& src = !messageObj.empty() ? messageObj : accumulated;
        ExtractJsonStringField(src, "content", content);

        // Log thinking for diagnostics but NEVER promote it to the user-visible reply.
        {
            std::string thinking;
            ExtractJsonStringField(src, "thinking", thinking);
            if (!thinking.empty()) {
                LogToFile("message.thinking present (" + std::to_string(thinking.size()) + " chars) — ignored for reply");
            }
        }

        size_t tcPos = src.find("\"tool_calls\"");
        if (tcPos != std::string::npos) {
            size_t arr = src.find('[', tcPos);
            if (arr != std::string::npos) {
                if (ExtractBalanced(src, arr, toolCallsRaw) && toolCallsRaw.size() > 2)
                    hasTools = true;
            }
        }
    }

    // Process tool_calls array and append tool role messages to messagesJson
    static void AppendToolResults(std::string& messagesJson, const std::string& content,
        const std::string& toolCallsRaw, bool openaiStyle)
    {
        if (!messagesJson.empty() && messagesJson.back() == ']')
            messagesJson.pop_back();

        // Assistant message that requested the tools
        messagesJson += ",{\"role\":\"assistant\",\"content\":\"" + JsonEscape(content)
            + "\",\"tool_calls\":" + toolCallsRaw + "}";

        size_t ti = 1; // skip leading '['
        while (ti < toolCallsRaw.size()) {
            if (g_cancelRequested.load() || g_shuttingDown.load()) break;

            while (ti < toolCallsRaw.size()
                && (toolCallsRaw[ti] == ' ' || toolCallsRaw[ti] == '\n'
                    || toolCallsRaw[ti] == '\r' || toolCallsRaw[ti] == '\t'
                    || toolCallsRaw[ti] == ',')) {
                ++ti;
            }
            if (ti >= toolCallsRaw.size() || toolCallsRaw[ti] == ']') break;
            if (toolCallsRaw[ti] != '{') { ++ti; continue; }

            std::string tcObj;
            if (!ExtractBalanced(toolCallsRaw, ti, tcObj)) break;
            ti += tcObj.size();

            std::string toolId, toolName, argsStr;
            ExtractJsonStringField(tcObj, "id", toolId);

            size_t fnPos = tcObj.find("\"function\"");
            if (fnPos != std::string::npos) {
                size_t brace = tcObj.find('{', fnPos);
                std::string fnObj;
                if (brace != std::string::npos && ExtractBalanced(tcObj, brace, fnObj)) {
                    ExtractJsonStringField(fnObj, "name", toolName);
                    size_t argPos = fnObj.find("\"arguments\"");
                    if (argPos != std::string::npos) {
                        size_t colon = fnObj.find(':', argPos);
                        if (colon != std::string::npos) {
                            size_t p = colon + 1;
                            while (p < fnObj.size() && (fnObj[p] == ' ' || fnObj[p] == '\t')) ++p;
                            if (p < fnObj.size() && fnObj[p] == '"') {
                                ExtractJsonStringField(fnObj, "arguments", argsStr);
                            }
                            else if (p < fnObj.size() && fnObj[p] == '{') {
                                ExtractBalanced(fnObj, p, argsStr);
                            }
                        }
                    }
                }
            }

            if (toolId.empty()) toolId = "call_" + std::to_string(ti);
            LogToFile("Tool call: " + toolName + " args=" + argsStr.substr(0, 200));
            std::string result = ExecuteTool(toolName, argsStr);
            LogToFile("Tool result (" + std::to_string(result.size()) + " chars): " + result.substr(0, 150));

            // OpenAI and Ollama both accept role=tool with tool_call_id
            messagesJson += ",{\"role\":\"tool\",\"tool_call_id\":\"" + JsonEscape(toolId)
                + "\",\"name\":\"" + JsonEscape(toolName)
                + "\",\"content\":\"" + JsonEscape(result) + "\"}";
            (void)openaiStyle;
        }
        messagesJson += "]";
    }

    // ---------------------------------------------------------------------------
    // HTTP POST helper
    // ---------------------------------------------------------------------------
    static bool HttpPostJson(const std::wstring& host, INTERNET_PORT port, bool https,
        const std::wstring& path, const std::wstring& extraHeaders,
        const std::string& payload, std::string& accumulated, DWORD& statusCode)
    {
        accumulated.clear();
        statusCode = 0;

        HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
        if (!hSession) {
            LogToFile("WinHttpOpen failed");
            return false;
        }
        g_aiSession.store(hSession);
        WinHttpSetTimeouts(hSession, 30000, 30000, 30000, 180000);

        // Prefer TLS 1.2+
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
        if (!hConnect) {
            CloseHttp(g_aiSession);
            LogToFile("WinHttpConnect failed");
            return false;
        }
        g_aiConnect.store(hConnect);

        DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hReq) {
            CloseHttp(g_aiConnect);
            CloseHttp(g_aiSession);
            LogToFile("WinHttpOpenRequest failed");
            return false;
        }
        g_aiRequest.store(hReq);

        std::wstring headers = L"Content-Type: application/json\r\n";
        headers += extraHeaders;

        bool httpOk = false;
        if (WinHttpSendRequest(hReq, headers.c_str(), (DWORD)-1,
            (LPVOID)payload.data(), (DWORD)payload.size(), (DWORD)payload.size(), 0)
            && WinHttpReceiveResponse(hReq, nullptr)) {

            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

            char buffer[8192];
            DWORD bytesRead = 0;
            while (!g_shuttingDown.load() && !g_cancelRequested.load()
                && WinHttpReadData(hReq, buffer, sizeof(buffer) - 1, &bytesRead)
                && bytesRead > 0) {
                accumulated.append(buffer, bytesRead);
            }
            if (statusCode == 200) httpOk = true;
            else LogToFile("Error body: " + accumulated.substr(0, 500));
        }
        else {
            LogToFile("WinHttpSend/Receive failed, err=" + std::to_string(GetLastError()));
        }

        CloseHttp(g_aiRequest);
        CloseHttp(g_aiConnect);
        CloseHttp(g_aiSession);
        return httpOk;
    }

    bool GetResponse(const std::string& userPrompt, std::string& response) {
        LogToFile("=== AI Request start ===");
        response.clear();
        g_cancelRequested.store(false);

        if (userPrompt.empty() || g_shuttingDown.load()) return false;
        LogToFile("Prompt: " + userPrompt);

        EnsureKnowledgeLoaded();

        const std::string systemPrompt = GetCachedSystemPrompt();
        if (!systemPrompt.empty())
            LogToFile("System prompt loaded, length: " + std::to_string(systemPrompt.size()));
        else
            LogToFile("No CONTEXT.md found, proceeding without system prompt");

        int currentMaxMem = 4, currentMaxChat = 20;
        bool includeGlobal = true;
        std::string model, host, thinkLvl, provider, openaiBase, openaiKey, openaiModel;
        int port = 11434;
        bool thinkOn = false;
        float temperature = 1.0f;
        int numPredict = 512;
        int numCtx = 32768;
        {
            std::lock_guard lock(g_settingsMtx);
            currentMaxMem = g_maxMemory;
            currentMaxChat = g_maxChatContext;
            includeGlobal = g_includeGlobalChat;
            model = g_ollamaModel;
            host = g_ollamaHost;
            port = g_ollamaPort;
            thinkOn = g_thinkEnabled;
            thinkLvl = g_thinkLevel;
            temperature = g_ollamaTemperature;
            numPredict = g_ollamaNumPredict;
            numCtx = g_ollamaNumCtx;
            provider = g_aiProvider;
            openaiBase = g_openaiBaseUrl;
            openaiKey = g_openaiApiKey;
            openaiModel = g_openaiModel;
        }

        const bool useOpenAI = (ToLower(TrimCopy(provider)) == "openai");

        if (useOpenAI) {
            if (openaiModel.empty()) openaiModel = "gpt-4o-mini";
            if (openaiBase.empty()) openaiBase = "https://api.openai.com";
            model = openaiModel;
        }
        else {
            if (model.empty()) model = "gemma4:e2b";
            if (host.empty()) host = "localhost";
            if (port <= 0 || port > 65535) port = 11434;
        }
        if (temperature < 0.f) temperature = 0.f;
        if (temperature > 2.f) temperature = 2.f;
        if (numPredict < 16) numPredict = 16;
        if (numPredict > 16549) numPredict = 16549;
        if (numCtx < 2048) numCtx = 2048;
        if (numCtx > 131072) numCtx = 131072;

        std::string globalBlock;
        if (includeGlobal) {
            std::lock_guard lock(g_mtx);
            const int n = (int)g_globalChat.size();
            const int startIdx = (n > currentMaxChat) ? (n - currentMaxChat) : 0;
            for (int i = startIdx; i < n; ++i) {
                const auto& e = g_globalChat[(size_t)i];
                if (!e.speaker.empty()) globalBlock += e.speaker + ": " + e.body + "\n";
                else globalBlock += e.plain + "\n";
            }
        }
        if (!globalBlock.empty())
            LogToFile("Global chat context chars: " + std::to_string(globalBlock.size()));

        std::string composed = userPrompt;
        if (!globalBlock.empty()) {
            composed = "Recent server chat (context only, do not repeat it):\n"
                + globalBlock
                + "\nThe latest message that mentioned you (reply to this, naturally, in character):\n"
                + userPrompt;
        }
        Extras::SetLastComposedPrompt(composed);

        // messages JSON array
        std::string messagesJson = "[";
        bool needComma = false;
        auto pushMsg = [&](const std::string& role, const std::string& content, const std::string& extraJson = "") {
            if (needComma) messagesJson += ",";
            needComma = true;
            messagesJson += "{\"role\":\"" + role + "\",\"content\":\"" + JsonEscape(content) + "\"";
            if (!extraJson.empty()) messagesJson += extraJson;
            messagesJson += "}";
            };
        if (!systemPrompt.empty())
            pushMsg("system", systemPrompt);
        {
            std::lock_guard lock(g_mtx);
            for (const auto& m : g_memory)
                pushMsg(m.role, m.content);
        }
        pushMsg("user", composed);
        messagesJson += "]";

        const std::string toolsJson = BuildToolsJson();
        const int MAX_ROUNDS = 4;
        bool success = false;

        ParsedUrl oaUrl;
        if (useOpenAI) {
            if (!ParseOpenAiBase(openaiBase, oaUrl)) {
                LogToFile("Invalid OpenAI base URL: " + openaiBase);
                LogToFile("=== AI Request end ===");
                return false;
            }
            if (openaiKey.empty()) {
                LogToFile("OpenAI API key is empty");
                LogToFile("=== AI Request end ===");
                return false;
            }
        }

        for (int round = 0; round < MAX_ROUNDS; ++round) {
            if (g_shuttingDown.load() || g_cancelRequested.load()) {
                LogToFile("AI cancelled before round " + std::to_string(round));
                break;
            }

            std::string payload;
            if (useOpenAI) {
                // OpenAI-compatible chat completions
                char optBuf[128];
                snprintf(optBuf, sizeof(optBuf),
                    ",\"temperature\":%.3f,\"max_tokens\":%d",
                    (double)temperature, numPredict);
                payload = "{\"model\":\"" + JsonEscape(model)
                    + "\",\"messages\":" + messagesJson
                    + ",\"stream\":false"
                    + ",\"tools\":" + toolsJson
                    + ",\"tool_choice\":\"auto\""
                    + optBuf
                    + "}";
            }
            else {
                payload = "{\"model\":\"" + JsonEscape(model)
                    + "\",\"messages\":" + messagesJson
                    + ",\"stream\":false"
                    + ",\"tools\":" + toolsJson;
                // Always send think explicitly. Gemma4 defaults to thinking when the key is omitted.
                if (thinkOn) {
                    std::string lvl = ToLower(TrimCopy(thinkLvl));
                    if (lvl.empty() || lvl == "true" || lvl == "1" || lvl == "on")
                        payload += ",\"think\":true";
                    else if (lvl == "false" || lvl == "0" || lvl == "off")
                        payload += ",\"think\":false";
                    else
                        payload += ",\"think\":\"" + JsonEscape(lvl) + "\"";
                }
                else {
                    payload += ",\"think\":false";
                }
                {
                    char optBuf[192];
                    snprintf(optBuf, sizeof(optBuf),
                        ",\"options\":{\"temperature\":%.3f,\"num_predict\":%d,\"top_p\":0.95,\"top_k\":64,\"num_ctx\":%d}",
                        (double)temperature, numPredict, numCtx);
                    payload += optBuf;
                }
                payload += "}";
            }

            LogToFile(std::string("Payload flags: provider=") + (useOpenAI ? "openai" : "ollama")
                + " model=" + model
                + " thinkOn=" + (thinkOn ? "1" : "0")
                + " thinkLvl=" + thinkLvl
                + " num_ctx=" + std::to_string(numCtx)
                + " messages_chars=" + std::to_string(messagesJson.size())
                + " tools_chars=" + std::to_string(toolsJson.size())
                + " payload_chars=" + std::to_string(payload.size())
                + " sys_prompt_chars=" + std::to_string(systemPrompt.size()));
            LogToFile(std::string("Payload head: ") + payload.substr(0, 200));
            if (payload.size() > 200)
                LogToFile(std::string("Payload tail: ") + payload.substr(payload.size() - 120));

            std::string accumulated;
            DWORD statusCode = 0;
            bool httpOk = false;

            if (useOpenAI) {
                std::wstring auth = L"Authorization: Bearer " + Utf8ToWide(openaiKey) + L"\r\n";
                httpOk = HttpPostJson(oaUrl.host, oaUrl.port, oaUrl.https, oaUrl.path, auth,
                    payload, accumulated, statusCode);
            }
            else {
                httpOk = HttpPostJson(Utf8ToWide(host), (INTERNET_PORT)port, false,
                    L"/api/chat", L"", payload, accumulated, statusCode);
            }

            LogToFile("HTTP Status: " + std::to_string(statusCode) + " (round " + std::to_string(round) + ")");

            if (g_cancelRequested.load() || g_shuttingDown.load()) {
                LogToFile("AI cancelled during/after HTTP");
                break;
            }

            if (!httpOk) {
                LogToFile("AI HTTP failed on round " + std::to_string(round));
                break;
            }

            std::string content;
            std::string toolCallsRaw;
            bool hasTools = false;
            ParseAssistantMessage(accumulated, useOpenAI, content, toolCallsRaw, hasTools);

            // Final answer (no tools)
            if (!hasTools) {
                if (!content.empty()) {
                    response = content;
                    success = true;
                    std::lock_guard lock(g_mtx);
                    g_memory.push_back({ "user", userPrompt });
                    g_memory.push_back({ "assistant", response });
                    while (g_memory.size() > (size_t)currentMaxMem * 2)
                        g_memory.pop_front();
                    LogToFile("Success, response length: " + std::to_string(response.size()));
                }
                else {
                    std::string preview = accumulated.substr(0, (std::min)(accumulated.size(), (size_t)800));
                    LogToFile("Empty content and no tool_calls; body preview: " + preview);
                    std::string errMsg;
                    ExtractJsonStringField(accumulated, "error", errMsg);
                    if (!errMsg.empty())
                        LogToFile("API error field: " + errMsg);
                }
                break;
            }

            // Tool calls present: execute and continue
            AppendToolResults(messagesJson, content, toolCallsRaw, useOpenAI);
        }

        if (g_cancelRequested.load()) {
            LogToFile("AI request cancelled — discarding response");
            response.clear();
            success = false;
        }

        LogToFile("=== AI Request end ===");
        return success;
    }

} // namespace AI
