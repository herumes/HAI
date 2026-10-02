#pragma once
// Settings symbols — definitions live in settings.cpp
#include "pch.h"
#include <string>
#include <set>
#include <mutex>
#include <atomic>
#include <cstdint>

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

// =============================================================================
// Everything added for the chat overhaul. Unlike the globals above, every field
// here is loaded, saved, clamped and reset through one table in settings.cpp,
// so a new option is one line there instead of a branch in Load and Save.
//
// Threading: the UI thread is the only writer and takes g_settingsMtx to write;
// it may read without the lock. Any other thread must lock to read.
// =============================================================================
enum class PrefGroup : uint8_t {
    ChatDisplay, Highlights, CopyLog, Input, Compact, Window, Hotkeys, Translation, Ai, Farm,
};

struct ChatPrefs {
    // ---- chat log display ----
    float chatFontSize = 0.0f;           // 0 = follow the UI font size
    int   timeFormat = 0;                // 0 "3:07 PM", 1 "15:07", 2 "15:07:42", 3 "3:07:42 PM", 4 custom
    std::string timeCustom = "%H:%M:%S"; // strftime pattern for timeFormat 4
    int   timeZone = 0;                  // 0 US Pacific (the old fixed behaviour), 1 this PC's zone, 2 UTC
    bool  gameColors = true;             // false = everything in the default text colour
    float minBrightness = 0.40f;         // lifts dark game colours (e.g. &0 black) so they read on a dark panel
    bool  accentBars = true;
    bool  zebra = false;                 // faint background on every other message
    bool  hangingIndent = true;          // wrapped rows line up after the timestamp
    bool  showTranslateChip = true;      // the small "tr" button after untranslated lines
    bool  autoScroll = true;             // follow new messages while scrolled to the bottom
    int   ignoredDisplay = 0;            // lines from ignored players: 0 show, 1 dim, 2 hide
    std::string hidePatterns;            // one per line; lines containing any of them are hidden
    int   maxLogLines = 400;             // history kept in memory
    ImVec4 colText{ 0.92f, 0.92f, 0.92f, 1.0f };
    ImVec4 colOutgoing{ 0.45f, 0.85f, 1.00f, 1.0f };
    ImVec4 colTimestamp{ 0.48f, 0.50f, 0.56f, 1.0f };
    ImVec4 colTranslation{ 0.70f, 0.95f, 0.70f, 1.0f };
    ImVec4 colSelection{ 0.26f, 0.46f, 0.82f, 0.55f };
    ImVec4 colSearchHit{ 1.00f, 0.82f, 0.25f, 0.35f };
    ImVec4 colMentionBg{ 1.00f, 0.78f, 0.25f, 0.10f };
    ImVec4 colBarOutgoing{ 0.45f, 0.85f, 1.00f, 0.85f };
    ImVec4 colBarBot{ 0.45f, 0.95f, 0.55f, 0.85f };
    ImVec4 colBarOther{ 0.45f, 0.50f, 0.62f, 0.55f };
    ImVec4 colBarMention{ 1.00f, 0.78f, 0.25f, 0.95f };
    ImVec4 colZebra{ 1.00f, 1.00f, 1.00f, 0.025f };

    // ---- highlights & alerts ----
    bool  highlightMyName = true;
    std::string highlightWords;          // comma- or newline-separated words / names
    bool  highlightWholeWord = true;
    int   mentionSound = 0;              // 0 none, 1 Windows notification sound, 2 beep, 3 .wav file
    std::string mentionSoundPath;
    bool  mentionFlash = true;           // flash the taskbar button
    bool  alertOnlyUnfocused = true;     // no sound/flash while the overlay itself is focused

    // ---- copy & logging ----
    int   copyCodeFormat = 0;            // 0 "&c" nearest, 1 "&#RRGGBB", 2 "§c" nearest, 3 "<#RRGGBB>"
    int   ctrlCMode = 1;                 // 0 plain text, 1 plain + rich (Word/Docs keep colours), 2 colour codes
    bool  copyTimestamps = true;         // line/log copies include the time
    bool  copyTranslations = true;       // line/log copies include the translation
    bool  richDarkBackground = true;     // rich copies sit on a dark panel so light colours stay readable
    bool  saveChatToDisk = false;        // append every line to chatlogs\YYYY-MM-DD.txt

    // ---- chat input ----
    std::string whisperTemplate = "/msg {name} ";
    int   inputHistory = 50;

