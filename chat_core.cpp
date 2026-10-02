#include "pch.h"
#include "chat.hpp"
#include "common.hpp"
#include "settings.hpp"
#include "ai.hpp"
#include "aiflow.hpp"
#include "aiprompt.hpp"
#include "aitext.hpp"
#include "extras.hpp"
#include "knowledge.hpp"
#include "translate.hpp"
#include "hooks.hpp"
#include "shared.hpp"
#include "autoreply.hpp"

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif

// =============================================================================
// Chat / game runtime globals
// =============================================================================
HWND                    g_hWnd = nullptr;
WNDPROC                 g_oWndProc = nullptr;
std::atomic<bool>       g_show{ true };

tChatFn oSendMessage = nullptr;
tChatFn oAddMessage = nullptr;
tChatFn oOnReceive = nullptr;

std::atomic<void*>    g_pChat{ nullptr };
std::atomic<uint64_t> g_chatVT{ 0 };
std::atomic<uint64_t> g_stringVT{ 0 };
std::atomic<DWORD>    g_gameThread{ 0 };
std::atomic<bool>     g_inProcessPending{ false };
std::atomic<bool>     g_sendDisabled{ false };
std::atomic<bool>     g_chatReady{ false };
const UINT            JOSE_TIMER_ID = 0x4A53; // 'JS'

std::mutex g_mtx;
std::vector<ChatLine> g_log;
std::vector<PendingSend> g_pending;
char g_input[512]{};
bool g_scroll = false;
std::vector<std::shared_ptr<uint8_t>> g_keepalive;
uint64_t g_nextLineId = 1;
std::atomic<bool> g_aiInProgress{ false };

std::set<std::string> g_ignored;
std::deque<ChatMessage> g_memory;
std::deque<GlobalChatEntry> g_globalChat;
char g_ignoreInput[64]{};
char g_localNameBuf[64] = "Player";
char g_suggestFilter[64]{};

// Seen players — keep type local
// SeenPlayer defined in shared.hpp
std::deque<SeenPlayer> g_seenPlayers;

std::set<std::string> g_whitelist;

std::chrono::steady_clock::time_point g_startTime = std::chrono::steady_clock::now();
std::atomic<uint64_t> g_wakeCount{ 0 };
std::atomic<uint64_t> g_replySuccess{ 0 };
std::atomic<uint64_t> g_replyFail{ 0 };

char g_chatFilter[96]{};

std::atomic<bool> g_chatHooked{ false };
HANDLE g_installerThread = nullptr;

std::string g_lastWakePrompt;
std::chrono::steady_clock::time_point g_lastWakeTime{};
std::mutex g_wakeMtx;

std::atomic<long long> g_lastSendMs{ 0 };

// RecentOut in shared.hpp
std::deque<RecentOut> g_recentOut;
std::mutex g_dedupMtx;
// DedupEntry in shared.hpp
std::deque<DedupEntry> g_recentProcessed;

// External window (owned by ui.cpp but referenced from DllMain path)
extern HWND g_hwndOverlay;

// Test chat / tools (used by UI)
std::vector<ChatMessage> g_testChat;
char g_testInput[1024]{};
bool g_testScroll = false;
std::atomic<bool> g_testAiInProgress{ false };
std::string g_testStatus;

// Tool playground buffers
int g_toolKind = 0;
char g_toolQuery[256]{};
char g_toolDrops[128]{};
char g_toolTrait[128]{};
int g_toolMinHealth = 0;
int g_toolMaxHealth = 0;
int g_toolLimit = 8;
char g_toolQuality[64]{};
char g_toolCategory[64]{};
char g_toolRemUser[64]{};
char g_toolRemMsg[256]{};
int g_toolInSeconds = 60;
int g_toolRemId = 1;
char g_toolFresh[32]{};
std::string g_toolResult;
bool g_toolScroll = false;

// Settings UI buffers (used by RenderOverlay)
char g_aiPrefixBuf[96]{};
char g_aiColorCodeBuf[16]{};
char g_nicknameOnBuf[96]{};
char g_nicknameOffBuf[64]{};
char g_wakeWordBuf[64]{};
char g_wakeWord2Buf[64]{};
char g_botNameBuf[64]{};
char g_ollamaModelBuf[96]{};
char g_ollamaHostBuf[64]{};
char g_thinkLevelBuf[16]{};
char g_failMessageBuf[256]{};
char g_openaiBaseBuf[256]{};
char g_openaiKeyBuf[256]{};
char g_openaiModelBuf[96]{};
char g_webSearchKeyBuf[256]{};
char g_webSearchFreshBuf[32]{};
char g_whitelistInput[64]{};
char g_privatePassBuf[128]{};
char g_privateTargetBuf[64]{};
char g_privateMsgBuf[512]{};

long long g_lastAutoReactMs = 0;

const char* LangName(const std::string& code) {
    const std::string c = ToLower(code);
    if (c.empty()) return "?";
    if (c == "en") return "English";
    for (size_t i = 0; i < OUT_LANGS_COUNT; ++i) {
        const auto& l = OUT_LANGS[i];
        if (c == l.code && l.code && l.code[0]) return l.name;
    }
    if (c == "ja") return "Japanese";
    if (c == "ko") return "Korean";
    if (c == "zh" || c == "zh-cn" || c == "zh-tw") return "Chinese";
    if (c == "ar") return "Arabic";
    if (c == "cs") return "Czech";
    if (c == "hu") return "Hungarian";
    if (c == "ro") return "Romanian";
    if (c == "el") return "Greek";
    if (c == "fi") return "Finnish";
    if (c == "da") return "Danish";
    if (c == "no" || c == "nb") return "Norwegian";
    if (c == "wv") return "Word Value";
    if (c == "mc") return "Morse";
    if (c == "cc") return "Caesar";
    if (c == "cl") return "Count Letters";
    if (c == "pn") return "Pattern";
    return code.c_str();
}

std::string LangBadge(const std::string& code) {
    if (code.empty()) return "?";
    if (code == "WV" || code == "wv") return "WV";
    if (code == "MC" || code == "mc") return "MC";
    if (code == "CC" || code == "cc") return "CC";
    if (code == "CL" || code == "cl") return "CL";
    if (code == "PN" || code == "pn") return "PN";
    return ToUpper(code);
}

ImVec4 LangColor(const std::string& code) {
    std::string c = ToLower(code);
    if (c == "fr") return ImVec4(0.45f, 0.72f, 1.00f, 1.0f);
    if (c == "de") return ImVec4(1.00f, 0.82f, 0.38f, 1.0f);
    if (c == "es") return ImVec4(1.00f, 0.62f, 0.38f, 1.0f);
    if (c == "it") return ImVec4(0.45f, 0.90f, 0.55f, 1.0f);
    if (c == "pt") return ImVec4(0.40f, 0.85f, 0.72f, 1.0f);
    if (c == "nl") return ImVec4(1.00f, 0.70f, 0.35f, 1.0f);
    if (c == "pl") return ImVec4(1.00f, 0.50f, 0.55f, 1.0f);
    if (c == "ru" || c == "uk") return ImVec4(0.70f, 0.60f, 1.00f, 1.0f);
    if (c == "tr") return ImVec4(0.90f, 0.40f, 0.40f, 1.0f);
    if (c == "en") return ImVec4(0.55f, 0.85f, 0.95f, 1.0f);
    if (c == "wv") return ImVec4(0.55f, 0.85f, 1.00f, 1.0f);   // cyan
    if (c == "mc") return ImVec4(0.70f, 0.85f, 1.00f, 1.0f);   // soft blue
    if (c == "cc") return ImVec4(0.85f, 0.75f, 1.00f, 1.0f);   // soft purple
    if (c == "cl") return ImVec4(0.55f, 0.95f, 0.70f, 1.0f);   // soft green
    if (c == "pn") return ImVec4(1.00f, 0.75f, 0.45f, 1.0f);   // soft orange
    return ImVec4(0.70f, 0.75f, 0.85f, 1.0f);
}



// =============================================================================
// Color & Formatting helpers
// =============================================================================
static bool ParseHytaleColor(const std::string& s, ImVec4& outColor) {
    if (s.empty()) return false;

    if (s[0] == '#') {
        if (s.size() == 4) {
            unsigned r, g, b;
            if (sscanf_s(s.c_str(), "#%1x%1x%1x", &r, &g, &b) == 3) {
                outColor = ImVec4((r * 17) / 255.f, (g * 17) / 255.f, (b * 17) / 255.f, 1.0f);
                return true;
            }
        }
        else if (s.size() == 7) {
            unsigned r, g, b;
            if (sscanf_s(s.c_str(), "#%02x%02x%02x", &r, &g, &b) == 3) {
                outColor = ImVec4(r / 255.f, g / 255.f, b / 255.f, 1.0f);
                return true;
            }
        }
        else if (s.size() == 9) {
            unsigned a, r, g, b;
            if (sscanf_s(s.c_str(), "#%02x%02x%02x%02x", &a, &r, &g, &b) == 4) {
                outColor = ImVec4(r / 255.f, g / 255.f, b / 255.f, 1.0f);
                return true;
            }
        }
    }

    std::string l = ToLower(s);
    if (l == "black") { outColor = ImVec4(0.00f, 0.00f, 0.00f, 1.0f); return true; }
    if (l == "dark_blue") { outColor = ImVec4(0.00f, 0.00f, 0.67f, 1.0f); return true; }
    if (l == "dark_green") { outColor = ImVec4(0.00f, 0.67f, 0.00f, 1.0f); return true; }
    if (l == "dark_aqua" || l == "dark_cyan") { outColor = ImVec4(0.00f, 0.67f, 0.67f, 1.0f); return true; }
    if (l == "dark_red") { outColor = ImVec4(0.67f, 0.00f, 0.00f, 1.0f); return true; }
    if (l == "dark_purple" || l == "purple") { outColor = ImVec4(0.67f, 0.00f, 0.67f, 1.0f); return true; }
    if (l == "gold" || l == "orange") { outColor = ImVec4(1.00f, 0.67f, 0.00f, 1.0f); return true; }
    if (l == "gray" || l == "grey") { outColor = ImVec4(0.67f, 0.67f, 0.67f, 1.0f); return true; }
    if (l == "dark_gray" || l == "dark_grey") { outColor = ImVec4(0.33f, 0.33f, 0.33f, 1.0f); return true; }
    if (l == "blue") { outColor = ImVec4(0.33f, 0.33f, 1.00f, 1.0f); return true; }
    if (l == "green") { outColor = ImVec4(0.33f, 1.00f, 0.33f, 1.0f); return true; }
    if (l == "aqua" || l == "cyan") { outColor = ImVec4(0.33f, 1.00f, 1.00f, 1.0f); return true; }
    if (l == "red") { outColor = ImVec4(1.00f, 0.33f, 0.33f, 1.0f); return true; }
    if (l == "light_purple" || l == "pink") { outColor = ImVec4(1.00f, 0.33f, 1.00f, 1.0f); return true; }
    if (l == "yellow") { outColor = ImVec4(1.00f, 1.00f, 0.33f, 1.0f); return true; }
    if (l == "white") { outColor = ImVec4(1.00f, 1.00f, 1.00f, 1.0f); return true; }

    return false;
}

static bool IsColor(const std::string& s) {
    ImVec4 dummy;
    return ParseHytaleColor(s, dummy);
}

static bool IsUseless(const std::string& s) {
    if (s.empty()) return true;
    if (s == " " || s == ": " || s == ":" || s == ">" || s == "»") return false;

    if (s.find('\\') != std::string::npos || s.find('/') != std::string::npos) {
        if (s.find("AppData") != std::string::npos || s.find("Client") != std::string::npos ||
            s.find("release") != std::string::npos || s.find("package") != std::string::npos ||
            s.find("Shared") != std::string::npos || s.find(".ui") != std::string::npos ||
            s.find(".glsl") != std::string::npos || s.find(".ttf") != std::string::npos ||
            s.find(".otf") != std::string::npos || s.find(".png") != std::string::npos ||
            s.find("Fonts") != std::string::npos || s.find("Interface") != std::string::npos) {
            return true;
        }
    }

    if (s.find(".glsl") != std::string::npos || s.find("Program") != std::string::npos ||
        s.find("Shader") != std::string::npos || s.find("Dithering") != std::string::npos ||
        s.find("BasicFog") != std::string::npos) {
        return true;
    }

    if (s == "en-US" || s == "en-GB" || s == "de-DE" || s == "fr-FR" || s == "es-ES")
        return true;

    if (s.size() >= 32 && s.find('-') != std::string::npos && s.find(' ') == std::string::npos)
        return true;

    if (s.rfind("com.", 0) == 0 || s.rfind("net.", 0) == 0 || s.rfind("org.", 0) == 0 ||
        s.rfind("server.", 0) == 0 || s.rfind("chat.", 0) == 0 || s.rfind("gui.", 0) == 0 ||
        s.rfind("engine.", 0) == 0 || s.rfind("key.", 0) == 0)
        return true;

    return false;
}

