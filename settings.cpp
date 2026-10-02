#include "pch.h"
#include "settings.hpp"
#include "common.hpp"
#include "chat.hpp"
#include "shared.hpp"
#include "prefab.hpp"   // build-guide hotkeys, persisted here with the rest

#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstddef>

// =============================================================================
// Settings globals (definitions — declared in settings.hpp)
// =============================================================================
std::mutex g_settingsMtx;
bool g_showOwnFormatted = false;
bool g_allowSelfTrigger = false;
std::string g_localName = "Player";
int g_maxMemory = 4;
int g_maxChatContext = 20;
bool g_includeGlobalChat = true;
bool g_liveTranslate = false;
bool g_showTimestamps = true;
std::string g_outgoingLang;
bool g_coloredChat = false;
bool g_useNickname = false;
int g_nicknameDelayMs = 400;
std::string g_aiPrefix = "HyAI > ";
std::string g_aiColorCode = "&a";
std::string g_nicknameOn = "/nickname &cHYAI";
std::string g_nicknameOff = "/nickname off";
std::string g_wakeWord = "hyai";
std::string g_wakeWord2;
std::string g_botName = "HyAI";
std::string g_ollamaModel = "gemma4:e4b";
std::string g_ollamaHost = "localhost";
int g_ollamaPort = 11434;
float g_ollamaTemperature = 1.0f;
int g_ollamaNumPredict = 512;
int g_ollamaNumCtx = 32768;
bool g_thinkEnabled = false;
std::string g_thinkLevel = "false";
bool g_blockLeadingSlash = true;
std::string g_failMessage = "sorry couldnt process that rn";
bool g_botMuted = false;
bool g_alwaysOnTop = false;
float g_windowOpacity = 1.0f;
bool g_whitelistMode = false;
int g_themePreset = 0;
std::string g_aiProvider = "ollama";
std::string g_openaiBaseUrl = "https://api.openai.com";
std::string g_openaiApiKey;
std::string g_openaiModel = "gpt-4o-mini";
bool g_webSearchEnabled = false;
std::string g_webSearchApiKey;
int g_webSearchCount = 4;
bool g_webSearchSummary = false;
std::string g_webSearchFreshness = "noLimit";
int g_webSearchMaxChars = 1600;
bool g_autoWelcome = false;
bool g_autoWelcomeBack = false;
bool g_autoGg = false;
bool g_wordValueShow = true;
bool g_wordValueAuto = false;
bool g_morseShow = true;
bool g_morseAuto = false;
bool g_caesarShow = true;
bool g_caesarAuto = false;
bool g_countLettersShow = true;
bool g_countLettersAuto = false;
bool g_patternShow = true;
bool g_patternAuto = false;

// UI-related settings used by both settings load and ui
float g_fontSize = 16.0f;
float g_uiScale = 1.0f;
float g_uiRounding = 6.0f;
float g_chatLineGap = 1.0f;  // tighter default (was hard-coded 3px)
bool  g_compactChatMode = false;
float g_compactOpacity = 0.78f;
float g_compactFontSize = 17.0f;
int   g_compactMaxLines = 80;
float g_compactPadX = 10.0f;
float g_customAccent[3] = { 0.45f, 0.65f, 1.00f };
bool  g_customAccentDark = true;
bool  g_settingsDirty = false;
bool  g_windowStyleDirty = true;
bool  g_themeDirty = true;
bool  g_fontDirty = false;
int   g_appliedTheme = -1;

// Private chat UI flags (settings-persisted)
bool g_privateMode = false;
bool g_privateDecryptDisplay = true;
bool g_privateViaMsg = true;

ChatPrefs g_prefs;
std::atomic<uint32_t> g_prefsVersion{ 1 };