    // ---- compact overlay ----
    int   compactX = -1, compactY = -1;  // -1 = bottom-left default
    int   compactW = 440, compactH = 210;
    ImVec4 compactBg{ 0.055f, 0.063f, 0.094f, 1.0f };
    float compactRadius = 14.0f;
    bool  compactShowPos = true;
    bool  compactTimestamps = true;
    bool  compactShadow = true;          // dark outline behind text - readable over bright scenery
    bool  compactEmoji = true;           // :heart: / <3 / :) become emoji
    std::string compactFont = "Segoe UI";
    int   compactFadeSec = 0;            // when not hovered, hide lines older than this (0 = never)
    bool  compactHideBgIdle = false;     // panel background only while hovered
    // what to show in front of a player's name (chatprefix.hpp); the defaults show everything
    bool  compactShowPrestige = true;    // "P. 69" in [P. 69 · Lvl. 7415]
    bool  compactShowLevel = true;       // "Lvl. 7415"
    int   compactRankMode = 0;           // [Titan]: 0 show, 1 hide, 2 hide all except the list, 3 hide only the list
    std::string compactRankList;         // rank names for modes 2 and 3, comma-separated
    bool  compactShowTitle = true;       // "Warden —" before the name

    // ---- window ----
    int   fullX = 100, fullY = 100, fullW = 920, fullH = 720;
    bool  startHidden = false;
    bool  tabIgnored = true, tabAi = true, tabTest = true, tabTools = true, tabPosition = true;
    bool  tabPlayer = true, tabPackets = true, tabShapes = true, tabBuild = true, tabCopy = true;
    bool  tabFarm = true;
    bool  tabStudio = true;              // the AI Studio tab (inbox, prompts, live context)
    bool  extraFonts = true;             // merge symbol / emoji / CJK / Korean fallbacks into the UI font
    std::string uiFontPath;              // empty = Segoe UI

    // ---- hotkeys (Win32 virtual-key codes, 0 = unbound) ----
    int   keyToggleOverlay = VK_INSERT;
    int   keyToggleCompact = VK_F8;
    int   keyFocusChat = 0;              // show the overlay on the Chat tab with the input focused
    bool  hotkeysNeedFocus = true;       // only while Hytale or the overlay is the active window
    int   keyFarmPlay = VK_F9;           // auto farm: start / stop driving the chosen route
    int   keyFarmRecord = VK_F10;        // auto farm: start / finish recording a lap
    int   keyAiApprove = VK_F6;          // AI review mode: send the oldest draft / allow the tool it asks about
    int   keyAiDiscard = VK_F7;          // ... throw it away / refuse the tool
    int   keyAiRetry = 0;                // ... ask again (0 = unbound)

    // ---- translation ----
    std::string txTarget = "en";         // language incoming chat is translated into

    // ---- AI sampling ----
    float topP = 0.95f;
    int   topK = 64;

    // ---- AI control (aiflow.hpp) ----
    int   aiReplyMode = 0;               // 0 send by itself, 1 ask me first (drafts), 2 never send (drafts only)
    bool  aiAskBeforeModel = false;      // hold each wake for my OK before the model is called at all
    bool  aiAutoWhitelist = false;       // "ask me first": replies to whitelisted players still go out by themselves
    int   aiDraftTimeoutSec = 0;         // a draft nobody answered is dealt with after this long (0 = never)
    int   aiDraftTimeoutSend = 0;        // ... 0 discard it, 1 send it
    int   aiToolPolicy = 0;              // 0 tools run by themselves, 1 ask before tools with side effects or costs, 2 ask before every tool
    std::string aiToolRules;             // per tool: name=off|ask|auto, separated by ;
    int   aiToolWaitSec = 60;            // an unanswered tool question counts as "no" after this long
    int   aiCooldownSec = 0;             // minimum gap between any two answers (0 = none)
    int   aiPlayerCooldownSec = 0;       // minimum gap between answers to the same player
    int   aiMaxPerMinute = 0;            // answers per minute, all players (0 = unlimited)
    bool  aiWakeAtStart = false;         // the wake word only counts at the start of a message ("hyai what is ...")
    int   aiQueueMax = 0;                // wakes kept while the AI is busy (0 = drop them, the old behaviour)
    bool  aiNoteInChat = true;           // a "draft ready" line in the chat log
    bool  aiDraftSound = true;           // a sound when something needs my OK
    bool  aiDraftFlash = true;           // flash the taskbar button too
    bool  aiSaveHistory = false;         // append every exchange to ailogs\YYYY-MM-DD.jsonl
    int   aiHistoryMax = 60;             // exchanges kept in the Studio