static bool IsMcFormatCode(unsigned char n) {
    n = (unsigned char)tolower(n);
    return (n >= '0' && n <= '9') || (n >= 'a' && n <= 'f')
        || n == 'k' || n == 'l' || n == 'm' || n == 'n' || n == 'o' || n == 'r';
}

static ImVec4 McCodeToColor(char code, ImVec4 defaultCol) {
    code = (char)tolower((unsigned char)code);
    switch (code) {
    case '0': return ImVec4(0.00f, 0.00f, 0.00f, 1.0f);
    case '1': return ImVec4(0.00f, 0.00f, 0.67f, 1.0f);
    case '2': return ImVec4(0.00f, 0.67f, 0.00f, 1.0f);
    case '3': return ImVec4(0.00f, 0.67f, 0.67f, 1.0f);
    case '4': return ImVec4(0.67f, 0.00f, 0.00f, 1.0f);
    case '5': return ImVec4(0.67f, 0.00f, 0.67f, 1.0f);
    case '6': return ImVec4(1.00f, 0.67f, 0.00f, 1.0f);
    case '7': return ImVec4(0.67f, 0.67f, 0.67f, 1.0f);
    case '8': return ImVec4(0.33f, 0.33f, 0.33f, 1.0f);
    case '9': return ImVec4(0.33f, 0.33f, 1.00f, 1.0f);
    case 'a': return ImVec4(0.33f, 1.00f, 0.33f, 1.0f);
    case 'b': return ImVec4(0.33f, 1.00f, 1.00f, 1.0f);
    case 'c': return ImVec4(1.00f, 0.33f, 0.33f, 1.0f);
    case 'd': return ImVec4(1.00f, 0.33f, 1.00f, 1.0f);
    case 'e': return ImVec4(1.00f, 1.00f, 0.33f, 1.0f);
    case 'f': return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
    case 'r': return ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
    default: return defaultCol;
    }
}

static void AddTextSegments(std::vector<ChatSeg>& segs, const std::string& text, ImVec4 initialColor) {
    if (text.empty()) return;

    ImVec4 curColor = initialColor;
    std::string curBuf;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = (unsigned char)text[i];

        if ((c == 0xA7 || c == '&') && i + 1 < text.size() && IsMcFormatCode((unsigned char)text[i + 1])) {
            if (!curBuf.empty()) {
                segs.push_back({ curBuf, curColor });
                curBuf.clear();
            }
            curColor = McCodeToColor(text[i + 1], initialColor);
            ++i;
            continue;
        }
        if (c == 0xC2 && i + 2 < text.size() && (unsigned char)text[i + 1] == 0xA7 && IsMcFormatCode((unsigned char)text[i + 2])) {
            if (!curBuf.empty()) {
                segs.push_back({ curBuf, curColor });
                curBuf.clear();
            }
            curColor = McCodeToColor(text[i + 2], initialColor);
            i += 2;
            continue;
        }

        curBuf += (char)c;
    }

    if (!curBuf.empty()) {
        segs.push_back({ curBuf, curColor });
    }
}

std::atomic<uint64_t> g_logVersion{ 1 };

static long long UnixNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static uint64_t PushLine(ChatLine line) {
    if (line.plain.empty()) return 0;

    int cap = 400;
    {
        std::lock_guard slock(g_settingsMtx);
        cap = g_prefs.maxLogLines;
    }
    if (cap < 100) cap = 100;

    std::lock_guard lock(g_mtx);

    if (!g_log.empty() &&
        g_log.back().plain == line.plain &&
        g_log.back().speaker == line.speaker &&
        g_log.back().outgoing == line.outgoing)
        return 0;

    if (line.id == 0)
        line.id = ++g_nextLineId;
    if (line.timeStr.empty())
        line.timeStr = NowHHMM();
    if (line.unixMs == 0)
        line.unixMs = UnixNowMs();

    uint64_t id = line.id;
    g_log.push_back(std::move(line));
    if (g_log.size() > (size_t)cap)
        g_log.erase(g_log.begin(), g_log.begin() + (g_log.size() - (size_t)cap));
    g_scroll = true;
    g_logVersion.fetch_add(1);
    return id;
}

bool SnapshotChatLog(std::vector<ChatLine>& cache, uint64_t& seenVersion) {
    const uint64_t v = g_logVersion.load();
    if (v == seenVersion) return false;

    std::lock_guard lock(g_mtx);
    seenVersion = g_logVersion.load();

    // Both sides are sorted by id (ids only grow, and the log only loses lines
    // at the front or all at once), so one merge pass pairs them up. Unchanged
    // lines are moved over; only new or edited ones are copied.
    std::vector<ChatLine> out;
    out.reserve(g_log.size());
    size_t j = 0;
    for (const ChatLine& src : g_log) {
        while (j < cache.size() && cache[j].id < src.id) ++j;
        if (j < cache.size() && cache[j].id == src.id && cache[j].rev == src.rev)
            out.push_back(std::move(cache[j++]));
        else
            out.push_back(src);
    }
    cache.swap(out);
    return true;
}

void CollectNewChatLines(uint64_t& lastId, std::vector<ChatLine>& out) {
    std::lock_guard lock(g_mtx);
    // Walk back from the end; new lines are always at the back.
    size_t first = g_log.size();
    while (first > 0 && g_log[first - 1].id > lastId) --first;
    for (size_t i = first; i < g_log.size(); ++i)
        out.push_back(g_log[i]);
    if (!g_log.empty() && g_log.back().id > lastId)
        lastId = g_log.back().id;
}

void ClearChatLog() {
    std::lock_guard lock(g_mtx);
    g_log.clear();
    g_logVersion.fetch_add(1);
}

void TouchChatLine(ChatLine& line) {
    ++line.rev;
    g_logVersion.fetch_add(1);
}

void PushLocalNote(const std::string& text, ImVec4 color) {
    ChatLine note;
    note.plain = text;
    note.segs.push_back({ text, color });
    note.outgoing = true;
    note.local = true;
    PushLine(std::move(note));
}

void QueueSend(PendingSend ps) {
    if (ps.gameText.empty()) return;
    if (ps.gameText.size() > HYTALE_CHAT_MAX) {
        // Back up to a character boundary; cutting inside a UTF-8 sequence
        // used to put a replacement box at the end of long non-English lines.
        size_t cut = HYTALE_CHAT_MAX;
        while (cut > 0 && ((unsigned char)ps.gameText[cut] & 0xC0) == 0x80) --cut;
        ps.gameText.resize(cut);
    }
    std::lock_guard lock(g_mtx);
    g_pending.push_back(std::move(ps));
}

// =============================================================================
// Case-insensitive helpers
// =============================================================================

// =============================================================================
// Speaker extraction + ignore matching
// =============================================================================
static std::string StripMcFormatting(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xA7 && i + 1 < s.size()) { ++i; continue; }
        if (c == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0xA7) {
            i += 2;
            if (i < s.size()) continue;
            break;
        }
        if (c == '&' && i + 1 < s.size() && IsMcFormatCode((unsigned char)s[i + 1])) {
            ++i;
            continue;
        }
        o += (char)c;
    }
    return o;
}

// =============================================================================
// Chat minigames (local, no API)
// =============================================================================

// --- Word Value (A=1 … Z=26) ---
static bool TryParseWordValueGame(const std::string& text, std::string& wordOut, int& valueOut) {
    const std::string lower = ToLower(text);
    if (lower.find("word value") == std::string::npos && lower.find("a=1") == std::string::npos)
        return false;

    size_t p = lower.find("what is ");
    if (p == std::string::npos) return false;
    size_t end = lower.find(" worth", p + 8);
    if (end == std::string::npos) return false;

    std::string word = TrimCopy(text.substr(p + 8, end - (p + 8)));
    while (!word.empty() && (word.back() == '?' || word.back() == '.' || word.back() == '!'))
        word.pop_back();
    word = TrimCopy(word);
    if (word.empty() || word.size() > 32) return false;

    int sum = 0;
    for (unsigned char c : word) {
        if (std::isalpha(c))
            sum += (std::tolower(c) - 'a' + 1);
    }
    if (sum <= 0) return false;

    wordOut = ToUpper(word);
    valueOut = sum;
    return true;
}