// =============================================================================
// The prefs table
// =============================================================================
// One row per ChatPrefs field. Load parses and clamps by type, Save writes every
// row, and Reset copies the field back from a default-constructed ChatPrefs -
// which is why rows store an offset rather than a pointer.
namespace {

enum class PT : uint8_t { Bool, Int, Float, Str, Color };

struct PrefRow {
    const char* key;
    PT type;
    size_t offset;
    float lo, hi;
    PrefGroup group;
};

#define PREF(key, member, type, lo, hi, group) \
    PrefRow{ key, PT::type, offsetof(ChatPrefs, member), (float)(lo), (float)(hi), PrefGroup::group }

const std::vector<PrefRow>& PrefTable() {
    static const std::vector<PrefRow> rows = {
        PREF("chat.fontsize",        chatFontSize,       Float, 0, 40,  ChatDisplay),
        PREF("chat.timeformat",      timeFormat,         Int,   0, 4,   ChatDisplay),
        PREF("chat.timecustom",      timeCustom,         Str,   0, 0,   ChatDisplay),
        PREF("chat.timezone",        timeZone,           Int,   0, 2,   ChatDisplay),
        PREF("chat.gamecolors",      gameColors,         Bool,  0, 0,   ChatDisplay),
        PREF("chat.minbright",       minBrightness,      Float, 0, 1,   ChatDisplay),
        PREF("chat.accentbars",      accentBars,         Bool,  0, 0,   ChatDisplay),
        PREF("chat.zebra",           zebra,              Bool,  0, 0,   ChatDisplay),
        PREF("chat.hangindent",      hangingIndent,      Bool,  0, 0,   ChatDisplay),
        PREF("chat.trchip",          showTranslateChip,  Bool,  0, 0,   ChatDisplay),
        PREF("chat.autoscroll",      autoScroll,         Bool,  0, 0,   ChatDisplay),
        PREF("chat.ignoreddisplay",  ignoredDisplay,     Int,   0, 2,   ChatDisplay),
        PREF("chat.hidepatterns",    hidePatterns,       Str,   0, 0,   ChatDisplay),
        PREF("chat.maxlines",        maxLogLines,        Int,   100, 5000, ChatDisplay),
        PREF("chat.col.text",        colText,            Color, 0, 0,   ChatDisplay),
        PREF("chat.col.outgoing",    colOutgoing,        Color, 0, 0,   ChatDisplay),
        PREF("chat.col.time",        colTimestamp,       Color, 0, 0,   ChatDisplay),
        PREF("chat.col.translation", colTranslation,     Color, 0, 0,   ChatDisplay),
        PREF("chat.col.selection",   colSelection,       Color, 0, 0,   ChatDisplay),
        PREF("chat.col.searchhit",   colSearchHit,       Color, 0, 0,   ChatDisplay),
        PREF("chat.col.mentionbg",   colMentionBg,       Color, 0, 0,   ChatDisplay),
        PREF("chat.col.barout",      colBarOutgoing,     Color, 0, 0,   ChatDisplay),
        PREF("chat.col.barbot",      colBarBot,          Color, 0, 0,   ChatDisplay),
        PREF("chat.col.barother",    colBarOther,        Color, 0, 0,   ChatDisplay),
        PREF("chat.col.barmention",  colBarMention,      Color, 0, 0,   ChatDisplay),
        PREF("chat.col.zebra",       colZebra,           Color, 0, 0,   ChatDisplay),

        PREF("hl.myname",            highlightMyName,    Bool,  0, 0,   Highlights),
        PREF("hl.words",             highlightWords,     Str,   0, 0,   Highlights),
        PREF("hl.wholeword",         highlightWholeWord, Bool,  0, 0,   Highlights),
        PREF("hl.sound",             mentionSound,       Int,   0, 3,   Highlights),
        PREF("hl.soundpath",         mentionSoundPath,   Str,   0, 0,   Highlights),
        PREF("hl.flash",             mentionFlash,       Bool,  0, 0,   Highlights),
        PREF("hl.onlyunfocused",     alertOnlyUnfocused, Bool,  0, 0,   Highlights),

        PREF("copy.codeformat",      copyCodeFormat,     Int,   0, 3,   CopyLog),
        PREF("copy.ctrlc",           ctrlCMode,          Int,   0, 2,   CopyLog),
        PREF("copy.timestamps",      copyTimestamps,     Bool,  0, 0,   CopyLog),
        PREF("copy.translations",    copyTranslations,   Bool,  0, 0,   CopyLog),
        PREF("copy.richdark",        richDarkBackground, Bool,  0, 0,   CopyLog),
        PREF("log.todisk",           saveChatToDisk,     Bool,  0, 0,   CopyLog),

        PREF("input.whisper",        whisperTemplate,    Str,   0, 0,   Input),
        PREF("input.history",        inputHistory,       Int,   0, 500, Input),

        PREF("compact.x",            compactX,           Int,   -32000, 32000, Compact),
        PREF("compact.y",            compactY,           Int,   -32000, 32000, Compact),
        PREF("compact.w",            compactW,           Int,   160, 4000, Compact),
        PREF("compact.h",            compactH,           Int,   100, 4000, Compact),
        PREF("compact.bg",           compactBg,          Color, 0, 0,   Compact),
        PREF("compact.radius",       compactRadius,      Float, 0, 40,  Compact),
        PREF("compact.showpos",      compactShowPos,     Bool,  0, 0,   Compact),
        PREF("compact.timestamps",   compactTimestamps,  Bool,  0, 0,   Compact),
        PREF("compact.shadow",       compactShadow,      Bool,  0, 0,   Compact),
        PREF("compact.emoji",        compactEmoji,       Bool,  0, 0,   Compact),
        PREF("compact.font",         compactFont,        Str,   0, 0,   Compact),
        PREF("compact.fadesec",      compactFadeSec,     Int,   0, 3600, Compact),
        PREF("compact.hidebgidle",   compactHideBgIdle,  Bool,  0, 0,   Compact),
        PREF("compact.showprestige", compactShowPrestige, Bool, 0, 0,   Compact),
        PREF("compact.showlevel",    compactShowLevel,   Bool,  0, 0,   Compact),
        PREF("compact.rankmode",     compactRankMode,    Int,   0, 3,   Compact),
        PREF("compact.ranklist",     compactRankList,    Str,   0, 0,   Compact),
        PREF("compact.showtitle",    compactShowTitle,   Bool,  0, 0,   Compact),

        PREF("win.x",                fullX,              Int,   -32000, 32000, Window),
        PREF("win.y",                fullY,              Int,   -32000, 32000, Window),
        PREF("win.w",                fullW,              Int,   480, 8000, Window),
        PREF("win.h",                fullH,              Int,   320, 8000, Window),
        PREF("win.starthidden",      startHidden,        Bool,  0, 0,   Window),
        PREF("tab.ignored",          tabIgnored,         Bool,  0, 0,   Window),
        PREF("tab.ai",               tabAi,              Bool,  0, 0,   Window),
        PREF("tab.test",             tabTest,            Bool,  0, 0,   Window),
        PREF("tab.tools",            tabTools,           Bool,  0, 0,   Window),
        PREF("tab.position",         tabPosition,        Bool,  0, 0,   Window),
        PREF("tab.player",           tabPlayer,          Bool,  0, 0,   Window),
        PREF("tab.packets",          tabPackets,         Bool,  0, 0,   Window),
        PREF("tab.shapes",           tabShapes,          Bool,  0, 0,   Window),
        PREF("tab.build",            tabBuild,           Bool,  0, 0,   Window),
        PREF("tab.copy",             tabCopy,            Bool,  0, 0,   Window),
        PREF("tab.farm",             tabFarm,            Bool,  0, 0,   Window),
        PREF("font.extra",           extraFonts,         Bool,  0, 0,   Window),
        PREF("font.uipath",          uiFontPath,         Str,   0, 0,   Window),

        PREF("key.overlay",          keyToggleOverlay,   Int,   0, 255, Hotkeys),
        PREF("key.compact",          keyToggleCompact,   Int,   0, 255, Hotkeys),
        PREF("key.focuschat",        keyFocusChat,       Int,   0, 255, Hotkeys),
        PREF("key.needfocus",        hotkeysNeedFocus,   Bool,  0, 0,   Hotkeys),
        PREF("key.farmplay",         keyFarmPlay,        Int,   0, 255, Hotkeys),
        PREF("key.farmrecord",       keyFarmRecord,      Int,   0, 255, Hotkeys),

        PREF("tx.target",            txTarget,           Str,   0, 0,   Translation),

        PREF("ai.topp",              topP,               Float, 0, 1,   Ai),
        PREF("ai.topk",              topK,               Int,   0, 500, Ai),

        PREF("farm.keys",            farmKeys,           Str,   0, 0,   Farm),
        PREF("farm.input",           farmInputMode,      Int,   0, 2,   Farm),
        PREF("farm.inputlearned",    farmInputLearned,   Int,   0, 2,   Farm),
        PREF("farm.steer",           farmSteer,          Int,   0, 2,   Farm),
        PREF("farm.degpercount",     farmDegPerCount,    Float, -10, 10, Farm),
        PREF("farm.extralead",       farmExtraLeadMs,    Float, -40, 60, Farm),
        PREF("farm.timing",          farmTiming,         Int,   0, 100, Farm),
        PREF("farm.useride",         farmUseRide,        Bool,  0, 0,   Farm),
        PREF("farm.maxoff",          farmMaxOff,         Float, 1, 20,  Farm),
        PREF("farm.stucksec",        farmStuckSec,       Int,   1, 30,  Farm),
        PREF("farm.stoponkeys",      farmStopOnKeys,     Bool,  0, 0,   Farm),
        PREF("farm.focusloss",       farmFocusLoss,      Int,   0, 1,   Farm),
        PREF("farm.laps",            farmLaps,           Int,   0, 10000, Farm),
        PREF("farm.minutes",         farmMinutes,        Int,   0, 1440, Farm),
        PREF("farm.countdown",       farmCountdown,      Int,   0, 10,  Farm),
        PREF("farm.autofinish",      farmAutoFinish,     Bool,  0, 0,   Farm),
        PREF("farm.sounds",          farmSounds,         Bool,  0, 0,   Farm),
        PREF("farm.flash",           farmFlash,          Bool,  0, 0,   Farm),
        PREF("farm.compact",         farmCompact,        Bool,  0, 0,   Farm),

        PREF("tab.studio",    tabStudio,          Bool,  0, 0,   Window),
        PREF("key.aiapprove",        keyAiApprove,       Int,   0, 255, Hotkeys),
        PREF("key.aidiscard",        keyAiDiscard,       Int,   0, 255, Hotkeys),
        PREF("key.airetry",          keyAiRetry,         Int,   0, 255, Hotkeys),

        PREF("ai.mode",              aiReplyMode,        Int,   0, 2,   Ai),
        PREF("ai.askbeforemodel",    aiAskBeforeModel,   Bool,  0, 0,   Ai),
        PREF("ai.autowhitelist",     aiAutoWhitelist,    Bool,  0, 0,   Ai),
        PREF("ai.drafttimeout",      aiDraftTimeoutSec,  Int,   0, 3600, Ai),
        PREF("ai.drafttimeoutsend",  aiDraftTimeoutSend, Int,   0, 1,   Ai),
        PREF("ai.toolpolicy",        aiToolPolicy,       Int,   0, 2,   Ai),
        PREF("ai.toolrules",         aiToolRules,        Str,   0, 0,   Ai),
        PREF("ai.toolwait",          aiToolWaitSec,      Int,   5, 600, Ai),
        PREF("ai.cooldown",          aiCooldownSec,      Int,   0, 3600, Ai),
        PREF("ai.playercooldown",    aiPlayerCooldownSec, Int,  0, 3600, Ai),
        PREF("ai.perminute",         aiMaxPerMinute,     Int,   0, 120, Ai),
        PREF("ai.wakeatstart",       aiWakeAtStart,      Bool,  0, 0,   Ai),
        PREF("ai.queuemax",          aiQueueMax,         Int,   0, 10,  Ai),
        PREF("ai.noteinchat",        aiNoteInChat,       Bool,  0, 0,   Ai),
        PREF("ai.draftsound",        aiDraftSound,       Bool,  0, 0,   Ai),
        PREF("ai.draftflash",        aiDraftFlash,       Bool,  0, 0,   Ai),
        PREF("ai.savehistory",       aiSaveHistory,      Bool,  0, 0,   Ai),
        PREF("ai.historymax",        aiHistoryMax,       Int,   10, 500, Ai),

        PREF("ai.stack",             aiStack,            Str,   0, 0,   Ai),
        PREF("ai.autosaveprompt",    aiAutoSavePrompt,   Bool,  0, 0,   Ai),
        PREF("ai.chatfmt",           aiChatLineFmt,      Str,   0, 0,   Ai),
        PREF("ai.chatintro",         aiChatIntro,        Str,   0, 0,   Ai),
        PREF("ai.chatoutro",         aiChatOutro,        Str,   0, 0,   Ai),
        PREF("ai.chatmaxage",        aiChatMaxAgeMin,    Int,   0, 1440, Ai),
        PREF("ai.chatmaxchars",      aiChatMaxLineChars, Int,   0, 1000, Ai),
        PREF("ai.chatincludebot",    aiChatIncludeBot,   Bool,  0, 0,   Ai),
        PREF("ai.chatincludesystem", aiChatIncludeSystem, Bool, 0, 0,   Ai),

        PREF("ai.post.lower",        aiPostLower,        Bool,  0, 0,   Ai),
        PREF("ai.post.emoji",        aiPostStripEmoji,   Bool,  0, 0,   Ai),
        PREF("ai.post.markdown",     aiPostStripMarkdown, Bool, 0, 0,   Ai),
        PREF("ai.post.maxchars",     aiPostMaxChars,     Int,   0, 2000, Ai),
        PREF("ai.post.blocklist",    aiPostBlocklist,    Str,   0, 0,   Ai),
        PREF("ai.post.blockaction",  aiPostBlockAction,  Int,   0, 1,   Ai),
        PREF("ai.post.prefix",       aiPostPrefix,       Str,   0, 0,   Ai),
        PREF("ai.post.suffix",       aiPostSuffix,       Str,   0, 0,   Ai),
        PREF("ai.prefab.notes",      aiPrefabNotes,      Str,   0, 0,   Ai),
        PREF("ai.prefab.newchat",    aiPrefabNewChat,    Bool,  0, 0,   Ai),
        PREF("ai.prefab.race",       aiPrefabRace,       Bool,  0, 0,   Ai),
        PREF("ai.prefab.maxblocks2", aiPrefabMaxBlocks,  Int,   0, 2000000000, Ai),
    };
    return rows;
}
#undef PREF

template <typename T> T& At(ChatPrefs& p, const PrefRow& r) {
    return *reinterpret_cast<T*>(reinterpret_cast<char*>(&p) + r.offset);
}

// Strings may hold several lines (hide patterns, highlight words), and the file
// is one key per line, so newlines and backslashes are escaped.
std::string EscapeValue(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') continue;
        else o += c;
    }
    return o;
}

