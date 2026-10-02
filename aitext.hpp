#pragma once
// The pure text side of the AI: prompt variables, the preset stack, how the chat
// context is written out, cleaning up a reply, tool rules and trigger limits.
//
// Nothing here touches the game, the settings or the network, so all of it is
// covered by tests/aitest without Hytale running. The live parts (aicontext.cpp,
// aiflow.cpp) feed it plain values.
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace AiText {

// =============================================================================
// Variables: "{me}" "{time}" in prompts and templates
// =============================================================================
using Vars = std::map<std::string, std::string>;
// Replaces {name} with its value. Names not in `vars` (and stray braces) stay as typed,
// so a prompt that mentions JSON or {curly} things is never mangled. "\{" writes a literal {.
std::string ExpandVars(const std::string& text, const Vars& vars);

// Rough token count: about 4 bytes of ASCII per token, one token per other character.
// Good enough to see whether a context fits; never used to cut anything.
int EstimateTokens(const std::string& text);

// =============================================================================
// The prompt stack: which preset files are in use, and in what order
// =============================================================================
struct StackEntry {
    std::string name;
    bool enabled = true;
};
// One entry per line: "+name" (on) or "-name" (off). Blank lines and duplicates are dropped.
std::vector<StackEntry> ParseStack(const std::string& text);
std::string SerializeStack(const std::vector<StackEntry>& stack);
// Keeps the order of `stack`, drops entries with no file, appends files nobody listed yet
// (switched off, so a new file never changes the prompt behind your back).
std::vector<StackEntry> ReconcileStack(const std::vector<StackEntry>& stack,
    const std::vector<std::string>& available, bool newOnesEnabled = false);
// A file name that is safe to create: no path characters, no leading dots, 1..48 chars.
std::string SafePresetName(const std::string& in);

// =============================================================================
// The chat context sent with a question
// =============================================================================
struct CtxEntry {
    std::string speaker, body, plain;
    long long unixMs = 0;
    bool fromBot = false;
    bool hidden = false;        // switched off by hand in the live preview
};
struct CtxOptions {
    int maxLines = 20;
    std::string lineFormat = "{speaker}: {body}";   // also {plain} {time}
    int maxAgeMin = 0;                              // 0 = any age
    int maxLineChars = 0;                           // 0 = whole line
    bool includeBot = true;                         // the bot's own earlier replies
    bool includeSystem = true;                      // announcements and lines without a speaker
    long long nowMs = 0;                            // for the age filter
    std::function<std::string(long long)> timeFn;   // {time} for an arrival time; empty = HH:MM in UTC
};
struct CtxLine {
    int index;                  // into the entries passed in
    std::string text;
};
// The lines that make it into the context, oldest first. `maxLines` counts from the newest.
std::vector<CtxLine> BuildChatLines(const std::vector<CtxEntry>& entries, const CtxOptions& o);
std::string JoinLines(const std::vector<CtxLine>& lines);

// =============================================================================
// Cleaning up a reply before it is shown or sent
// =============================================================================
struct PostOptions {
    bool lowercase = false;         // A-Z only
    bool stripEmoji = false;
    bool stripMarkdown = false;     // **bold**, `code`, # headings, - bullets, [text](url)
    int maxChars = 0;               // 0 = no limit; cut at a sentence or word end
    std::string blocklist;          // words separated by commas / new lines
    int blockAction = 0;            // 0 mask the word with ***, 1 mark the reply blocked
    std::string prefix, suffix;
};
struct PostResult {
    std::string text;
    bool blocked = false;           // blocklist hit with blockAction 1
    std::vector<std::string> notes; // what was changed, for the draft view
};
PostResult PostProcess(const std::string& in, const PostOptions& o);
std::vector<std::string> SplitList(const std::string& list);   // commas, semicolons, new lines

// =============================================================================
// Tools
// =============================================================================
enum class ToolMode { Off, Ask, Auto };
struct ToolRules { std::map<std::string, ToolMode> byName; };
// "lookup_mob=auto;set_reminder=ask;web_search=off"
ToolRules ParseToolRules(const std::string& s);
std::string SerializeToolRules(const ToolRules& r);
// A rule for the tool wins; otherwise the policy: 0 run all, 1 ask when the tool has side
// effects or costs something, 2 ask for every tool.
ToolMode ResolveToolMode(const ToolRules& r, int policy, const std::string& name, bool sideEffect);

// =============================================================================
// Wake words
// =============================================================================
// Does the message *start* with one of the wake words? Leading @ / punctuation and a
// greeting ("hey hyai", "yo, hyai") are allowed. Case-insensitive.
bool WakeAtStart(const std::string& message, const std::vector<std::string>& wakes);

// =============================================================================
// How often the bot may answer
// =============================================================================
class RateLimiter {
public:
    // Would an answer to `who` at `nowMs` be allowed? Does not record anything.
    bool Allow(long long nowMs, const std::string& who, int globalCooldownSec, int playerCooldownSec,
        int perMinute, std::string* why = nullptr) const;
    void Record(long long nowMs, const std::string& who);
    void Clear();
private:
    std::deque<long long> recent_;                  // every recorded answer in the last minute
    long long lastAny_ = 0;
    std::map<std::string, long long> lastByWho_;
};

} // namespace AiText