// --- Morse Code ---
static const std::unordered_map<std::string, char> kMorseTable = {
    {".-", 'A'},  {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'},  {".", 'E'},
    {"..-.", 'F'}, {"--.", 'G'},  {"....", 'H'}, {"..", 'I'},   {".---", 'J'},
    {"-.-", 'K'},  {".-..", 'L'}, {"--", 'M'},   {"-.", 'N'},   {"---", 'O'},
    {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'},  {"...", 'S'},  {"-", 'T'},
    {"..-", 'U'},  {"...-", 'V'}, {".--", 'W'},  {"-..-", 'X'}, {"-.--", 'Y'},
    {"--..", 'Z'},
    {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'}, {"....-", '4'},
    {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'}
};

static std::string DecodeMorse(const std::string& raw) {
    // Servers dress Morse up with typographic characters. Map the UTF-8 forms
    // back to dot / dash / space before decoding; a byte-at-a-time check used
    // to turn a no-break space (C2 A0) into a dot and drop en/em dashes.
    struct Sub { const char* from; char to; };
    static const Sub kSubs[] = {
        { "\xC2\xB7", '.' },      // middle dot
        { "\xE2\x80\xA2", '.' },  // bullet
        { "\xE2\x8B\x85", '.' },  // dot operator
        { "\xE2\x88\x99", '.' },  // bullet operator
        { "\xE2\x80\x93", '-' },  // en dash
        { "\xE2\x80\x94", '-' },  // em dash
        { "\xE2\x88\x92", '-' },  // minus sign
        { "\xE2\x80\x90", '-' },  // hyphen
        { "\xC2\xA0", ' ' },      // no-break space
    };
    std::string cleaned;
    cleaned.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        bool matched = false;
        for (const Sub& s : kSubs) {
            const size_t n = strlen(s.from);
            if (raw.compare(i, n, s.from) == 0) {
                cleaned += s.to;
                i += n;
                matched = true;
                break;
            }
        }
        if (matched) continue;
        const unsigned char c = (unsigned char)raw[i++];
        if (c == '.' || c == '-') cleaned += (char)c;
        else if (c == '_') cleaned += '-';
        else if (c == ' ' || c == '/' || c == '|' || std::isspace(c)) cleaned += ' ';
    }

    std::string result;
    std::string token;
    auto flush = [&]() {
        if (token.empty()) return;
        auto it = kMorseTable.find(token);
        if (it != kMorseTable.end())
            result += it->second;
        token.clear();
        };

    for (char c : cleaned) {
        if (c == ' ') {
            flush();
            // treat multiple spaces or / as word separator
            if (!result.empty() && result.back() != ' ')
                result += ' ';
        }
        else {
            token += c;
        }
    }
    flush();

    // collapse multiple spaces and trim
    std::string final;
    bool space = false;
    for (char c : result) {
        if (c == ' ') {
            if (!space) { final += ' '; space = true; }
        }
        else {
            final += c;
            space = false;
        }
    }
    return TrimCopy(final);
}

static bool TryParseMorseGame(const std::string& text, std::string& decodedOut) {
    const std::string lower = ToLower(text);
    if (lower.find("morse code") == std::string::npos &&
        lower.find("decode this morse") == std::string::npos)
        return false;

    size_t colon = text.rfind(':');
    if (colon == std::string::npos) return false;

    std::string morse = TrimCopy(text.substr(colon + 1));
    if (morse.empty()) return false;

    std::string decoded = DecodeMorse(morse);
    if (decoded.empty() || decoded.size() > 48) return false;

    // must contain at least one letter
    bool hasLetter = false;
    for (unsigned char c : decoded)
        if (std::isalpha(c)) { hasLetter = true; break; }
    if (!hasLetter) return false;

    decodedOut = ToUpper(decoded);
    return true;
}

// --- Caesar Cipher ---
static std::string DecryptCaesar(const std::string& cipher, int shift) {
    shift %= 26;
    if (shift < 0) shift += 26;

    std::string out;
    out.reserve(cipher.size());
    for (unsigned char c : cipher) {
        if (std::isalpha(c)) {
            const bool upper = std::isupper(c);
            const int base = upper ? 'A' : 'a';
            int v = (c - base - shift + 26) % 26;
            out += (char)(base + v);
        }
        else {
            out += (char)c;
        }
    }
    return out;
}

static bool TryParseCaesarGame(const std::string& text, std::string& plainOut) {
    const std::string lower = ToLower(text);
    if (lower.find("caesar cipher") == std::string::npos &&
        lower.find("caesar") == std::string::npos)
        return false;

    // find shift number
    size_t shiftPos = lower.find("shift ");
    if (shiftPos == std::string::npos) return false;
    shiftPos += 6;
    while (shiftPos < lower.size() && !std::isdigit((unsigned char)lower[shiftPos]))
        ++shiftPos;
    if (shiftPos >= lower.size()) return false;

    int shift = 0;
    try {
        shift = std::stoi(lower.substr(shiftPos));
    }
    catch (...) { return false; }

    // find the ciphertext after "decode:"
    size_t decPos = lower.find("decode");
    if (decPos == std::string::npos) return false;
    size_t colon = text.find(':', decPos);
    if (colon == std::string::npos) return false;

    std::string cipher = TrimCopy(text.substr(colon + 1));
    // strip trailing punctuation
    while (!cipher.empty() && (cipher.back() == '.' || cipher.back() == '!' || cipher.back() == '?'))
        cipher.pop_back();
    cipher = TrimCopy(cipher);
    if (cipher.empty() || cipher.size() > 64) return false;

    std::string plain = DecryptCaesar(cipher, shift);
    if (plain.empty()) return false;

    // must have at least one letter
    bool hasLetter = false;
    for (unsigned char c : plain)
        if (std::isalpha(c)) { hasLetter = true; break; }
    if (!hasLetter) return false;

    plainOut = ToUpper(plain);
    return true;
}

static int TryParseCountLetters(const std::string& plain) {
    std::string lower = ToLower(TrimCopy(plain));

    // Must mention the game name
    if (lower.find("count the letters") == std::string::npos)
        return -1;

    // Prefer the explicit question form
    const char* markers[] = {
        "how many letters are in ",
        "how many letters in ",
        "letters are in ",
        "letters in "
    };

    size_t pos = std::string::npos;
    size_t markerLen = 0;
    for (const char* m : markers) {
        pos = lower.find(m);
        if (pos != std::string::npos) {
            markerLen = strlen(m);
            break;
        }
    }
    if (pos == std::string::npos)
        return -1;

    // Word starts after the marker
    size_t start = pos + markerLen;
    while (start < lower.size() && !std::isalpha((unsigned char)lower[start]))
        ++start;

    // Collect consecutive letters (stop at ?, punctuation, or end)
    std::string word;
    for (size_t i = start; i < lower.size(); ++i) {
        unsigned char c = (unsigned char)lower[i];
        if (std::isalpha(c))
            word.push_back((char)c);
        else if (!word.empty())
            break; // end of the target word
    }

    if (word.empty())
        return -1;

    // Pure letter count (no A=1 weighting — that is the other game)
    return (int)word.size();
}

// --- Pattern (what comes next) ---
// Handles common chat sequences: geometric, arithmetic, 2nd-order, Fibonacci-like.
static bool ExtractPatternNumbers(const std::string& text, std::vector<long long>& nums) {
    nums.clear();
    // Prefer the segment after "next" / ":" so we don't pick random digits from the banner.
    std::string lower = ToLower(text);
    size_t start = 0;
    size_t p = lower.find("comes next");
    if (p != std::string::npos) start = p;
    else {
        p = lower.find("next");
        if (p != std::string::npos) start = p;
    }
    p = text.find(':', start);
    if (p != std::string::npos) start = p + 1;

    const std::string slice = text.substr(start);
    std::string token;
    auto flush = [&]() {
        if (token.empty()) return;
        try {
            // strip trailing punctuation that sometimes sticks to the last number
            while (!token.empty() && !std::isdigit((unsigned char)token.back()) && token.back() != '-')
                token.pop_back();
            if (!token.empty())
                nums.push_back(std::stoll(token));
        }
        catch (...) {}
        token.clear();
        };

    for (size_t i = 0; i < slice.size(); ++i) {
        unsigned char c = (unsigned char)slice[i];
        if (std::isdigit(c) || (c == '-' && token.empty()))
            token += (char)c;
        else
            flush();
    }
    flush();
    return nums.size() >= 3;
}

static bool TrySolveSequence(const std::vector<long long>& a, long long& nextOut) {
    const size_t n = a.size();
    if (n < 3) return false;

    // 1) Geometric (constant ratio) — prefer exact integer ratios
    {
        bool ok = true;
        long long rNum = 0, rDen = 1;
        // ratio a[1]/a[0] as reduced fraction if possible
        if (a[0] != 0) {
            // check all consecutive ratios equal
            for (size_t i = 1; i < n; ++i) {
                if (a[i - 1] == 0) { ok = false; break; }
                // a[i] * a[0] == a[i-1] * a[1]  (cross-multiply to avoid floats)
                if (a[i] * a[0] != a[i - 1] * a[1]) { ok = false; break; }
            }
            if (ok) {
                // next = a[n-1] * a[1] / a[0]
                if (a[0] != 0 && (a[n - 1] * a[1]) % a[0] == 0) {
                    nextOut = (a[n - 1] * a[1]) / a[0];
                    return true;
                }
            }
        }
        // also try pure integer multiply/divide with common ratio via integer division
        ok = true;
        if (a[0] != 0 && a[1] % a[0] == 0) {
            long long r = a[1] / a[0];
            for (size_t i = 1; i < n; ++i) {
                if (a[i - 1] == 0 || a[i] != a[i - 1] * r) { ok = false; break; }
            }
            if (ok) {
                nextOut = a[n - 1] * r;
                return true;
            }
        }
        // decreasing geometric with integer division (81,27,9,3 → ÷3)
        ok = true;
        if (a[1] != 0 && a[0] % a[1] == 0) {
            long long d = a[0] / a[1]; // divisor
            if (d != 0) {
                for (size_t i = 1; i < n; ++i) {
                    if (a[i] == 0 || a[i - 1] % a[i] != 0 || a[i - 1] / a[i] != d) {
                        ok = false; break;
                    }
                }
                if (ok && a[n - 1] % d == 0) {
                    nextOut = a[n - 1] / d;
                    return true;
                }
            }
        }
    }

    // 2) Arithmetic (constant difference)
    {
        long long d = a[1] - a[0];
        bool ok = true;
        for (size_t i = 2; i < n; ++i) {
            if (a[i] - a[i - 1] != d) { ok = false; break; }
        }
        if (ok) {
            nextOut = a[n - 1] + d;
            return true;
        }
    }

    // 3) Second-order arithmetic (constant 2nd difference) — squares, triangular, etc.
    if (n >= 4) {
        std::vector<long long> d1;
        for (size_t i = 1; i < n; ++i) d1.push_back(a[i] - a[i - 1]);
        long long d2 = d1[1] - d1[0];
        bool ok = true;
        for (size_t i = 2; i < d1.size(); ++i) {
            if (d1[i] - d1[i - 1] != d2) { ok = false; break; }
        }
        if (ok) {
            long long nextD1 = d1.back() + d2;
            nextOut = a[n - 1] + nextD1;
            return true;
        }
    }

    // 4) Fibonacci-like (each term = sum of previous two)
    {
        bool ok = true;
        for (size_t i = 2; i < n; ++i) {
            if (a[i] != a[i - 1] + a[i - 2]) { ok = false; break; }
        }
        if (ok) {
            nextOut = a[n - 1] + a[n - 2];
            return true;
        }
    }

    return false;
}

static bool TryParsePatternGame(const std::string& text, std::string& answerOut) {
    const std::string lower = ToLower(text);
    if (lower.find("pattern") == std::string::npos &&
        lower.find("what comes next") == std::string::npos &&
        lower.find("comes next") == std::string::npos)
        return false;

    std::vector<long long> nums;
    if (!ExtractPatternNumbers(text, nums))
        return false;

    long long next = 0;
    if (!TrySolveSequence(nums, next))
        return false;

    answerOut = std::to_string(next);
    return true;
}

static size_t FindChatColon(const std::string& s) {
    size_t p = s.find(": ");
    if (p != std::string::npos) return p;
    p = s.find(':');
    if (p != std::string::npos) return p;
    return s.find("\xEF\xBC\x9A");
}

static size_t ColonSkip(const std::string& s, size_t pos) {
    if (pos >= s.size()) return 0;
    if (s[pos] == ':' && pos + 1 < s.size() && s[pos + 1] == ' ') return 2;
    if (s[pos] == ':') return 1;
    if ((unsigned char)s[pos] == 0xEF) return 3;
    return 1;
}

std::string NormalizeSpeaker(std::string speaker) {
    speaker = StripMcFormatting(speaker);
    speaker = TrimCopy(speaker);

    while (!speaker.empty()) {
        char c = speaker.back();
        if (c == ']' || c == '>' || c == ')' || c == ':' || isspace((unsigned char)c))
            speaker.pop_back();
        else
            break;
    }
    speaker = TrimCopy(speaker);

    size_t br = speaker.find_last_of("[]<>");
    if (br != std::string::npos)
        speaker = TrimCopy(speaker.substr(br + 1));

    size_t sp = speaker.find_last_of(" \t|•·★☆");
    if (sp != std::string::npos && sp + 1 < speaker.size()) {
        std::string last = TrimCopy(speaker.substr(sp + 1));
        if (last.size() >= 2)
            speaker = last;
    }

    while (!speaker.empty() && ispunct((unsigned char)speaker.front()) && (unsigned char)speaker.front() < 0x80)
        speaker.erase(0, 1);
    while (!speaker.empty() && ispunct((unsigned char)speaker.back()) && (unsigned char)speaker.back() < 0x80)
        speaker.pop_back();

    return TrimCopy(speaker);
}

// Alphanumeric-only key for tolerant name compare (The_Jose == TheJose).
static std::string AlnumKey(std::string s) {
    s = ToLower(NormalizeSpeaker(std::move(s)));
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c)) o += (char)c;
    }
    return o;
}

// True if chat speaker is the configured local player (ranks / underscores / tags tolerated).
bool IsLocalPlayerSpeaker(const std::string& speaker, const std::string& localNameRaw) {
    const std::string sp = ToLower(NormalizeSpeaker(speaker));
    const std::string loc = ToLower(NormalizeSpeaker(localNameRaw));
    if (sp.empty() || loc.empty()) return false;
    if (sp == loc) return true;

    // Suffix whole-token match: "VIP The_Jose" still ends with the local name.
    if (sp.size() > loc.size()) {
        const size_t pos = sp.rfind(loc);
        if (pos != std::string::npos && pos + loc.size() == sp.size()) {
            if (pos == 0 || !std::isalnum((unsigned char)sp[pos - 1]))
                return true;
        }
    }

    const std::string spA = AlnumKey(speaker);
    const std::string locA = AlnumKey(localNameRaw);
    if (!spA.empty() && spA == locA) return true;
    if (!locA.empty() && spA.size() > locA.size()) {
        const size_t pos = spA.rfind(locA);
        if (pos != std::string::npos && pos + locA.size() == spA.size())
            return true;
    }
    return false;
}

static bool ExtractSpeakerAndBody(const std::string& plainText, std::string& speaker, std::string& body) {
    speaker.clear();
    body = plainText;

    const std::string cleaned = StripMcFormatting(plainText);
    size_t colonPos = FindChatColon(cleaned);
    if (colonPos == std::string::npos || colonPos >= 64)
        return false;

    speaker = NormalizeSpeaker(cleaned.substr(0, colonPos));
    body = TrimCopy(cleaned.substr(colonPos + ColonSkip(cleaned, colonPos)));
    return !speaker.empty();
}

bool IsBotIdentity(const std::string& name) {
    std::string bot;
    {
        std::lock_guard lock(g_settingsMtx);
        bot = ToLower(g_botName);
    }
    const std::string s = ToLower(NormalizeSpeaker(name));
    if (!bot.empty() && s.find(bot) != std::string::npos) return true;
    if (s.find("hyai") != std::string::npos) return true;
    return false;
}

