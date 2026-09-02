#pragma once
// Cross-module types and declarations shared by chat_core, hooks, ui, translate, settings.
#include "pch.h"
#include "chat.hpp"
#include "translate.hpp"
#include <deque>
#include <set>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>

// ---------------------------------------------------------------------------
// Language table
// ---------------------------------------------------------------------------
struct LangInfo {
    const char* code;
    const char* name;
    const char* prefix;
};

inline const LangInfo OUT_LANGS[] = {
    { "",   "As-is",       nullptr },
    { "de", "German",      "/de " },
    { "fr", "French",      "/fr " },
    { "fr-CA", "French (Canada)",  "/fr-ca " },
    { "es", "Spanish",     "/es " },
    { "it", "Italian",     "/it " },
    { "pt", "Portuguese",  "/pt " },
    { "nl", "Dutch",       "/nl " },
    { "pl", "Polish",      "/pl " },
    { "ru", "Russian",     "/ru " },
    { "tr", "Turkish",     "/tr " },
    { "sv", "Swedish",     "/sv " },
    { "uk", "Ukrainian",   "/uk " },
};
inline constexpr size_t OUT_LANGS_COUNT = sizeof(OUT_LANGS) / sizeof(OUT_LANGS[0]);

const char* LangName(const std::string& code);
std::string LangBadge(const std::string& code);
ImVec4 LangColor(const std::string& code);

// ---------------------------------------------------------------------------
// Supporting types
// ---------------------------------------------------------------------------
struct RecentOut {
    std::string gameText;
    std::string typedText;
    std::string bodyKey;
    bool silent = false;
    long long whenMs = 0;
};

struct SeenPlayer {
    std::string display;
    std::string key;
    long long lastSeenMs = 0;
};

struct DedupEntry {
    void* ptr = nullptr;
    std::string plain;
    long long whenMs = 0;
};

// ---------------------------------------------------------------------------
// Runtime state (defined in chat_core.cpp unless noted)
// ---------------------------------------------------------------------------
extern WNDPROC g_oWndProc;
extern std::atomic<bool> g_show;
extern std::atomic<uint64_t> g_chatVT;
extern std::atomic<DWORD> g_gameThread;
extern std::atomic<bool> g_inProcessPending;
extern std::atomic<bool> g_sendDisabled;
extern const UINT JOSE_TIMER_ID;

extern std::vector<std::shared_ptr<uint8_t>> g_keepalive;
extern uint64_t g_nextLineId;
extern char g_ignoreInput[64];
extern char g_localNameBuf[64];
extern char g_suggestFilter[64];
extern std::deque<SeenPlayer> g_seenPlayers;
extern std::chrono::steady_clock::time_point g_startTime;
extern HANDLE g_installerThread;
extern std::atomic<bool> g_chatHooked;

extern std::string g_lastWakePrompt;
extern std::chrono::steady_clock::time_point g_lastWakeTime;
extern std::mutex g_wakeMtx;
extern std::atomic<long long> g_lastSendMs;
extern std::deque<RecentOut> g_recentOut;
extern std::mutex g_dedupMtx;
extern std::deque<DedupEntry> g_recentProcessed;
extern long long g_lastAutoReactMs;

// UI buffers (definitions in chat_core.cpp)
extern char g_aiPrefixBuf[96];
extern char g_aiColorCodeBuf[16];
extern char g_nicknameOnBuf[96];
extern char g_nicknameOffBuf[64];
extern char g_wakeWordBuf[64];
extern char g_wakeWord2Buf[64];
extern char g_botNameBuf[64];
extern char g_ollamaModelBuf[96];
extern char g_ollamaHostBuf[64];
extern char g_thinkLevelBuf[16];
extern char g_failMessageBuf[256];
extern char g_openaiBaseBuf[256];
extern char g_openaiKeyBuf[256];
extern char g_openaiModelBuf[96];
extern char g_webSearchKeyBuf[256];
extern char g_webSearchFreshBuf[32];
extern char g_whitelistInput[64];
extern char g_privatePassBuf[128];
extern char g_privateTargetBuf[64];
extern char g_privateMsgBuf[512];

// Test chat / tools
extern std::vector<ChatMessage> g_testChat;
extern char g_testInput[1024];
extern bool g_testScroll;
extern std::atomic<bool> g_testAiInProgress;
extern std::string g_testStatus;
extern int g_toolKind;
extern char g_toolQuery[256];
extern char g_toolDrops[128];
extern char g_toolTrait[128];
extern int g_toolMinHealth;
extern int g_toolMaxHealth;
extern int g_toolLimit;
extern char g_toolQuality[64];
extern char g_toolCategory[64];
extern char g_toolRemUser[64];
extern char g_toolRemMsg[256];
extern int g_toolInSeconds;
extern int g_toolRemId;
extern char g_toolFresh[32];
extern std::string g_toolResult;
extern bool g_toolScroll;

// Overlay (defined in ui.cpp)
extern HWND g_hwndOverlay;
extern ID3D11Device* g_pd3dDeviceOverlay;
extern ID3D11DeviceContext* g_pd3dDeviceContextOverlay;
extern IDXGISwapChain* g_pSwapChainOverlay;
extern ID3D11RenderTargetView* g_mainRenderTargetViewOverlay;

// TX internal (translate.cpp)
extern std::mutex g_txMtx;
extern std::atomic<bool> g_txInProgress;

// Helpers implemented in chat_core
void RememberOutgoing(const PendingSend& ps);
void InvalidateChatSession(const char* why);
bool ChatObjectAlive(void* chat);
bool ChatSessionStable();
std::string NormalizeSpeaker(std::string speaker);
bool IsBotIdentity(const std::string& name);
bool IsReservedSpeakerName(const std::string& key);
bool LooksLikeBotLine(const std::string& speaker, const std::string& body, const std::string& plain);
bool IsLocalPlayerSpeaker(const std::string& speaker, const std::string& localNameRaw);