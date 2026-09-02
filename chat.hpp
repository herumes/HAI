#pragma once
// Chat state, memory, pending sends, speaker extraction types.
#include "pch.h"
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <set>
#include <atomic>
#include <memory>

struct ChatSeg {
    std::string text;
    ImVec4 color{ 0.92f, 0.92f, 0.92f, 1.0f };
};

enum class TxStatus : uint8_t { None, Pending, Done, Skipped, Failed };

struct ChatLine {
    uint64_t id = 0;
    std::string plain;
    std::vector<ChatSeg> segs;
    bool outgoing = false;
    std::string speaker;
    std::string body;
    std::string timeStr;
    TxStatus txStatus = TxStatus::None;
    std::string txText;
    std::string txFrom;
    std::string txTo;
};

struct ChatMessage {
    std::string role;
    std::string content;
};

struct GlobalChatEntry {
    std::string speaker;
    std::string body;
    std::string plain;
    bool fromBot = false;
};

struct PendingSend {
    std::string gameText;
    std::string typedText;
    std::string txFrom;
    std::string txTo;
    std::string txText;
    bool silent = false;
    int retries = 0;
    int delayBeforeMs = 0;
};

struct TranslateResult; // from translate.hpp

extern std::mutex g_mtx;
extern std::deque<ChatMessage> g_memory;
extern std::deque<GlobalChatEntry> g_globalChat;
extern std::vector<ChatLine> g_log;
extern std::vector<PendingSend> g_pending;
extern std::set<std::string> g_ignored;
extern std::set<std::string> g_whitelist;

extern HWND g_hWnd;
extern std::atomic<void*> g_pChat;
extern std::atomic<uint64_t> g_stringVT;
extern std::atomic<bool> g_aiInProgress;
extern std::atomic<bool> g_chatReady;
extern std::atomic<uint64_t> g_wakeCount;
extern std::atomic<uint64_t> g_replySuccess;
extern std::atomic<uint64_t> g_replyFail;
extern bool g_scroll;
extern char g_input[512];
extern char g_chatFilter[96];
extern char g_ignoreInput[64];
extern char g_localNameBuf[64];
extern char g_suggestFilter[64];
extern char g_whitelistInput[64];

void ProcessMessage(void* message);
void ProcessPending();
void QueueSend(PendingSend ps);
void QueueBotReply(const std::string& rawText);
void HandleAIRequest(const std::string& userPrompt);
bool HandleOwnerCommand(const std::string& rawText, bool fromChatLine);
void ApplyIncomingResult(uint64_t lineId, const TranslateResult& r, const std::string& tl);
void HandleTestAIRequest(const std::string& userPrompt);
void RunToolTest();
bool QueuePrivateEncrypted(const std::string& plaintext, const std::string& targetPlayer, bool viaMsg);