bool LooksLikeBotLine(const std::string& speaker, const std::string& body, const std::string& plain) {
    if (IsBotIdentity(speaker))
        return true;
    if (StartsWithCI(body, "HyAI > ") || StartsWithCI(body, "HYAI: ") ||
        StartsWithCI(body, "hyai > ") || StartsWithCI(body, "hyai: "))
        return true;

    const std::string cleaned = TrimCopy(StripMcFormatting(plain));
    const std::string cleanedLower = ToLower(cleaned);
    if (cleanedLower.find("[hyai]") != std::string::npos)
        return true;
    if (StartsWithCI(cleaned, "hyai >") || StartsWithCI(cleaned, "hyai:") ||
        StartsWithCI(cleaned, "hyai »") || StartsWithCI(cleaned, "[hyai]"))
        return true;

    if (!cleaned.empty() && cleaned[0] == '[') {
        const size_t rb = cleaned.find(']');
        if (rb != std::string::npos && rb > 1 && rb < 24 &&
            IsBotIdentity(cleaned.substr(1, rb - 1)))
            return true;
    }

    size_t sep = cleaned.find_first_of(":>»|");
    if (sep != std::string::npos && sep >= 2 && sep < 24 &&
        IsBotIdentity(cleaned.substr(0, sep)))
        return true;

    return false;
}

static std::string WakeHay(const std::string& speaker, const std::string& body, const std::string& plain) {
    if (!speaker.empty() && !body.empty())
        return body;

    std::string cleaned = TrimCopy(StripMcFormatting(plain));
    if (!cleaned.empty() && cleaned[0] == '[') {
        const size_t rb = cleaned.find(']');
        if (rb != std::string::npos && rb < 32)
            cleaned = TrimCopy(cleaned.substr(rb + 1));
    }

    const size_t colon = FindChatColon(cleaned);
    if (colon != std::string::npos && colon < 64)
        return TrimCopy(cleaned.substr(colon + ColonSkip(cleaned, colon)));

    size_t sep = cleaned.find_first_of("»>");
    if (sep != std::string::npos && sep >= 2 && sep < 24)
        return TrimCopy(cleaned.substr(sep + 1));

    return cleaned;
}


static bool IsUserWhitelisted(const std::string& speaker) {
    const std::string speakerNorm = ToLower(NormalizeSpeaker(speaker));
    if (speakerNorm.empty()) return false;
    std::lock_guard lock(g_mtx);
    return g_whitelist.count(speakerNorm) > 0;
}

static bool IsUserIgnored(const std::string& speaker, const std::string& plainText) {
    const std::string speakerNorm = ToLower(NormalizeSpeaker(speaker));

    std::string prefix = StripMcFormatting(plainText);
    size_t colon = FindChatColon(prefix);
    if (colon != std::string::npos && colon < 80)
        prefix = prefix.substr(0, colon);
    const std::string prefixLower = ToLower(prefix);
    const std::string prefixNorm = ToLower(NormalizeSpeaker(prefix));

    std::lock_guard lock(g_mtx);
    if (!speakerNorm.empty() && g_ignored.count(speakerNorm))
        return true;
    if (!prefixNorm.empty() && g_ignored.count(prefixNorm))
        return true;

    for (const auto& n : g_ignored) {
        if (n.empty()) continue;
        if (speakerNorm == n || prefixNorm == n)
            return true;
        if (HasWholeWord(prefixLower, n) || HasWholeWord(speakerNorm, n))
            return true;
    }
    return false;
}

bool IsReservedSpeakerName(const std::string& key) {
    static const char* kDeny[] = {
        "server", "console", "system", "hyai", "hyai_", "hyai-bot", "hyai_bot",
        "announcement", "announce", "notify", "notification", "news", "broadcast",
        "admin", "administrator", "notif", "info", "warn", "warning", "event",
        "lobby", "proxy", "velocity", "bungee", "paper", "spigot", "unknown",
        "you", "everyone", "discord", "twitch", "youtube", "motd", "tab",
        "scoreboard", "actionbar", "title", "subtitle", "bossbar", "death",
        "join", "leave", "whisper", "msg", "tell", "party", "guild", "staff",
        "helper", "mod", "moderator", "owner", "null", "undefined", "histatu",
        "hytale", "world", "chat", "global", "local", "trade", "shop", "auction",
        "spawn", "hub", "skyblock", "island", "coop", "co-op", "visitor",
        "guest", "npc", "quest", "alert", "notice", "tip", "help", "rules",
        "vote", "store", "donate", "ranks", "rank", "vip", "mvp", "media",
        "tiktok", "website", "web", "mail", "inbox",
        "pm", "dm", "team", "clan", "faction", "war", "pvp", "pve", "minigame",
        "game", "match", "queue", "afk", "auto", "bot", "plugin", "command",
        "timer", "hd builder", "builder", "hologram", "bazaar", "ah",
        nullptr
    };
    for (int i = 0; kDeny[i]; ++i)
        if (key == kDeny[i]) return true;
    return false;
}

static bool IsPlausiblePlayerName(const std::string& name) {
    if (name.size() < 2 || name.size() > 32) return false;
    if (name.find(' ') != std::string::npos) return false;
    if (IsColor(name)) return false;

    int letters = 0;
    for (unsigned char c : name) {
        if (isalpha(c)) { ++letters; continue; }
        if (isdigit(c) || c == '_' || c == '-') continue;
        return false;
    }
    if (letters < 1) return false;

    const std::string key = ToLower(name);
    if (IsBotIdentity(key) || IsReservedSpeakerName(key))
        return false;
    return true;
}

static bool LooksLikeAnnouncementBody(const std::string& body) {
    const std::string b = ToLower(TrimCopy(body));
    if (b.empty()) return true;
    static const char* kPhrases[] = {
        "joined the game", "left the game", "has joined", "has left",
        "joined the server", "left the server", "joined the lobby",
        "has connected", "has disconnected", "welcome to",
        "voted for the server", "was slain", "was killed",
        "fell out of the world", "drowned", "burned to death",
        "your nickname is", "nickname set to", "you are now nicked",
        "no one answered correctly", "lots of information",
        nullptr
    };
    for (int i = 0; kPhrases[i]; ++i)
        if (b.find(kPhrases[i]) != std::string::npos)
            return true;
    return false;
}

static void RememberSeenPlayer(const std::string& speaker) {
    const std::string display = NormalizeSpeaker(speaker);
    if (!IsPlausiblePlayerName(display))
        return;

    const std::string key = ToLower(display);

    std::string localName;
    {
        std::lock_guard slock(g_settingsMtx);
        localName = ToLower(NormalizeSpeaker(g_localName));
    }
    if (!localName.empty() && key == localName)
        return;

    const long long now = NowSteadyMs();
    std::lock_guard lock(g_mtx);
    for (auto it = g_seenPlayers.begin(); it != g_seenPlayers.end(); ++it) {
        if (it->key == key) {
            SeenPlayer updated = *it;
            updated.lastSeenMs = now;
            g_seenPlayers.erase(it);
            g_seenPlayers.push_back(std::move(updated));
            return;
        }
    }

    SeenPlayer sp;
    sp.display = display;
    sp.key = key;
    sp.lastSeenMs = now;
    g_seenPlayers.push_back(std::move(sp));
    while (g_seenPlayers.size() > SEEN_PLAYERS_MAX)
        g_seenPlayers.pop_front();
}

static void RememberGlobalChat(const std::string& speaker, const std::string& body,
    const std::string& plain, bool fromBot) {
    GlobalChatEntry e;
    e.speaker = speaker;
    e.body = body.empty() ? plain : body;
    e.plain = plain;
    e.fromBot = fromBot;
    e.unixMs = UnixNowMs();
    static uint64_t s_nextCtxId = 0;

    int cap = 20;
    {
        std::lock_guard slock(g_settingsMtx);
        cap = g_maxChatContext;
    }
    if (cap < 4) cap = 4;
    if (cap > 80) cap = 80;

    std::lock_guard lock(g_mtx);
    if (!g_globalChat.empty()) {
        const auto& last = g_globalChat.back();
        if (last.plain == e.plain || (last.speaker == e.speaker && last.body == e.body))
            return;
    }
    e.id = ++s_nextCtxId;      // under g_mtx
    g_globalChat.push_back(std::move(e));
    while ((int)g_globalChat.size() > cap)
        g_globalChat.pop_front();
}

static bool ConsumeIfDuplicateMessage(void* message, const std::string& plain) {
    const long long now = NowSteadyMs();
    std::lock_guard lock(g_dedupMtx);
    while (!g_recentProcessed.empty() && now - g_recentProcessed.front().whenMs > 2500)
        g_recentProcessed.pop_front();

    for (const auto& e : g_recentProcessed) {
        if (message && e.ptr == message && now - e.whenMs < 2000)
            return true;
        if (!plain.empty() && e.plain == plain && now - e.whenMs < 1000)
            return true;
    }

    DedupEntry e;
    e.ptr = message;
    e.plain = plain;
    e.whenMs = now;
    g_recentProcessed.push_back(std::move(e));
    while (g_recentProcessed.size() > 32)
        g_recentProcessed.pop_front();
    return false;
}

static bool MatchRecentOutgoing(const std::string& plain, const std::string& body, bool& silentOut) {
    silentOut = false;
    const std::string plainClean = TrimCopy(StripMcFormatting(plain));
    const std::string bodyClean = TrimCopy(StripMcFormatting(body.empty() ? plain : body));
    const std::string plainKey = ToLower(plainClean);
    const std::string bodyKey = ToLower(bodyClean);
    const long long now = NowSteadyMs();

    std::lock_guard lock(g_mtx);
    while (!g_recentOut.empty() && now - g_recentOut.front().whenMs > 15000)
        g_recentOut.pop_front();

    for (const auto& ro : g_recentOut) {
        const std::string game = TrimCopy(StripMcFormatting(ro.gameText));
        const std::string typed = TrimCopy(StripMcFormatting(ro.typedText));
        const std::string gameKey = ToLower(game);
        const std::string typedKey = ToLower(typed);

        auto matches = [&](const std::string& candidate) {
            if (candidate.empty()) return false;
            if (plainKey == candidate || bodyKey == candidate) return true;
            if (plainKey.size() >= candidate.size() &&
                plainKey.compare(plainKey.size() - candidate.size(), candidate.size(), candidate) == 0)
                return true;
            if (bodyKey == candidate) return true;
            return false;
            };

        if (matches(gameKey) || matches(typedKey) || (!ro.bodyKey.empty() && matches(ro.bodyKey))) {
            silentOut = ro.silent;
            return true;
        }
    }
    return false;
}


// Private + owner + AI trigger + send
// =============================================================================
// Local Ollama Provider
// =============================================================================

// OllamaProvider removed – use AI::GetResponse from ai.hpp
namespace OllamaProvider {
    inline bool GetResponse(const std::string& userPrompt, std::string& response) {
        return AI::GetResponse(userPrompt, response);
    }
}

// True if `text` starts with an owner-command prefix (wake word / wake2 / bot name / legacy "josebot").
// On match, sets restOut to the trimmed remainder after the prefix.

// Build and queue an encrypted private message (public or /msg <player>).
bool QueuePrivateEncrypted(const std::string& plaintext, const std::string& targetPlayer, bool viaMsg) {
    if (plaintext.empty()) return false;
    if (!Extras::HasPrivateKey()) return false;
    std::string wire = Extras::EncryptPrivateMessage(plaintext);
    if (wire.empty()) return false;

    std::string gameText;
    std::string display;
    std::string tgt = TrimCopy(targetPlayer);
    if (viaMsg && !tgt.empty()) {
        gameText = "/msg " + tgt + " " + wire;
        display = "[private /msg " + tgt + "] " + plaintext;
    }
    else {
        gameText = wire;
        display = "[private public] " + plaintext;
    }
    if (gameText.size() > HYTALE_CHAT_MAX) {
        LogToFile("Private msg too long for chat (" + std::to_string(gameText.size()) + " > " + std::to_string(HYTALE_CHAT_MAX) + ")");
        return false;
    }
    QueueSend({ gameText, display, "", "", "" });
    return true;
}