    // ---- AI prompt and context (aiprompt.hpp, aicontext.hpp) ----
    std::string aiStack = "+CONTEXT.md\n"; // preset files in use, in order: +name on, -name off
    bool  aiAutoSavePrompt = false;      // the prompt editor saves itself a moment after you stop typing
    std::string aiChatLineFmt = "{speaker}: {body}"; // a chat line in the context; also {plain} {time} {date}
    std::string aiChatIntro = "Recent server chat (context only, do not repeat it):";
    std::string aiChatOutro = "The latest message that mentioned you (reply to this, naturally, in character):";
    int   aiChatMaxAgeMin = 0;           // leave out chat older than this (0 = any age)
    int   aiChatMaxLineChars = 0;        // shorten long chat lines (0 = whole line)
    bool  aiChatIncludeBot = true;       // the bot's own earlier replies
    bool  aiChatIncludeSystem = true;    // announcements and other lines without a speaker

    // ---- AI reply clean-up (aitext.hpp) ----
    bool  aiPostLower = false;
    bool  aiPostStripEmoji = false;
    bool  aiPostStripMarkdown = false;
    int   aiPostMaxChars = 0;            // 0 = no limit
    std::string aiPostBlocklist;         // words the bot must not say
    int   aiPostBlockAction = 0;         // 0 mask them, 1 hold the reply for review
    std::string aiPostPrefix, aiPostSuffix;

    // ---- AI-made builds (buildscript.hpp, Build tab) ----
    std::string aiPrefabNotes;           // your standing wishes, added to every request ("survival blocks only", "dark wood")
    bool  aiPrefabNewChat = true;        // copy the full prompt with the request (off: the request alone, for a chat already going)
    bool  aiPrefabRace = false;          // the prompt that also teaches the server's custom race-track blocks (ai_prefab_prompt_race.md)
    int   aiPrefabMaxBlocks = 0;         // a script that expands past this is refused; 0 = no limit at all (the default)

    // ---- auto farm (autofarm.cpp) ----
    std::string farmKeys = "W A S D Space Shift Ctrl"; // what a recording captures and playback presses
    int   farmInputMode = 0;             // 0 automatic, 1 background keys, 2 direct keys (Hytale must be in front)
    int   farmInputLearned = 0;          // what automatic found: 0 not yet, 1 background works, 2 needs direct
    int   farmSteer = 0;                 // turn the camera like the recording: 0 when the lap turns it, 1 always, 2 never
    float farmDegPerCount = 0.0f;        // measured camera turn per mouse count; 0 = measure on first use
    float farmExtraLeadMs = 0.0f;        // on top of the automatic one-frame lead: + earlier, - later
    int   farmTiming = 50;               // 0 follow where you are closely .. 100 follow the recorded timing
    bool  farmUseRide = true;            // line up by the tractor's own direction once it's been found
    float farmMaxOff = 5.0f;             // blocks off the route before it gives up
    int   farmStuckSec = 4;              // no progress for this long = stuck
    bool  farmStopOnKeys = true;         // pressing a route key yourself hands control back
    int   farmFocusLoss = 0;             // direct keys and Hytale loses focus: 0 pause until it's back, 1 stop
    int   farmLaps = 0;                  // stop after this many laps (0 = keep going)
    int   farmMinutes = 0;               // stop after this long (0 = keep going)
    int   farmCountdown = 3;             // seconds, when started from the overlay's button
    bool  farmAutoFinish = true;         // a recording ends itself when you get back to where it started
    bool  farmSounds = true;
    bool  farmFlash = true;              // flash Hytale's taskbar button when it stops by itself
    bool  farmCompact = true;            // show what it's doing in the compact chat window
};
extern ChatPrefs g_prefs;
// Bumped whenever a display-affecting pref changes, so cached layouts rebuild.
extern std::atomic<uint32_t> g_prefsVersion;

// Call after changing g_prefs (under g_settingsMtx or not): marks the file dirty
// and invalidates cached chat layouts.
void MarkPrefsChanged();
// Put one group back to its defaults.
void ResetPrefsGroup(PrefGroup group);
// Saves when dirty, at most twice a second - a dragged slider used to rewrite
// the file on every frame.
void SaveSettingsThrottled();
// Timestamp for a chat line, in the configured format and zone.
std::string FormatChatTime(long long unixMs, bool forceSeconds = false);

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