std::string UnescapeValue(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            o += (n == 'n') ? '\n' : n;
        }
        else o += s[i];
    }
    return o;
}

std::string ColorToHex(const ImVec4& c) {
    auto b = [](float v) { return (unsigned)((std::max)(0.0f, (std::min)(1.0f, v)) * 255.0f + 0.5f); };
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", b(c.x), b(c.y), b(c.z), b(c.w));
    return buf;
}

bool HexToColor(const std::string& s, ImVec4& out) {
    unsigned r = 0, g = 0, b = 0, a = 255;
    if (s.size() == 9 && sscanf_s(s.c_str(), "#%02x%02x%02x%02x", &r, &g, &b, &a) == 4) {}
    else if (s.size() == 7 && sscanf_s(s.c_str(), "#%02x%02x%02x", &r, &g, &b) == 3) {}
    else return false;
    out = ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
    return true;
}

// True if the key belonged to the table (whether or not the value parsed).
bool LoadPrefRow(const std::string& key, const std::string& val) {
    for (const PrefRow& r : PrefTable()) {
        if (key != r.key) continue;
        try {
            switch (r.type) {
            case PT::Bool:  At<bool>(g_prefs, r) = (val == "1"); break;
            case PT::Int:   At<int>(g_prefs, r) = (std::max)((int)r.lo, (std::min)((int)r.hi, std::stoi(val))); break;
            case PT::Float: At<float>(g_prefs, r) = (std::max)(r.lo, (std::min)(r.hi, std::stof(val))); break;
            case PT::Str:   At<std::string>(g_prefs, r) = UnescapeValue(val); break;
            case PT::Color: HexToColor(val, At<ImVec4>(g_prefs, r)); break;
            }
        }
        catch (...) {}
        return true;
    }
    return false;
}

