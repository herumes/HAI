#include "pch.h"
#include "aicontext.hpp"
#include "aiprompt.hpp"
#include "aitools.hpp"
#include "chat.hpp"
#include "common.hpp"
#include "extras.hpp"
#include "position.hpp"
#include "settings.hpp"
#include "shared.hpp"

namespace AiContext {
namespace {

long long UnixNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

AiText::Vars LiveVars(const std::string& speaker) {
    AiText::Vars v;
    std::string me, bot, wake;
    {
        std::lock_guard lock(g_settingsMtx);
        me = g_localName;
        bot = g_botName;
        wake = g_wakeWord;
    }
    v["me"] = me.empty() ? "the player" : me;
    v["bot"] = bot.empty() ? "HyAI" : bot;
    v["wake"] = wake.empty() ? "hyai" : wake;
    v["speaker"] = speaker.empty() ? "someone" : speaker;

    const long long now = UnixNow();
    v["time"] = FormatChatTime(now);
    {
        const time_t t = (time_t)(now / 1000);
        struct tm tmv {};
        localtime_s(&tmv, &t);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
        v["date"] = buf;
        strftime(buf, sizeof(buf), "%A", &tmv);
        v["weekday"] = buf;
    }
    if (g_posLocked.load()) {
        char buf[96];
        snprintf(buf, sizeof(buf), "X %.0f Y %.0f Z %.0f", g_posX.load(), g_posY.load(), g_posZ.load());
        v["pos"] = buf;
    }
    else v["pos"] = "unknown";

    std::string players;
    {
        std::lock_guard lock(g_mtx);
        int n = 0;
        for (auto it = g_seenPlayers.rbegin(); it != g_seenPlayers.rend() && n < 12; ++it, ++n) {
            if (!players.empty()) players += ", ";
            players += it->display;
        }
    }
    v["players"] = players.empty() ? "nobody yet" : players;
    return v;
}

Built Build(const Request& req) {
    Built b;

    // ---- settings, copied once ------------------------------------------------
    int maxMem, maxChat, numCtx, numPredict;
    bool includeGlobal;
    AiText::CtxOptions co;
    std::string intro, outro;
    {
        std::lock_guard lock(g_settingsMtx);
        maxMem = g_maxMemory;
        maxChat = g_maxChatContext;
        includeGlobal = g_includeGlobalChat;
        numCtx = g_ollamaNumCtx;
        numPredict = g_ollamaNumPredict;
        co.lineFormat = g_prefs.aiChatLineFmt;
        co.maxAgeMin = g_prefs.aiChatMaxAgeMin;
        co.maxLineChars = g_prefs.aiChatMaxLineChars;
        co.includeBot = g_prefs.aiChatIncludeBot;
        co.includeSystem = g_prefs.aiChatIncludeSystem;
        intro = g_prefs.aiChatIntro;
        outro = g_prefs.aiChatOutro;
    }
    co.maxLines = maxChat;
    co.nowMs = UnixNow();
    co.timeFn = [](long long ms) { return FormatChatTime(ms); };
    b.numCtx = numCtx;
    b.numPredict = numPredict;

    const AiText::Vars vars = LiveVars(req.speaker);

    // ---- system prompt ---------------------------------------------------------
    b.systemPrompt = AiPrompt::Assemble(vars, &b.presets);
    if (!b.systemPrompt.empty()) {
        Part p;
        p.kind = Part::System;
        p.role = "system";
        p.label = "System prompt (" + std::to_string(b.presets) + (b.presets == 1 ? " preset)" : " presets)");
        p.text = b.systemPrompt;
        b.parts.push_back(std::move(p));
    }

    // ---- conversation memory ---------------------------------------------------
    {
        std::lock_guard lock(g_mtx);
        const size_t keep = maxMem > 0 ? (size_t)maxMem * 2 : 0;
        const size_t first = g_memory.size() > keep ? g_memory.size() - keep : 0;
        for (size_t i = first; i < g_memory.size(); ++i) {
            Part p;
            p.kind = Part::Memory;
            p.role = g_memory[i].role;
            p.label = g_memory[i].role == "user" ? "Earlier question" : "Earlier answer";
            p.text = g_memory[i].content;
            p.memIndex = i;
            b.parts.push_back(std::move(p));
        }
    }

    // ---- a one-off instruction (retry with a note) -----------------------------
    if (!TrimCopy(req.extraNote).empty()) {
        Part p;
        p.kind = Part::Note;
        p.role = "system";
        p.label = "Instruction for this reply";
        p.text = TrimCopy(req.extraNote);
        b.parts.push_back(std::move(p));
    }

    // ---- chat context + the question ---------------------------------------------
    if (req.useFrozen) {
        b.chatBlock = req.frozenChat;
    }
    else if (includeGlobal) {
        std::vector<AiText::CtxEntry> entries;
        std::vector<uint64_t> ids;
        {
            std::lock_guard lock(g_mtx);
            for (const GlobalChatEntry& e : g_globalChat) {
                AiText::CtxEntry c;
                c.speaker = e.speaker;
                c.body = e.body;
                c.plain = e.plain;
                c.unixMs = e.unixMs;
                c.fromBot = e.fromBot;
                c.hidden = e.hidden;
                entries.push_back(std::move(c));
                ids.push_back(e.id);
            }
        }
        const std::vector<AiText::CtxLine> lines = AiText::BuildChatLines(entries, co);
        for (const AiText::CtxLine& l : lines) b.chatLines.push_back({ ids[(size_t)l.index], l.text });
        b.chatBlock = AiText::JoinLines(lines);
    }
    if (req.useFrozen) {
        // A frozen block has no per-line ids, but the preview still lists the lines.
        size_t pos = 0;
        while (pos < b.chatBlock.size()) {
            size_t nl = b.chatBlock.find('\n', pos);
            if (nl == std::string::npos) nl = b.chatBlock.size();
            b.chatLines.push_back({ 0, b.chatBlock.substr(pos, nl - pos) });
            pos = nl + 1;
        }
    }

    b.intro = TrimCopy(AiText::ExpandVars(intro, vars));
    b.outro = TrimCopy(AiText::ExpandVars(outro, vars));

    std::string composed = req.userPrompt;
    if (!b.chatBlock.empty()) {
        composed.clear();
        if (!b.intro.empty()) composed += b.intro + "\n";
        composed += b.chatBlock;
        composed += "\n";
        if (!b.outro.empty()) composed += b.outro + "\n";
        composed += req.userPrompt;
    }
    {
        Part p;
        p.kind = Part::User;
        p.role = "user";
        p.label = b.chatBlock.empty() ? "Question" : "Chat context + question";
        p.text = composed;
        b.parts.push_back(std::move(p));
    }

    // ---- numbers for the budget bar --------------------------------------------
    for (Part& p : b.parts) {
        p.tokens = AiText::EstimateTokens(p.text) + 4;      // a few tokens of framing per message
        switch (p.kind) {
        case Part::System: case Part::Note: b.tokSystem += p.tokens; break;
        case Part::Memory: b.tokMemory += p.tokens; break;
        case Part::User: b.tokUser += p.tokens; break;
        }
    }
    b.tokChat = AiText::EstimateTokens(b.chatBlock + b.intro + b.outro);
    b.tokUser = (std::max)(0, b.tokUser - b.tokChat);
    b.toolsJson = AiTools::ToolsJson();
    b.tokTools = AiText::EstimateTokens(b.toolsJson);
    b.tokTotal = b.tokSystem + b.tokMemory + b.tokChat + b.tokUser + b.tokTools;
    return b;
}

std::string MessagesJson(const Built& b) {
    std::string j = "[";
    bool comma = false;
    for (const Part& p : b.parts) {
        if (comma) j += ",";
        comma = true;
        j += "{\"role\":\"" + p.role + "\",\"content\":\"" + JsonEscape(p.text) + "\"}";
    }
    j += "]";
    return j;
}

} // namespace AiContext
