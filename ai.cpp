#include "pch.h"
#include "ai.hpp"
#include "knowledge.hpp"
#include "common.hpp"
#include "chat.hpp"
#include "settings.hpp"
#include "extras.hpp"
#include "aicontext.hpp"
#include "aitools.hpp"

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

    // Runs the tool calls of one model turn and appends the assistant message plus a
    // tool message per call to messagesJson. Each call is checked against the tool
    // rules first: a tool that is off never runs, and one set to "ask" waits for
    // opt.approveTool (which may block, and may edit the arguments).
    static void AppendToolResults(std::string& messagesJson, const std::string& content,
        const std::string& toolCallsRaw, const CallOptions& opt, Trace* trace)
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

            ToolEvent ev;
            ev.name = toolName;
            const long long t0 = NowSteadyMs();
            std::string result;
            if (!AiTools::Find(toolName)) {
                result = "Unknown tool: " + toolName;
                ev.outcome = 3;
            }
            else if (AiTools::ModeFor(toolName) == AiText::ToolMode::Off) {
                result = "This tool is switched off. Answer without it.";
                ev.outcome = 2;
            }
            else {
                bool run = true;
                if (AiTools::ModeFor(toolName) == AiText::ToolMode::Ask && opt.approveTool)
                    run = opt.approveTool(toolName, argsStr) == ToolVerdict::Run;
                if (run) {
                    result = AiTools::Execute(toolName, argsStr);
                }
                else {
                    result = "The user declined to run this tool. Answer without it.";
                    ev.outcome = 1;
                }
            }
            ev.args = argsStr;
            ev.result = result;
            ev.ms = NowSteadyMs() - t0;
            if (trace) trace->tools.push_back(ev);
            LogToFile("Tool result (" + std::to_string(result.size()) + " chars): " + result.substr(0, 150));

            // OpenAI and Ollama both accept role=tool with tool_call_id
            messagesJson += ",{\"role\":\"tool\",\"tool_call_id\":\"" + JsonEscape(toolId)
                + "\",\"name\":\"" + JsonEscape(toolName)
                + "\",\"content\":\"" + JsonEscape(result) + "\"}";
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

    // First integer after "key": in a response body (token counts), or -1.
    static int BodyInt(const std::string& body, const char* key) {
        const std::string pat = std::string("\"") + key + "\"";
        size_t pos = body.find(pat);
        if (pos == std::string::npos) return -1;
        pos = body.find(':', pos + pat.size());
        if (pos == std::string::npos) return -1;
        ++pos;
        while (pos < body.size() && body[pos] == ' ') ++pos;
        size_t end = pos;
        while (end < body.size() && isdigit((unsigned char)body[end])) ++end;
        if (end == pos || end - pos > 9) return -1;
        return std::atoi(body.substr(pos, end - pos).c_str());
    }

    bool GetResponse(const std::string& userPrompt, std::string& response) {
        return GetResponse(userPrompt, response, CallOptions{}, nullptr);
    }

    bool GetResponse(const std::string& userPrompt, std::string& response,
        const CallOptions& opt, Trace* trace)
    {
        LogToFile("=== AI Request start ===");
        response.clear();
        g_cancelRequested.store(false);
        const long long startMs = NowSteadyMs();
        struct Finish {
            Trace* t; long long start;
            ~Finish() { if (t) t->ms = NowSteadyMs() - start; }
        } finish{ trace, startMs };

        if (userPrompt.empty() || g_shuttingDown.load()) return false;
        LogToFile("Prompt: " + userPrompt);

        EnsureKnowledgeLoaded();

        std::string model, host, thinkLvl, provider, openaiBase, openaiKey, openaiModel;
        int port = 11434;
        int currentMaxMem = 4;
        bool thinkOn = false;
        float temperature = 1.0f;
        int numPredict = 512;
        int numCtx = 32768;
        float topP = 0.95f;
        int topK = 64;
        {
            std::lock_guard lock(g_settingsMtx);
            topP = g_prefs.topP;
            topK = g_prefs.topK;
            currentMaxMem = g_maxMemory;
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
        if (trace) {
            trace->provider = useOpenAI ? "openai" : "ollama";
            trace->model = model;
        }

        // Everything sent besides the tools: system prompt, memory, chat context, question.
        AiContext::Request creq;
        creq.userPrompt = userPrompt;
        creq.speaker = opt.speaker;
        creq.extraNote = opt.extraNote;
        creq.useFrozen = opt.useFrozenChat;
        creq.frozenChat = opt.frozenChat;
        const AiContext::Built built = AiContext::Build(creq);
        if (!built.systemPrompt.empty())
            LogToFile("System prompt loaded, length: " + std::to_string(built.systemPrompt.size()));
        else
            LogToFile("No system prompt (CONTEXT.md missing or every preset is off), proceeding without");
        if (!built.chatBlock.empty())
            LogToFile("Global chat context chars: " + std::to_string(built.chatBlock.size()));
        if (opt.onContext) opt.onContext(built);
        if (!built.parts.empty()) Extras::SetLastComposedPrompt(built.parts.back().text);

        std::string messagesJson = AiContext::MessagesJson(built);
        if (trace) trace->requestJson = messagesJson;

        std::string toolsJson = built.toolsJson;
        const bool haveTools = toolsJson.size() > 2;
        const int MAX_ROUNDS = 4;
        bool success = false;

        ParsedUrl oaUrl;
        if (useOpenAI) {
            if (!ParseOpenAiBase(openaiBase, oaUrl)) {
                LogToFile("Invalid OpenAI base URL: " + openaiBase);
                if (trace) trace->error = "invalid OpenAI base URL";
                LogToFile("=== AI Request end ===");
                return false;
            }
            if (openaiKey.empty()) {
                LogToFile("OpenAI API key is empty");
                if (trace) trace->error = "OpenAI API key is empty";
                LogToFile("=== AI Request end ===");
                return false;
            }
        }

        for (int round = 0; round < MAX_ROUNDS; ++round) {
            if (g_shuttingDown.load() || g_cancelRequested.load()) {
                LogToFile("AI cancelled before round " + std::to_string(round));
                break;
            }
            if (trace) trace->rounds = round + 1;

            std::string payload;
            if (useOpenAI) {
                // OpenAI-compatible chat completions
                char optBuf[160];
                snprintf(optBuf, sizeof(optBuf),
                    ",\"temperature\":%.3f,\"max_tokens\":%d,\"top_p\":%.3f",
                    (double)temperature, numPredict, (double)topP);
                payload = "{\"model\":\"" + JsonEscape(model)
                    + "\",\"messages\":" + messagesJson
                    + ",\"stream\":false";
                if (haveTools)
                    payload += ",\"tools\":" + toolsJson + ",\"tool_choice\":\"auto\"";
                payload += optBuf;
                payload += "}";
            }
            else {
                payload = "{\"model\":\"" + JsonEscape(model)
                    + "\",\"messages\":" + messagesJson
                    + ",\"stream\":false";
                if (haveTools)
                    payload += ",\"tools\":" + toolsJson;
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
                    char optBuf[224];
                    snprintf(optBuf, sizeof(optBuf),
                        ",\"options\":{\"temperature\":%.3f,\"num_predict\":%d,\"top_p\":%.3f,\"top_k\":%d,\"num_ctx\":%d}",
                        (double)temperature, numPredict, (double)topP, topK, numCtx);
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
                + " sys_prompt_chars=" + std::to_string(built.systemPrompt.size()));
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
                if (trace) {
                    std::string em;
                    ExtractJsonStringField(accumulated, "message", em);
                    if (em.empty()) ExtractJsonStringField(accumulated, "error", em);
                    trace->error = statusCode ? "HTTP " + std::to_string(statusCode) + (em.empty() ? "" : ": " + em)
                                              : "could not reach the model";
                }
                break;
            }

            if (trace) {
                // Ollama: prompt_eval_count / eval_count. OpenAI: usage.prompt_tokens / completion_tokens.
                int pt = BodyInt(accumulated, "prompt_eval_count");
                if (pt < 0) pt = BodyInt(accumulated, "prompt_tokens");
                int rt = BodyInt(accumulated, "eval_count");
                if (rt < 0) rt = BodyInt(accumulated, "completion_tokens");
                if (pt >= 0) trace->promptTokens = (std::max)(0, trace->promptTokens) + pt;
                if (rt >= 0) trace->replyTokens = (std::max)(0, trace->replyTokens) + rt;
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
                    if (opt.remember) {
                        std::lock_guard lock(g_mtx);
                        g_memory.push_back({ "user", userPrompt });
                        g_memory.push_back({ "assistant", response });
                        while (g_memory.size() > (size_t)currentMaxMem * 2)
                            g_memory.pop_front();
                    }
                    LogToFile("Success, response length: " + std::to_string(response.size()));
                }
                else {
                    std::string preview = accumulated.substr(0, (std::min)(accumulated.size(), (size_t)800));
                    LogToFile("Empty content and no tool_calls; body preview: " + preview);
                    std::string errMsg;
                    ExtractJsonStringField(accumulated, "error", errMsg);
                    if (!errMsg.empty())
                        LogToFile("API error field: " + errMsg);
                    if (trace) trace->error = errMsg.empty() ? "the model returned an empty reply" : errMsg;
                }
                break;
            }

            // Tool calls present: execute and continue
            AppendToolResults(messagesJson, content, toolCallsRaw, opt, trace);
            if (round + 1 == MAX_ROUNDS && trace) trace->error = "still calling tools after " + std::to_string(MAX_ROUNDS) + " rounds";
        }

        if (g_cancelRequested.load()) {
            LogToFile("AI request cancelled — discarding response");
            response.clear();
            success = false;
            if (trace) trace->error = "cancelled";
        }

        LogToFile("=== AI Request end ===");
        return success;
    }

} // namespace AI