void SavePrefRows(std::ostream& f) {
    for (const PrefRow& r : PrefTable()) {
        f << r.key << '=';
        switch (r.type) {
        case PT::Bool:  f << (At<bool>(g_prefs, r) ? "1" : "0"); break;
        case PT::Int:   f << At<int>(g_prefs, r); break;
        case PT::Float: f << At<float>(g_prefs, r); break;
        case PT::Str:   f << EscapeValue(At<std::string>(g_prefs, r)); break;
        case PT::Color: f << ColorToHex(At<ImVec4>(g_prefs, r)); break;
        }
        f << '\n';
    }
}

long long g_lastSaveMs = 0;

} // namespace

void MarkPrefsChanged() {
    g_settingsDirty = true;
    g_prefsVersion.fetch_add(1);
}

void ResetPrefsGroup(PrefGroup group) {
    static const ChatPrefs defaults;
    ChatPrefs& d = const_cast<ChatPrefs&>(defaults);
    std::lock_guard lock(g_settingsMtx);
    for (const PrefRow& r : PrefTable()) {
        if (r.group != group) continue;
        switch (r.type) {
        case PT::Bool:  At<bool>(g_prefs, r) = At<bool>(d, r); break;
        case PT::Int:   At<int>(g_prefs, r) = At<int>(d, r); break;
        case PT::Float: At<float>(g_prefs, r) = At<float>(d, r); break;
        case PT::Str:   At<std::string>(g_prefs, r) = At<std::string>(d, r); break;
        case PT::Color: At<ImVec4>(g_prefs, r) = At<ImVec4>(d, r); break;
        }
    }
    // The older settings that share a page with these reset along with them.
    switch (group) {
    case PrefGroup::Hotkeys:
        g_buildKeyAdvance.store(VK_OEM_6);
        g_buildKeyUndo.store(VK_OEM_4);
        g_buildKeySkip.store(VK_OEM_5);
        break;
    case PrefGroup::ChatDisplay:
        g_chatLineGap = 1.0f;
        g_showTimestamps = true;
        g_showOwnFormatted = false;
        SetTimeZoneMode(g_prefs.timeZone);
        break;
    case PrefGroup::Compact:
        g_compactOpacity = 0.78f;
        g_compactFontSize = 17.0f;
        g_compactPadX = 10.0f;
        g_compactMaxLines = 80;
        g_windowStyleDirty = true;
        break;
    case PrefGroup::Window:
        g_alwaysOnTop = false;
        g_windowOpacity = 1.0f;
        g_themePreset = 0;
        g_fontSize = 16.0f;
        g_uiScale = 1.0f;
        g_uiRounding = 6.0f;
        g_windowStyleDirty = true;
        g_themeDirty = true;
        g_fontDirty = true;
        break;
    default:
        break;
    }
    MarkPrefsChanged();
}

