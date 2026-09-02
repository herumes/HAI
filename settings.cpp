#include "pch.h"
#include "settings.hpp"
#include "common.hpp"
#include "chat.hpp"
#include "shared.hpp"

#include <fstream>
#include <filesystem>
#include <algorithm>

// =============================================================================
// Settings globals (definitions — declared in settings.hpp)
// =============================================================================
std::mutex g_settingsMtx;
bool g_showOwnFormatted = false;
bool g_allowSelfTrigger = false;
std::string g_localName = "The_Jose";
int g_maxMemory = 4;
int g_maxChatContext = 20;
bool g_includeGlobalChat = true;
bool g_liveTranslate = false;
bool g_showTimestamps = true;
std::string g_outgoingLang;
bool g_coloredChat = false;
bool g_useNickname = false;
int g_nicknameDelayMs = 400;
std::string g_aiPrefix = "Josebot > ";
std::string g_aiColorCode = "&a";
std::string g_nicknameOn = "/nickname &cJOSEBOT";
std::string g_nicknameOff = "/nickname off";
std::string g_wakeWord = "josebot";
std::string g_wakeWord2;
std::string g_botName = "Josebot";
std::string g_ollamaModel = "gemma4:e2b";
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
    std::ifstream f(GetThisModuleDirectory() + L"josebot_settings.txt");
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
            try { g_maxMemory = (std::max)(1, (std::min)(8, std::stoi(val))); }
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
                g_fontSize = (std::max)(12.0f, (std::min)(22.0f, v));
                g_fontDirty = true;
            }
            catch (...) {}
        }
        else if (key == "uiscale") {
            try {
                float v = std::stof(val);
                g_uiScale = (std::max)(0.85f, (std::min)(1.35f, v));
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
                g_compactOpacity = (std::max)(0.25f, (std::min)(1.0f, v));
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
    }
}

void SaveSettings() {
    std::lock_guard lock(g_settingsMtx);
    std::ofstream f(GetThisModuleDirectory() + L"josebot_settings.txt", std::ios::trunc);
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

    g_settingsDirty = false;
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