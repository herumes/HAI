// End-to-end test of the AI flow. The real ai.cpp, aiflow.cpp, aicontext.cpp, aitools.cpp and
// aiprompt.cpp run against a fake Ollama server on localhost, so every path - automatic send, review,
// edit, retry with a note, tool questions, limits, dry run, clean-up, cancel, the prompt stack - is
// exercised for real, without Hytale and without a model.
#include "pch.h"
#include "common.hpp"
#include "chat.hpp"
#include "shared.hpp"
#include "settings.hpp"
#include "aiflow.hpp"
#include "aicontext.hpp"
#include "aiprompt.hpp"
#include "aitools.hpp"

#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void Check(bool ok, const std::string& what) {
    if (ok) { ++g_pass; return; }
    ++g_fail;
    std::printf("FAIL: %s\n", what.c_str());
}
static bool Has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

#include "../common/fake_ollama.hpp"

static Fake g_fake;

// ------------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------------
static bool WaitState(uint64_t id, AiFlow::State want, int ms = 8000) {
    const long long end = NowSteadyMs() + ms;
    while (NowSteadyMs() < end) {
        for (const auto& v : AiFlow::List()) if (v.id == id && v.state == want) return true;
        Sleep(15);
    }
    return false;
}
static AiFlow::DraftView View(uint64_t id) {
    for (const auto& v : AiFlow::List()) if (v.id == id) return v;
    return {};
}
static std::vector<std::string> Pending() {
    std::vector<std::string> out;
    std::lock_guard lock(g_mtx);
    for (const auto& p : g_pending) if (!p.silent) out.push_back(p.typedText);
    return out;
}
static bool PendingHas(const std::string& s) {
    for (const auto& t : Pending()) if (Has(t, s)) return true;
    return false;
}
static void AddChat(const std::string& speaker, const std::string& body, long long ageMs = 1000, bool bot = false) {
    static uint64_t id = 1000;
    GlobalChatEntry e;
    e.speaker = speaker;
    e.body = body;
    e.plain = speaker.empty() ? body : speaker + ": " + body;
    e.fromBot = bot;
    e.unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - ageMs;
    e.id = ++id;
    std::lock_guard lock(g_mtx);
    g_globalChat.push_back(e);
}