void SaveSettingsThrottled() {
    if (!g_settingsDirty) return;
    const long long now = NowSteadyMs();
    if (now - g_lastSaveMs < 500) return;
    SaveSettings();
}

// strftime hands an unknown conversion to the CRT's invalid-parameter handler,
// which terminates the process - the game with it. A typed pattern is therefore
// cut down to conversions known to be safe; anything else is printed literally.
static std::string SafeStrftimePattern(const std::string& in) {
    static const char* kOk = "aAbBcdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%";
    std::string out;
    for (size_t i = 0; i < in.size() && out.size() < 60; ++i) {
        if (in[i] != '%') { out += in[i]; continue; }
        size_t j = i + 1;
        const bool hash = j < in.size() && in[j] == '#';
        if (hash) ++j;
        if (j < in.size() && strchr(kOk, in[j])) {
            out += '%';
            if (hash) out += '#';
            out += in[j];
            i = j;
        }
        else out += "%%";
    }
    return out;
}

std::string FormatChatTime(long long unixMs, bool forceSeconds) {
    if (unixMs <= 0) return {};
    EnsureTimeZone();
    const time_t t = (time_t)(unixMs / 1000);
    struct tm tmv {};
    if (localtime_s(&tmv, &t) != 0) return {};

    std::string fmt;
    switch (g_prefs.timeFormat) {
    case 1: fmt = forceSeconds ? "%H:%M:%S" : "%H:%M"; break;
    case 2: fmt = "%H:%M:%S"; break;
    case 3: fmt = "%I:%M:%S %p"; break;
    case 4: fmt = SafeStrftimePattern(g_prefs.timeCustom.empty() ? "%H:%M:%S" : g_prefs.timeCustom); break;
    default: fmt = forceSeconds ? "%I:%M:%S %p" : "%I:%M %p"; break;
    }
    char buf[96];
    const size_t n = strftime(buf, sizeof(buf), fmt.c_str(), &tmv);
    if (n == 0) return {};
    std::string s(buf, n);
    // "03:07 PM" reads better as "3:07 PM".
    const bool twelveHour = g_prefs.timeFormat == 0 || g_prefs.timeFormat == 3;
    if (twelveHour && s.size() > 1 && s[0] == '0')
        s.erase(0, 1);
    return s;
}

// Prompt cache
static std::mutex g_promptMtx;
static std::string g_systemPrompt;
static std::string g_promptStatus = "not loaded";
static std::string g_promptPathUtf8;
static std::filesystem::file_time_type g_promptMtime{};
static std::atomic<uint64_t> g_promptBytes{ 0 };
static std::atomic<bool> g_promptOk{ false };

// Chat sets live in chat state (defined in chat_core)
extern std::set<std::string> g_ignored;
extern std::set<std::string> g_whitelist;
extern std::mutex g_mtx;

void LoadIgnored() {
    std::set<std::string> loaded;
    {
        std::ifstream f(GetThisModuleDirectory() + L"ignored_users.txt");
        std::string line;
        while (std::getline(f, line)) {
            line = TrimCopy(line);
            if (!line.empty())
                loaded.insert(ToLower(line));
        }
    }
    std::lock_guard lock(g_mtx);
    g_ignored.swap(loaded);
}

void SaveIgnored() {
    std::set<std::string> copy;
    {
        std::lock_guard lock(g_mtx);
        copy = g_ignored;
    }
    std::ofstream f(GetThisModuleDirectory() + L"ignored_users.txt", std::ios::trunc);
    for (const auto& n : copy)
        f << n << "\n";
}


void LoadWhitelist() {
    std::set<std::string> loaded;
    {
        std::ifstream f(GetThisModuleDirectory() + L"whitelist_users.txt");
        std::string line;
        while (std::getline(f, line)) {
            line = TrimCopy(line);
            if (!line.empty())
                loaded.insert(ToLower(line));
        }
    }
    std::lock_guard lock(g_mtx);
    g_whitelist.swap(loaded);
}

void SaveWhitelist() {
    std::set<std::string> copy;
    {
        std::lock_guard lock(g_mtx);
        copy = g_whitelist;
    }
    std::ofstream f(GetThisModuleDirectory() + L"whitelist_users.txt", std::ios::trunc);
    for (const auto& n : copy)
        f << n << "\n";
}