// If body contains ~jb1.... ciphertext (public or inside a /msg / whisper line), try decrypt.
// Robust extraction: finds ~jb1. then keeps the longest run of Base64URL characters.
bool TryDecryptPrivateInBody(const std::string& messageBody, std::string& plainOut) {
    plainOut.clear();
    if (messageBody.empty()) return false;

    size_t p = messageBody.find("~hy1.");
    if (p == std::string::npos) return false;

    // After the prefix keep only valid Base64URL chars (A-Za-z0-9-_)
    size_t start = p;
    size_t end = start + 5; // past "~hy1."
    while (end < messageBody.size()) {
        unsigned char c = (unsigned char)messageBody[end];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            ++end;
        }
        else {
            break;
        }
    }
    std::string token = messageBody.substr(start, end - start);
    if (!Extras::IsPrivateCiphertext(token)) {
        LogToFile("PrivateDecrypt: found ~hy1. but cleaned token invalid, len=" +
            std::to_string(token.size()) + " tok=" + token.substr(0, 28));
        return false;
    }

    plainOut = Extras::DecryptPrivateMessage(token);
    if (plainOut.empty()) {
        LogToFile("PrivateDecrypt: GCM/key failed  token_len=" +
            std::to_string(token.size()) + " tok=" + token.substr(0, 28) +
            " key_loaded=" + (Extras::HasPrivateKey() ? "yes" : "NO") +
            " fp=" + Extras::PrivateKeyFingerprint());
        return false;
    }

    LogToFile("PrivateDecrypt: OK  plain_len=" + std::to_string(plainOut.size()) +
        " token_len=" + std::to_string(token.size()) +
        " fp=" + Extras::PrivateKeyFingerprint());
    return true;
}

static bool MatchOwnerCmdPrefix(const std::string& text, std::string& restOut, std::string& usedPrefixOut) {
    restOut.clear();
    usedPrefixOut.clear();
    const std::string t = TrimCopy(text);
    if (t.empty()) return false;

    std::string wake, wake2, bot;
    {
        std::lock_guard lock(g_settingsMtx);
        wake = TrimCopy(g_wakeWord);
        wake2 = TrimCopy(g_wakeWord2);
        bot = TrimCopy(g_botName);
    }
    if (wake.empty()) wake = "hyai";

    // Candidates: configured wake words + bot name + legacy josebot.
    std::string candidates[4];
    int nCand = 0;
    auto addPrefix = [&](const std::string& p) {
        if (p.empty() || nCand >= 4) return;
        const std::string pl = ToLower(p);
        for (int i = 0; i < nCand; ++i)
            if (ToLower(candidates[i]) == pl) return;
        candidates[nCand++] = p;
        };
    addPrefix(wake);
    addPrefix(wake2);
    addPrefix(bot);
    addPrefix("hyai"); // always accept legacy prefix too

    // Prefer the longest matching prefix.
    const std::string lower = ToLower(t);
    int bestIdx = -1;
    size_t bestLen = 0;
    for (int i = 0; i < nCand; ++i) {
        const std::string pl = ToLower(candidates[i]);
        if (pl.empty() || lower.size() < pl.size()) continue;
        if (lower.compare(0, pl.size(), pl) != 0) continue;
        // Exact prefix alone, or prefix + space/args (not "hitbotxyz").
        if (lower.size() != pl.size() && lower[pl.size()] != ' ') continue;
        if (pl.size() > bestLen) {
            bestLen = pl.size();
            bestIdx = i;
        }
    }
    if (bestIdx < 0) return false;
    usedPrefixOut = candidates[bestIdx];
    restOut = (t.size() > bestLen) ? TrimCopy(t.substr(bestLen)) : std::string();
    return true;
}

// Forward decl — used by owner commands + UI before the definition near HandleAIRequest
void CancelAI();

