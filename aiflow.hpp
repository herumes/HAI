#pragma once
// From "someone said the wake word" to "a reply went out": the trigger limits, the
// queue, the review step (send / edit / retry / discard), tool questions, the
// clean-up of the reply, and a history of every exchange.
//
// Every wake becomes a Draft. In automatic mode a finished draft is sent at once; in
// review mode it waits for a person. Either way it stays in the list, so the Studio
// can show what was asked, what the model saw, which tools it used, and offer a retry.
//
// Threads: Submit / Approve / ... may be called from any thread. The model runs on
// one worker at a time. Nothing here holds its lock while calling out (chat, the
// model, the settings), so a slow model never freezes the overlay.
#include "ai.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace AiFlow {

enum class State {
    Queued,         // waiting for the worker
    AwaitingRun,    // waiting for my OK before the model is called ("ask before asking")
    Thinking,       // the model is working
    AwaitingTool,   // the model wants to run a tool and I have to say yes or no
    Ready,          // a reply is waiting for me
    Sent,
    Discarded,      // thrown away (or a dry run: never sent, but can still be sent by hand)
    Failed,
};
const char* StateName(State s);
bool NeedsMe(State s);      // AwaitingRun, AwaitingTool, Ready

enum class Source { Wake, Studio, Retry };

// The list view: small, safe to copy every frame.
struct DraftView {
    uint64_t id = 0;
    long long whenMs = 0;           // unix ms of the wake
    std::string speaker, prompt;
    Source source = Source::Wake;
    State state = State::Queued;
    std::string reply;              // the text as it will be sent (after my edits)
    std::string error;
    std::string note;               // why it is held / what was cleaned up
    int tries = 1;
    int attempts = 0;               // earlier replies kept for this wake
    std::string askTool, askArgs;   // AwaitingTool: what it wants to run
    long long ms = 0;               // how long the model took
    int promptTokens = -1, replyTokens = -1;
    std::string model;
    int toolCount = 0;
};

// One draft in full, for the inspector.
struct Detail {
    DraftView view;
    std::string raw;                    // the model's words before the clean-up
    std::string systemPrompt;
    std::string chatBlock;              // the chat lines it was shown
    std::string requestJson;
    std::string retryNote;
    std::vector<AI::ToolEvent> tools;
    std::vector<std::string> attempts;
    std::vector<std::string> notes;     // what the clean-up changed
};

// A wake word was heard. Applies the limits, then either queues the model call or
// (ask-before-asking) waits for approval. Returns 0 when it was skipped, else the draft id.
uint64_t Submit(const std::string& prompt, const std::string& speaker, Source src = Source::Wake);

void Approve(uint64_t id);                          // AwaitingRun: run the model. Ready / dry run: send.
void Discard(uint64_t id);                          // drop it (a running one is cancelled)
void Retry(uint64_t id, const std::string& note = "", bool freshChat = false);
void SetReply(uint64_t id, const std::string& text);            // my edit of a Ready draft
void UseAttempt(uint64_t id, size_t index);                     // swap an earlier reply back in
void AnswerTool(uint64_t id, bool allow, const std::string& editedArgs);
void Forget(uint64_t id);                                       // remove a finished draft from the list
void ClearFinished();

// The hotkey / owner-command targets: the oldest thing waiting for me.
bool ApproveOldest();
bool DiscardOldest();
bool RetryNewest();
uint64_t OldestWaiting();

std::vector<DraftView> List();                      // oldest first
bool Get(uint64_t id, Detail& out);
int WaitingCount();                                 // drafts that need me
int BusyCount();                                    // queued + thinking
std::vector<std::string> RecentSkips();             // "Steve: cooldown 12s left", newest last

// The compact overlay's status text ("AI reply to Steve ready: ..." / "AI thinking...").
std::wstring BannerText();

// Once per frame from the overlay thread: timeouts, and the "needs me" notification.
void Tick();
// The cancel button / owner command: stop the running request, drop the queue.
void OnCancel();
// Forget the cooldown and per-minute counters and the list of skipped wakes.
void ResetLimits();

// "F6", "P", "key 219" - for hints; empty for an unbound key.
std::string KeyLabel(int vk);

// Pure helper shared with the tests: what actually goes into chat (one line, trimmed).
std::string OneLine(const std::string& text);

} // namespace AiFlow
