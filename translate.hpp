#pragma once
// Live translation (Google).
#include "pch.h"
#include <string>
#include <atomic>

struct TranslateResult {
    bool ok = false;
    std::string translated;
    std::string detected;
};

struct TranslateJob {
    uint64_t lineId = 0;
    std::string text;
    std::string sl;
    std::string tl;
    bool priority = false;
    bool outgoing = false;
};

extern std::atomic<uint32_t> g_txCalls;
extern std::atomic<uint32_t> g_txCacheHits;
extern std::atomic<uint32_t> g_txSkipped;

void EnsureTxWorker();
void RequestIncomingTranslate(uint64_t lineId, const std::string& body, bool priority);
void RequestOutgoingTranslate(const std::string& text, const std::string& tl);
bool ShouldAutoTranslate(const std::string& text);
bool StripLangPrefix(std::string& text, std::string& langOut);

size_t TxCacheSize();
size_t TxQueueSize();