void LoadSettings() {
    std::ifstream f(GetThisModuleDirectory() + L"hyai_settings.txt");
    if (!f.is_open()) return;
    std::string line;

    std::lock_guard lock(g_settingsMtx);
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "selftrigger") g_allowSelfTrigger = (val == "1");
        else if (key == "showown") g_showOwnFormatted = (val == "1");
        else if (key == "livetranslate") g_liveTranslate = (val == "1");
        else if (key == "timestamps") g_showTimestamps = (val == "1");
        else if (key == "includeglobalchat") g_includeGlobalChat = (val != "0");
        else if (key == "coloredchat") g_coloredChat = (val == "1");
        else if (key == "usenickname") g_useNickname = (val == "1");
        else if (key == "nickdelay") {
            try { g_nicknameDelayMs = (std::max)(0, (std::min)(2000, std::stoi(val))); }
            catch (...) {}
        }
        else if (key == "outgoinglang") {
            g_outgoingLang = val;
            if (g_outgoingLang == "none" || g_outgoingLang == "en") g_outgoingLang.clear();
        }
        else if (key == "localname") {
            g_localName = val;
            strncpy_s(g_localNameBuf, val.c_str(), 63);
        }
        else if (key == "maxmemory") {
            try { g_maxMemory = (std::max)(0, (std::min)(20, std::stoi(val))); }
            catch (...) {}
        }
        else if (key == "maxchatcontext") {
            try { g_maxChatContext = (std::max)(4, (std::min)(60, std::stoi(val))); }
            catch (...) {}
        }
        else if (key == "aiprefix") {
            g_aiPrefix = val;
            strncpy_s(g_aiPrefixBuf, val.c_str(), 95);
        }
        else if (key == "aicolor") {
            g_aiColorCode = val;
            strncpy_s(g_aiColorCodeBuf, val.c_str(), 15);
        }
        else if (key == "nickon") {
            g_nicknameOn = val;
            strncpy_s(g_nicknameOnBuf, val.c_str(), 95);
        }
        else if (key == "nickoff") {
            g_nicknameOff = val;
            strncpy_s(g_nicknameOffBuf, val.c_str(), 63);
        }
        else if (key == "wakeword") {
            g_wakeWord = val;
            strncpy_s(g_wakeWordBuf, val.c_str(), 63);
        }
        else if (key == "wakeword2") {
            g_wakeWord2 = val;
            strncpy_s(g_wakeWord2Buf, val.c_str(), 63);
        }
        else if (key == "botname") {
            g_botName = val;
            strncpy_s(g_botNameBuf, val.c_str(), 63);
        }
        else if (key == "ollamamodel") {
            g_ollamaModel = val;
            strncpy_s(g_ollamaModelBuf, val.c_str(), 95);
        }
        else if (key == "ollamahost") {
            g_ollamaHost = val;
            strncpy_s(g_ollamaHostBuf, val.c_str(), 63);
        }
        else if (key == "ollamaport") {
            try { g_ollamaPort = (std::max)(1, (std::min)(65535, std::stoi(val))); }
            catch (...) {}
        }
        else if (key == "temperature") {
            try {
                float t = std::stof(val);
                g_ollamaTemperature = (std::max)(0.0f, (std::min)(2.0f, t));
            }
            catch (...) {}
        }
        else if (key == "numpredict") {
            try {
                int n = std::stoi(val);
                g_ollamaNumPredict = (std::max)(16, (std::min)(16549, n));
            }
            catch (...) {}
        }
        else if (key == "numctx") {
            try {
                int n = std::stoi(val);
                g_ollamaNumCtx = (std::max)(2048, (std::min)(131072, n));
            }
            catch (...) {}
        }
        else if (key == "aiprovider") {
            g_aiProvider = (val == "openai") ? "openai" : "ollama";
        }
        else if (key == "openaibase") {
            g_openaiBaseUrl = val;
            strncpy_s(g_openaiBaseBuf, val.c_str(), 255);
        }
        else if (key == "openaiakey") {
            g_openaiApiKey = val;
            strncpy_s(g_openaiKeyBuf, val.c_str(), 255);
        }
        else if (key == "openaimodel") {
            g_openaiModel = val;
            strncpy_s(g_openaiModelBuf, val.c_str(), 95);
        }
        else if (key == "websearch") g_webSearchEnabled = (val == "1");
        else if (key == "websearchkey") {
            g_webSearchApiKey = val;
            strncpy_s(g_webSearchKeyBuf, val.c_str(), 255);
        }
        else if (key == "websearchcount") {
            try { g_webSearchCount = (std::max)(1, (std::min)(10, std::stoi(val))); }
            catch (...) {}
        }
        else if (key == "websearchsummary") g_webSearchSummary = (val == "1");
        else if (key == "websearchfresh") {
            g_webSearchFreshness = val;
            strncpy_s(g_webSearchFreshBuf, val.c_str(), 31);
        }
        else if (key == "websearchmaxchars") {
            try { g_webSearchMaxChars = (std::max)(400, (std::min)(8000, std::stoi(val))); }
            catch (...) {}
        }
        else if (key == "autowelcome") g_autoWelcome = (val == "1");
        else if (key == "autowb") g_autoWelcomeBack = (val == "1");
        else if (key == "autogg") g_autoGg = (val == "1");
        else if (key == "wordvalueshow") g_wordValueShow = (val != "0");
        else if (key == "wordvalueauto") g_wordValueAuto = (val == "1");
        else if (key == "morseshow")     g_morseShow = (val != "0");
        else if (key == "morseauto")     g_morseAuto = (val == "1");
        else if (key == "caesarshow")    g_caesarShow = (val != "0");
        else if (key == "caesarauto")    g_caesarAuto = (val == "1");
        else if (key == "countlettersshow") g_countLettersShow = (val != "0");
        else if (key == "countlettersauto") g_countLettersAuto = (val == "1");
        else if (key == "patternshow")   g_patternShow = (val != "0");
        else if (key == "patternauto")   g_patternAuto = (val == "1");
        else if (key == "think") g_thinkEnabled = (val == "1");
        else if (key == "thinklevel") {
            g_thinkLevel = val;
            strncpy_s(g_thinkLevelBuf, val.c_str(), 15);
        }
        else if (key == "blockslash") g_blockLeadingSlash = (val != "0");
        else if (key == "chunksize") {
            try { MAX_CHAT_CHUNK = (size_t)(std::max)(32, (std::min)(200, std::stoi(val))); }
            catch (...) {}
        }
        else if (key == "sendgap") {
            try { SEND_GAP_MS = (std::max)(50, (std::min)(500, std::stoi(val))); }
            catch (...) {}
        }
        else if (key == "failmsg") {
            g_failMessage = val;
            strncpy_s(g_failMessageBuf, val.c_str(), 255);
        }
        else if (key == "muted") g_botMuted = (val == "1");
        else if (key == "alwaysontop") g_alwaysOnTop = (val == "1");
        else if (key == "opacity") {
            try {
                float o = std::stof(val);
                g_windowOpacity = (std::max)(0.25f, (std::min)(1.0f, o));
            }
            catch (...) {}
        }
        else if (key == "whitelistmode") g_whitelistMode = (val == "1");
        else if (key == "themepreset") {
            try { g_themePreset = (std::max)(0, (std::min)(5, std::stoi(val))); }
            catch (...) {}
        }
        else if (key == "accentr" || key == "accentg" || key == "accentb") {
            try {
                float v = (std::max)(0.0f, (std::min)(1.0f, std::stof(val)));
                if (key == "accentr") g_customAccent[0] = v;
                else if (key == "accentg") g_customAccent[1] = v;
                else g_customAccent[2] = v;
            }
            catch (...) {}
        }
        else if (key == "accentdark") g_customAccentDark = (val != "0");
        else if (key == "fontsize") {
            try {
                float v = std::stof(val);
                g_fontSize = (std::max)(11.0f, (std::min)(26.0f, v));
                g_fontDirty = true;
            }
            catch (...) {}
        }
        else if (key == "uiscale") {
            try {
                float v = std::stof(val);
                g_uiScale = (std::max)(0.75f, (std::min)(1.60f, v));
            }
            catch (...) {}
        }
        else if (key == "uirounding") {
            try {
                float v = std::stof(val);
                g_uiRounding = (std::max)(0.0f, (std::min)(12.0f, v));
            }
            catch (...) {}
        }
        else if (key == "compactchat") g_compactChatMode = (val == "1");
        else if (key == "compactopacity") {
            try {
                float v = std::stof(val);
                g_compactOpacity = (std::max)(0.10f, (std::min)(1.0f, v));
            }
            catch (...) {}
        }
        else if (key == "compactfontsize") {
            try {
                float v = std::stof(val);
                g_compactFontSize = (std::max)(10.0f, (std::min)(28.0f, v));
            }
            catch (...) {}
        }
        else if (key == "compactmaxlines") {
            try {
                int n = std::stoi(val);
                g_compactMaxLines = (std::max)(20, (std::min)(300, n));
            }
            catch (...) {}
        }
        else if (key == "compactpadx") {
            try {
                float v = std::stof(val);
                g_compactPadX = (std::max)(4.0f, (std::min)(32.0f, v));
            }
            catch (...) {}
        }
        else if (key == "chatlinegap") {
            try {
                float v = std::stof(val);
                g_chatLineGap = (std::max)(0.0f, (std::min)(16.0f, v));
            }
            catch (...) {}
        }
        else if (key == "privatemode") g_privateMode = (val == "1");
        else if (key == "privatedisplay") g_privateDecryptDisplay = (val != "0");
        else if (key == "privateviamsg") g_privateViaMsg = (val != "0");
        else if (key == "key.buildadvance" || key == "key.buildundo" || key == "key.buildskip") {
            try {
                const int vk = (std::max)(0, (std::min)(255, std::stoi(val)));
                if (key == "key.buildadvance") g_buildKeyAdvance.store(vk);
                else if (key == "key.buildundo") g_buildKeyUndo.store(vk);
                else g_buildKeySkip.store(vk);
            }
            catch (...) {}
        }
        else LoadPrefRow(key, val);
    }
    SetTimeZoneMode(g_prefs.timeZone);
    g_prefsVersion.fetch_add(1);
}

