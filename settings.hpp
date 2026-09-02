#pragma once
// Settings symbols — definitions live in settings.cpp
#include "pch.h"
#include <string>
#include <set>
#include <mutex>
#include <atomic>

extern std::mutex g_settingsMtx;
extern bool g_showOwnFormatted;
extern bool g_allowSelfTrigger;
extern std::string g_localName;
extern int g_maxMemory;
extern int g_maxChatContext;
extern bool g_includeGlobalChat;
extern bool g_liveTranslate;
extern bool g_showTimestamps;
extern std::string g_outgoingLang;
extern bool g_coloredChat;
extern bool g_useNickname;
extern int g_nicknameDelayMs;
extern std::string g_aiPrefix;
extern std::string g_aiColorCode;
extern std::string g_nicknameOn;
extern std::string g_nicknameOff;
extern std::string g_wakeWord;
extern std::string g_wakeWord2;
extern std::string g_botName;
extern std::string g_ollamaModel;
extern std::string g_ollamaHost;
extern int g_ollamaPort;
extern float g_ollamaTemperature;
extern int g_ollamaNumPredict;
extern int g_ollamaNumCtx;
extern bool g_thinkEnabled;
extern std::string g_thinkLevel;
extern bool g_blockLeadingSlash;
extern std::string g_failMessage;
extern bool g_botMuted;
extern bool g_alwaysOnTop;
extern float g_windowOpacity;
extern bool g_whitelistMode;
extern int g_themePreset;

// AI provider: "ollama" or "openai"
extern std::string g_aiProvider;
extern std::string g_openaiBaseUrl;
extern std::string g_openaiApiKey;
extern std::string g_openaiModel;

// Web search (LangSearch) — tool omitted from schema when disabled
extern bool g_webSearchEnabled;
extern std::string g_webSearchApiKey;
extern int g_webSearchCount;
extern bool g_webSearchSummary;
extern std::string g_webSearchFreshness;
extern int g_webSearchMaxChars;

// Auto chat reactions (system messages)
extern bool g_autoWelcome;
extern bool g_autoWelcomeBack;
extern bool g_autoGg;
extern bool g_wordValueShow;
extern bool g_wordValueAuto;
extern bool g_morseShow;
extern bool g_morseAuto;
extern bool g_caesarShow;
extern bool g_caesarAuto;
extern bool g_countLettersShow;
extern bool g_countLettersAuto;
extern bool g_patternShow;
extern bool g_patternAuto;

// UI appearance (shared with ui.cpp)
extern float g_fontSize;
extern float g_uiScale;
extern float g_uiRounding;
extern float g_chatLineGap;  // vertical gap between chat log lines (px)
extern bool  g_compactChatMode; // minimal chat-only overlay (Shaeldor-style)
extern float g_compactOpacity;  // layered alpha for compact panel (see-through)
extern float g_compactFontSize; // GDI font height (px) used only by compact chat
extern int   g_compactMaxLines; // max recent lines kept when painting compact (perf)
extern float g_compactPadX;     // left/right text padding in compact (px)
extern float g_customAccent[3];
extern bool  g_customAccentDark;
extern bool  g_settingsDirty;
extern bool  g_windowStyleDirty;
extern bool  g_themeDirty;
extern bool  g_fontDirty;
extern int   g_appliedTheme;

// Private chat flags
extern bool g_privateMode;
extern bool g_privateDecryptDisplay;
extern bool g_privateViaMsg;

void LoadSettings();
void SaveSettings();
void LoadIgnored();
void SaveIgnored();
void LoadWhitelist();
void SaveWhitelist();
std::string GetCachedSystemPrompt();
bool ReloadSystemPrompt(bool force = false);

// Prompt status for UI
std::string GetPromptStatus();
std::string GetPromptPathUtf8();
bool IsPromptOk();

// Cancel in-flight AI + clear pending bot sends that are queued
void CancelAI();