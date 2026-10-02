#include "pch.h"
#include "aiflow.hpp"
#include "aicontext.hpp"
#include "aitext.hpp"
#include "aitools.hpp"
#include "chat.hpp"
#include "chatview.hpp"
#include "common.hpp"
#include "settings.hpp"
#include "shared.hpp"

#include <condition_variable>

namespace AiFlow {

std::string KeyLabel(int vk);   // defined below, declared in the header

namespace {

// =============================================================================
// State
// =============================================================================
struct Rec {
    DraftView v;
    std::string raw, systemPrompt, chatBlock, requestJson, retryNote;
    bool frozenValid = false;       // chatBlock was recorded, so a retry can reuse it
    bool useFrozen = false;         // the next run reuses chatBlock
    bool cancelled = false;
    bool forceReview = false;       // never sent by itself (Studio questions, blocklist hits)
    bool dryRun = false;            // a dry run: kept, never sent unless I send it by hand
    std::vector<AI::ToolEvent> tools;
    std::vector<std::string> attempts;
    std::vector<std::string> notes;
    long long stateMs = 0;          // steady clock, when the state last changed
    int toolDecision = 0;           // 0 waiting, 1 allow, 2 deny
    int gen = 0;                    // bumped by a retry, so a run that was cancelled can't overwrite the new one
};

std::mutex g_m;
std::condition_variable g_cv;
std::vector<Rec> g_recs;
std::deque<uint64_t> g_jobs;
bool g_workerOn = false;
uint64_t g_running = 0;
uint64_t g_nextId = 0;
AiText::RateLimiter g_limiter;
std::vector<std::string> g_skips;

long long UnixNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

Rec* FindLocked(uint64_t id) {
    for (Rec& r : g_recs) if (r.v.id == id) return &r;
    return nullptr;
}

bool Finished(State s) { return s == State::Sent || s == State::Discarded || s == State::Failed; }

// =============================================================================
// Settings, copied once per decision
// =============================================================================
struct Cfg {
    int mode = 0, timeoutSec = 0, timeoutSend = 0, cooldown = 0, playerCooldown = 0, perMinute = 0;
    int queueMax = 0, toolWait = 60, histMax = 60;
    bool askBefore = false, autoWhitelist = false, noteInChat = true, sound = true, flash = true, saveHistory = false;
    int keyApprove = 0, keyDiscard = 0;
    AiText::PostOptions post;
    std::string failMessage;
};

Cfg ReadCfg() {
    Cfg c;
    std::lock_guard lock(g_settingsMtx);
    const ChatPrefs& p = g_prefs;
    c.mode = p.aiReplyMode; c.timeoutSec = p.aiDraftTimeoutSec; c.timeoutSend = p.aiDraftTimeoutSend;
    c.cooldown = p.aiCooldownSec; c.playerCooldown = p.aiPlayerCooldownSec; c.perMinute = p.aiMaxPerMinute;
    c.queueMax = p.aiQueueMax; c.toolWait = p.aiToolWaitSec; c.histMax = p.aiHistoryMax;
    c.askBefore = p.aiAskBeforeModel; c.autoWhitelist = p.aiAutoWhitelist; c.noteInChat = p.aiNoteInChat;
    c.sound = p.aiDraftSound; c.flash = p.aiDraftFlash; c.saveHistory = p.aiSaveHistory;
    c.keyApprove = p.keyAiApprove; c.keyDiscard = p.keyAiDiscard;
    c.post.lowercase = p.aiPostLower; c.post.stripEmoji = p.aiPostStripEmoji; c.post.stripMarkdown = p.aiPostStripMarkdown;
    c.post.maxChars = p.aiPostMaxChars; c.post.blocklist = p.aiPostBlocklist; c.post.blockAction = p.aiPostBlockAction;
    c.post.prefix = p.aiPostPrefix; c.post.suffix = p.aiPostSuffix;
    c.failMessage = g_failMessage;
    return c;
}

// "F6 send, F7 discard" for whichever keys are bound; empty when none.
std::string KeyHint(const Cfg& c, const char* yes, const char* no) {
    std::string s;
    if (c.keyApprove > 0) s += KeyLabel(c.keyApprove) + " " + yes;
    if (c.keyDiscard > 0) { if (!s.empty()) s += ", "; s += KeyLabel(c.keyDiscard) + " " + no; }
    return s;
}

std::string Clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t cut = n;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

bool IsWhitelisted(const std::string& speaker) {
    const std::string key = ToLower(NormalizeSpeaker(speaker));
    if (key.empty()) return false;
    std::lock_guard lock(g_mtx);
    return g_whitelist.count(key) > 0;
}

// =============================================================================
// Telling me
// =============================================================================
void Notify(const std::string& line, const Cfg& c) {
    if (c.noteInChat) PushLocalNote(line, ImVec4(1.0f, 0.82f, 0.35f, 1.0f));
    if (c.sound || c.flash) ChatUi::Ping(c.sound, c.flash);
}

void NotifyReady(const Rec& r, const Cfg& c) {
    std::string hint = KeyHint(c, "send", "discard");
    Notify("[AI] reply to " + r.v.speaker + ": \"" + Clip(r.v.reply, 110) + "\"" + (hint.empty() ? "" : "  -  " + hint), c);
}

void NotifyAwaitingRun(const Rec& r, const Cfg& c) {
    std::string hint = KeyHint(c, "ask the AI", "skip");
    Notify("[AI] " + Clip(r.v.prompt, 110) + (hint.empty() ? "" : "  -  " + hint), c);
}

void NotifyTool(const Rec& r, const Cfg& c) {
    std::string hint = KeyHint(c, "allow", "deny");
    Notify("[AI] wants to run " + r.v.askTool + " " + Clip(r.v.askArgs, 80) + (hint.empty() ? "" : "  -  " + hint), c);
}

// =============================================================================
// History on disk
// =============================================================================
void AppendHistory(const Rec& r) {
    try {
        const std::wstring dir = GetThisModuleDirectory() + L"ailogs";
        CreateDirectoryW(dir.c_str(), nullptr);
        const time_t t = (time_t)(r.v.whenMs / 1000);
        struct tm tmv {};
        localtime_s(&tmv, &t);
        char day[16];
        strftime(day, sizeof(day), "%Y-%m-%d", &tmv);
        std::ofstream f(dir + L"\\" + Utf8ToWide(day) + L".jsonl", std::ios::app | std::ios::binary);
        if (!f.is_open()) return;
        std::string tools;
        for (const auto& e : r.tools) {
            if (!tools.empty()) tools += ",";
            tools += "\"" + JsonEscape(e.name) + "\"";
        }
        f << "{\"t\":" << r.v.whenMs << ",\"speaker\":\"" << JsonEscape(r.v.speaker) << "\",\"prompt\":\"" << JsonEscape(r.v.prompt)
          << "\",\"reply\":\"" << JsonEscape(r.v.reply) << "\",\"state\":\"" << StateName(r.v.state)
          << "\",\"tries\":" << r.v.tries << ",\"ms\":" << r.v.ms << ",\"model\":\"" << JsonEscape(r.v.model)
          << "\",\"tokens\":[" << r.v.promptTokens << "," << r.v.replyTokens << "],\"tools\":[" << tools << "]"
          << (r.v.error.empty() ? "" : ",\"error\":\"" + JsonEscape(r.v.error) + "\"") << "}\n";
    }
    catch (...) {}
}

void TrimHistoryLocked(int keep) {
    int finished = 0;
    for (const Rec& r : g_recs) if (Finished(r.v.state)) ++finished;
    for (auto it = g_recs.begin(); it != g_recs.end() && finished > keep;) {
        if (Finished(it->v.state)) { it = g_recs.erase(it); --finished; }
        else ++it;
    }
}

// =============================================================================
// Sending
// =============================================================================
// Puts the reply into chat, remembers the exchange, marks the draft sent.
void SendNow(uint64_t id) {
    Rec copy;
    {
        std::lock_guard lk(g_m);
        Rec* r = FindLocked(id);
        if (!r || r->v.reply.empty()) return;
        r->v.state = State::Sent;
        r->stateMs = NowSteadyMs();
        r->dryRun = false;
        copy = *r;
    }
    const Cfg c = ReadCfg();
    QueueBotReply(OneLine(copy.v.reply));
    g_replySuccess.fetch_add(1);
    {
        int keep;
        { std::lock_guard lock(g_settingsMtx); keep = g_maxMemory; }
        std::lock_guard lock(g_mtx);
        g_memory.push_back({ "user", copy.v.prompt });
        g_memory.push_back({ "assistant", copy.v.reply });
        while (g_memory.size() > (size_t)keep * 2) g_memory.pop_front();
    }
    if (c.saveHistory) AppendHistory(copy);
    LogToFile("AiFlow: sent draft " + std::to_string(id));
}

void MarkDiscarded(uint64_t id, const std::string& why) {
    Rec copy;
    bool log = false;
    Cfg c = ReadCfg();
    {
        std::lock_guard lk(g_m);
        Rec* r = FindLocked(id);
        if (!r || Finished(r->v.state)) return;
        r->v.state = State::Discarded;
        r->stateMs = NowSteadyMs();
        if (!why.empty()) r->v.note = why;
        g_jobs.erase(std::remove(g_jobs.begin(), g_jobs.end(), id), g_jobs.end());
        if (id == g_running) r->cancelled = true;
        log = c.saveHistory;
        copy = *r;
    }
    g_cv.notify_all();
    if (log) AppendHistory(copy);
}

// =============================================================================
// Tool questions (worker thread)
// =============================================================================
AI::ToolVerdict AskTool(uint64_t id, const std::string& name, std::string& args) {
    const Cfg c = ReadCfg();
    Rec copy;
    {
        std::lock_guard lk(g_m);
        Rec* r = FindLocked(id);
        if (!r) return AI::ToolVerdict::Deny;
        r->v.state = State::AwaitingTool;
        r->v.askTool = name;
        r->v.askArgs = args;
        r->toolDecision = 0;
        r->stateMs = NowSteadyMs();
        copy = *r;
    }
    NotifyTool(copy, c);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds((std::max)(5, c.toolWait));
    int decision = 0;
    std::unique_lock lk(g_m);
    for (;;) {
        Rec* r = FindLocked(id);
        if (!r) return AI::ToolVerdict::Deny;
        if (r->toolDecision != 0) { decision = r->toolDecision; args = r->v.askArgs; break; }
        if (g_shuttingDown.load() || AI::IsCancelRequested() || r->cancelled) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        g_cv.wait_for(lk, std::chrono::milliseconds(250));
    }
    if (Rec* r = FindLocked(id)) {
        if (!r->cancelled && r->v.state == State::AwaitingTool) r->v.state = State::Thinking;
        r->v.askTool.clear();
        r->v.askArgs.clear();
        r->toolDecision = 0;
        r->stateMs = NowSteadyMs();
    }
    return decision == 1 ? AI::ToolVerdict::Run : AI::ToolVerdict::Deny;
}

// =============================================================================
// One model call
// =============================================================================
void RunJob(uint64_t id) {
    std::string prompt, speaker, note, frozen;
    bool useFrozen = false;
    int myGen = 0;
    {
        std::lock_guard lk(g_m);
        Rec* r = FindLocked(id);
        if (!r || r->cancelled || Finished(r->v.state)) return;
        myGen = r->gen;
        r->v.state = State::Thinking;
        r->stateMs = NowSteadyMs();
        prompt = r->v.prompt;
        speaker = r->v.speaker;
        note = r->retryNote;
        useFrozen = r->useFrozen && r->frozenValid;
        frozen = r->chatBlock;
        r->tools.clear();
        r->v.error.clear();
    }

    AI::CallOptions opt;
    opt.speaker = speaker;
    opt.extraNote = note;
    opt.useFrozenChat = useFrozen;
    opt.frozenChat = frozen;
    opt.remember = false;                       // remembered when it is actually sent
    opt.approveTool = [id](const std::string& name, std::string& args) { return AskTool(id, name, args); };
    opt.onContext = [id](const AiContext::Built& b) {
        std::lock_guard lk(g_m);
        if (Rec* r = FindLocked(id)) {
            r->systemPrompt = b.systemPrompt;
            r->chatBlock = b.chatBlock;
            r->frozenValid = true;
        }
    };

    std::string resp;
    AI::Trace trace;
    bool ok = false;
    try { ok = AI::GetResponse(prompt, resp, opt, &trace); }
    catch (...) { trace.error = "the AI request threw"; }

    if (g_shuttingDown.load()) return;

    const Cfg c = ReadCfg();
    enum class Next { Nothing, Send, NotifyReady, Fail };
    Next next = Next::Nothing;
    Rec copy;
    {
        std::lock_guard lk(g_m);
        Rec* r = FindLocked(id);
        if (!r || r->gen != myGen) return;      // gone, or a retry has taken over this draft
        r->tools = trace.tools;
        r->requestJson = trace.requestJson;
        r->v.ms = trace.ms;
        r->v.promptTokens = trace.promptTokens;
        r->v.replyTokens = trace.replyTokens;
        r->v.model = trace.model;
        r->v.toolCount = (int)trace.tools.size();
        r->v.askTool.clear();
        r->v.askArgs.clear();
        r->useFrozen = false;
        r->retryNote.clear();
        r->stateMs = NowSteadyMs();

        if (r->cancelled || AI::IsCancelRequested()) {
            r->v.state = State::Discarded;
            if (r->v.note.empty()) r->v.note = "cancelled";
            return;
        }
        if (ok && !resp.empty()) {
            r->raw = resp;
            AiText::PostResult pr = AiText::PostProcess(resp, c.post);
            std::string text = OneLine(pr.text);
            r->notes = pr.notes;
            if (text.empty()) {
                r->v.state = State::Failed;
                r->v.error = "nothing was left of the reply after the clean-up";
                next = Next::Fail;
            }
            else {
                r->v.reply = text;
                r->v.note = pr.notes.empty() ? "" : pr.notes.front();
                for (size_t i = 1; i < pr.notes.size(); ++i) r->v.note += ", " + pr.notes[i];
                if (pr.blocked) r->forceReview = true;
                bool held = r->forceReview || r->v.source == Source::Studio;
                if (c.mode == 2 && !held) {
                    // Dry run: kept in the list, never spoken.
                    r->v.state = State::Discarded;
                    r->dryRun = true;
                    r->v.note = "dry run - not sent" + (r->v.note.empty() ? std::string() : ", " + r->v.note);
                    next = Next::Nothing;
                }
                else if (!held && (c.mode == 0 || (c.autoWhitelist && IsWhitelisted(r->v.speaker)))) {
                    next = Next::Send;
                }
                else {
                    r->v.state = State::Ready;
                    next = Next::NotifyReady;
                }
            }
        }
        else {
            r->v.state = State::Failed;
            r->v.error = trace.error.empty() ? "the AI request failed or came back empty" : trace.error;
            next = Next::Fail;
        }
        copy = *r;
    }

    switch (next) {
    case Next::Send:
        SendNow(id);
        break;
    case Next::NotifyReady:
        NotifyReady(copy, c);
        break;
    case Next::Fail:
        g_replyFail.fetch_add(1);
        // Automatic mode keeps the old behaviour: the bot says its failure message.
        if (copy.v.source == Source::Wake && c.mode == 0) {
            QueueBotReply(c.failMessage.empty() ? "sorry couldnt process that rn" : c.failMessage);
            LogToFile("AiFlow: draft " + std::to_string(id) + " failed: " + copy.v.error);
        }
        if (c.saveHistory) AppendHistory(copy);
        break;
    case Next::Nothing:
        if (c.saveHistory) AppendHistory(copy);
        break;
    }
}

void WorkerMain() {
    for (;;) {
        uint64_t id = 0;
        {
            std::lock_guard lk(g_m);
            if (g_shuttingDown.load() || g_jobs.empty()) {
                g_workerOn = false;
                g_running = 0;
                g_aiInProgress = false;
                return;
            }
            id = g_jobs.front();
            g_jobs.pop_front();
            g_running = id;
            g_aiInProgress = true;
        }
        try { RunJob(id); }
        catch (...) { LogToFile("AiFlow: RunJob threw"); }
        {
            std::lock_guard lk(g_m);
            g_running = 0;
        }
    }
}

// Starts the worker if there is work and none is running.
void Kick() {
    bool start = false;
    {
        std::lock_guard lk(g_m);
        if (!g_workerOn && !g_jobs.empty() && !g_shuttingDown.load()) {
            g_workerOn = true;
            g_aiInProgress = true;
            start = true;
        }
    }
    if (!start) return;
    try { StartWorker(WorkerMain); }
    catch (...) {
        std::lock_guard lk(g_m);
        g_workerOn = false;
        g_aiInProgress = false;
    }
}

DraftView ViewOf(const Rec& r) {
    DraftView v = r.v;
    v.attempts = (int)r.attempts.size();
    return v;
}

} // namespace

// =============================================================================
// Public
// =============================================================================
std::string KeyLabel(int vk) {
    if (vk <= 0) return "";
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
    char buf[16];
    snprintf(buf, sizeof(buf), "key %d", vk);
    return buf;
}

const char* StateName(State s) {
    switch (s) {
    case State::Queued: return "queued";
    case State::AwaitingRun: return "waiting for you";
    case State::Thinking: return "thinking";
    case State::AwaitingTool: return "wants a tool";
    case State::Ready: return "ready";
    case State::Sent: return "sent";
    case State::Discarded: return "discarded";
    case State::Failed: return "failed";
    }
    return "";
}

bool NeedsMe(State s) { return s == State::AwaitingRun || s == State::AwaitingTool || s == State::Ready; }

std::string OneLine(const std::string& text) {
    std::string out;
    bool space = false;
    for (char c : text) {
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!space && !out.empty()) { out += ' '; space = true; }
        }
        else {
            out += c;
            space = (c == ' ');
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

uint64_t Submit(const std::string& prompt, const std::string& speaker, Source src) {
    if (g_shuttingDown.load() || TrimCopy(prompt).empty()) return 0;
    const Cfg c = ReadCfg();
    const long long steady = NowSteadyMs();
    uint64_t id = 0;
    bool queued = false;
    Rec copy;
    {
        std::lock_guard lk(g_m);
        if (src == Source::Wake) {
            std::string why;
            const bool busy = g_workerOn || !g_jobs.empty();
            if (!g_limiter.Allow(steady, speaker, c.cooldown, c.playerCooldown, c.perMinute, &why))
                why = why.empty() ? "limit reached" : why;
            else if (busy && (int)g_jobs.size() >= c.queueMax)
                why = "the AI is busy";
            else
                why.clear();
            if (!why.empty()) {
                g_skips.push_back(Clip(speaker.empty() ? prompt : speaker, 24) + ": " + why);
                while (g_skips.size() > 8) g_skips.erase(g_skips.begin());
                LogToFile("AiFlow: wake skipped (" + why + ") " + Clip(prompt, 80));
                return 0;
            }
            g_limiter.Record(steady, speaker);
        }
        Rec r;
        r.v.id = id = ++g_nextId;
        r.v.whenMs = UnixNow();
        r.v.speaker = speaker;
        r.v.prompt = prompt;
        r.v.source = src;
        r.forceReview = src == Source::Studio;
        r.stateMs = steady;
        if (src == Source::Wake && c.askBefore) {
            r.v.state = State::AwaitingRun;
        }
        else {
            r.v.state = State::Queued;
            g_jobs.push_back(id);
            queued = true;
        }
        copy = r;
        g_recs.push_back(std::move(r));
        TrimHistoryLocked(c.histMax);
    }
    if (src == Source::Wake) g_wakeCount.fetch_add(1);
    if (queued) Kick();
    else NotifyAwaitingRun(copy, c);
    return id;
}

void Approve(uint64_t id) {
    State st = State::Queued;
    {
        std::lock_guard lk(g_m);
        Rec* r = FindLocked(id);
        if (!r) return;
        st = r->v.state;
    }
    if (st == State::AwaitingRun) {
        {
            std::lock_guard lk(g_m);
            if (Rec* r = FindLocked(id)) {
                r->v.state = State::Queued;
                r->stateMs = NowSteadyMs();
                g_jobs.push_back(id);
            }
        }
        Kick();
    }
    else if (st == State::Ready || st == State::Discarded) {     // a discarded one is "send it anyway"
        SendNow(id);
    }
    else if (st == State::Failed) {
        Retry(id);
    }
}

void Discard(uint64_t id) {
    bool wasRunning = false;
    {
        std::lock_guard lk(g_m);
        Rec* r = FindLocked(id);
        if (!r || r->v.state == State::Sent) return;
        wasRunning = id == g_running && (r->v.state == State::Thinking || r->v.state == State::AwaitingTool);
    }
    MarkDiscarded(id, "discarded");
    if (wasRunning) {
        AI::RequestCancel();
        AbortHttp();
    }
}

void Retry(uint64_t id, const std::string& note, bool freshChat) {
    {
        std::lock_guard lk(g_m);
        Rec* r = FindLocked(id);
        if (!r) return;
        const State s = r->v.state;
        if (s == State::Queued || s == State::Thinking || s == State::AwaitingTool) return;
        if (!r->v.reply.empty() && (r->attempts.empty() || r->attempts.back() != r->v.reply)) r->attempts.push_back(r->v.reply);
        r->v.tries++;
        r->v.state = State::Queued;
        r->v.error.clear();
        r->v.note.clear();
        r->v.source = r->v.source == Source::Studio ? Source::Studio : Source::Retry;
        r->retryNote = note;
        r->gen++;
        r->useFrozen = !freshChat;
        r->cancelled = false;
        r->dryRun = false;
        r->stateMs = NowSteadyMs();
        g_jobs.push_back(id);
    }
    Kick();
}

void SetReply(uint64_t id, const std::string& text) {
    std::lock_guard lk(g_m);
    Rec* r = FindLocked(id);
    if (!r) return;
    if (r->v.state == State::Ready || r->v.state == State::Discarded || r->v.state == State::Failed)
        r->v.reply = text;
}

void UseAttempt(uint64_t id, size_t index) {
    std::lock_guard lk(g_m);
    Rec* r = FindLocked(id);
    if (!r || index >= r->attempts.size()) return;
    std::swap(r->attempts[index], r->v.reply);
}

void AnswerTool(uint64_t id, bool allow, const std::string& editedArgs) {
    {
        std::lock_guard lk(g_m);
        Rec* r = FindLocked(id);
        if (!r || r->v.state != State::AwaitingTool) return;
        if (allow && !editedArgs.empty()) r->v.askArgs = editedArgs;
        r->toolDecision = allow ? 1 : 2;
    }
    g_cv.notify_all();
}

void Forget(uint64_t id) {
    std::lock_guard lk(g_m);
    for (auto it = g_recs.begin(); it != g_recs.end(); ++it)
        if (it->v.id == id && Finished(it->v.state)) { g_recs.erase(it); return; }
}

void ClearFinished() {
    std::lock_guard lk(g_m);
    g_recs.erase(std::remove_if(g_recs.begin(), g_recs.end(), [](const Rec& r) { return Finished(r.v.state); }), g_recs.end());
}

uint64_t OldestWaiting() {
    std::lock_guard lk(g_m);
    // A tool question first: the worker is blocked on it.
    for (const Rec& r : g_recs) if (r.v.state == State::AwaitingTool) return r.v.id;
    for (const Rec& r : g_recs) if (NeedsMe(r.v.state)) return r.v.id;
    return 0;
}

bool ApproveOldest() {
    const uint64_t id = OldestWaiting();
    if (!id) return false;
    State st;
    {
        std::lock_guard lk(g_m);
        Rec* r = FindLocked(id);
        if (!r) return false;
        st = r->v.state;
    }
    if (st == State::AwaitingTool) AnswerTool(id, true, "");
    else Approve(id);
    return true;
}

bool DiscardOldest() {
    const uint64_t id = OldestWaiting();
    if (!id) return false;
    State st;
    {
        std::lock_guard lk(g_m);
        Rec* r = FindLocked(id);
        if (!r) return false;
        st = r->v.state;
    }
    if (st == State::AwaitingTool) AnswerTool(id, false, "");
    else Discard(id);
    return true;
}

bool RetryNewest() {
    uint64_t id = 0;
    {
        std::lock_guard lk(g_m);
        for (auto it = g_recs.rbegin(); it != g_recs.rend(); ++it) {
            const State s = it->v.state;
            if (s == State::Ready || s == State::Failed || s == State::Sent || s == State::Discarded) { id = it->v.id; break; }
        }
    }
    if (!id) return false;
    Retry(id);
    return true;
}

std::vector<DraftView> List() {
    std::lock_guard lk(g_m);
    std::vector<DraftView> out;
    out.reserve(g_recs.size());
    for (const Rec& r : g_recs) out.push_back(ViewOf(r));
    return out;
}

bool Get(uint64_t id, Detail& out) {
    std::lock_guard lk(g_m);
    const Rec* r = nullptr;
    for (const Rec& x : g_recs) if (x.v.id == id) r = &x;
    if (!r) return false;
    out.view = ViewOf(*r);
    out.raw = r->raw;
    out.systemPrompt = r->systemPrompt;
    out.chatBlock = r->chatBlock;
    out.requestJson = r->requestJson;
    out.retryNote = r->retryNote;
    out.tools = r->tools;
    out.attempts = r->attempts;
    out.notes = r->notes;
    return true;
}

int WaitingCount() {
    std::lock_guard lk(g_m);
    int n = 0;
    for (const Rec& r : g_recs) if (NeedsMe(r.v.state)) ++n;
    return n;
}

int BusyCount() {
    std::lock_guard lk(g_m);
    int n = 0;
    for (const Rec& r : g_recs) if (r.v.state == State::Queued || r.v.state == State::Thinking) ++n;
    return n;
}

std::vector<std::string> RecentSkips() {
    std::lock_guard lk(g_m);
    return g_skips;
}

std::wstring BannerText() {
    const Cfg c = ReadCfg();
    std::lock_guard lk(g_m);
    const Rec* first = nullptr;
    int waiting = 0;
    bool thinking = false;
    for (const Rec& r : g_recs) {
        if (NeedsMe(r.v.state)) {
            ++waiting;
            if (!first || (r.v.state == State::AwaitingTool && first->v.state != State::AwaitingTool)) first = &r;
        }
        if (r.v.state == State::Thinking || r.v.state == State::Queued) thinking = true;
    }
    std::string s;
    if (first) {
        const std::string tail = waiting > 1 ? "  (+" + std::to_string(waiting - 1) + ")" : "";
        if (first->v.state == State::Ready) {
            const std::string h = KeyHint(c, "send", "drop");
            s = "AI reply to " + first->v.speaker + ": " + Clip(first->v.reply, 70) + (h.empty() ? "" : "  [" + h + "]") + tail;
        }
        else if (first->v.state == State::AwaitingTool) {
            const std::string h = KeyHint(c, "allow", "deny");
            s = "AI wants to run " + first->v.askTool + (h.empty() ? "" : "  [" + h + "]") + tail;
        }
        else {
            const std::string h = KeyHint(c, "run", "skip");
            s = "AI wake from " + first->v.speaker + (h.empty() ? "" : "  [" + h + "]") + tail;
        }
    }
    else if (thinking) {
        s = "AI thinking...";
    }
    return s.empty() ? std::wstring() : Utf8ToWide(s);
}

void Tick() {
    const Cfg c = ReadCfg();
    if (c.timeoutSec <= 0) return;
    const long long now = NowSteadyMs();
    std::vector<uint64_t> send, drop;
    {
        std::lock_guard lk(g_m);
        for (const Rec& r : g_recs) {
            if (now - r.stateMs < (long long)c.timeoutSec * 1000) continue;
            if (r.v.state == State::Ready) (c.timeoutSend ? send : drop).push_back(r.v.id);
            else if (r.v.state == State::AwaitingRun) drop.push_back(r.v.id);
        }
    }
    for (uint64_t id : send) SendNow(id);
    for (uint64_t id : drop) MarkDiscarded(id, "timed out");
}

void ResetLimits() {
    std::lock_guard lk(g_m);
    g_limiter.Clear();
    g_skips.clear();
}

void OnCancel() {
    std::vector<uint64_t> ids;
    {
        std::lock_guard lk(g_m);
        for (const Rec& r : g_recs)
            if (r.v.state == State::Queued || r.v.state == State::Thinking || r.v.state == State::AwaitingTool) ids.push_back(r.v.id);
        g_jobs.clear();
    }
    for (uint64_t id : ids) MarkDiscarded(id, "cancelled");
    g_cv.notify_all();
}

} // namespace AiFlow
