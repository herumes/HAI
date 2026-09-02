#pragma once
#include "pch.h"
#include <functional>

// =============================================================================
// Constants (shared)
// =============================================================================
extern size_t HYTALE_CHAT_MAX;
extern size_t MAX_CHAT_CHUNK;
extern int    SEND_GAP_MS;
static const int    MAX_SEND_RETRIES = 5;
static const size_t KEEPALIVE_MAX = 8192;
static const size_t SEEN_PLAYERS_MAX = 150;

// =============================================================================
// Logging & String Helpers
// =============================================================================
void EnsurePacificTz();
void LogToFile(const std::string& msg);

std::string ToLower(std::string s);
std::string ToUpper(std::string s);
std::string TrimCopy(std::string s);
long long NowSteadyMs();
std::string NowHHMM();
std::string NowHHMMSS();
std::string FormatUptime();

// =============================================================================
// Paths
// =============================================================================
std::wstring GetThisModuleDirectory();
std::string WideToUtf8Path(const std::wstring& w);
std::string WideToUtf8(const wchar_t* w, int len);
std::wstring Utf8ToWide(const std::string& s);

// =============================================================================
// JSON helpers (lightweight, no external deps)
// =============================================================================
std::string JsonEscape(const std::string& s);
bool JsonUnescape(const std::string& in, std::string& out);
bool ExtractJsonStringField(const std::string& json, const char* key,
    std::string& value, size_t startPos = 0);

// =============================================================================
// Case-insensitive / word helpers
// =============================================================================
bool ContainsWord(const std::string& text, const std::string& word);
bool StartsWithCI(const std::string& text, const std::string& prefix);
bool HasWholeWord(const std::string& lowerHay, const std::string& word);

// =============================================================================
// Shutdown / worker helpers
// =============================================================================
extern std::atomic<bool> g_shuttingDown;
extern std::atomic<int>  g_hooksInFlight;
extern std::atomic<int>  g_workersAlive;

struct HookGuard {
    HookGuard() { g_hooksInFlight.fetch_add(1, std::memory_order_acq_rel); }
    ~HookGuard() { g_hooksInFlight.fetch_sub(1, std::memory_order_acq_rel); }
};

struct WorkerGuard {
    WorkerGuard() { g_workersAlive.fetch_add(1, std::memory_order_acq_rel); }
    ~WorkerGuard() { g_workersAlive.fetch_sub(1, std::memory_order_acq_rel); }
};

void CloseHttp(std::atomic<HINTERNET>& h);
void AbortHttp();
void StartWorker(std::function<void()> fn);
void DetachAllWorkers();

// WinHTTP handles used by AI / translate (declared here so AbortHttp can close them)
extern std::atomic<HINTERNET> g_aiSession;
extern std::atomic<HINTERNET> g_aiConnect;
extern std::atomic<HINTERNET> g_aiRequest;
extern std::atomic<HINTERNET> g_txSession;
extern std::atomic<HINTERNET> g_txConnect;
extern std::atomic<HINTERNET> g_txRequest;