void SaveSettings() {
    std::lock_guard lock(g_settingsMtx);
    // Written beside the real file and swapped in, so a crash or a full disk
    // mid-write can no longer leave a half-written settings file behind.
    const std::wstring finalPath = GetThisModuleDirectory() + L"hyai_settings.txt";
    const std::wstring tmpPath = finalPath + L".tmp";
    std::ofstream f(tmpPath, std::ios::trunc);
    if (!f.is_open()) return;
    f << "selftrigger=" << (g_allowSelfTrigger ? "1" : "0") << "\n";
    f << "showown=" << (g_showOwnFormatted ? "1" : "0") << "\n";
    f << "livetranslate=" << (g_liveTranslate ? "1" : "0") << "\n";
    f << "timestamps=" << (g_showTimestamps ? "1" : "0") << "\n";
    f << "includeglobalchat=" << (g_includeGlobalChat ? "1" : "0") << "\n";
    f << "coloredchat=" << (g_coloredChat ? "1" : "0") << "\n";
    f << "usenickname=" << (g_useNickname ? "1" : "0") << "\n";
    f << "nickdelay=" << g_nicknameDelayMs << "\n";
    f << "outgoinglang=" << g_outgoingLang << "\n";
    f << "localname=" << g_localName << "\n";
    f << "maxmemory=" << g_maxMemory << "\n";
    f << "maxchatcontext=" << g_maxChatContext << "\n";

    f << "aiprefix=" << g_aiPrefix << "\n";
    f << "aicolor=" << g_aiColorCode << "\n";
    f << "nickon=" << g_nicknameOn << "\n";
    f << "nickoff=" << g_nicknameOff << "\n";
    f << "wakeword=" << g_wakeWord << "\n";
    f << "wakeword2=" << g_wakeWord2 << "\n";
    f << "botname=" << g_botName << "\n";
    f << "ollamamodel=" << g_ollamaModel << "\n";
    f << "ollamahost=" << g_ollamaHost << "\n";
    f << "ollamaport=" << g_ollamaPort << "\n";
    f << "temperature=" << g_ollamaTemperature << "\n";
    f << "numpredict=" << g_ollamaNumPredict << "\n";
    f << "numctx=" << g_ollamaNumCtx << "\n";
    f << "aiprovider=" << g_aiProvider << "\n";
    f << "openaibase=" << g_openaiBaseUrl << "\n";
    f << "openaiakey=" << g_openaiApiKey << "\n";
    f << "openaimodel=" << g_openaiModel << "\n";
    f << "websearch=" << (g_webSearchEnabled ? "1" : "0") << "\n";
    f << "websearchkey=" << g_webSearchApiKey << "\n";
    f << "websearchcount=" << g_webSearchCount << "\n";
    f << "websearchsummary=" << (g_webSearchSummary ? "1" : "0") << "\n";
    f << "websearchfresh=" << g_webSearchFreshness << "\n";
    f << "websearchmaxchars=" << g_webSearchMaxChars << "\n";
    f << "autowelcome=" << (g_autoWelcome ? "1" : "0") << "\n";
    f << "autowb=" << (g_autoWelcomeBack ? "1" : "0") << "\n";
    f << "autogg=" << (g_autoGg ? "1" : "0") << "\n";
    f << "wordvalueshow=" << (g_wordValueShow ? "1" : "0") << "\n";
    f << "wordvalueauto=" << (g_wordValueAuto ? "1" : "0") << "\n";
    f << "morseshow=" << (g_morseShow ? "1" : "0") << "\n";
    f << "morseauto=" << (g_morseAuto ? "1" : "0") << "\n";
    f << "caesarshow=" << (g_caesarShow ? "1" : "0") << "\n";
    f << "caesarauto=" << (g_caesarAuto ? "1" : "0") << "\n";
    f << "countlettersshow=" << (g_countLettersShow ? "1" : "0") << "\n";
    f << "countlettersauto=" << (g_countLettersAuto ? "1" : "0") << "\n";
    f << "patternshow=" << (g_patternShow ? "1" : "0") << "\n";
    f << "patternauto=" << (g_patternAuto ? "1" : "0") << "\n";
    f << "think=" << (g_thinkEnabled ? "1" : "0") << "\n";
    f << "thinklevel=" << g_thinkLevel << "\n";
    f << "blockslash=" << (g_blockLeadingSlash ? "1" : "0") << "\n";
    f << "chunksize=" << MAX_CHAT_CHUNK << "\n";
    f << "sendgap=" << SEND_GAP_MS << "\n";
    f << "failmsg=" << g_failMessage << "\n";
    f << "muted=" << (g_botMuted ? "1" : "0") << "\n";
    f << "alwaysontop=" << (g_alwaysOnTop ? "1" : "0") << "\n";
    f << "opacity=" << g_windowOpacity << "\n";
    f << "whitelistmode=" << (g_whitelistMode ? "1" : "0") << "\n";
    f << "themepreset=" << g_themePreset << "\n";
    f << "accentr=" << g_customAccent[0] << "\n";
    f << "accentg=" << g_customAccent[1] << "\n";
    f << "accentb=" << g_customAccent[2] << "\n";
    f << "accentdark=" << (g_customAccentDark ? "1" : "0") << "\n";
    f << "fontsize=" << g_fontSize << "\n";
    f << "uiscale=" << g_uiScale << "\n";
    f << "uirounding=" << g_uiRounding << "\n";
    f << "chatlinegap=" << g_chatLineGap << "\n";
    f << "compactchat=" << (g_compactChatMode ? "1" : "0") << "\n";
    f << "compactopacity=" << g_compactOpacity << "\n";
    f << "compactfontsize=" << g_compactFontSize << "\n";
    f << "compactmaxlines=" << g_compactMaxLines << "\n";
    f << "compactpadx=" << g_compactPadX << "\n";
    f << "privatemode=" << (g_privateMode ? "1" : "0") << "\n";
    f << "privatedisplay=" << (g_privateDecryptDisplay ? "1" : "0") << "\n";
    f << "privateviamsg=" << (g_privateViaMsg ? "1" : "0") << "\n";
    f << "key.buildadvance=" << g_buildKeyAdvance.load() << "\n";
    f << "key.buildundo=" << g_buildKeyUndo.load() << "\n";
    f << "key.buildskip=" << g_buildKeySkip.load() << "\n";
    SavePrefRows(f);

    f.close();
    if (!f.fail())
        MoveFileExW(tmpPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

    g_settingsDirty = false;
    g_lastSaveMs = NowSteadyMs();
}


static std::string LoadContextMarkdownFromDisk() {
    const std::wstring path = GetThisModuleDirectory() + L"CONTEXT.md";
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return {};
    return std::string((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
}

bool ReloadSystemPrompt(bool force) {
    const std::wstring wpath = GetThisModuleDirectory() + L"CONTEXT.md";
    const std::string pathUtf8 = WideToUtf8Path(wpath);

    std::error_code ec;
    const bool exists = std::filesystem::exists(wpath, ec);
    std::filesystem::file_time_type mtime{};
    if (exists && !ec)
        mtime = std::filesystem::last_write_time(wpath, ec);

    {
        std::lock_guard lock(g_promptMtx);
        if (!force && g_promptOk.load() && exists && !ec && mtime == g_promptMtime) {
            g_promptPathUtf8 = pathUtf8;
            return true;
        }
    }

    std::string loaded;
    if (exists)
        loaded = LoadContextMarkdownFromDisk();

    std::lock_guard lock(g_promptMtx);
    g_promptPathUtf8 = pathUtf8;
    g_systemPrompt = std::move(loaded);
    g_promptBytes = g_systemPrompt.size();
    g_promptOk = !g_systemPrompt.empty();
    if (!exists)
        g_promptStatus = "CONTEXT.md not found (" + pathUtf8 + ")";
    else if (g_systemPrompt.empty())
        g_promptStatus = "CONTEXT.md empty (" + pathUtf8 + ")";
    else {
        g_promptMtime = mtime;
        g_promptStatus = "loaded " + std::to_string(g_systemPrompt.size()) +
            " bytes at " + NowHHMMSS();
    }
    LogToFile(std::string("System prompt reload: ") + g_promptStatus);
    return g_promptOk.load();
}

std::string GetCachedSystemPrompt() {
    ReloadSystemPrompt(false);
    std::lock_guard lock(g_promptMtx);
    return g_systemPrompt;
}



std::string GetPromptStatus() {
    std::lock_guard lock(g_promptMtx);
    return g_promptStatus;
}
std::string GetPromptPathUtf8() {
    std::lock_guard lock(g_promptMtx);
    return g_promptPathUtf8;
}
bool IsPromptOk() {
    return g_promptOk.load();
}