bool HandleOwnerCommand(const std::string& rawText, bool fromChatLine) {
    std::string cmd = rawText;
    if (fromChatLine) {
        size_t colon = rawText.find(": ");
        if (colon != std::string::npos && colon < 80)
            cmd = rawText.substr(colon + 2);
    }
    cmd = TrimCopy(cmd);

    std::string rest, usedPrefix;
    if (!MatchOwnerCmdPrefix(cmd, rest, usedPrefix))
        return false;

    std::string restLower = ToLower(rest);

    std::string tagName;
    {
        std::lock_guard lock(g_settingsMtx);
        tagName = g_botName.empty() ? "HyAI" : g_botName;
    }
    const std::string tag = "[" + tagName + "] ";

    auto pushConf = [](const std::string& msg, ImVec4 col) { PushLocalNote(msg, col); };

    if (rest.empty() || restLower == "help") {
        pushConf(tag + "ignore/unignore/ignorelist/whitelist/clearmemory/reloadprompt/live/color/nick/think/mute/sendlang/echo/context/reminders/cancelrem/privkey/privsend/approve/discard/retry/mode/preset/presets",
            ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
        return true;
    }

    if (StartsWithCI(rest, "ignore ")) {
        std::string name = rest.substr(7);
        name = NormalizeSpeaker(name);
        if (!name.empty()) {
            std::string lname = ToLower(name);
            {
                std::lock_guard lock(g_mtx);
                g_ignored.insert(lname);
            }
            SaveIgnored();
            pushConf(tag + "ignored " + name, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
            LogToFile("Ignored user: [" + lname + "]");
            return true;
        }
    }
    else if (StartsWithCI(rest, "unignore ")) {
        std::string name = rest.substr(9);
        name = NormalizeSpeaker(name);
        if (!name.empty()) {
            std::string lname = ToLower(name);
            bool removed = false;
            {
                std::lock_guard lock(g_mtx);
                removed = g_ignored.erase(lname) > 0;
            }
            if (removed) SaveIgnored();
            pushConf(removed ? (tag + "unignored " + name) : (tag + name + " was not ignored"),
                ImVec4(0.5f, 1.0f, 0.6f, 1.0f));
            return true;
        }
    }
    else if (restLower == "ignorelist" || restLower == "ignored") {
        std::string list;
        {
            std::lock_guard lock(g_mtx);
            for (const auto& n : g_ignored) {
                if (!list.empty()) list += ", ";
                list += n;
            }
        }
        pushConf(list.empty() ? tag + "ignore list empty" : (tag + "ignored: " + list),
            ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
        return true;
    }
    else if (restLower == "clearmemory" || restLower == "forget") {
        {
            std::lock_guard lock(g_mtx);
            g_memory.clear();
            g_globalChat.clear();
        }
        pushConf(tag + "memory + chat context cleared", ImVec4(0.8f, 0.7f, 1.0f, 1.0f));
        return true;
    }
    else if (restLower == "cancel" || restLower == "stop" || restLower == "cancelai") {
        CancelAI();
        pushConf(tag + "AI cancelled + queue cleared", ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        return true;
    }
    else if (restLower == "approve" || restLower == "send" || restLower == "yes") {
        pushConf(tag + (AiFlow::ApproveOldest() ? "approved" : "nothing is waiting for approval"),
            ImVec4(0.45f, 1.0f, 0.55f, 1.0f));
        return true;
    }
    else if (restLower == "discard" || restLower == "drop" || restLower == "no" || restLower == "skip") {
        pushConf(tag + (AiFlow::DiscardOldest() ? "discarded" : "nothing is waiting for approval"),
            ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
        return true;
    }
    else if (restLower == "retry" || restLower == "again") {
        pushConf(tag + (AiFlow::RetryNewest() ? "asking again" : "nothing to retry"), ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
        return true;
    }
    else if (restLower == "mode" || StartsWithCI(rest, "mode ")) {
        static const char* names[] = { "auto (replies go out by themselves)", "ask (drafts wait for your OK)", "dry (never sends, just drafts)" };
        const std::string arg = restLower.size() > 5 ? TrimCopy(restLower.substr(5)) : std::string();
        int mode = -1;
        if (arg == "auto" || arg == "0" || arg == "on") mode = 0;
        else if (arg == "ask" || arg == "review" || arg == "1") mode = 1;
        else if (arg == "dry" || arg == "off" || arg == "2" || arg == "log") mode = 2;
        {
            std::lock_guard lock(g_settingsMtx);
            if (mode >= 0) { g_prefs.aiReplyMode = mode; MarkPrefsChanged(); }
            else mode = g_prefs.aiReplyMode;
        }
        pushConf(tag + "AI mode: " + names[mode] + (arg.empty() ? "   (mode auto | ask | dry)" : ""), ImVec4(0.85f, 0.75f, 1.0f, 1.0f));
        return true;
    }
    else if (restLower == "presets") {
        std::string list;
        for (const auto& it : AiPrompt::Items()) {
            if (!list.empty()) list += ", ";
            list += (it.enabled ? "+" : "-") + it.name;
        }
        pushConf(tag + "prompt presets: " + list + "   (preset <name> [on|off])", ImVec4(0.85f, 0.75f, 1.0f, 1.0f));
        return true;
    }
    else if (StartsWithCI(rest, "preset ")) {
        std::string arg = TrimCopy(rest.substr(7));
        int force = -1;                                   // trailing "on" / "off"
        const std::string la = ToLower(arg);
        if (la.size() > 3 && la.compare(la.size() - 3, 3, " on") == 0) { force = 1; arg = TrimCopy(arg.substr(0, arg.size() - 3)); }
        else if (la.size() > 4 && la.compare(la.size() - 4, 4, " off") == 0) { force = 0; arg = TrimCopy(arg.substr(0, arg.size() - 4)); }
        const std::string want = ToLower(arg);
        const AiPrompt::Item* hit = nullptr;
        const auto items = AiPrompt::Items();
        for (const auto& it : items) if (ToLower(it.name) == want) hit = &it;
        if (!hit) {                                       // a unique partial name is enough
            int n = 0;
            for (const auto& it : items) if (ToLower(it.name).find(want) != std::string::npos) { hit = &it; ++n; }
            if (n != 1) hit = nullptr;
        }
        if (!hit) {
            pushConf(tag + "no single preset matches \"" + arg + "\" (see: presets)", ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
        }
        else {
            const bool on = force < 0 ? !hit->enabled : force == 1;
            AiPrompt::SetEnabled(hit->name, on);
            pushConf(tag + "preset " + hit->name + (on ? " on" : " off"), ImVec4(0.85f, 0.75f, 1.0f, 1.0f));
        }
        return true;
    }
    else if (restLower == "reloadprompt" || restLower == "reload" || restLower == "reloadcontext") {
        ReloadSystemPrompt(true);
        std::string status;
        {
            /* prompt lock in settings */
            status = GetPromptStatus() /*was g_promptStatus*/;
        }
        pushConf(tag + status, ImVec4(0.7f, 0.9f, 0.5f, 1.0f));
        return true;
    }
    else if (restLower == "live" || restLower == "translate" || restLower == "toggletranslate") {
        bool newState;
        {
            std::lock_guard lock(g_settingsMtx);
            g_liveTranslate = !g_liveTranslate;
            newState = g_liveTranslate;
        }
        SaveSettings();
        pushConf(tag + "live translate " + (newState ? "on" : "off"),
            ImVec4(0.55f, 0.90f, 0.80f, 1.0f));
        return true;
    }
    else if (restLower == "color" || restLower == "colored" || restLower == "togglecolor") {
        bool newState;
        {
            std::lock_guard lock(g_settingsMtx);
            g_coloredChat = !g_coloredChat;
            newState = g_coloredChat;
        }
        SaveSettings();
        pushConf(tag + "colored chat " + (newState ? "on (&a)" : "off"),
            ImVec4(0.45f, 1.00f, 0.55f, 1.0f));
        return true;
    }
    else if (restLower == "nick" || restLower == "nickname" || restLower == "togglenick") {
        bool newState;
        {
            std::lock_guard lock(g_settingsMtx);
            g_useNickname = !g_useNickname;
            newState = g_useNickname;
        }
        SaveSettings();
        pushConf(tag + "nickname " + (newState ? "on (/nickname &cHYAI)" : "off"),
            ImVec4(1.00f, 0.45f, 0.45f, 1.0f));
        return true;
    }

    else if (restLower == "mute" || restLower == "afk" || restLower == "togglemute") {
        bool newState;
        {
            std::lock_guard lock(g_settingsMtx);
            g_botMuted = !g_botMuted;
            newState = g_botMuted;
        }
        SaveSettings();
        pushConf(tag + (newState ? "muted (AFK) — will not reply" : "unmuted — listening again"),
            ImVec4(1.00f, 0.75f, 0.40f, 1.0f));
        return true;
    }
    else if (restLower == "think" || restLower == "togglethink") {
        bool newState;
        {
            std::lock_guard lock(g_settingsMtx);
            g_thinkEnabled = !g_thinkEnabled;
            newState = g_thinkEnabled;
        }
        SaveSettings();
        pushConf(tag + "think mode " + (newState ? "on" : "off"),
            ImVec4(0.85f, 0.70f, 1.00f, 1.0f));
        return true;
    }
    else if (StartsWithCI(rest, "whitelist ")) {
        std::string name = rest.substr(10);
        name = NormalizeSpeaker(name);
        if (!name.empty()) {
            std::string lname = ToLower(name);
            {
                std::lock_guard lock(g_mtx);
                g_whitelist.insert(lname);
            }
            SaveWhitelist();
            pushConf(tag + "whitelisted " + name, ImVec4(0.5f, 1.0f, 0.7f, 1.0f));
            return true;
        }
    }
    else if (StartsWithCI(rest, "unwhitelist ")) {
        std::string name = rest.substr(12);
        name = NormalizeSpeaker(name);
        if (!name.empty()) {
            std::string lname = ToLower(name);
            bool removed = false;
            {
                std::lock_guard lock(g_mtx);
                removed = g_whitelist.erase(lname) > 0;
            }
            if (removed) SaveWhitelist();
            pushConf(removed ? (tag + "removed " + name + " from whitelist") : (tag + name + " was not on whitelist"),
                ImVec4(0.5f, 1.0f, 0.6f, 1.0f));
            return true;
        }
    }

    else if (StartsWithCI(rest, "sendlang ")) {
        std::string lang = ToLower(TrimCopy(rest.substr(9)));
        std::string finalLang;
        {
            std::lock_guard lock(g_settingsMtx);
            if (lang == "none" || lang == "off" || lang == "en" || lang == "as" || lang == "as-is" || lang == "asis")
                g_outgoingLang.clear();
            else
                g_outgoingLang = lang;
            finalLang = g_outgoingLang;
        }
        SaveSettings();
        pushConf(tag + "outgoing language " + (finalLang.empty() ? "as-is" : finalLang),
            ImVec4(0.55f, 0.90f, 0.80f, 1.0f));
        return true;
    }
    else if (StartsWithCI(rest, "echo ")) {
        std::string body = rest.substr(5);
        body = Extras::ExpandEchoPlaceholders(body);
        if (!body.empty()) {
            if (body.size() > HYTALE_CHAT_MAX) body.resize(HYTALE_CHAT_MAX);
            QueueSend({ body, body, "", "", "" });
            pushConf(tag + "echoed (" + std::to_string(body.size()) + " chars)",
                ImVec4(0.55f, 0.90f, 0.70f, 1.0f));
        }
        return true;
    }
    else if (restLower == "context" || restLower == "aicontext") {
        std::string dump = Extras::BuildAiContextDump();
        LogToFile(dump);
        {
            std::ofstream f("C:\\temp\\hyai_context_dump.txt", std::ios::trunc);
            if (f.is_open()) f << dump;
        }
        // Also show a short summary in overlay
        size_t memN = 0, chatN = 0;
        {
            std::lock_guard lock(g_mtx);
            memN = g_memory.size();
            chatN = g_globalChat.size();
        }
        pushConf(tag + "context dump written to C:\\temp\\hyai_context_dump.txt ("
            + std::to_string(dump.size()) + " chars, memory msgs=" + std::to_string(memN)
            + ", global=" + std::to_string(chatN) + ")",
            ImVec4(0.70f, 0.85f, 1.00f, 1.0f));
        return true;
    }
    else if (restLower == "reminders" || restLower == "listreminders") {
        std::string list = Extras::FormatReminderList("");
        // split into overlay lines
        pushConf(tag + list, ImVec4(0.85f, 0.90f, 0.55f, 1.0f));
        return true;
    }
    else if (StartsWithCI(rest, "cancelrem ")) {
        std::string idStr = TrimCopy(rest.substr(10));
        try {
            uint64_t id = (uint64_t)std::stoull(idStr);
            bool ok = Extras::CancelReminderById(id);
            pushConf(tag + (ok ? ("cancelled reminder #" + idStr) : ("no reminder #" + idStr)),
                ImVec4(0.90f, 0.75f, 0.45f, 1.0f));
        }
        catch (...) {
            pushConf(tag + "usage: cancelrem <id>", ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
        }
        return true;
    }
    else if (StartsWithCI(rest, "privkey ")) {
        std::string pass = TrimCopy(rest.substr(8));
        Extras::SetPrivatePassphrase(pass);
        bool ok = Extras::HasPrivateKey();
        std::string fp = Extras::PrivateKeyFingerprint();
        pushConf(tag + (ok
            ? ("private key set (AES-256-GCM). fp=" + fp +
                "  Friends need the exact same passphrase.")
            : "failed to derive private key"),
            ok ? ImVec4(0.45f, 0.95f, 0.60f, 1.0f) : ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
        LogToFile(std::string("privkey set ok=") + (ok ? "1" : "0") +
            " fp=" + fp + " pass_len=" + std::to_string(pass.size()));
        return true;
        }
    else if (restLower == "privkey" || restLower == "privclear") {
            Extras::ClearPrivateKey();
            g_privateMode = false;
            pushConf(tag + "private key cleared", ImVec4(1.0f, 0.7f, 0.4f, 1.0f));
            LogToFile("privkey cleared");
            return true;
            }
    else if (StartsWithCI(rest, "privsend ")) {
        // privsend <player> <msg>  -> /msg player ~jb1...
        // privsend public <msg>    -> public chat ~jb1...
        // privsend <msg>           -> uses UI target / via-msg defaults
        std::string args = TrimCopy(rest.substr(9));
        if (!Extras::HasPrivateKey()) {
            pushConf(tag + "set a key first: " + usedPrefix + " privkey <passphrase>",
                ImVec4(1.0f, 0.55f, 0.35f, 1.0f));
            return true;
        }
        if (args.empty()) {
            pushConf(tag + "usage: privsend <player> <msg> | privsend public <msg>",
                ImVec4(1.0f, 0.7f, 0.4f, 1.0f));
            return true;
        }
        std::string target;
        std::string msg;
        bool viaMsg = g_privateViaMsg;
        // split first token
        size_t sp = args.find(' ');
        std::string first = (sp == std::string::npos) ? args : args.substr(0, sp);
        std::string restMsg = (sp == std::string::npos) ? std::string() : TrimCopy(args.substr(sp + 1));
        std::string firstLower = ToLower(first);
        if (firstLower == "public" || firstLower == "all" || firstLower == "global") {
            viaMsg = false;
            target.clear();
            msg = restMsg;
        }
        else if (!restMsg.empty()) {
            // player + message
            target = first;
            msg = restMsg;
            viaMsg = true;
        }
        else {
            // only one token-group: treat entire args as message, use UI target
            msg = args;
            target = TrimCopy(g_privateTargetBuf);
            viaMsg = g_privateViaMsg && !target.empty();
        }
        if (msg.empty()) {
            pushConf(tag + "usage: privsend <player> <msg> | privsend public <msg>",
                ImVec4(1.0f, 0.7f, 0.4f, 1.0f));
            return true;
        }
        if (!QueuePrivateEncrypted(msg, target, viaMsg)) {
            pushConf(tag + "encrypt/send failed (too long or crypto error)",
                ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            return true;
        }
        pushConf(tag + (viaMsg && !target.empty()
            ? ("private /msg -> " + target)
            : "private public chat"),
            ImVec4(0.95f, 0.80f, 0.35f, 1.0f));
        return true;
    }

    pushConf(tag + "unknown command. Type: " + usedPrefix + " help", ImVec4(1.0f, 0.7f, 0.4f, 1.0f));
    return true;
}

// =============================================================================
// AI Trigger and Response handling
// =============================================================================
static size_t Utf8SafeEnd(const std::string& s, size_t start, size_t maxBytes) {
    if (start >= s.size()) return s.size();
    size_t end = (std::min)(start + maxBytes, s.size());
    if (end < s.size()) {
        while (end > start && ((unsigned char)s[end] & 0xC0) == 0x80)
            --end;
    }
    if (end < s.size()) {
        size_t spacePos = s.find_last_of(" \t", end);
        if (spacePos != std::string::npos && spacePos > start && spacePos + 40 >= end)
            end = spacePos;
    }
    if (end <= start)
        end = (std::min)(start + maxBytes, s.size());
    return end;
}

static std::string SanitizeGameChunk(std::string chunk, bool blockSlash = true) {
    chunk = TrimCopy(chunk);
    if (chunk.empty()) return chunk;
    if (blockSlash && (chunk[0] == '/' || chunk[0] == '!'))
        chunk.insert(chunk.begin(), '>');
    return chunk;
}

void QueueBotReply(const std::string& rawText) {
    std::string oneLine = TrimCopy(rawText);
    if (oneLine.empty()) {
        std::lock_guard lock(g_settingsMtx);
        oneLine = g_failMessage.empty() ? "sorry couldnt process that rn" : g_failMessage;
    }

    bool colored = false;
    bool useNick = false;
    bool blockSlash = true;
    int nickDelay = 400;
    std::string colorCode, tagPrefix, nickOn, nickOff, botName;
    {
        std::lock_guard lock(g_settingsMtx);
        colored = g_coloredChat;
        useNick = g_useNickname;
        blockSlash = g_blockLeadingSlash;
        colorCode = g_aiColorCode;
        tagPrefix = g_aiPrefix;
        nickOn = g_nicknameOn;
        nickOff = g_nicknameOff;
        botName = g_botName;
        nickDelay = g_nicknameDelayMs;
    }
    if (colorCode.empty()) colorCode = "&a";
    if (tagPrefix.empty()) tagPrefix = "HyAI > ";
    if (nickOn.empty()) nickOn = "/nickname &cHYAI";
    if (nickOff.empty()) nickOff = "/nickname off";
    if (botName.empty()) botName = "HyAI";
    if (nickDelay < 0) nickDelay = 0;
    if (nickDelay > 2000) nickDelay = 2000;

    const std::string colorPrefix = colored ? colorCode : "";
    const std::string effectiveTag = useNick ? "" : tagPrefix;
    const std::string gamePrefix = colorPrefix + effectiveTag;

    const size_t prefixLen = gamePrefix.size();
    size_t chunkBudget = MAX_CHAT_CHUNK;
    if (prefixLen >= HYTALE_CHAT_MAX)
        chunkBudget = 8;
    else if (prefixLen + chunkBudget > HYTALE_CHAT_MAX)
        chunkBudget = HYTALE_CHAT_MAX - prefixLen;
    if (chunkBudget > MAX_CHAT_CHUNK)
        chunkBudget = MAX_CHAT_CHUNK;
    if (chunkBudget < 8)
        chunkBudget = 8;

    std::vector<PendingSend> batch;

    size_t start = 0;
    while (start < oneLine.length()) {
        size_t end = Utf8SafeEnd(oneLine, start, chunkBudget);
        std::string chunk = SanitizeGameChunk(oneLine.substr(start, end - start), blockSlash);

        if (!chunk.empty()) {
            PendingSend ps;
            ps.gameText = gamePrefix + chunk;
            if (ps.gameText.size() > HYTALE_CHAT_MAX)
                ps.gameText.resize(HYTALE_CHAT_MAX);
            ps.typedText = useNick ? (ToUpper(botName) + ": " + chunk) : (effectiveTag + chunk);
            batch.push_back(std::move(ps));
        }

        start = end;
        while (start < oneLine.length() && oneLine[start] == ' ') ++start;
    }

    LogToFile("Sending " + std::to_string(batch.size()) + " chunk(s) of <=" +
        std::to_string(chunkBudget) + " chars colored=" +
        std::string(colored ? "1" : "0") + " nick=" + (useNick ? "1" : "0") +
        " nickDelay=" + std::to_string(nickDelay));

    std::lock_guard lock(g_mtx);
    if (useNick) {
        PendingSend nickOnPs;
        nickOnPs.gameText = nickOn;
        nickOnPs.silent = true;
        nickOnPs.delayBeforeMs = 0;
        g_pending.push_back(std::move(nickOnPs));
    }
    bool firstChunk = true;
    for (auto& ps : batch) {
        // Wait for the server to apply /nickname before the first spoken line.
        if (useNick && firstChunk) {
            ps.delayBeforeMs = nickDelay;
            firstChunk = false;
        }
        g_pending.push_back(std::move(ps));
    }
    if (useNick) {
        PendingSend nickOffPs;
        nickOffPs.gameText = nickOff;
        nickOffPs.silent = true;
        // Brief gap so the last line is attributed under the nick before turning it off.
        nickOffPs.delayBeforeMs = (std::max)(SEND_GAP_MS, nickDelay / 2);
        g_pending.push_back(std::move(nickOffPs));
    }
}

// Cancel in-flight AI HTTP + drop any pending chat sends (bot queue).
void CancelAI() {
    LogToFile("CancelAI invoked");
    AI::RequestCancel();
    AbortHttp();
    AiFlow::OnCancel();
    {
        std::lock_guard lock(g_mtx);
        if (!g_pending.empty()) {
            LogToFile(std::string("CancelAI cleared ") + std::to_string(g_pending.size()) + " pending send(s)");
            g_pending.clear();
        }
    }
    // Allow a new request soon; worker ClearFlag will also clear g_aiInProgress.
    g_aiInProgress.store(false);
}

void HandleAIRequest(const std::string& userPrompt) {
    // The queue, limits and review step live in aiflow.cpp now.
    AiFlow::Submit(userPrompt, "", AiFlow::Source::Wake);
}

// =============================================================================
// Message walker
// =============================================================================
struct MessageWalker {
    std::vector<ChatSeg> segs;
    std::set<void*> visitedObjects;
    int nodes = 0;

    void Walk(void* node, int depth, ImVec4 inheritedColor = ImVec4(0.92f, 0.92f, 0.92f, 1.0f)) {
        const int MAX_NODES = 250;
        const int MAX_DEPTH = 24;
        if (!node || depth > MAX_DEPTH || nodes >= MAX_NODES) return;
        if (!IsPlausiblePtr(node)) return;

        std::string directStr;
        if (ReadSystemString(node, directStr)) {
            if (!IsColor(directStr) && !IsUseless(directStr)) {
                AddTextSegments(segs, directStr, inheritedColor);
            }
            return;
        }

        if (!visitedObjects.insert(node).second) return;
        ++nodes;

        // Conservative array walk (Hytale chat often stores child segments as a short ptr table).
        // Gated by SafeRead/RegionReadable so junk lengths do not SEH-storm the game thread.
        {
            int32_t arrLen = 0;
            if (SafeRead((uint8_t*)node + 8, &arrLen, 4) && arrLen > 0 && arrLen <= 32) {
                void* firstElem = nullptr;
                if (SafeRead((uint8_t*)node + 0x10, &firstElem, 8) &&
                    (firstElem == nullptr || IsPlausiblePtr(firstElem)) &&
                    RegionReadable((uint8_t*)node + 0x10, (size_t)arrLen * 8)) {
                    bool any = false;
                    for (int i = 0; i < arrLen; ++i) {
                        void* elem = nullptr;
                        if (!SafeRead((uint8_t*)node + 0x10 + (size_t)i * 8, &elem, 8))
                            break;
                        if (!elem)
                            continue;
                        if (!IsPlausiblePtr(elem))
                            break;
                        Walk(elem, depth + 1, inheritedColor);
                        any = true;
                    }
                    if (any)
                        return;
                }
            }
        }

        ImVec4 nodeColor = inheritedColor;
        for (int off = 0x08; off <= 0x38; off += 8) {
            void* p = nullptr;
            if (SafeRead((uint8_t*)node + off, &p, 8) && p && IsPlausiblePtr(p)) {
                std::string s;
                if (ReadSystemString(p, s)) {
                    ImVec4 parsed;
                    if (ParseHytaleColor(s, parsed)) {
                        nodeColor = parsed;
                        break;
                    }
                }
            }
        }

        std::string rawText;
        void* rawTextPtr = nullptr;
        for (int off : { 0x08, 0x10 }) {
            void* p = nullptr;
            if (SafeRead((uint8_t*)node + off, &p, 8) && p && IsPlausiblePtr(p)) {
                std::string s;
                if (ReadSystemString(p, s)) {
                    if (!IsColor(s) && !IsUseless(s)) {
                        rawText = s;
                        rawTextPtr = p;
                        break;
                    }
                }
            }
        }

        if (!rawText.empty()) {
            AddTextSegments(segs, rawText, nodeColor);
        }

        for (int off = 0x08; off <= 0x40; off += 8) {
            void* p = nullptr;
            if (SafeRead((uint8_t*)node + off, &p, 8) && p && IsPlausiblePtr(p)) {
                if (p == rawTextPtr) continue;

                std::string s;
                if (ReadSystemString(p, s)) {
                    if (rawText.empty() && !IsColor(s) && !IsUseless(s)) {
                        AddTextSegments(segs, s, nodeColor);
                        rawText = s;
                    }
                    continue;
                }

                Walk(p, depth + 1, nodeColor);
            }
        }
    }
};

void ProcessMessage(void* message) {
    if (!message || g_shuttingDown.load())
        return;

    MessageWalker walker;
    walker.Walk(message, 0);
    if (walker.segs.empty())
        return;

    ChatLine line;
    for (const auto& s : walker.segs) {
        line.segs.push_back(s);
        line.plain += s.text;
    }

    std::string plainText = line.plain;
    if (ConsumeIfDuplicateMessage(message, plainText))
        return;

    std::string speaker;
    std::string messageBody = plainText;
    ExtractSpeakerAndBody(plainText, speaker, messageBody);

    line.speaker = speaker;
    line.body = messageBody;

    std::string speakerLower = ToLower(NormalizeSpeaker(speaker));

    bool locAllowSelfTrigger = false;
    bool locLiveTranslate = false;
    bool locMuted = false;
    bool locWhitelistMode = false;
    std::string locLocalName;
    std::string locWakeWord, locWakeWord2;
    {
        std::lock_guard lock(g_settingsMtx);
        locAllowSelfTrigger = g_allowSelfTrigger;
        locLiveTranslate = g_liveTranslate;
        locMuted = g_botMuted;
        locWhitelistMode = g_whitelistMode;
        locLocalName = g_localName;
        locWakeWord = g_wakeWord;
        locWakeWord2 = g_wakeWord2;
    }
    if (locWakeWord.empty()) locWakeWord = "hyai";

    const bool isFromLocal = IsLocalPlayerSpeaker(speaker, locLocalName);

    bool silentEcho = false;
    if (MatchRecentOutgoing(plainText, messageBody, silentEcho)) {
        return;
    }

    if (StartsWithCI(TrimCopy(messageBody), "/nickname"))
        return;

    // Auto replies (Settings > Auto replies). Only queued here; the rules run
    // on the UI thread so a slow pattern can never stall the game.
    if (!locMuted)
        AutoReply::Offer(TrimCopy(StripMcFormatting(plainText)), speaker, messageBody, isFromLocal,
            LooksLikeBotLine(speaker, messageBody, plainText));

    // =========================================================================
// Chat minigames (Word Value / Morse / Caesar / Count Letters / Pattern)
// =========================================================================
    {
        bool wvShow = false, wvAuto = false;
        bool mcShow = false, mcAuto = false;
        bool ccShow = false, ccAuto = false;
        bool clShow = false, clAuto = false;
        bool pnShow = false, pnAuto = false;
        {
            std::lock_guard lock(g_settingsMtx);
            wvShow = g_wordValueShow;  wvAuto = g_wordValueAuto;
            mcShow = g_morseShow;      mcAuto = g_morseAuto;
            ccShow = g_caesarShow;     ccAuto = g_caesarAuto;
            clShow = g_countLettersShow; clAuto = g_countLettersAuto;
            pnShow = g_patternShow;    pnAuto = g_patternAuto;
        }

        if (!locMuted && (wvShow || wvAuto || mcShow || mcAuto || ccShow || ccAuto ||
            clShow || clAuto || pnShow || pnAuto)) {
            std::string answer;
            const char* badge = nullptr;
            bool doShow = false;
            bool doAuto = false;

            // Word Value
            if (wvShow || wvAuto) {
                std::string word;
                int val = 0;
                if (TryParseWordValueGame(plainText, word, val) ||
                    TryParseWordValueGame(messageBody, word, val)) {
                    answer = std::to_string(val);
                    badge = "WV";
                    doShow = wvShow;
                    doAuto = wvAuto;
                }
            }

            // Morse
            if (answer.empty() && (mcShow || mcAuto)) {
                std::string decoded;
                if (TryParseMorseGame(plainText, decoded) ||
                    TryParseMorseGame(messageBody, decoded)) {
                    answer = decoded;
                    badge = "MC";
                    doShow = mcShow;
                    doAuto = mcAuto;
                }
            }

            // Caesar
            if (answer.empty() && (ccShow || ccAuto)) {
                std::string plain;
                if (TryParseCaesarGame(plainText, plain) ||
                    TryParseCaesarGame(messageBody, plain)) {
                    answer = plain;
                    badge = "CC";
                    doShow = ccShow;
                    doAuto = ccAuto;
                }
            }

            // Count the Letters
            if (answer.empty() && (clShow || clAuto)) {
                int n = TryParseCountLetters(plainText);
                if (n < 0) n = TryParseCountLetters(messageBody);
                if (n >= 0) {
                    answer = std::to_string(n);
                    badge = "CL";
                    doShow = clShow;
                    doAuto = clAuto;
                }
            }

            // Pattern
            if (answer.empty() && (pnShow || pnAuto)) {
                std::string next;
                if (TryParsePatternGame(plainText, next) ||
                    TryParsePatternGame(messageBody, next)) {
                    answer = next;
                    badge = "PN";
                    doShow = pnShow;
                    doAuto = pnAuto;
                }
            }

            if (!answer.empty()) {
                if (doShow) {
                    line.txStatus = TxStatus::Done;
                    line.txText = answer;
                    line.txFrom = badge;
                }

                PushLine(std::move(line));

                if (doAuto) {
                    long long nowMs = NowSteadyMs();
                    const long long COOLDOWN_MS = 2500;
                    if ((nowMs - g_lastAutoReactMs) >= COOLDOWN_MS) {
                        g_lastAutoReactMs = nowMs;
                        QueueSend({ answer, answer, "", "", "" });
                        LogToFile(std::string("Minigame auto-reply [") + badge + "] " + answer);
                    }
                }
                return;   // do not fall through to AI / translate
            }
        }
    }

    // Private encrypted channel: decrypt ~jb1.... from public chat OR /msg / whisper lines
    if (g_privateDecryptDisplay && Extras::HasPrivateKey()) {
        std::string plain;
        bool ok = TryDecryptPrivateInBody(messageBody, plain);
        // Fallback: some whisper formats put the ciphertext only in the full plain line
        if (!ok && !plainText.empty() && plainText != messageBody) {
            ok = TryDecryptPrivateInBody(plainText, plain);
        }
        if (ok) {
            line.plain = (speaker.empty() ? std::string() : (speaker + ": ")) + "[private] " + plain;
            line.body = plain;
            line.segs.clear();
            line.segs.push_back({ line.plain, ImVec4(0.95f, 0.82f, 0.35f, 1.0f) });
            // Show decrypted in overlay only; do not wake AI on private traffic
            PushLine(std::move(line));
            return;
        }
    }

    // Owner commands: fuzzy local-name match so a slightly off "Local player name"
    // setting does not let "<wakeword> mute" fall through to the AI wake path.
    // Prefix is the configured wake word / bot name (not hardcoded "josebot").
    {
        const std::string bodyTrim = TrimCopy(messageBody);
        std::string ownerRest, ownerPrefix;
        const bool looksLikeOwnerCmd = MatchOwnerCmdPrefix(bodyTrim, ownerRest, ownerPrefix);
        if (looksLikeOwnerCmd && !isFromLocal) {
            LogToFile("Owner-like cmd but speaker mismatch: speaker=[" + speaker +
                "] local=[" + locLocalName + "] prefix=[" + ownerPrefix +
                "] body=" + bodyTrim.substr(0, 80));
        }
        if (isFromLocal || (looksLikeOwnerCmd && IsLocalPlayerSpeaker(speaker, locLocalName))) {
            if (HandleOwnerCommand(plainText, true)) {
                PushLine(std::move(line));
                return;
            }
        }
    }

    const bool isBotLine = LooksLikeBotLine(speaker, messageBody, plainText);

    const bool ignored = IsUserIgnored(speaker, plainText);
    if (ignored) {
        PushLine(std::move(line));
        return;
    }

    // Whitelist mode: only respond to listed players
    if (locWhitelistMode && !isBotLine && !isFromLocal) {
        if (!IsUserWhitelisted(speaker)) {
            PushLine(std::move(line));
            if (!IsReservedSpeakerName(speakerLower) && !LooksLikeAnnouncementBody(messageBody))
                RememberSeenPlayer(speaker);
            RememberGlobalChat(speaker, messageBody, plainText, false);
            return;
        }
    }


    if (!isBotLine)
        RememberGlobalChat(speaker, messageBody, plainText, false);
    else
        RememberGlobalChat("HyAI", messageBody, plainText, true);

    if (speaker.empty() || IsReservedSpeakerName(speakerLower) || LooksLikeAnnouncementBody(messageBody)) {
        uint64_t lId = PushLine(std::move(line));

        if (locLiveTranslate && lId && !messageBody.empty()) {
            if (ShouldAutoTranslate(messageBody)) {
                RequestIncomingTranslate(lId, messageBody, false);
            }
        }
        return;
    }

    if (!isBotLine && !isFromLocal && !LooksLikeAnnouncementBody(messageBody))
        RememberSeenPlayer(speaker);

    if (isBotLine || IsBotIdentity(speaker)) {
        PushLine(std::move(line));
        return;
    }

    if (!locAllowSelfTrigger && isFromLocal) {
        PushLine(std::move(line));
        return;
    }


    // Mute / AFK: log but never trigger AI
    if (locMuted) {
        PushLine(std::move(line));
        return;
    }

    const std::string hay = ToLower(WakeHay(speaker, messageBody, plainText));
    const std::string wakeLower = ToLower(locWakeWord);
    const std::string wake2Lower = ToLower(locWakeWord2);
    bool wakeHit = !hay.empty() && HasWholeWord(hay, wakeLower);
    if (!wakeHit && !wake2Lower.empty())
        wakeHit = HasWholeWord(hay, wake2Lower);
    if (wakeHit) {
        bool atStart;
        { std::lock_guard lock(g_settingsMtx); atStart = g_prefs.aiWakeAtStart; }
        if (atStart && !AiText::WakeAtStart(messageBody.empty() ? plainText : messageBody, { wakeLower, wake2Lower }))
            wakeHit = false;
    }
    if (wakeHit && !IsBotIdentity(speaker)) {
        std::string prompt = plainText;
        if (!speaker.empty())
            prompt = speaker + ": " + messageBody;

        bool dup = false;
        {
            std::lock_guard wlock(g_wakeMtx);
            auto now = std::chrono::steady_clock::now();
            if (prompt == g_lastWakePrompt &&
                now - g_lastWakeTime < std::chrono::seconds(2))
                dup = true;
            else {
                g_lastWakePrompt = prompt;
                g_lastWakeTime = now;
            }
        }

        if (!dup && !prompt.empty()) {
            LogToFile("Wake word detected from speaker=[" + speaker + "] prompt=" + prompt);
            AiFlow::Submit(prompt, speaker, AiFlow::Source::Wake);
        }
    }

    uint64_t lineId = PushLine(std::move(line));

    if (locLiveTranslate && lineId && !messageBody.empty()) {
        if (ShouldAutoTranslate(messageBody)) {
            RequestIncomingTranslate(lineId, messageBody, false);
        }
    }
}

// =============================================================================
// Send path
// =============================================================================
// Pure SEH wrapper — must not construct any C++ objects (MSVC C2712).
static bool CallSendSafeSeh(void* self, void* msg) {
    __try {
        oSendMessage(self, msg);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CallSendSafe(void* self, void* msg) {
    if (CallSendSafeSeh(self, msg))
        return true;
    LogToFile("CallSendSafe fault — disabling send path");
    g_sendDisabled.store(true);
    return false;
}

void InvalidateChatSession(const char* why) {
    g_pChat.store(nullptr);
    g_chatReady.store(false);
    {
        std::lock_guard lock(g_mtx);
        if (!g_pending.empty()) {
            LogToFile(std::string("Cleared ") + std::to_string(g_pending.size()) +
                " pending send(s) (" + (why ? why : "invalidate") + ")");
            g_pending.clear();
        }
    }
}

bool ChatObjectAlive(void* chat) {
    if (!chat || !IsPlausiblePtr(chat))
        return false;
    uint64_t currentVT = 0;
    const uint64_t expected = g_chatVT.load();
    if (!expected)
        return false;
    if (!SafeRead(chat, &currentVT, 8) || currentVT != expected)
        return false;
    return true;
}

// Ready = player has typed once on this chat instance (same rule as overlay "READY").
bool ChatSessionStable() {
    return g_chatReady.load() && ChatObjectAlive(g_pChat.load());
}


void RememberOutgoing(const PendingSend& ps) {
    RecentOut ro;
    ro.gameText = ps.gameText;
    ro.typedText = ps.typedText;
    std::string key = ps.typedText.empty() ? ps.gameText : ps.typedText;
    ro.bodyKey = ToLower(TrimCopy(key));
    ro.silent = ps.silent;
    ro.whenMs = NowSteadyMs();
    std::lock_guard lock(g_mtx);
    g_recentOut.push_back(std::move(ro));
    while (g_recentOut.size() > 32)
        g_recentOut.pop_front();
}

// Everything a bot send involves except the call itself: picks the next message
// that is due, builds its string and records it as sent. The caller makes the
// call, because HOW it is made is the difference between safe and a crash - the
// frame hook has the game call it in place of one frame's work, so none of our
// code is on the stack underneath (see hookthunk.hpp). Game thread only.
bool TakePendingSend(void*& outChat, void*& outStr) {
    outChat = nullptr;
    outStr = nullptr;
    if (g_shuttingDown.load()) return false;
    if (g_sendDisabled.load()) return false;
    if (!g_chatReady.load()) return false;

    if (g_hWnd && !IsWindow(g_hWnd)) {
        InvalidateChatSession("hwnd dead");
        return false;
    }

    void* chat = g_pChat.load();
    if (!chat || !oSendMessage || !g_stringVT.load()) return false;

    const DWORD sendTid = g_gameThread.load();
    if (!sendTid || GetCurrentThreadId() != sendTid)
        return false;

    if (!ChatObjectAlive(chat)) {
        InvalidateChatSession("chat object dead/mismatched");
        return false;
    }

    const long long now = NowSteadyMs();

    PendingSend ps;
    {
        std::lock_guard lock(g_mtx);
        if (g_pending.empty()) return false;

        int needGap = SEND_GAP_MS;
        if (g_pending.front().delayBeforeMs > needGap)
            needGap = g_pending.front().delayBeforeMs;

        // Leave the item in the queue; the next frame retries once the delay elapses.
        if (now - g_lastSendMs.load() < needGap)
            return false;

        ps = std::move(g_pending.front());
        g_pending.erase(g_pending.begin());
    }

    void* str = CreateSystemString(ps.gameText);
    if (!str) {
        LogToFile("CreateSystemString failed, dropping: " + ps.gameText);
        return false;
    }

    if (!ChatObjectAlive(chat) || g_pChat.load() != chat || !g_chatReady.load()) {
        InvalidateChatSession("chat died before send");
        return false;
    }

    RememberOutgoing(ps);
    g_lastSendMs.store(now);

    // Recorded before the call rather than after it succeeds: once the game's
    // function is running there is no "after" for us to run in.
    if (!ps.silent) {
        ChatLine line;
        line.plain = ps.typedText.empty() ? ps.gameText : ps.typedText;
        line.segs.push_back({ line.plain, ImVec4(0.45f, 0.85f, 1.0f, 1.0f) });
        line.outgoing = true;
        if (!ps.txTo.empty()) {
            line.txStatus = TxStatus::Done;
            line.txText = ps.gameText;
            line.txFrom = ps.txFrom;
            line.txTo = ps.txTo;
        }
        PushLine(std::move(line));
    }

    outChat = chat;
    outStr = str;
    return true;
}

// Sends directly, with our frame under the game's SendMessage. Only for builds
// where the frame hook cannot carry the send (see FlushPendingFallback in
// hooks.cpp); a collection landing inside the call still takes the game down.
void ProcessPending() {
    if (g_inProcessPending.exchange(true)) return;
    struct Clear {
        ~Clear() { g_inProcessPending.store(false); }
    } clear;

    void* chat = nullptr;
    void* str = nullptr;
    if (!TakePendingSend(chat, str)) return;

    if (!CallSendSafe(chat, str)) {
        InvalidateChatSession("CallSendSafe fault");
        LogToFile("Send failed (disabled)");
    }
}


// Test chat + tools playground
// =============================================================================
// Private Test Chat + Tools playground (ImGui only, never hits game server)
// =============================================================================
static void TestAiWorkerMain(const std::string& userPrompt) {
    WorkerGuard alive;
    struct ClearFlag {
        ~ClearFlag() { g_testAiInProgress = false; }
    } clearFlag;

    try {
        std::string aiResponse;
        bool ok = AI::GetResponse(userPrompt, aiResponse);

        if (g_shuttingDown.load())
            return;

        std::string oneLine;
        if (ok && !aiResponse.empty()) {
            bool space = false;
            for (char c : aiResponse) {
                if (c == '\n' || c == '\r' || c == '\t') {
                    if (!space && !oneLine.empty()) { oneLine += ' '; space = true; }
                }
                else {
                    oneLine += c;
                    space = (c == ' ');
                }
            }
            while (!oneLine.empty() && oneLine.back() == ' ') oneLine.pop_back();
        }

        std::string status;
        {
            std::lock_guard lock(g_mtx);
            if (!oneLine.empty()) {
                g_testChat.push_back({ "assistant", oneLine });
                g_testStatus = "ok — " + std::to_string(oneLine.size()) + " chars";
            }
            else {
                g_testChat.push_back({ "assistant", "[AI failed or empty response]" });
                g_testStatus = "failed / empty";
            }
            g_testScroll = true;
            status = g_testStatus;   // the overlay writes it too, so not read unlocked below
        }
        LogToFile("TestChat response: " + status);
    }
    catch (...) {
        std::lock_guard lock(g_mtx);
        g_testChat.push_back({ "assistant", "[exception in AI worker]" });
        g_testStatus = "exception";
        g_testScroll = true;
        LogToFile("TestChat AI threw");
    }
}

void HandleTestAIRequest(const std::string& userPrompt) {
    if (g_shuttingDown.load() || userPrompt.empty()) return;
    if (g_testAiInProgress.exchange(true)) {
        std::lock_guard lock(g_mtx);
        g_testStatus = "already thinking…";
        return;
    }
    {
        std::lock_guard lock(g_mtx);
        g_testChat.push_back({ "user", userPrompt });
        g_testStatus = "thinking…";
        g_testScroll = true;
    }
    try {
        StartWorker([userPrompt]() { TestAiWorkerMain(userPrompt); });
    }
    catch (...) {
        g_testAiInProgress = false;
        std::lock_guard lock(g_mtx);
        g_testStatus = "failed to start worker";
    }
}

void RunToolTest() {
    AI::EnsureKnowledgeLoaded();
    std::string result;
    switch (g_toolKind) {
    case 0:
        result = Knowledge::ToolLookupMob(g_toolQuery);
        break;
    case 1:
        result = Knowledge::ToolSearchMobs(
            g_toolQuery, g_toolDrops, g_toolTrait,
            g_toolMinHealth, g_toolMaxHealth, g_toolLimit);
        break;
    case 2:
        result = Knowledge::ToolLookupItem(g_toolQuery);
        break;
    case 3:
        result = Knowledge::ToolSearchItems(
            g_toolQuery, g_toolQuality, g_toolCategory, g_toolLimit);
        break;
    case 4:
        result = Extras::ToolGetTime(g_toolQuery);
        break;
    case 5:
        result = Extras::ToolSetReminder(
            g_toolRemUser[0] ? g_toolRemUser : g_toolQuery,
            g_toolInSeconds,
            g_toolRemMsg[0] ? g_toolRemMsg : "reminder");
        break;
    case 6:
        result = Extras::ToolListReminders(g_toolRemUser[0] ? g_toolRemUser : g_toolQuery);
        break;
    case 7:
        result = Extras::ToolCancelReminder((uint64_t)g_toolRemId);
        break;
    case 8:
        result = Extras::ToolWebSearch(g_toolQuery, g_toolFresh[0] ? g_toolFresh : "");
        break;
    default:
        result = "Unknown tool index";
        break;
    }
    g_toolResult = result;
    g_toolScroll = true;
    LogToFile("ToolTest kind=" + std::to_string(g_toolKind) + " result_len=" + std::to_string(result.size()));
}