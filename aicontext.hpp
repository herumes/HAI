#pragma once
// Everything the model is sent for one question, worked out in one place - so the
// live preview in the Studio shows exactly what a real request would contain.
#include "aitext.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace AiContext {

// {me} {bot} {speaker} {wake} {time} {date} {weekday} {pos} {players}: what prompts and templates can use.
AiText::Vars LiveVars(const std::string& speaker);

struct Request {
    std::string userPrompt;         // "Steve: hey hyai what is a kweebec"
    std::string speaker;            // "Steve" - for {speaker} in prompts
    std::string extraNote;          // one-off instruction for this reply (retry with a note)
    bool useFrozen = false;         // reuse `frozenChat` instead of the live chat (a retry sees what the first try saw)
    std::string frozenChat;
};

struct Part {
    enum Kind { System, Memory, Note, User } kind = User;
    std::string role;               // as sent: system / user / assistant
    std::string label;              // for the preview
    std::string text;
    int tokens = 0;
    size_t memIndex = 0;            // Memory: position in the conversation memory, to delete it
};

struct ChatLineInfo {
    uint64_t id = 0;                // GlobalChatEntry::id, to switch it off
    std::string text;
};

struct Built {
    std::vector<Part> parts;                // in the order they are sent
    std::string systemPrompt;
    std::string chatBlock;                  // the chat lines, one per line
    std::vector<ChatLineInfo> chatLines;    // the same, one by one
    std::string intro, outro;               // the wording around the chat lines, variables filled in
    int presets = 0;                        // system prompt presets that are in use
    int tokSystem = 0, tokMemory = 0, tokChat = 0, tokUser = 0, tokTools = 0, tokTotal = 0;
    int numCtx = 0, numPredict = 0;         // the model's window and the room kept for its answer
    std::string toolsJson;
};

Built Build(const Request& req);

// The "messages" array of a chat request.
std::string MessagesJson(const Built& b);

} // namespace AiContext
