#pragma once
// Auto replies: user-defined "when chat says X, send Y" rules.
//
// These replace the three hard-coded reactions (wb / welcome / gg), which were
// written for one server's join and rank-up messages. The old toggles seed the
// first rule set, so nothing changes for anyone who had them on.
#include <string>
#include <vector>
#include <cstdint>

struct AutoReplyRule {
    enum Match : int { Contains = 0, StartsWith = 1, Exact = 2, Regex = 3 };

    bool enabled = true;
    int match = Contains;
    bool caseSensitive = false;
    bool skipOwn = true;          // never react to lines you sent yourself
    int cooldownSec = 10;         // per rule
    std::string pattern;
    // Placeholders: {speaker} {me} {bot} {message}, {1}..{9} for regex groups,
    // plus everything echo understands: ${time} ${math(...)} ${random:N} ...
    std::string reply;

    // Not saved.
    std::string error;            // regex compile error, shown in the editor
    uint64_t fired = 0;
};

namespace AutoReply {
    // Reads auto_replies.txt beside the DLL. On first run there is no file, and
    // the rules are seeded from the old auto welcome / wb / gg toggles.
    void Load();
    std::vector<AutoReplyRule> GetRules();
    // Replaces the rule set, recompiles patterns and saves.
    void SetRules(std::vector<AutoReplyRule> rules);
    // Changes whenever the rule set is replaced or reloaded, so an editor
    // holding a copy knows to refresh it.
    uint64_t Generation();

    // Game thread: hand over a chat line. Only copies it into a queue - rules
    // (regexes especially) are evaluated on the UI thread, not in the hook.
    void Offer(const std::string& plain, const std::string& speaker,
        const std::string& body, bool fromLocal, bool fromBot);
    // UI thread, every frame: evaluate queued lines and send any replies.
    void Tick();

    // For the editor: what would happen if `sample` arrived from `speaker`.
    std::string Test(const std::string& sample, const std::string& speaker);
}