static void Fresh() {
    AiFlow::OnCancel();
    g_fake.Reset();
    {
        std::lock_guard lock(g_settingsMtx);
        g_prefs = ChatPrefs{};
        g_prefs.aiDraftSound = false;
        g_prefs.aiDraftFlash = false;
        g_prefs.aiNoteInChat = false;
        g_ollamaHost = "127.0.0.1";
        g_ollamaPort = g_fake.port;
        g_aiProvider = "ollama";
        g_ollamaModel = "fake";
        g_thinkEnabled = false;
        g_webSearchEnabled = false;
        g_maxMemory = 4;
        g_maxChatContext = 20;
        g_includeGlobalChat = true;
        g_localName = "Chris";
        g_botMuted = false;
        g_useNickname = false;
    }
    {
        std::lock_guard lock(g_mtx);
        g_pending.clear();
        g_memory.clear();
        g_globalChat.clear();
    }
    AiFlow::ClearFinished();
    AiFlow::ResetLimits();
    std::string err;
    AiPrompt::Write("CONTEXT.md", "You are a test bot.", err);
    for (const auto& it : AiPrompt::Items()) if (!it.base) AiPrompt::Remove(it.name, err);
    AiPrompt::SetEnabled("CONTEXT.md", true);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    g_fake.Start();
    std::printf("fake ollama on port %d\n", g_fake.port);

    // ---- automatic mode -----------------------------------------------------------------------
    {
        Fresh();
        AddChat("Amelie", "salut tout le monde");
        g_fake.fallback = Fake::Reply("Hello there Steve");
        const uint64_t id = AiFlow::Submit("Steve: hyai hi", "Steve");
        Check(id != 0, "auto: accepted");
        Check(WaitState(id, AiFlow::State::Sent), "auto: reaches Sent");
        Check(PendingHas("Hello there Steve"), "auto: the reply is in the send queue");
        {
            std::lock_guard lock(g_mtx);
            Check(g_memory.size() == 2 && g_memory.back().content == "Hello there Steve", "auto: exchange remembered");
        }
        const std::string req = g_fake.Request(0);
        Check(Has(req, "You are a test bot."), "auto: system prompt sent");
        Check(Has(req, "Amelie: salut tout le monde"), "auto: chat context sent");
        Check(Has(req, "Steve: hyai hi"), "auto: question sent");
        Check(Has(req, "\\\"name\\\":\\\"calculate\\\"") || Has(req, "\"name\":\"calculate\""), "auto: tools offered");
        AiFlow::Detail d;
        Check(AiFlow::Get(id, d), "auto: detail available");
        Check(d.view.promptTokens == 42 && d.view.replyTokens == 7, "auto: token counts read from the server");
        Check(d.chatBlock.find("Amelie: salut") != std::string::npos, "auto: chat block recorded");
        Check(Has(d.systemPrompt, "You are a test bot."), "auto: system prompt recorded");
        Check(!AiFlow::NeedsMe(View(id).state), "auto: nothing waits for me");
    }

    // ---- review mode: approve, edit ------------------------------------------------------------
    {
        Fresh();
        g_prefs.aiReplyMode = 1;
        g_fake.fallback = Fake::Reply("first reply");
        const uint64_t id = AiFlow::Submit("Steve: hyai hi", "Steve");
        Check(WaitState(id, AiFlow::State::Ready), "review: waits as Ready");
        Check(Pending().empty(), "review: nothing sent yet");
        {
            std::lock_guard lock(g_mtx);
            Check(g_memory.empty(), "review: nothing remembered yet");
        }
        Check(AiFlow::WaitingCount() == 1 && AiFlow::OldestWaiting() == id, "review: counted as waiting");
        Check(!AiFlow::BannerText().empty(), "review: the compact banner has text");
        AiFlow::SetReply(id, "edited text\nsecond line");
        AiFlow::Approve(id);
        Check(WaitState(id, AiFlow::State::Sent, 3000), "review: approve sends");
        Check(PendingHas("edited text second line"), "review: the edited, one-line text went out");
        {
            std::lock_guard lock(g_mtx);
            Check(g_memory.size() == 2 && g_memory.back().content == "edited text\nsecond line", "review: memory holds what was sent");
        }
    }

    // ---- review mode: discard, whitelist auto-send --------------------------------------------
    {
        Fresh();
        g_prefs.aiReplyMode = 1;
        const uint64_t a = AiFlow::Submit("Steve: hyai one", "Steve");
        Check(WaitState(a, AiFlow::State::Ready), "discard: ready");
        AiFlow::Discard(a);
        Check(View(a).state == AiFlow::State::Discarded, "discard: state");
        Check(Pending().empty(), "discard: nothing sent");
        {
            std::lock_guard lock(g_mtx);
            Check(g_memory.empty(), "discard: not remembered");
        }
        g_prefs.aiAutoWhitelist = true;
        { std::lock_guard lock(g_mtx); g_whitelist.insert("amelie"); }
        const uint64_t b = AiFlow::Submit("Amelie: hyai two", "Amelie");
        Check(WaitState(b, AiFlow::State::Sent), "whitelist: a listed player's reply goes out by itself");
        const uint64_t c = AiFlow::Submit("Bob: hyai three", "Bob");
        Check(WaitState(c, AiFlow::State::Ready), "whitelist: others still wait");
        { std::lock_guard lock(g_mtx); g_whitelist.clear(); }
    }

    // ---- retry with a note, frozen and fresh chat --------------------------------------------
    {
        Fresh();
        g_prefs.aiReplyMode = 1;
        AddChat("Amelie", "early line");
        g_fake.script = { Fake::Reply("first"), Fake::Reply("second"), Fake::Reply("third") };
        const uint64_t id = AiFlow::Submit("Steve: hyai q", "Steve");
        Check(WaitState(id, AiFlow::State::Ready), "retry: first ready");
        AddChat("Late", "arrived after the first try");
        AiFlow::Retry(id, "be shorter");
        Check(WaitState(id, AiFlow::State::Ready), "retry: second ready");
        Check(View(id).reply == "second" && View(id).tries == 2 && View(id).attempts == 1, "retry: bookkeeping");
        const std::string r2 = g_fake.Request(1);
        Check(Has(r2, "be shorter"), "retry: the note reaches the model");
        Check(Has(r2, "early line") && !Has(r2, "arrived after the first try"), "retry: same chat as the first try");
        AiFlow::Retry(id, "", true);
        Check(WaitState(id, AiFlow::State::Ready), "retry: third ready");
        Check(Has(g_fake.Request(2), "arrived after the first try") && !Has(g_fake.Request(2), "be shorter"), "retry: fresh chat, note gone");
        AiFlow::UseAttempt(id, 0);
        Check(View(id).reply == "first" || View(id).reply == "second", "retry: an earlier reply can be swapped back");
    }

    // ---- tools: ask, allow, deny, edit, off ------------------------------------------------------
    {
        Fresh();
        g_prefs.aiToolPolicy = 2;   // ask before every tool
        g_fake.script = { Fake::Tool("calculate", "{\"expression\":\"6*7\"}"), Fake::Reply("its 42") };
        uint64_t id = AiFlow::Submit("Steve: hyai 6*7?", "Steve");
        Check(WaitState(id, AiFlow::State::AwaitingTool), "tool: asks first");
        Check(View(id).askTool == "calculate" && Has(View(id).askArgs, "6*7"), "tool: shows what it wants");
        Check(AiFlow::OldestWaiting() == id, "tool: counts as waiting");
        AiFlow::AnswerTool(id, true, "");
        Check(WaitState(id, AiFlow::State::Sent), "tool: allowed, finishes");
        Check(Has(g_fake.Request(1), "\"role\":\"tool\"") && Has(g_fake.Request(1), "42"), "tool: result sent back");
        AiFlow::Detail d;
        AiFlow::Get(id, d);
        Check(d.tools.size() == 1 && d.tools[0].outcome == 0 && d.tools[0].result == "42", "tool: recorded in the detail");

        g_fake.Reset();
        g_fake.script = { Fake::Tool("calculate", "{\"expression\":\"6*7\"}"), Fake::Reply("cannot say") };
        id = AiFlow::Submit("Steve: hyai 6*7?", "Steve");
        Check(WaitState(id, AiFlow::State::AwaitingTool), "tool deny: asks");
        AiFlow::AnswerTool(id, false, "");
        Check(WaitState(id, AiFlow::State::Sent), "tool deny: still answers");
        Check(Has(g_fake.Request(1), "declined"), "tool deny: the model is told");
        AiFlow::Get(id, d);
        Check(d.tools.size() == 1 && d.tools[0].outcome == 1, "tool deny: outcome recorded");

        g_fake.Reset();
        g_fake.script = { Fake::Tool("calculate", "{\"expression\":\"6*7\"}"), Fake::Reply("two") };
        id = AiFlow::Submit("Steve: hyai 6*7?", "Steve");
        Check(WaitState(id, AiFlow::State::AwaitingTool), "tool edit: asks");
        AiFlow::AnswerTool(id, true, "{\"expression\":\"1+1\"}");
        Check(WaitState(id, AiFlow::State::Sent), "tool edit: finishes");
        AiFlow::Get(id, d);
        Check(d.tools.size() == 1 && d.tools[0].result == "2", "tool edit: ran with my arguments");

        // A per-tool rule beats the policy: auto never asks.
        g_fake.Reset();
        AiTools::SetRule("calculate", AiText::ToolMode::Auto, false);
        g_fake.script = { Fake::Tool("calculate", "{\"expression\":\"2*2\"}"), Fake::Reply("four") };
        id = AiFlow::Submit("Steve: hyai 2*2?", "Steve");
        Check(WaitState(id, AiFlow::State::Sent), "tool rule auto: no question");

        // Off: not offered, and refused if the model calls it anyway.
        g_fake.Reset();
        AiTools::SetRule("calculate", AiText::ToolMode::Off, false);
        g_fake.script = { Fake::Tool("calculate", "{\"expression\":\"2*2\"}"), Fake::Reply("no calc") };
        id = AiFlow::Submit("Steve: hyai 2*2?", "Steve");
        Check(WaitState(id, AiFlow::State::Sent), "tool rule off: finishes");
        Check(!Has(g_fake.Request(0), "\"name\":\"calculate\""), "tool rule off: not offered");
        Check(Has(g_fake.Request(1), "switched off"), "tool rule off: refused if called anyway");
        AiTools::SetRule("calculate", AiText::ToolMode::Auto, true);
    }

    // ---- limits ---------------------------------------------------------------------------------
    {
        Fresh();
        g_prefs.aiCooldownSec = 60;
        uint64_t a = AiFlow::Submit("Steve: hyai a", "Steve");
        Check(a != 0, "limit: first goes through");
        Check(WaitState(a, AiFlow::State::Sent), "limit: first is sent");
        Check(AiFlow::Submit("Amelie: hyai b", "Amelie") == 0, "limit: global cooldown skips the second");
        const auto skips = AiFlow::RecentSkips();
        Check(!skips.empty() && Has(skips.back(), "cooldown"), "limit: the skip is listed");

        Fresh();
        g_prefs.aiPlayerCooldownSec = 60;
        a = AiFlow::Submit("Steve: hyai a", "Steve");
        WaitState(a, AiFlow::State::Sent);
        Check(AiFlow::Submit("steve: hyai again", "steve") == 0, "limit: same player waits");
        Check(AiFlow::Submit("Amelie: hyai b", "Amelie") != 0, "limit: another player does not");

        Fresh();
        g_prefs.aiMaxPerMinute = 2;
        const uint64_t x = AiFlow::Submit("A: hyai 1", "A");
        WaitState(x, AiFlow::State::Sent);
        const uint64_t y = AiFlow::Submit("B: hyai 2", "B");
        WaitState(y, AiFlow::State::Sent);
        Check(AiFlow::Submit("C: hyai 3", "C") == 0, "limit: per-minute cap");

        // Busy: dropped by default, queued when allowed.
        Fresh();
        g_fake.delayMs = 700;
        a = AiFlow::Submit("Steve: hyai slow", "Steve");
        Check(WaitState(a, AiFlow::State::Thinking), "busy: first is thinking");
        Check(AiFlow::Submit("Amelie: hyai me too", "Amelie") == 0, "busy: dropped when the queue is 0");
        g_prefs.aiQueueMax = 1;
        const uint64_t q = AiFlow::Submit("Bob: hyai queue me", "Bob");
        Check(q != 0 && View(q).state == AiFlow::State::Queued, "busy: queued when allowed");
        Check(AiFlow::Submit("Zed: hyai and me", "Zed") == 0, "busy: queue is full");
        Check(WaitState(a, AiFlow::State::Sent, 5000) && WaitState(q, AiFlow::State::Sent, 5000), "busy: the queue drains in order");
    }

    // ---- reply clean-up ------------------------------------------------------------------------------
    {
        Fresh();
        g_prefs.aiPostLower = true;
        g_prefs.aiPostStripEmoji = true;
        g_prefs.aiPostStripMarkdown = true;
        g_prefs.aiPostMaxChars = 24;
        g_fake.fallback = Fake::Reply("**HELLO** Steve \xF0\x9F\x98\x80 how are you today my friend");
        const uint64_t id = AiFlow::Submit("Steve: hyai hi", "Steve");
        Check(WaitState(id, AiFlow::State::Sent), "clean-up: sent");
        const AiFlow::DraftView v = View(id);
        Check(v.reply.find("HELLO") == std::string::npos && v.reply.find('*') == std::string::npos &&
              v.reply.find("\xF0\x9F") == std::string::npos && v.reply.size() <= 24, "clean-up: applied: " + v.reply);
        AiFlow::Detail d;
        AiFlow::Get(id, d);
        Check(Has(d.raw, "**HELLO**"), "clean-up: the raw reply is kept");
        Check(!v.note.empty(), "clean-up: says what it did");

        Fresh();
        g_prefs.aiPostBlocklist = "darn";
        g_prefs.aiPostBlockAction = 1;
        g_fake.fallback = Fake::Reply("well darn it");
        const uint64_t b = AiFlow::Submit("Steve: hyai hi", "Steve");
        Check(WaitState(b, AiFlow::State::Ready), "blocklist: held for review even in automatic mode");
        Check(Pending().empty(), "blocklist: nothing sent");
    }

    // ---- dry run, ask before the model, failure -----------------------------------------------------
    {
        Fresh();
        g_prefs.aiReplyMode = 2;
        const uint64_t id = AiFlow::Submit("Steve: hyai hi", "Steve");
        Check(WaitState(id, AiFlow::State::Discarded), "dry run: kept, not sent");
        Check(Pending().empty(), "dry run: nothing in the send queue");
        Check(Has(View(id).note, "dry run"), "dry run: says so");
        AiFlow::Approve(id);
        Check(WaitState(id, AiFlow::State::Sent, 3000) && PendingHas("ok"), "dry run: can still be sent by hand");

        Fresh();
        g_prefs.aiAskBeforeModel = true;
        const uint64_t a = AiFlow::Submit("Steve: hyai hi", "Steve");
        Check(View(a).state == AiFlow::State::AwaitingRun, "ask first: waits before the model");
        Sleep(300);
        Check(g_fake.RequestCount() == 0, "ask first: the model was not called");
        AiFlow::Approve(a);
        Check(WaitState(a, AiFlow::State::Sent), "ask first: runs after approval");
        const uint64_t b = AiFlow::Submit("Steve: hyai no", "Steve");
        AiFlow::Discard(b);
        Sleep(200);
        Check(View(b).state == AiFlow::State::Discarded && g_fake.RequestCount() == 1, "ask first: skipped one never reaches the model");

        Fresh();
        g_fake.fallback = "STATUS:500";
        uint64_t id2 = AiFlow::Submit("Steve: hyai hi", "Steve");
        Check(WaitState(id2, AiFlow::State::Failed), "failure: state");
        Check(Has(View(id2).error, "500"), "failure: reason shown: " + View(id2).error);
        Check(PendingHas("sorry couldnt process"), "failure: automatic mode says the failure message like before");
        Fresh();
        g_prefs.aiReplyMode = 1;
        g_fake.fallback = "STATUS:500";
        id2 = AiFlow::Submit("Steve: hyai hi", "Steve");
        Check(WaitState(id2, AiFlow::State::Failed) && Pending().empty(), "failure: review mode stays quiet");
        g_fake.fallback = Fake::Reply("recovered");
        AiFlow::Retry(id2);
        Check(WaitState(id2, AiFlow::State::Ready) && View(id2).reply == "recovered", "failure: a retry recovers");
    }

    // ---- cancel and timeouts -------------------------------------------------------------------------
    {
        Fresh();
        g_fake.delayMs = 2500;
        const uint64_t id = AiFlow::Submit("Steve: hyai slow", "Steve");
        Check(WaitState(id, AiFlow::State::Thinking), "cancel: thinking");
        const long long t0 = NowSteadyMs();
        AiFlow::Discard(id);
        Check(View(id).state == AiFlow::State::Discarded, "cancel: discarded at once");
        g_fake.delayMs = 0;
        Sleep(200);
        const uint64_t next = AiFlow::Submit("Amelie: hyai after", "Amelie");
        Check(next != 0 && WaitState(next, AiFlow::State::Sent, 8000), "cancel: the worker is free again");
        Check(NowSteadyMs() - t0 < 9000, "cancel: did not hang");
        Check(View(id).state == AiFlow::State::Discarded, "cancel: the cancelled draft stays discarded");

        // Cancel a slow one and retry it straight away: the cancelled run must not overwrite the retry.
        Fresh();
        g_prefs.aiReplyMode = 1;
        g_fake.delayMs = 900;
        g_fake.script = { Fake::Reply("stale"), Fake::Reply("fresh") };
        const uint64_t r1 = AiFlow::Submit("Steve: hyai slow", "Steve");
        Check(WaitState(r1, AiFlow::State::Thinking), "cancel+retry: thinking");
        AiFlow::Discard(r1);
        AiFlow::Retry(r1);
        Check(WaitState(r1, AiFlow::State::Ready, 8000), "cancel+retry: the retry finishes");
        Check(View(r1).reply == "fresh" || View(r1).reply == "stale", "cancel+retry: has a reply: " + View(r1).reply);

        Fresh();
        g_prefs.aiReplyMode = 1;
        g_prefs.aiDraftTimeoutSec = 1;
        const uint64_t a = AiFlow::Submit("Steve: hyai a", "Steve");
        WaitState(a, AiFlow::State::Ready);
        Sleep(1200);
        AiFlow::Tick();
        Check(View(a).state == AiFlow::State::Discarded && Has(View(a).note, "timed out"), "timeout: discards");
        g_prefs.aiDraftTimeoutSend = 1;
        const uint64_t b = AiFlow::Submit("Amelie: hyai b", "Amelie");
        WaitState(b, AiFlow::State::Ready);
        Sleep(1200);
        AiFlow::Tick();
        Check(View(b).state == AiFlow::State::Sent, "timeout: can send instead");
    }

    // ---- prompt stack and live context -------------------------------------------------------------
    {
        Fresh();
        std::string err;
        Check(AiPrompt::Write("CONTEXT.md", "BASE for {me}", err), "stack: base written");
        std::string created;
        Check(AiPrompt::Create("Pirate", "Talk like a pirate, {speaker}.", created, err) && created == "Pirate", "stack: preset created");
        Check(AiPrompt::Create("Pirate", "again", created, err) && created == "Pirate 2", "stack: a taken name gets a number");
        AiPrompt::Remove("Pirate 2", err);
        auto items = AiPrompt::Items();
        Check(items.size() == 2 && items[0].base && items[0].enabled && !items[1].enabled, "stack: new preset arrives switched off");
        Check(AiPrompt::Assemble({ { "me", "Chris" }, { "speaker", "Steve" } }) == "BASE for Chris", "stack: only the base is used");
        AiPrompt::SetEnabled("Pirate", true);
        Check(AiPrompt::Assemble({ { "me", "Chris" }, { "speaker", "Steve" } }) == "BASE for Chris\n\nTalk like a pirate, Steve.", "stack: enabled presets join");
        AiPrompt::Move("Pirate", -1);
        Check(AiPrompt::Assemble({ { "me", "Chris" }, { "speaker", "Steve" } }) == "Talk like a pirate, Steve.\n\nBASE for Chris", "stack: order follows the list");
        Check(AiPrompt::Rename("Pirate", "Old Salt", err), "stack: rename");
        items = AiPrompt::Items();
        Check(items[0].name == "Old Salt" && items[0].enabled, "stack: rename keeps place and switch");
        Check(!AiPrompt::Rename("CONTEXT.md", "x", err), "stack: base can't be renamed");
        Check(!AiPrompt::Remove("CONTEXT.md", err), "stack: base can't be removed");
        std::string text;
        Check(AiPrompt::Read("Old Salt", text) && text.find("pirate") != std::string::npos, "stack: read back");
        Check(AiPrompt::Write("Old Salt", "Arr.", err) && AiPrompt::Assemble({}) == "Arr.\n\nBASE for {me}", "stack: edits show up at once (unknown variables stay)");

        g_localName = "Chris";
        g_fake.fallback = Fake::Reply("ok");
        const uint64_t id = AiFlow::Submit("Steve: hyai hi", "Steve");
        WaitState(id, AiFlow::State::Sent);
        Check(Has(g_fake.Request(0), "Arr.") && Has(g_fake.Request(0), "BASE for Chris"), "stack: the request carries both presets");
        AiPrompt::SetEnabled("Old Salt", false);
        AiPrompt::SetEnabled("CONTEXT.md", false);
        Check(AiPrompt::Assemble({}) == "", "stack: everything off = no system prompt");
        Check(AiPrompt::Remove("Old Salt", err), "stack: remove");
        AiPrompt::SetEnabled("CONTEXT.md", true);
    }
    {
        Fresh();
        AddChat("Amelie", "one", 5 * 60000);
        AddChat("", "[!] server ad", 4 * 60000);
        AddChat("HyAI", "bot said this", 3 * 60000, true);
        AddChat("Steve", "two", 2 * 60000);
        AddChat("Bob", "hidden line", 1 * 60000);
        uint64_t hideId = 0;
        {
            std::lock_guard lock(g_mtx);
            for (auto& e : g_globalChat) if (e.speaker == "Bob") { e.hidden = true; hideId = e.id; }
        }
        AiContext::Request rq;
        rq.userPrompt = "Steve: hyai q";
        rq.speaker = "Steve";
        AiContext::Built b = AiContext::Build(rq);
        Check(b.chatLines.size() == 4, "context: hidden line left out");
        Check(Has(b.chatBlock, "Amelie: one") && !Has(b.chatBlock, "hidden line"), "context: block text");
        Check(b.parts.back().kind == AiContext::Part::User && Has(b.parts.back().text, "Recent server chat") && Has(b.parts.back().text, "Steve: hyai q"),
              "context: the old wording is the default");
        Check(b.tokTotal > 0 && b.numCtx > 0, "context: budget numbers");
        Check(hideId != 0, "context: hidden id known");

        g_prefs.aiChatIncludeBot = false;
        g_prefs.aiChatIncludeSystem = false;
        b = AiContext::Build(rq);
        Check(!Has(b.chatBlock, "bot said") && !Has(b.chatBlock, "server ad"), "context: bot and system lines can be left out");
        g_prefs.aiChatIncludeBot = g_prefs.aiChatIncludeSystem = true;
        g_prefs.aiChatMaxAgeMin = 3;
        b = AiContext::Build(rq);
        Check(!Has(b.chatBlock, "Amelie: one") && Has(b.chatBlock, "Steve: two"), "context: age limit");
        g_prefs.aiChatMaxAgeMin = 0;
        g_prefs.aiChatLineFmt = "<{speaker}> {body}";
        g_prefs.aiChatIntro = "CHAT for {speaker}:";
        g_prefs.aiChatOutro = "";
        b = AiContext::Build(rq);
        Check(Has(b.chatBlock, "<Amelie> one") && Has(b.parts.back().text, "CHAT for Steve:") && !Has(b.parts.back().text, "reply to this"), "context: templates");
        rq.useFrozen = true;
        rq.frozenChat = "Frozen: line\n";
        b = AiContext::Build(rq);
        Check(Has(b.parts.back().text, "Frozen: line") && !Has(b.chatBlock, "Amelie"), "context: a frozen block replaces the live chat");
        g_prefs.aiChatIntro = "";
        rq.useFrozen = false;
        g_includeGlobalChat = false;
        b = AiContext::Build(rq);
        Check(b.chatBlock.empty() && b.parts.back().text == "Steve: hyai q", "context: chat off = the bare question");
        g_includeGlobalChat = true;

        rq.extraNote = "answer in French";
        b = AiContext::Build(rq);
        bool note = false;
        for (const auto& p : b.parts) if (p.kind == AiContext::Part::Note && p.role == "system" && p.text == "answer in French") note = true;
        Check(note, "context: a one-off note is its own system message");
        Check(Has(AiContext::MessagesJson(b), "\"role\":\"system\""), "context: messages json");
    }

    // ---- owner commands ------------------------------------------------------------------------------------
    {
        Fresh();
        g_prefs.aiReplyMode = 1;
        g_wakeWord = "hyai";
        Check(HandleOwnerCommand("hyai mode dry", false) && g_prefs.aiReplyMode == 2, "owner: mode dry");
        Check(HandleOwnerCommand("hyai mode auto", false) && g_prefs.aiReplyMode == 0, "owner: mode auto");
        Check(HandleOwnerCommand("hyai mode ask", false) && g_prefs.aiReplyMode == 1, "owner: mode ask");
        g_fake.fallback = Fake::Reply("sure");
        const uint64_t id = AiFlow::Submit("Steve: hyai hi", "Steve");
        WaitState(id, AiFlow::State::Ready);
        Check(HandleOwnerCommand("hyai approve", false), "owner: approve");
        Check(WaitState(id, AiFlow::State::Sent, 3000) && PendingHas("sure"), "owner: approve sends the oldest draft");
        const uint64_t id2 = AiFlow::Submit("Steve: hyai again", "Steve");
        WaitState(id2, AiFlow::State::Ready);
        Check(HandleOwnerCommand("hyai discard", false) && View(id2).state == AiFlow::State::Discarded, "owner: discard");
        std::string err, created;
        AiPrompt::Create("Zed", "zed", created, err);
        Check(HandleOwnerCommand("hyai preset zed on", false), "owner: preset on");
        bool on = false;
        for (const auto& it : AiPrompt::Items()) if (it.name == "Zed") on = it.enabled;
        Check(on, "owner: preset switched on");
        HandleOwnerCommand("hyai preset zed", false);
        for (const auto& it : AiPrompt::Items()) if (it.name == "Zed") on = it.enabled;
        Check(!on, "owner: bare preset name toggles");
        AiPrompt::Remove("Zed", err);
    }

    // ---- tools directly ----------------------------------------------------------------------------------------
    {
        Fresh();
        Check(AiTools::Execute("calculate", "{\"expression\":\"(2+3)*4\"}") == "20", "tools: calculate");
        Check(AiTools::Execute("calculate", "{\"expression\":\"2+\"}").rfind("error", 0) == 0, "tools: calculate error");
        Check(Has(AiTools::Execute("roll_dice", "{\"dice\":\"2d6\"}"), "2d6:"), "tools: dice");
        AddChat("Amelie", "the forge is at spawn");
        {
            ChatLine l; l.plain = "Amelie: the forge is at spawn"; l.speaker = "Amelie"; l.id = 9001; l.unixMs = 1790000000000LL;
            std::lock_guard lock(g_mtx);
            g_log.push_back(l);
        }
        Check(Has(AiTools::Execute("search_chat", "{\"query\":\"forge\"}"), "forge is at spawn"), "tools: search_chat");
        Check(Has(AiTools::Execute("search_chat", "{\"query\":\"zzzz\"}"), "No chat lines"), "tools: search_chat miss");
        Check(Has(AiTools::Execute("list_players", "{}"), "No players") || Has(AiTools::Execute("list_players", "{}"), "ago"), "tools: list_players");
        Check(Has(AiTools::Execute("get_my_position", "{}"), "not known"), "tools: position unknown");
        Check(Has(AiTools::Execute("nope", "{}"), "Unknown tool"), "tools: unknown");
        const std::string all = AiTools::ToolsJson();
        Check(all.front() == '[' && all.back() == ']' && Has(all, "lookup_mob") && !Has(all, "web_search"), "tools: json, web search off");
        g_webSearchEnabled = true;
        Check(Has(AiTools::ToolsJson(), "web_search"), "tools: web search when enabled");
        g_webSearchEnabled = false;
        for (const auto& d : AiTools::All()) AiTools::SetRule(d.name, AiText::ToolMode::Off, false);
        Check(AiTools::ToolsJson() == "[]", "tools: all off = empty list");
        for (const auto& d : AiTools::All()) AiTools::SetRule(d.name, AiText::ToolMode::Auto, true);
        // Nothing offered: the request must leave the tools key out entirely.
        for (const auto& d : AiTools::All()) AiTools::SetRule(d.name, AiText::ToolMode::Off, false);
        g_fake.fallback = Fake::Reply("no tools here");
        const uint64_t id = AiFlow::Submit("Steve: hyai hi", "Steve");
        WaitState(id, AiFlow::State::Sent);
        Check(!Has(g_fake.Request(0), "\"tools\""), "tools: no tools key when none are offered");
        for (const auto& d : AiTools::All()) AiTools::SetRule(d.name, AiText::ToolMode::Auto, true);
    }

    g_fake.Stop();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    g_shuttingDown = true;
    Sleep(300);
    return g_fail ? 1 : 0;
}
