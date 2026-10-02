#include "pch.h"
#include "chatview.hpp"
#include "common.hpp"
#include "settings.hpp"
#include "shared.hpp"
#include "translate.hpp"
#include "ui.hpp"

#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

// =============================================================================
// UTF-8
// =============================================================================
// Mirrors how ImGui walks text, so byte offsets measured here land on the same
// glyphs ImGui draws: a bad lead byte is one U+FFFD, and anything past the BMP
// is U+FFFD too because this build's ImWchar is 16-bit.
uint32_t DecodeUtf8(const char* s, size_t i, size_t end, uint32_t& cp) {
    const unsigned char c = (unsigned char)s[i];
    auto cont = [&](size_t k) { return i + k < end && ((unsigned char)s[i + k] & 0xC0) == 0x80; };
    if (c < 0x80) { cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && cont(1)) {
        cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        cp = ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) | ((unsigned char)s[i + 2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        cp = IM_UNICODE_CODEPOINT_INVALID;
        return 4;
    }
    cp = IM_UNICODE_CODEPOINT_INVALID;
    return 1;
}

bool IsWordCp(uint32_t cp) {
    return cp == '_' || cp == '\'' || (cp < 0x80 ? (isalnum((int)cp) != 0) : cp != 0xA0 && cp != 0x3000);
}

// Chat text is shown on one visual line per paragraph; control characters would
// render as boxes (or, for \n, break the row maths).
std::string CleanText(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\t') o += ' ';
        else if (c >= 0x20 && c != 0x7F) o += (char)c;
    }
    return o;
}

// =============================================================================
// Colours
// =============================================================================
float Luma(const ImVec4& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

struct LegacyColor { char code; ImVec4 col; };
const LegacyColor kLegacy[16] = {
    { '0', { 0.00f, 0.00f, 0.00f, 1 } }, { '1', { 0.00f, 0.00f, 0.67f, 1 } },
    { '2', { 0.00f, 0.67f, 0.00f, 1 } }, { '3', { 0.00f, 0.67f, 0.67f, 1 } },
    { '4', { 0.67f, 0.00f, 0.00f, 1 } }, { '5', { 0.67f, 0.00f, 0.67f, 1 } },
    { '6', { 1.00f, 0.67f, 0.00f, 1 } }, { '7', { 0.67f, 0.67f, 0.67f, 1 } },
    { '8', { 0.33f, 0.33f, 0.33f, 1 } }, { '9', { 0.33f, 0.33f, 1.00f, 1 } },
    { 'a', { 0.33f, 1.00f, 0.33f, 1 } }, { 'b', { 0.33f, 1.00f, 1.00f, 1 } },
    { 'c', { 1.00f, 0.33f, 0.33f, 1 } }, { 'd', { 1.00f, 0.33f, 1.00f, 1 } },
    { 'e', { 1.00f, 1.00f, 0.33f, 1 } }, { 'f', { 1.00f, 1.00f, 1.00f, 1 } },
};

// "Redmean" colour distance: cheap, and much closer to what the eye sees than
// plain RGB distance - a luminance-weighted one snapped light blue to grey.
float ColorDistance(const ImVec4& a, const ImVec4& b) {
    const float rm = (a.x + b.x) * 0.5f;
    const float dr = a.x - b.x, dg = a.y - b.y, db = a.z - b.z;
    return (2.0f + rm) * dr * dr + 4.0f * dg * dg + (3.0f - rm) * db * db;
}

char NearestLegacy(const ImVec4& c) {
    // A tinted colour goes to the nearest tinted code and a grey to the
    // nearest grey. Across the whole palette a pale green or pale blue is
    // numerically closest to &7 grey, which loses the one thing it said.
    // Tinted colours are matched on hue first, then brightness.
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, s, v);
    const bool tinted = s >= 0.18f && v >= 0.15f;
    char best = 'f';
    float bestD = 1e9f;
    for (const auto& l : kLegacy) {
        const bool grey = l.code == '0' || l.code == '7' || l.code == '8' || l.code == 'f';
        if (grey == tinted) continue;
        float d;
        if (tinted) {
            float lh, ls, lv;
            ImGui::ColorConvertRGBtoHSV(l.col.x, l.col.y, l.col.z, lh, ls, lv);
            float dh = fabsf(h - lh);
            dh = (std::min)(dh, 1.0f - dh);
            d = dh * dh * 16.0f + (v - lv) * (v - lv);
        }
        else d = ColorDistance(c, l.col);
        if (d < bestD) { bestD = d; best = l.code; }
    }
    return best;
}

std::string Hex6(const ImVec4& c) {
    auto b = [](float v) { return (unsigned)((std::max)(0.0f, (std::min)(1.0f, v)) * 255.0f + 0.5f); };
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X%02X%02X", b(c.x), b(c.y), b(c.z));
    return buf;
}

// Discord's ```ansi blocks know eight foreground colours.
int NearestAnsi(const ImVec4& c) {
    struct A { int code; ImVec4 col; };
    static const A kAnsi[] = {
        { 30, { 0.31f, 0.33f, 0.36f, 1 } }, { 31, { 0.86f, 0.20f, 0.18f, 1 } },
        { 32, { 0.52f, 0.60f, 0.00f, 1 } }, { 33, { 0.71f, 0.54f, 0.00f, 1 } },
        { 34, { 0.15f, 0.55f, 0.82f, 1 } }, { 35, { 0.83f, 0.21f, 0.51f, 1 } },
        { 36, { 0.16f, 0.63f, 0.60f, 1 } }, { 37, { 1.00f, 1.00f, 1.00f, 1 } },
    };
    int best = 37;
    float bestD = 1e9f;
    for (const auto& a : kAnsi) {
        const float d = ColorDistance(c, a.col);
        if (d < bestD) { bestD = d; best = a.code; }
    }
    return best;
}

// For formats that land on white paper (RTF in WordPad/Word): light game
// colours are darkened until they read on white, hue kept.
ImVec4 ForWhitePaper(ImVec4 c) {
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, s, v);
    if (Luma(c) > 0.55f) {
        v = (std::min)(v, 0.55f);
        if (s < 0.1f) v = 0.25f; // white / light grey -> dark grey text
    }
    ImVec4 o(0, 0, 0, 1);
    ImGui::ColorConvertHSVtoRGB(h, s, v, o.x, o.y, o.z);
    return o;
}

// =============================================================================
// Copy formats
// =============================================================================
struct Piece {
    std::string text;
    ImVec4 src;        // colour as the game sent it
    ImVec4 shown;      // colour as drawn in the overlay
    bool lineStart;    // starts a new line in the output
};

std::string JoinPlain(const std::vector<Piece>& pieces) {
    std::string out;
    for (const Piece& p : pieces) {
        if (p.lineStart && !out.empty()) out += '\n';
        out += p.text;
    }
    return out;
}

// fmt: 0 "&c" nearest, 1 "&#RRGGBB", 2 "§c" nearest, 3 "<#RRGGBB>"
std::string JoinCodes(const std::vector<Piece>& pieces, int fmt) {
    std::string out;
    std::string last;
    for (const Piece& p : pieces) {
        if (p.lineStart && !out.empty()) { out += '\n'; last.clear(); }
        std::string code;
        switch (fmt) {
        case 1: code = "&#" + Hex6(p.src); break;
        case 2: code = std::string("\xC2\xA7") + NearestLegacy(p.src); break;
        case 3: code = "<#" + Hex6(p.src) + ">"; break;
        default: code = std::string("&") + NearestLegacy(p.src); break;
        }
        if (code != last) { out += code; last = code; }
        out += p.text;
    }
    return out;
}

std::string HtmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

std::string JoinHtml(const std::vector<Piece>& pieces, bool darkBg) {
    std::string out = darkBg
        ? "<div style=\"background:#15171f;color:#ebebeb;padding:8px 10px;border-radius:6px;"
          "font-family:'Segoe UI',sans-serif;font-size:10.5pt;line-height:1.45\">"
        : "<div style=\"font-family:'Segoe UI',sans-serif;font-size:10.5pt;line-height:1.45\">";
    bool first = true;
    for (const Piece& p : pieces) {
        if (p.lineStart && !first) out += "<br>";
        first = false;
        const ImVec4 col = darkBg ? p.shown : ForWhitePaper(p.shown);
        out += "<span style=\"color:#" + Hex6(col) + "\">" + HtmlEscape(p.text) + "</span>";
    }
    out += "</div>";
    return out;
}

std::string JoinRtf(const std::vector<Piece>& pieces) {
    // Colour table first; RTF refers to colours by index.
    std::vector<std::string> table;
    std::vector<int> index;
    for (const Piece& p : pieces) {
        const std::string h = Hex6(ForWhitePaper(p.shown));
        auto it = std::find(table.begin(), table.end(), h);
        if (it == table.end()) { table.push_back(h); index.push_back((int)table.size()); }
        else index.push_back((int)(it - table.begin()) + 1);
    }
    std::string out = "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Segoe UI;}}{\\colortbl ;";
    for (const std::string& h : table) {
        unsigned r = 0, g = 0, b = 0;
        sscanf_s(h.c_str(), "%02x%02x%02x", &r, &g, &b);
        out += "\\red" + std::to_string(r) + "\\green" + std::to_string(g) + "\\blue" + std::to_string(b) + ";";
    }
    out += "}\\f0\\fs21 ";
    for (size_t i = 0; i < pieces.size(); ++i) {
        const Piece& p = pieces[i];
        if (p.lineStart && i > 0) out += "\\par\n";
        out += "\\cf" + std::to_string(index[i]) + " ";
        // Everything outside 7-bit ASCII goes out as \uN? with a '?' fallback.
        const std::wstring w = Utf8ToWide(p.text);
        for (wchar_t ch : w) {
            if (ch == L'\\' || ch == L'{' || ch == L'}') { out += '\\'; out += (char)ch; }
            else if (ch < 0x80) out += (char)ch;
            else out += "\\u" + std::to_string((int)(short)ch) + "?";
        }
    }
    out += "}";
    return out;
}

std::string JoinAnsi(const std::vector<Piece>& pieces) {
    std::string out = "```ansi\n";
    int last = -1;
    bool first = true;
    for (const Piece& p : pieces) {
        if (p.lineStart && !first) { out += '\n'; last = -1; }
        first = false;
        const int code = NearestAnsi(p.shown);
        if (code != last) {
            out += "\x1b[0;" + std::to_string(code) + "m";
            last = code;
        }
        // A ``` inside the text would end the code block early.
        std::string t = p.text;
        size_t q = 0;
        while ((q = t.find("```", q)) != std::string::npos) { t.replace(q, 3, "`\xE2\x80\x8B``"); q += 4; }
        out += t;
    }
    out += "\x1b[0m\n```";
    return out;
}

// CF_HTML wants a header with byte offsets into the UTF-8 payload.
std::string WrapCfHtml(const std::string& fragment) {
    const std::string pre = "<html><body><!--StartFragment-->";
    const std::string post = "<!--EndFragment--></body></html>";
    const char* hdrFmt =
        "Version:0.9\r\nStartHTML:%010u\r\nEndHTML:%010u\r\nStartFragment:%010u\r\nEndFragment:%010u\r\n";
    char probe[160];
    const int hdrLen = snprintf(probe, sizeof(probe), hdrFmt, 0u, 0u, 0u, 0u);
    const unsigned startHtml = (unsigned)hdrLen;
    const unsigned startFrag = startHtml + (unsigned)pre.size();
    const unsigned endFrag = startFrag + (unsigned)fragment.size();
    const unsigned endHtml = endFrag + (unsigned)post.size();
    char hdr[160];
    snprintf(hdr, sizeof(hdr), hdrFmt, startHtml, endHtml, startFrag, endFrag);
    return std::string(hdr) + pre + fragment + post;
}

bool PutClipboardData(UINT fmt, const void* data, size_t bytes) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return false;
    void* p = GlobalLock(h);
    if (!p) { GlobalFree(h); return false; }
    memcpy(p, data, bytes);
    GlobalUnlock(h);
    if (!SetClipboardData(fmt, h)) { GlobalFree(h); return false; }
    return true;
}

// =============================================================================
// Mentions and visibility
// =============================================================================
struct MentionCache {
    uint32_t version = 0;
    std::string myName;
    std::vector<std::string> words;
    bool whole = true;
};

MentionCache& Mentions() {
    static MentionCache m;
    const uint32_t v = g_prefsVersion.load();
    if (m.version == v) return m;
    m.version = v;
    m.words.clear();
    m.whole = g_prefs.highlightWholeWord;
    std::string name;
    {
        std::lock_guard lock(g_settingsMtx);
        name = g_localName;
    }
    name = ToLower(NormalizeSpeaker(name));
    // "Player" is the placeholder name before you set yours; highlighting every
    // line with the word "player" in it would be noise, not a mention.
    m.myName = (g_prefs.highlightMyName && name.size() >= 3 && name != "player") ? name : std::string();
    if (!m.myName.empty()) m.words.push_back(m.myName);

    std::string cur;
    auto flush = [&]() {
        cur = ToLower(TrimCopy(cur));
        if (!cur.empty()) m.words.push_back(cur);
        cur.clear();
    };
    for (char c : g_prefs.highlightWords) {
        if (c == ',' || c == '\n' || c == ';') flush();
        else cur += c;
    }
    flush();
    return m;
}

struct HideCache {
    uint32_t version = 0;
    std::vector<std::string> patterns;
};

HideCache& Hides() {
    static HideCache h;
    const uint32_t v = g_prefsVersion.load();
    if (h.version == v) return h;
    h.version = v;
    h.patterns.clear();
    std::string cur;
    for (char c : g_prefs.hidePatterns + "\n") {
        if (c == '\n') {
            cur = ToLower(TrimCopy(cur));
            if (!cur.empty()) h.patterns.push_back(cur);
            cur.clear();
        }
        else cur += c;
    }
    return h;
}

// =============================================================================
// Alerts
// =============================================================================
void PlayMentionSound() {
    static long long last = 0;
    const long long now = NowSteadyMs();
    if (now - last < 1500) return;
    last = now;
    switch (g_prefs.mentionSound) {
    case 1: PlaySoundW(L"SystemNotification", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT); break;
    case 2: MessageBeep(MB_OK); break;
    case 3:
        if (!g_prefs.mentionSoundPath.empty())
            PlaySoundW(Utf8ToWide(g_prefs.mentionSoundPath).c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        break;
    default: break;
    }
}

void FlashOverlay() {
    HWND h = g_hwndOverlay;
    if (!h) return;
    FLASHWINFO fi{ sizeof(fi) };
    fi.hwnd = h;
    fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    fi.uCount = 3;
    FlashWindowEx(&fi);
}

int g_unreadMentions = 0;
int g_chatTabFrame = -100;      // last frame the Chat tab was drawn
bool g_focusInputRequested = false;

} // namespace

// =============================================================================
// Public helpers
// =============================================================================
namespace ChatUi {

bool IsMention(const ChatLine& line) {
    if (line.outgoing || line.local) return false;
    const MentionCache& m = Mentions();
    if (m.words.empty()) return false;
    std::string me;
    {
        std::lock_guard lock(g_settingsMtx);
        me = g_localName;
    }
    if (!line.speaker.empty() && IsLocalPlayerSpeaker(line.speaker, me)) return false;

    const std::string speaker = ToLower(NormalizeSpeaker(line.speaker));
    const std::string hay = ToLower(line.body.empty() ? line.plain : line.body);
    for (const std::string& w : m.words) {
        // A highlighted name also highlights everything that player says.
        if (!speaker.empty() && speaker == w && w != m.myName) return true;
        if (m.whole ? HasWholeWord(hay, w) : hay.find(w) != std::string::npos) return true;
    }
    return false;
}

int LineVisibility(const ChatLine& line, const std::set<std::string>& ignoredLower) {
    if (line.outgoing || line.local) return 0;
    const HideCache& h = Hides();
    if (!h.patterns.empty()) {
        const std::string lower = ToLower(line.plain);
        for (const std::string& p : h.patterns)
            if (lower.find(p) != std::string::npos) return 2;
    }
    if (g_prefs.ignoredDisplay != 0 && !line.speaker.empty() && !ignoredLower.empty()) {
        if (ignoredLower.count(ToLower(NormalizeSpeaker(line.speaker))))
            return g_prefs.ignoredDisplay == 2 ? 2 : 1;
    }
    return 0;
}

ImVec4 DisplayColor(const ImVec4& in) {
    if (!g_prefs.gameColors) return g_prefs.colText;
    ImVec4 c = in;
    c.w = 1.0f;
    const float floor = g_prefs.minBrightness;
    const float l = Luma(c);
    if (floor > 0.0f && l < floor) {
        // Blend towards white just enough to reach the floor; keeps the hue of
        // dark blue/red instead of washing everything to grey.
        const float t = (floor - l) / (std::max)(0.001f, 1.0f - l);
        c.x += (1.0f - c.x) * t;
        c.y += (1.0f - c.y) * t;
        c.z += (1.0f - c.z) * t;
    }
    return c;
}

bool SetClipboard(const std::string& plainUtf8, const std::string& htmlFragment, const std::string& rtf) {
    if (!OpenClipboard(g_hwndOverlay)) return false;
    EmptyClipboard();

    // Windows text on the clipboard uses CRLF line ends.
    std::string crlf;
    crlf.reserve(plainUtf8.size() + 16);
    for (char c : plainUtf8) {
        if (c == '\n') crlf += "\r\n";
        else if (c != '\r') crlf += c;
    }
    const std::wstring w = Utf8ToWide(crlf);
    bool ok = PutClipboardData(CF_UNICODETEXT, w.c_str(), (w.size() + 1) * sizeof(wchar_t));

    if (!htmlFragment.empty()) {
        const std::string cf = WrapCfHtml(htmlFragment);
        PutClipboardData(RegisterClipboardFormatW(L"HTML Format"), cf.c_str(), cf.size() + 1);
    }
    if (!rtf.empty())
        PutClipboardData(RegisterClipboardFormatW(L"Rich Text Format"), rtf.c_str(), rtf.size() + 1);

    CloseClipboard();
    return ok;
}

void FocusInput() { g_focusInputRequested = true; }

} // namespace ChatUi

namespace {

// =============================================================================
// One message, ready to draw
// =============================================================================
enum SpanKind : uint8_t { kTime, kBody, kBadge, kTrans };
enum ChipKind : uint8_t { ChipNone, ChipTranslate, ChipRetry, ChipPending };

struct Span {
    uint32_t b, e;
    ImVec4 src;     // game colour, for colour-code copies
    ImVec4 shown;   // drawn colour
    uint8_t kind;
};

struct Row {
    uint32_t b, e;  // byte range in Entry::text
    float x0, y;    // entry-local
};

struct Entry {
    uint64_t id = 0;
    uint32_t rev = 0;
    uint32_t builtVersion = 0;
    std::string text;
    std::string lower;              // lazily filled for search
    std::vector<Span> spans;
    uint32_t bodyB = 0, bodyE = 0, txB = 0, txE = 0;
    std::string speaker, body, badgeTip;
    bool outgoing = false, local = false, bot = false, mention = false;
    int vis = 0;                    // 0 show, 1 dim, 2 hide
    uint8_t chip = ChipNone;

    // Layout cache, valid for (layW, layFont).
    float layW = -1.0f, layFont = 0.0f;
    std::vector<Row> rows;
    float height = 0.0f;
    ImVec2 chipPos{ 0, 0 };
    float chipW = 0.0f;
};

struct Metrics {
    ImFont* font = nullptr;
    ImFontBaked* baked = nullptr;
    float size = 16.0f;
    float scale = 1.0f;
    float lineH = 17.0f;

    float Adv(uint32_t cp) const { return baked->GetCharAdvance((ImWchar)cp) * scale; }
    float Measure(const std::string& t, uint32_t a, uint32_t b) const {
        float w = 0.0f;
        const char* s = t.data();
        for (size_t i = a; i < b;) {
            uint32_t cp;
            i += DecodeUtf8(s, i, b, cp);
            w += Adv(cp);
        }
        return w;
    }
};

const char* ChipLabel(uint8_t chip) {
    switch (chip) {
    case ChipTranslate: return "tr";
    case ChipRetry:     return "retry tr";
    case ChipPending:   return "translating...";
    default:            return "";
    }
}

void BuildEntry(Entry& e, const ChatLine& L) {
    e = Entry{};
    e.id = L.id;
    e.rev = L.rev;
    e.builtVersion = g_prefsVersion.load();
    e.outgoing = L.outgoing;
    e.local = L.local;
    e.speaker = L.speaker;
    e.body = L.body;
    e.bot = !L.outgoing && LooksLikeBotLine(L.speaker, L.body, L.plain);
    e.mention = ChatUi::IsMention(L);

    auto add = [&](const std::string& s, const ImVec4& src, const ImVec4& shown, uint8_t kind) {
        const std::string c = CleanText(s);
        if (c.empty()) return;
        const uint32_t b = (uint32_t)e.text.size();
        e.text += c;
        // Merge with the previous span when nothing visible changes - fewer
        // draw calls, and colour-code copies don't repeat the same code.
        if (!e.spans.empty()) {
            Span& p = e.spans.back();
            if (p.e == b && p.kind == kind && memcmp(&p.src, &src, sizeof(ImVec4)) == 0 &&
                memcmp(&p.shown, &shown, sizeof(ImVec4)) == 0) {
                p.e = (uint32_t)e.text.size();
                return;
            }
        }
        e.spans.push_back({ b, (uint32_t)e.text.size(), src, shown, kind });
    };

    if (g_showTimestamps) {
        std::string ts = FormatChatTime(L.unixMs);
        if (ts.empty()) ts = L.timeStr;
        if (!ts.empty()) add(ts + " ", g_prefs.colTimestamp, g_prefs.colTimestamp, kTime);
    }

    e.bodyB = (uint32_t)e.text.size();
    if (L.local) {
        for (const auto& s : L.segs) add(s.text, s.color, s.color, kBody);
        if (L.segs.empty()) add(L.plain, g_prefs.colOutgoing, g_prefs.colOutgoing, kBody);
    }
    else if (L.outgoing && !(g_showOwnFormatted && !L.segs.empty())) {
        add(L.plain, g_prefs.colOutgoing, g_prefs.colOutgoing, kBody);
    }
    else if (!L.segs.empty()) {
        for (const auto& s : L.segs) add(s.text, s.color, ChatUi::DisplayColor(s.color), kBody);
    }
    else {
        add(L.plain, g_prefs.colText, g_prefs.colText, kBody);
    }
    e.bodyE = (uint32_t)e.text.size();

    if (L.txStatus == TxStatus::Done && !L.txText.empty()) {
        e.text += '\n';
        e.txB = (uint32_t)e.text.size();
        const bool outTx = L.outgoing && !L.txTo.empty();
        const std::string code = outTx ? L.txTo : L.txFrom;
        const ImVec4 bc = LangColor(code);
        add("[" + std::string(outTx ? "\xE2\x86\x92" : "") + LangBadge(code) + "] ", bc, bc, kBadge);
        add(L.txText, g_prefs.colTranslation, g_prefs.colTranslation, kTrans);
        e.txE = (uint32_t)e.text.size();
        const char* name = LangName(code);
        e.badgeTip = outTx ? std::string("Sent as ") + (name ? name : code.c_str())
                           : std::string("Detected: ") + (name ? name : code.c_str());
    }
    else if (!L.outgoing && !L.local && !e.bot && !L.body.empty()) {
        if (L.txStatus == TxStatus::Pending) e.chip = ChipPending;
        else if (L.txStatus == TxStatus::Failed) e.chip = ChipRetry;
        else if (L.txStatus == TxStatus::None && g_prefs.showTranslateChip) e.chip = ChipTranslate;
    }
}

// Greedy word wrap of text[b, e) into rows. The first row starts at firstX,
// the rest at contX. Breaks after spaces where possible, mid-word otherwise.
void WrapParagraph(Entry& en, const Metrics& m, uint32_t b, uint32_t e,
    float firstX, float contX, float W, float& y) {
    const std::string& t = en.text;
    if (b >= e) {
        en.rows.push_back({ b, e, firstX, y });
        y += m.lineH;
        return;
    }
    uint32_t rowStart = b;
    float rowX0 = firstX;
    float x = firstX;
    uint32_t lastBreak = UINT32_MAX;
    uint32_t i = b;
    while (i < e) {
        uint32_t cp;
        const uint32_t n = DecodeUtf8(t.data(), i, e, cp);
        const float adv = m.Adv(cp);
        // A space may overhang the edge; breaking before it would start the
        // next row with a blank.
        if (x + adv > W && i > rowStart && cp != ' ') {
            const uint32_t cut = (lastBreak != UINT32_MAX && lastBreak > rowStart) ? lastBreak : i;
            en.rows.push_back({ rowStart, cut, rowX0, y });
            y += m.lineH;
            rowStart = cut;
            rowX0 = contX;
            x = contX + m.Measure(t, cut, i);
            lastBreak = UINT32_MAX;
            continue; // place this character again on the new row
        }
        x += adv;
        i += n;
        if (cp == ' ') lastBreak = i;
    }
    en.rows.push_back({ rowStart, e, rowX0, y });
    y += m.lineH;
}

void LayoutEntry(Entry& en, const Metrics& m, float W) {
    en.rows.clear();
    en.layW = W;
    en.layFont = m.size;

    float timeW = 0.0f;
    if (!en.spans.empty() && en.spans[0].kind == kTime)
        timeW = m.Measure(en.text, en.spans[0].b, en.spans[0].e);
    const float hang = g_prefs.hangingIndent ? (std::min)(timeW, W * 0.4f) : 0.0f;
    const float txIndent = (std::min)(m.size * 1.2f + hang, W * 0.5f);

    float y = 0.0f;
    const uint32_t bodyEnd = en.txB ? en.txB - 1 : (uint32_t)en.text.size();
    WrapParagraph(en, m, 0, bodyEnd, 0.0f, hang, W, y);

    en.chipW = 0.0f;
    if (en.chip != ChipNone) {
        const std::string label = ChipLabel(en.chip);
        en.chipW = m.Measure(label, 0, (uint32_t)label.size()) * 0.85f + 10.0f;
        const Row& last = en.rows.back();
        const float endX = last.x0 + m.Measure(en.text, last.b, last.e);
        if (endX + 6.0f + en.chipW <= W)
            en.chipPos = ImVec2(endX + 6.0f, last.y);
        else {
            en.chipPos = ImVec2(hang, y);
            y += m.lineH;
        }
    }

    if (en.txB)
        WrapParagraph(en, m, en.txB, en.txE, txIndent, txIndent, W, y);

    en.height = y;
}

// =============================================================================
// The view
// =============================================================================
class LogView {
public:
    explicit LogView(const char* id) : id_(id) {}

    void SetStatic(std::vector<ChatLine> lines) {
        live_ = false;
        lines_ = std::move(lines);
        linesChanged_ = true;
    }

    void Draw(ImVec2 size, const std::string& filter, bool hideNonMatching, bool mentionsOnly);
    void ScrollToBottom() { scrollReq_ = true; }
    size_t VisibleCount() const { return vis_.size(); }

private:
    struct Pos { uint64_t id = 0; uint32_t off = 0; };
    static bool Less(const Pos& a, const Pos& b) { return a.id != b.id ? a.id < b.id : a.off < b.off; }

    struct Hit {
        int vis = -1;       // index into vis_
        uint32_t off = 0;
        bool onChip = false;
        bool onText = false;
    };

    void Sync();
    void RebuildVisible(const std::string& filter, bool hideNonMatching, bool mentionsOnly);
    void Relayout(const Metrics& m, float W);
    Hit HitTest(const Metrics& m, ImVec2 local) const;
    bool HasSelection() const { return a_.id && (a_.id != b_.id || a_.off != b_.off); }
    void SelectionRange(Pos& lo, Pos& hi) const {
        lo = a_;
        hi = b_;
        if (Less(hi, lo)) std::swap(lo, hi);
    }
    void SelectAll();
    std::vector<Piece> PiecesForRange(Pos lo, Pos hi) const;
    std::vector<Piece> PiecesForEntry(const Entry& e, bool wholeLine) const;
    std::vector<Piece> PiecesForLog() const;
    void CopyPieces(const std::vector<Piece>& pieces, int how);
    void Toast(const std::string& s) { toast_ = s; toastUntil_ = NowSteadyMs() + 1600; }
    void ContextMenu();
    void ExportLog();

    const char* id_;
    bool live_ = true;
    std::vector<ChatLine> lines_;
    uint64_t seen_ = 0;
    bool linesChanged_ = true;
    std::vector<Entry> entries_;

    // What the visible list was built from.
    std::string filterKey_;
    bool filterHide_ = true, mentionsOnly_ = false;
    std::set<std::string> ignored_;
    uint32_t visVersion_ = 0;
    bool visDirty_ = true;
    std::string needle_, from_;
    std::vector<int> vis_;
    std::vector<float> tops_;
    float totalH_ = 0.0f;
    float layW_ = -1.0f, layFont_ = 0.0f, gap_ = -1.0f;
    bool layoutDirty_ = true;

    Pos a_, b_;
    bool dragging_ = false;

    bool stick_ = true;
    bool first_ = true;
    bool scrollReq_ = false;
    uint64_t bottomSeenId_ = 0;
    int unread_ = 0;

    uint64_t menuId_ = 0;
    std::string toast_;
    long long toastUntil_ = 0;
};

void LogView::Sync() {
    bool changed = linesChanged_;
    linesChanged_ = false;
    if (live_) changed |= SnapshotChatLog(lines_, seen_);

    const uint32_t pv = g_prefsVersion.load();
    bool styleChanged = false;
    for (const Entry& e : entries_) {
        if (e.builtVersion != pv) { styleChanged = true; break; }
    }
    if (!changed && !styleChanged) return;

    // Same merge as the snapshot: entries and lines are both in id order.
    std::vector<Entry> next;
    next.reserve(lines_.size());
    size_t j = 0;
    for (const ChatLine& L : lines_) {
        while (j < entries_.size() && entries_[j].id < L.id) ++j;
        if (j < entries_.size() && entries_[j].id == L.id && entries_[j].rev == L.rev &&
            entries_[j].builtVersion == pv) {
            next.push_back(std::move(entries_[j++]));
        }
        else {
            Entry e;
            BuildEntry(e, L);
            next.push_back(std::move(e));
        }
    }
    entries_.swap(next);
    visDirty_ = true;
    layoutDirty_ = true;
}

void LogView::RebuildVisible(const std::string& filter, bool hideNonMatching, bool mentionsOnly) {
    std::set<std::string> ignored;
    if (g_prefs.ignoredDisplay != 0) {
        std::lock_guard lock(g_mtx);
        ignored = g_ignored;
    }
    const uint32_t pv = g_prefsVersion.load();
    if (!visDirty_ && filter == filterKey_ && hideNonMatching == filterHide_ &&
        mentionsOnly == mentionsOnly_ && ignored == ignored_ && pv == visVersion_)
        return;

    filterKey_ = filter;
    filterHide_ = hideNonMatching;
    mentionsOnly_ = mentionsOnly;
    visVersion_ = pv;
    visDirty_ = false;
    layoutDirty_ = true;
    ignored_.swap(ignored);

    // "from:name" narrows by speaker; the rest is a case-insensitive search.
    needle_.clear();
    from_.clear();
    {
        std::string rest;
        std::istringstream ss(filter);
        std::string tok;
        while (ss >> tok) {
            if (StartsWithCI(tok, "from:")) from_ = ToLower(tok.substr(5));
            else {
                if (!rest.empty()) rest += ' ';
                rest += tok;
            }
        }
        needle_ = ToLower(rest);
    }

    vis_.clear();
    for (size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        {
            ChatLine probe;
            probe.plain = e.text.substr(e.bodyB, e.bodyE - e.bodyB);
            probe.speaker = e.speaker;
            probe.outgoing = e.outgoing;
            probe.local = e.local;
            e.vis = ChatUi::LineVisibility(probe, ignored_);
        }
        if (e.vis == 2) continue;
        if (mentionsOnly && !e.mention) continue;
        if (!from_.empty() && ToLower(NormalizeSpeaker(e.speaker)).find(from_) == std::string::npos) continue;
        if (!needle_.empty() && hideNonMatching) {
            if (e.lower.empty()) e.lower = ToLower(e.text);
            if (e.lower.find(needle_) == std::string::npos) continue;
        }
        vis_.push_back((int)i);
    }
}

void LogView::Relayout(const Metrics& m, float W) {
    const float gap = (std::max)(0.0f, g_chatLineGap);
    if (W != layW_ || m.size != layFont_ || gap != gap_) {
        layW_ = W;
        layFont_ = m.size;
        gap_ = gap;
        layoutDirty_ = true;
    }
    if (!layoutDirty_) return;
    layoutDirty_ = false;

    tops_.resize(vis_.size());
    float y = 3.0f;
    for (size_t k = 0; k < vis_.size(); ++k) {
        Entry& e = entries_[(size_t)vis_[k]];
        if (e.layW != W || e.layFont != m.size) LayoutEntry(e, m, W);
        tops_[k] = y;
        y += e.height + gap + 2.0f;
    }
    totalH_ = y + 3.0f;
}

LogView::Hit LogView::HitTest(const Metrics& m, ImVec2 local) const {
    Hit h;
    if (vis_.empty()) return h;
    // Last entry whose top is at or above the point.
    auto it = std::upper_bound(tops_.begin(), tops_.end(), local.y);
    const int k = (int)(it - tops_.begin()) - 1;
    if (k < 0) { h.vis = 0; h.off = 0; return h; }
    h.vis = k;
    const Entry& e = entries_[(size_t)vis_[(size_t)k]];
    const float ly = local.y - tops_[(size_t)k];

    if (e.chip != ChipNone && ly >= e.chipPos.y && ly < e.chipPos.y + m.lineH &&
        local.x >= e.chipPos.x && local.x < e.chipPos.x + e.chipW)
        h.onChip = true;

    if (e.rows.empty()) return h;
    if (ly >= e.height) { h.off = (uint32_t)e.text.size(); return h; }

    size_t r = 0;
    while (r + 1 < e.rows.size() && ly >= e.rows[r + 1].y) ++r;
    const Row& row = e.rows[r];
    if (local.x <= row.x0) { h.off = row.b; return h; }
    float x = row.x0;
    for (uint32_t i = row.b; i < row.e;) {
        uint32_t cp;
        const uint32_t n = DecodeUtf8(e.text.data(), i, row.e, cp);
        const float adv = m.Adv(cp);
        if (local.x < x + adv * 0.5f) { h.off = i; h.onText = true; return h; }
        x += adv;
        i += n;
    }
    h.onText = local.x <= x;
    h.off = row.e;
    return h;
}

void LogView::SelectAll() {
    if (vis_.empty()) return;
    a_ = { entries_[(size_t)vis_.front()].id, 0 };
    const Entry& last = entries_[(size_t)vis_.back()];
    b_ = { last.id, (uint32_t)last.text.size() };
}

std::vector<Piece> LogView::PiecesForRange(Pos lo, Pos hi) const {
    std::vector<Piece> out;
    for (int vi : vis_) {
        const Entry& e = entries_[(size_t)vi];
        if (e.id < lo.id || e.id > hi.id) continue;
        const uint32_t size = (uint32_t)e.text.size();
        const uint32_t sb = e.id == lo.id ? (std::min)(lo.off, size) : 0;
        const uint32_t se = e.id == hi.id ? (std::min)(hi.off, size) : size;
        if (sb >= se) continue;
        bool firstOfEntry = true;
        uint32_t prevEnd = sb;
        for (const Span& s : e.spans) {
            const uint32_t a = (std::max)(sb, s.b), z = (std::min)(se, s.e);
            if (a >= z) continue;
            const size_t nl = e.text.find('\n', prevEnd);
            const bool brk = firstOfEntry || (nl != std::string::npos && nl < a);
            out.push_back({ e.text.substr(a, z - a), s.src, s.shown, brk });
            firstOfEntry = false;
            prevEnd = z;
        }
    }
    return out;
}

std::vector<Piece> LogView::PiecesForEntry(const Entry& e, bool wholeLine) const {
    std::vector<Piece> out;
    bool first = true;
    for (const Span& s : e.spans) {
        if (!wholeLine && s.kind != kBody) continue;
        if (s.kind == kTime && !g_prefs.copyTimestamps) continue;
        if ((s.kind == kBadge || s.kind == kTrans) && !g_prefs.copyTranslations) continue;
        const bool brk = first || s.kind == kBadge;
        out.push_back({ e.text.substr(s.b, s.e - s.b), s.src, s.shown, brk });
        first = false;
    }
    return out;
}

std::vector<Piece> LogView::PiecesForLog() const {
    std::vector<Piece> all;
    for (int vi : vis_) {
        auto p = PiecesForEntry(entries_[(size_t)vi], true);
        all.insert(all.end(), p.begin(), p.end());
    }
    return all;
}

// how: 0 plain, 1 plain + rich, 2 colour codes (configured style), 3 Discord,
// 4 HTML source, 10..13 one specific code style.
void LogView::CopyPieces(const std::vector<Piece>& pieces, int how) {
    if (pieces.empty()) return;
    bool ok = false;
    const char* what = "Copied";
    switch (how) {
    case 1:
        ok = ChatUi::SetClipboard(JoinPlain(pieces), JoinHtml(pieces, g_prefs.richDarkBackground), JoinRtf(pieces));
        what = "Copied with colours";
        break;
    case 2:
        ok = ChatUi::SetClipboard(JoinCodes(pieces, g_prefs.copyCodeFormat));
        what = "Copied with colour codes";
        break;
    case 3:
        ok = ChatUi::SetClipboard(JoinAnsi(pieces));
        what = "Copied for Discord";
        break;
    case 4:
        ok = ChatUi::SetClipboard(JoinHtml(pieces, g_prefs.richDarkBackground));
        what = "Copied HTML";
        break;
    case 10: case 11: case 12: case 13:
        ok = ChatUi::SetClipboard(JoinCodes(pieces, how - 10));
        what = "Copied with colour codes";
        break;
    default:
        ok = ChatUi::SetClipboard(JoinPlain(pieces));
        break;
    }
    int lines = 0;
    for (const Piece& p : pieces) if (p.lineStart) ++lines;
    Toast(ok ? std::string(what) + (lines > 1 ? " (" + std::to_string(lines) + " lines)" : "")
             : std::string("Couldn't open the clipboard"));
}

void LogView::ExportLog() {
    wchar_t file[MAX_PATH] = L"hyai-chat.html";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwndOverlay;
    ofn.lpstrFilter = L"Web page, keeps colours (*.html)\0*.html\0Plain text (*.txt)\0*.txt\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"html";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;

    const std::vector<Piece> all = PiecesForLog();
    std::string content;
    if (ofn.nFilterIndex == 1) {
        content = "<!doctype html><html><head><meta charset=\"utf-8\"><title>HyAI chat</title></head>"
                  "<body style=\"background:#0f1117;margin:24px\">" + JoinHtml(all, true) + "</body></html>";
    }
    else {
        content = "\xEF\xBB\xBF" + JoinPlain(all); // BOM so Notepad reads it as UTF-8
    }
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) { Toast("Couldn't write the file"); return; }
    f << content;
    Toast("Exported " + std::to_string(vis_.size()) + " lines");
}

void LogView::ContextMenu() {
    if (!ImGui::BeginPopup("##chatctx")) return;

    const Entry* t = nullptr;
    for (const Entry& e : entries_) if (e.id == menuId_) { t = &e; break; }
    const bool sel = HasSelection();

    std::vector<Piece> subject;
    if (sel) { Pos lo, hi; SelectionRange(lo, hi); subject = PiecesForRange(lo, hi); }
    else if (t) subject = PiecesForEntry(*t, true);

    if (ImGui::MenuItem("Copy", "Ctrl+C", false, sel)) CopyPieces(subject, g_prefs.ctrlCMode);
    if (ImGui::BeginMenu(sel ? "Copy selection as" : "Copy line as", !subject.empty())) {
        if (ImGui::MenuItem("Plain text")) CopyPieces(subject, 0);
        if (ImGui::MenuItem("Rich text (keeps colours in Word, Docs, email)")) CopyPieces(subject, 1);
        ImGui::Separator();
        if (ImGui::MenuItem("Colour codes  &c")) CopyPieces(subject, 10);
        if (ImGui::MenuItem("Colour codes  &#RRGGBB")) CopyPieces(subject, 11);
        if (ImGui::MenuItem("Colour codes  \xC2\xA7" "c")) CopyPieces(subject, 12);
        if (ImGui::MenuItem("Colour tags  <#RRGGBB>")) CopyPieces(subject, 13);
        ImGui::Separator();
        if (ImGui::MenuItem("Discord (coloured code block)")) CopyPieces(subject, 3);
        if (ImGui::MenuItem("HTML source")) CopyPieces(subject, 4);
        ImGui::EndMenu();
    }

    if (t) {
        ImGui::Separator();
        if (ImGui::MenuItem("Copy message")) CopyPieces(PiecesForEntry(*t, false), 0);
        if (ImGui::MenuItem("Copy line")) CopyPieces(PiecesForEntry(*t, true), 0);
        if (t->txB && ImGui::MenuItem("Copy translation")) {
            ChatUi::SetClipboard(t->text.substr(t->txB, t->txE - t->txB));
            Toast("Copied");
        }

        const std::string name = NormalizeSpeaker(t->speaker);
        if (!name.empty() && !t->outgoing && !t->local) {
            if (ImGui::MenuItem(("Copy name \"" + name + "\"").c_str())) {
                ChatUi::SetClipboard(name);
                Toast("Copied");
            }

            ImGui::Separator();
            if (live_ && ImGui::MenuItem(("Mention " + name).c_str())) {
                std::string cur = g_input;
                if (!cur.empty() && cur.back() != ' ') cur += ' ';
                cur += name + " ";
                strncpy_s(g_input, cur.c_str(), _TRUNCATE);
                ChatUi::FocusInput();
            }
            if (live_ && ImGui::MenuItem(("Whisper " + name).c_str())) {
                std::string tpl = g_prefs.whisperTemplate.empty() ? "/msg {name} " : g_prefs.whisperTemplate;
                const size_t p = tpl.find("{name}");
                if (p != std::string::npos) tpl.replace(p, 6, name);
                strncpy_s(g_input, tpl.c_str(), _TRUNCATE);
                ChatUi::FocusInput();
            }
            if (live_ && !t->body.empty()) {
                if (ImGui::MenuItem(t->txB ? "Translate again" : "Translate"))
                    RequestIncomingTranslate(t->id, t->body, true);
            }
            if (live_ && ImGui::MenuItem(("Show only " + name).c_str()))
                strncpy_s(g_chatFilter, ("from:" + name).c_str(), _TRUNCATE);

            // Highlighting a player puts their name on the highlight list.
            const std::string lname = ToLower(name);
            bool highlighted = false;
            for (const std::string& w : Mentions().words) if (w == lname) { highlighted = true; break; }
            if (ImGui::MenuItem(highlighted ? ("Stop highlighting " + name).c_str() : ("Highlight " + name).c_str())) {
                std::lock_guard lock(g_settingsMtx);
                if (highlighted) {
                    std::string out, cur;
                    for (char c : g_prefs.highlightWords + "\n") {
                        if (c == ',' || c == '\n' || c == ';') {
                            const std::string w = TrimCopy(cur);
                            if (!w.empty() && ToLower(w) != lname) {
                                if (!out.empty()) out += ", ";
                                out += w;
                            }
                            cur.clear();
                        }
                        else cur += c;
                    }
                    g_prefs.highlightWords = out;
                }
                else {
                    std::string words = TrimCopy(g_prefs.highlightWords);
                    if (!words.empty()) words += ", ";
                    g_prefs.highlightWords = words + name;
                }
                MarkPrefsChanged();
            }

            if (live_) {
                bool isIgnored = false, isWl = false;
                {
                    std::lock_guard lock(g_mtx);
                    isIgnored = g_ignored.count(lname) > 0;
                    isWl = g_whitelist.count(lname) > 0;
                }
                if (ImGui::MenuItem(isIgnored ? ("Unignore " + name).c_str() : ("Ignore " + name + " (AI won't answer)").c_str())) {
                    {
                        std::lock_guard lock(g_mtx);
                        if (isIgnored) g_ignored.erase(lname);
                        else g_ignored.insert(lname);
                    }
                    SaveIgnored();
                }
                if (ImGui::MenuItem(isWl ? ("Remove " + name + " from whitelist").c_str() : ("Whitelist " + name).c_str())) {
                    {
                        std::lock_guard lock(g_mtx);
                        if (isWl) g_whitelist.erase(lname);
                        else g_whitelist.insert(lname);
                    }
                    SaveWhitelist();
                }
            }
        }
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Select all", "Ctrl+A", false, !vis_.empty())) SelectAll();
    if (ImGui::MenuItem("Clear selection", "Esc", false, sel)) a_ = b_ = {};
    if (ImGui::MenuItem("Copy whole log", nullptr, false, !vis_.empty())) {
        ChatUi::SetClipboard(JoinPlain(PiecesForLog()));
        Toast("Copied " + std::to_string(vis_.size()) + " lines");
    }
    if (live_) {
        if (ImGui::MenuItem("Export log...", nullptr, false, !vis_.empty())) ExportLog();
        if (ImGui::MenuItem("Clear log")) {
            ClearChatLog();
            a_ = b_ = {};
        }
    }
    ImGui::EndPopup();
}

void LogView::Draw(ImVec2 size, const std::string& filter, bool hideNonMatching, bool mentionsOnly) {
    ImGui::PushFont(nullptr, g_prefs.chatFontSize > 0.0f ? g_prefs.chatFontSize : 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::BeginChild(id_, size, ImGuiChildFlags_Borders,
        ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::EndChild();
        ImGui::PopFont();
        return;
    }

    Metrics m;
    m.font = ImGui::GetFont();
    m.size = ImGui::GetFontSize();
    m.baked = ImGui::GetFontBaked();
    m.scale = (m.baked && m.baked->Size > 0.0f) ? m.size / m.baked->Size : 1.0f;
    m.lineH = (float)(int)(m.size * 1.18f + 0.5f);

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 winPos = ImGui::GetWindowPos();
    const float viewW = ImGui::GetContentRegionAvail().x;
    const float viewH = ImGui::GetWindowHeight();
    const float gutter = g_prefs.accentBars ? 12.0f : 6.0f;
    const float textW = (std::max)(40.0f, viewW - gutter - 6.0f);

    Sync();
    RebuildVisible(filter, hideNonMatching, mentionsOnly);
    Relayout(m, textW);

    // ---- scroll state, from last frame's layout ----
    const float scrollY = ImGui::GetScrollY();
    const float scrollMax = ImGui::GetScrollMaxY();
    if (!dragging_)
        stick_ = first_ || scrollMax <= 0.0f || scrollY >= scrollMax - 2.0f;
    const uint64_t newestId = vis_.empty() ? 0 : entries_[(size_t)vis_.back()].id;
    if (stick_) bottomSeenId_ = newestId;
    unread_ = 0;
    if (!stick_)
        for (auto it = vis_.rbegin(); it != vis_.rend() && entries_[(size_t)*it].id > bottomSeenId_; ++it) ++unread_;

    const ImVec2 origin = ImGui::GetCursorScreenPos();   // content (0,0), scroll applied
    const ImVec2 textOrigin(origin.x + gutter, origin.y);

    Pos lo, hi;
    const bool sel = HasSelection();
    if (sel) SelectionRange(lo, hi);

    // ---- draw what's on screen ----
    const float visTop = scrollY - m.lineH;
    const float visBot = scrollY + viewH + m.lineH;
    int firstK = (int)(std::upper_bound(tops_.begin(), tops_.end(), visTop) - tops_.begin()) - 1;
    if (firstK < 0) firstK = 0;

    struct TipRegion { ImVec2 a, b; std::string tip; };
    std::vector<TipRegion> tips;
    const float textDy = (m.lineH - m.size) * 0.5f;

    for (int k = firstK; k < (int)vis_.size() && tops_[(size_t)k] < visBot; ++k) {
        Entry& e = entries_[(size_t)vis_[(size_t)k]];
        const float top = origin.y + tops_[(size_t)k];
        const float alpha = e.vis == 1 ? 0.45f : 1.0f;
        auto col = [&](ImVec4 c) { c.w *= alpha; return U32(c); };

        if (g_prefs.zebra && (k & 1))
            dl->AddRectFilled(ImVec2(winPos.x, top - 1), ImVec2(winPos.x + viewW, top + e.height + 1), U32(g_prefs.colZebra));
        if (e.mention)
            dl->AddRectFilled(ImVec2(winPos.x, top - 1), ImVec2(winPos.x + viewW, top + e.height + 1), U32(g_prefs.colMentionBg));

        // Selection and search hits sit behind the text.
        uint32_t sb = 0, se = 0;
        if (sel && e.id >= lo.id && e.id <= hi.id) {
            sb = e.id == lo.id ? lo.off : 0;
            se = e.id == hi.id ? hi.off : (uint32_t)e.text.size() + 1;
        }
        std::vector<std::pair<uint32_t, uint32_t>> hits;
        if (!needle_.empty()) {
            if (e.lower.empty()) e.lower = ToLower(e.text);
            for (size_t p = e.lower.find(needle_); p != std::string::npos; p = e.lower.find(needle_, p + needle_.size()))
                hits.push_back({ (uint32_t)p, (uint32_t)(p + needle_.size()) });
        }
        for (const Row& r : e.rows) {
            const float ry = top + r.y;
            // Only a row that ends a paragraph shows the selection running on
            // past its last character; a soft wrap continues on the next row.
            const bool paraEnd = r.e >= e.text.size() || e.text[r.e] == '\n';
            auto band = [&](uint32_t a, uint32_t z, ImU32 c, bool extendPastEnd) {
                const bool extend = extendPastEnd && paraEnd && z > r.e && a <= r.e;
                const uint32_t ra = (std::max)(a, r.b), rz = (std::min)(z, r.e);
                if (ra > rz || (ra == rz && !extend)) return;
                const float x1 = textOrigin.x + r.x0 + m.Measure(e.text, r.b, ra);
                float x2 = x1 + m.Measure(e.text, ra, rz);
                if (extend) x2 += m.size * 0.3f;
                dl->AddRectFilled(ImVec2(x1, ry), ImVec2(x2, ry + m.lineH), c, 2.0f);
            };
            if (se > sb) band(sb, se, U32(g_prefs.colSelection), true);
            for (const auto& hr : hits) band(hr.first, hr.second, U32(g_prefs.colSearchHit), false);
        }

        // Text, span by span within each row.
        for (const Row& r : e.rows) {
            float x = textOrigin.x + r.x0;
            const float ry = top + r.y;
            for (const Span& s : e.spans) {
                const uint32_t a = (std::max)(r.b, s.b), z = (std::min)(r.e, s.e);
                if (a >= z) continue;
                const float w = m.Measure(e.text, a, z);
                if (s.kind == kBadge) {
                    ImVec4 bg = s.shown;
                    bg.w = 0.16f * alpha;
                    dl->AddRectFilled(ImVec2(x - 2, ry + 1), ImVec2(x + w - m.Adv(' ') + 2, ry + m.lineH - 1), U32(bg), 3.0f);
                    if (!e.badgeTip.empty())
                        tips.push_back({ ImVec2(x, ry), ImVec2(x + w, ry + m.lineH), e.badgeTip });
                }
                dl->AddText(m.font, m.size, ImVec2(x, ry + textDy), col(s.shown), e.text.data() + a, e.text.data() + z);
                x += w;
            }
        }

        if (e.chip != ChipNone) {
            const char* label = ChipLabel(e.chip);
            const ImVec2 cmin(textOrigin.x + e.chipPos.x, top + e.chipPos.y + 1);
            const ImVec2 cmax(cmin.x + e.chipW, cmin.y + m.lineH - 2);
            const bool clickable = e.chip != ChipPending;
            const bool hot = clickable && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(cmin, cmax);
            ImVec4 cc = e.chip == ChipRetry ? ImVec4(1.0f, 0.55f, 0.45f, 1.0f) : ImVec4(0.55f, 0.70f, 0.95f, 1.0f);
            ImVec4 cbg = cc;
            cbg.w = hot ? 0.30f : 0.12f;
            dl->AddRectFilled(cmin, cmax, U32(cbg), 4.0f);
            cc.w = clickable ? 0.9f : 0.6f;
            const float cs = m.size * 0.85f;
            dl->AddText(m.font, cs, ImVec2(cmin.x + 5, cmin.y + (cmax.y - cmin.y - cs) * 0.5f), U32(cc), label);
            if (hot)
                tips.push_back({ cmin, cmax, e.chip == ChipRetry ? "Translation failed - click to try again"
                                                                 : "Translate this line" });
        }

        if (g_prefs.accentBars) {
            const ImVec4& bar = e.mention ? g_prefs.colBarMention
                : e.outgoing ? g_prefs.colBarOutgoing
                : e.bot ? g_prefs.colBarBot : g_prefs.colBarOther;
            dl->AddLine(ImVec2(winPos.x + 4.0f, top), ImVec2(winPos.x + 4.0f, top + e.height - 1.0f), col(bar), 3.0f);
        }
    }

    if (vis_.empty()) {
        const char* msg = entries_.empty()
            ? (live_ ? "No chat yet. Lines appear here once you've sent one message in Hytale's chat." : "")
            : "Nothing matches the current filter.";
        dl->AddText(ImVec2(textOrigin.x + 4, origin.y + 8), ImGui::GetColorU32(ImGuiCol_TextDisabled), msg);
    }

    // ---- input: an invisible button over the visible area owns the mouse ----
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(viewW, (std::max)(totalH_, 1.0f)));
    ImGui::SetCursorPos(ImVec2(0.0f, scrollY));
    ImGui::InvisibleButton("##hit", ImVec2((std::max)(1.0f, viewW), (std::max)(1.0f, viewH)),
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsWindowFocused();
    auto localOf = [&](ImVec2 p) { return ImVec2(p.x - textOrigin.x, p.y - origin.y); };

    if (hovered) {
        const Hit h = HitTest(m, localOf(io.MousePos));
        if (h.onChip) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else if (h.onText) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        for (const TipRegion& r : tips)
            if (ImGui::IsMouseHoveringRect(r.a, r.b)) { ImGui::SetTooltip("%s", r.tip.c_str()); break; }
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !vis_.empty()) {
        const Hit h = HitTest(m, localOf(io.MousePos));
        const Entry& e = entries_[(size_t)vis_[(size_t)(std::max)(0, h.vis)]];
        const int clicks = ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
        if (h.onChip && live_ && (e.chip == ChipTranslate || e.chip == ChipRetry)) {
            RequestIncomingTranslate(e.id, e.body, true);
        }
        else if (clicks == 2) {
            // The word under the pointer.
            const std::string& t = e.text;
            auto cpAt = [&](uint32_t i) { uint32_t cp; DecodeUtf8(t.data(), i, t.size(), cp); return cp; };
            auto prevStart = [&](uint32_t i) { do { --i; } while (i > 0 && ((unsigned char)t[i] & 0xC0) == 0x80); return i; };
            uint32_t b = (std::min)(h.off, (uint32_t)t.size()), z = b;
            while (b > 0 && t[b - 1] != '\n' && IsWordCp(cpAt(prevStart(b)))) b = prevStart(b);
            while (z < t.size() && t[z] != '\n' && IsWordCp(cpAt(z))) { uint32_t cp; z += DecodeUtf8(t.data(), z, t.size(), cp); }
            if (b == z && z < t.size()) { uint32_t cp; z += DecodeUtf8(t.data(), z, t.size(), cp); }
            a_ = { e.id, b };
            b_ = { e.id, z };
            dragging_ = false;
        }
        else if (clicks >= 3) {
            a_ = { e.id, 0 };
            b_ = { e.id, (uint32_t)e.text.size() };
            dragging_ = false;
        }
        else if (io.KeyShift && a_.id) {
            b_ = { e.id, h.off };
            dragging_ = true;
        }
        else {
            a_ = b_ = { e.id, h.off };
            dragging_ = true;
        }
    }

    if (dragging_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !vis_.empty()) {
            // Past the top or bottom edge, scroll towards the pointer so a
            // selection can run beyond what's on screen.
            const float topEdge = winPos.y, botEdge = winPos.y + viewH;
            if (io.MousePos.y < topEdge)
                ImGui::SetScrollY(scrollY - (std::min)(40.0f, (topEdge - io.MousePos.y) * 0.5f + 2.0f));
            else if (io.MousePos.y > botEdge)
                ImGui::SetScrollY(scrollY + (std::min)(40.0f, (io.MousePos.y - botEdge) * 0.5f + 2.0f));
            ImVec2 p = io.MousePos;
            p.y = (std::max)(topEdge, (std::min)(botEdge - 1.0f, p.y));
            const Hit h = HitTest(m, localOf(p));
            const Entry& e = entries_[(size_t)vis_[(size_t)(std::max)(0, h.vis)]];
            b_ = { e.id, h.off };
        }
        else dragging_ = false;
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const Hit h = HitTest(m, localOf(io.MousePos));
        menuId_ = (h.vis >= 0 && !vis_.empty()) ? entries_[(size_t)vis_[(size_t)h.vis]].id : 0;
        ImGui::OpenPopup("##chatctx");
    }
    ContextMenu();

    // ---- keyboard, while the log (not a text box) has focus ----
    if (focused && !io.WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C) && HasSelection()) {
            Pos l2, h2;
            SelectionRange(l2, h2);
            CopyPieces(PiecesForRange(l2, h2), g_prefs.ctrlCMode);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) SelectAll();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) a_ = b_ = {};
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) ImGui::SetScrollY(0.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_End)) scrollReq_ = true;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) ImGui::SetScrollY(scrollY - viewH * 0.9f);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) ImGui::SetScrollY(scrollY + viewH * 0.9f);
    }

    // ---- follow new messages ----
    const bool follow = (stick_ && g_prefs.autoScroll && !dragging_) || (first_ && !vis_.empty());
    if (scrollReq_ || follow) {
        ImGui::SetScrollY(totalH_ + viewH); // clamped to the real maximum next frame
        if (scrollReq_) bottomSeenId_ = newestId;
        scrollReq_ = false;
    }
    if (!vis_.empty()) first_ = false;

    // ---- overlays: "new messages" pill and the copy toast ----
    if (unread_ > 0) {
        char label[64];
        snprintf(label, sizeof(label), "  %d new message%s  ", unread_, unread_ == 1 ? "" : "s");
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const ImVec2 pos(winPos.x + (viewW - ts.x) * 0.5f, winPos.y + viewH - ts.y - 16.0f);
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label)) scrollReq_ = true;
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    if (!toast_.empty() && NowSteadyMs() < toastUntil_) {
        const ImVec2 ts = ImGui::CalcTextSize(toast_.c_str());
        const ImVec2 p(winPos.x + viewW - ts.x - 22.0f, winPos.y + 8.0f);
        dl->PushClipRectFullScreen();
        dl->AddRectFilled(ImVec2(p.x - 8, p.y - 4), ImVec2(p.x + ts.x + 8, p.y + ts.y + 4), IM_COL32(20, 24, 34, 235), 6.0f);
        dl->AddText(p, IM_COL32(170, 230, 180, 255), toast_.c_str());
        dl->PopClipRect();
    }

    ImGui::EndChild();
    ImGui::PopFont();
}

} // namespace

// =============================================================================
// The Chat tab
// =============================================================================
namespace {

LogView g_mainView("##chatlog");
LogView g_previewView("##chatpreview");
bool g_hideNonMatching = true;
bool g_mentionsOnly = false;

// ---- input line: history (Up/Down) and player-name completion (Tab) ----
struct InputState {
    std::vector<std::string> history;
    int histPos = -1;
    std::string draft;
    std::vector<std::string> matches;
    size_t matchIdx = 0;
    int matchStart = -1;
    std::string lastInserted;
};
InputState g_in;

std::vector<std::string> NamesStartingWith(const std::string& prefixIn) {
    std::string prefix = ToLower(prefixIn);
    std::vector<std::string> out;
    if (prefix.empty()) return out;
    std::lock_guard lock(g_mtx);
    // Most recent speakers first - they are who you are most likely answering.
    for (auto it = g_seenPlayers.rbegin(); it != g_seenPlayers.rend(); ++it) {
        if (it->key.compare(0, prefix.size(), prefix) == 0 &&
            std::find(out.begin(), out.end(), it->display) == out.end())
            out.push_back(it->display);
    }
    return out;
}

int InputCallback(ImGuiInputTextCallbackData* d) {
    InputState& s = g_in;
    if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (s.history.empty()) return 0;
        const int prev = s.histPos;
        if (d->EventKey == ImGuiKey_UpArrow) {
            if (s.histPos == -1) {
                s.draft.assign(d->Buf, (size_t)d->BufTextLen);
                s.histPos = (int)s.history.size() - 1;
            }
            else if (s.histPos > 0) --s.histPos;
        }
        else if (d->EventKey == ImGuiKey_DownArrow) {
            if (s.histPos != -1 && ++s.histPos >= (int)s.history.size()) s.histPos = -1;
        }
        if (prev != s.histPos) {
            const std::string& t = s.histPos == -1 ? s.draft : s.history[(size_t)s.histPos];
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, t.c_str());
        }
    }
    else if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        const int end = d->CursorPos;
        int start = end;
        while (start > 0 && d->Buf[start - 1] != ' ') --start;
        std::string word(d->Buf + start, d->Buf + end);
        const bool at = !word.empty() && word[0] == '@';
        const int nameStart = start + (at ? 1 : 0);
        if (at) word.erase(0, 1);

        // Tab again right after a completion cycles to the next match.
        if (!s.matches.empty() && s.matchStart == nameStart && word == s.lastInserted) {
            s.matchIdx = (s.matchIdx + 1) % s.matches.size();
        }
        else {
            s.matches = NamesStartingWith(word);
            s.matchIdx = 0;
            s.matchStart = nameStart;
            if (s.matches.empty()) return 0;
        }
        const std::string& pick = s.matches[s.matchIdx];
        d->DeleteChars(nameStart, end - nameStart);
        d->InsertChars(nameStart, pick.c_str());
        s.lastInserted = pick;
    }
    return 0;
}

// Longer than one chat message? Split at spaces rather than cut it off - the
// old input silently dropped everything past the limit.
std::vector<std::string> SplitForChat(const std::string& s, size_t max) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        if (s.size() - start <= max) { out.push_back(s.substr(start)); break; }
        size_t end = start + max;
        while (end > start && ((unsigned char)s[end] & 0xC0) == 0x80) --end; // UTF-8 boundary
        const size_t sp = s.rfind(' ', end);
        if (sp != std::string::npos && sp > start + max / 2) end = sp;
        out.push_back(TrimCopy(s.substr(start, end - start)));
        start = end;
        while (start < s.size() && s[start] == ' ') ++start;
    }
    return out;
}

void SendInput() {
    std::string text = TrimCopy(g_input);
    if (text.empty()) return;

    InputState& s = g_in;
    if (s.history.empty() || s.history.back() != text) s.history.push_back(text);
    const size_t cap = (size_t)(std::max)(0, g_prefs.inputHistory);
    while (s.history.size() > cap) s.history.erase(s.history.begin());
    s.histPos = -1;
    s.draft.clear();
    g_input[0] = 0;

    if (HandleOwnerCommand(text, false)) return;

    if (!(g_pChat.load() && g_stringVT.load())) {
        // Keep what was typed; losing it silently was the old behaviour.
        strncpy_s(g_input, text.c_str(), _TRUNCATE);
        PushLocalNote("Not connected to Hytale's chat yet - send one message in the game's own chat box, then try again.",
            ImVec4(1.0f, 0.70f, 0.35f, 1.0f));
        return;
    }

    std::string langOverride;
    const bool hadPrefix = StripLangPrefix(text, langOverride);
    std::string targetLang;
    {
        std::lock_guard lock(g_settingsMtx);
        targetLang = hadPrefix ? langOverride : g_outgoingLang;
    }
    if (text.empty()) return;

    if (!targetLang.empty() && targetLang != "en") {
        RequestOutgoingTranslate(text, targetLang);
        return;
    }
    for (const std::string& part : SplitForChat(text, HYTALE_CHAT_MAX))
        if (!part.empty()) QueueSend({ part, part, "", "", "" });
}

void ViewMenu() {
    if (!ImGui::BeginPopup("##chatview")) return;
    ImGui::TextDisabled("Chat view");
    bool ch = false;
    ImGui::SetNextItemWidth(180);
    float fs = g_prefs.chatFontSize;
    if (ImGui::SliderFloat("Text size", &fs, 0.0f, 32.0f, fs <= 0.0f ? "same as UI" : "%.0f px")) {
        std::lock_guard lock(g_settingsMtx);
        g_prefs.chatFontSize = fs < 8.0f ? 0.0f : fs;
        ch = true;
    }
    ImGui::SetNextItemWidth(180);
    float gap = g_chatLineGap;
    if (ImGui::SliderFloat("Line gap", &gap, 0.0f, 12.0f, "%.0f px")) {
        std::lock_guard lock(g_settingsMtx);
        g_chatLineGap = gap;
        ch = true;
    }
    auto flag = [&](const char* label, bool& v) {
        bool t = v;
        if (ImGui::Checkbox(label, &t)) {
            std::lock_guard lock(g_settingsMtx);
            v = t;
            ch = true;
        }
    };
    flag("Game colours", g_prefs.gameColors);
    flag("Accent bars", g_prefs.accentBars);
    flag("Striped rows", g_prefs.zebra);
    flag("Indent wrapped lines", g_prefs.hangingIndent);
    flag("Show \"tr\" buttons", g_prefs.showTranslateChip);
    flag("Follow new messages", g_prefs.autoScroll);
    if (ch) MarkPrefsChanged();
    ImGui::Separator();
    if (ImGui::MenuItem("All chat settings...")) UiOpenSettings("Chat display");
    ImGui::EndPopup();
}

} // namespace

namespace ChatUi {

int UnreadMentions() {
    // Nothing is unread while you're looking at the chat - checked against the
    // last frame, because the title bar is drawn before the tab is.
    if (ImGui::GetCurrentContext() && ImGui::GetFrameCount() - g_chatTabFrame <= 1) return 0;
    return g_unreadMentions;
}

void DrawChatTab() {
    g_unreadMentions = 0;
    g_chatTabFrame = ImGui::GetFrameCount();

    bool liveTx, showTs, colored, nick;
    std::string outLang;
    {
        std::lock_guard lock(g_settingsMtx);
        liveTx = g_liveTranslate;
        showTs = g_showTimestamps;
        colored = g_coloredChat;
        nick = g_useNickname;
        outLang = g_outgoingLang;
    }

    // ---- row 1: switches ----
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 6));
    if (ImGui::Checkbox("Live translate", &liveTx)) {
        std::lock_guard lock(g_settingsMtx);
        g_liveTranslate = liveTx;
        g_settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Translate incoming chat automatically.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Timestamps", &showTs)) {
        std::lock_guard lock(g_settingsMtx);
        g_showTimestamps = showTs;
        MarkPrefsChanged();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Color AI", &colored)) {
        std::lock_guard lock(g_settingsMtx);
        g_coloredChat = colored;
        g_settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Prepend the colour code (&a by default) so AI replies show coloured in-game.");
    ImGui::SameLine();
    if (ImGui::Checkbox("AI nick", &nick)) {
        std::lock_guard lock(g_settingsMtx);
        g_useNickname = nick;
        g_settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Sends /nickname before the reply and /nickname off after,\n"
        "and drops the 'HyAI >' prefix (the nickname says it already).");

    ImGui::SameLine(0, 16);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Send as");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    const char* preview = outLang.empty() ? "As-is" : LangName(outLang);
    if (ImGui::BeginCombo("##outlang", preview ? preview : "As-is")) {
        for (const auto& l : OUT_LANGS) {
            const bool selected = outLang == l.code;
            if (ImGui::Selectable(l.name, selected)) {
                std::lock_guard lock(g_settingsMtx);
                g_outgoingLang = l.code;
                g_settingsDirty = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Language your messages are sent in.\nOverride per message with /fr, /de, /en ...");

    ImGui::SameLine(0, 16);
    if (ImGui::Button("View")) ImGui::OpenPopup("##chatview");
    ViewMenu();
    ImGui::PopStyleVar();

    // ---- row 2: search ----
    ImGui::SetNextItemWidth((std::min)(360.0f, ImGui::GetContentRegionAvail().x * 0.5f));
    ImGui::InputTextWithHint("##filter", "Search chat...   (from:name to pick a player)", g_chatFilter, sizeof(g_chatFilter));
    ImGui::SameLine();
    if (g_chatFilter[0]) {
        if (ImGui::SmallButton("x")) g_chatFilter[0] = 0;
        ImGui::SameLine();
        ImGui::Checkbox("Hide others", &g_hideNonMatching);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: only matching lines are shown.\nOff: everything stays, matches are highlighted.");
        ImGui::SameLine();
    }
    ImGui::Checkbox("Mentions only", &g_mentionsOnly);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lines that mention you or a highlight word (Settings > Highlights).");

    // ---- the log ----
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 4.0f;
    g_mainView.Draw(ImVec2(0, -footer), g_chatFilter, g_hideNonMatching, g_mentionsOnly);

    // ---- input ----
    const bool ready = g_pChat.load() && g_stringVT.load();
    const size_t len = strlen(g_input);
    char counter[32];
    const size_t parts = len == 0 ? 0 : (len + HYTALE_CHAT_MAX - 1) / HYTALE_CHAT_MAX;
    if (parts > 1) snprintf(counter, sizeof(counter), "%zu/%zu (%zu msgs)", len, HYTALE_CHAT_MAX, parts);
    else snprintf(counter, sizeof(counter), "%zu/%zu", len, HYTALE_CHAT_MAX);
    const float counterW = ImGui::CalcTextSize(counter).x;

    if (g_focusInputRequested) {
        ImGui::SetKeyboardFocusHere();
        g_focusInputRequested = false;
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f - counterW - 16.0f);
    const char* hint = ready ? "Message...   (Up/Down: history   Tab: complete a name   /fr /de /en: language)"
                             : "Not hooked yet - send one message in Hytale's chat first";
    const bool enter = ImGui::InputTextWithHint("##in", hint, g_input, sizeof(g_input),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
        ImGuiInputTextFlags_CallbackCompletion, InputCallback);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (len > HYTALE_CHAT_MAX) ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.35f, 1.0f), "%s", counter);
    else ImGui::TextDisabled("%s", counter);
    ImGui::SameLine();
    const bool click = ImGui::Button("Send", ImVec2(70, 0));
    if (enter || click) {
        SendInput();
        ImGui::SetKeyboardFocusHere(-1);
    }
}

void DrawStylePreview(float height) {
    static uint32_t builtFor = 0;
    // Rebuilt when the local name changes, so the mention example stays yours.
    std::string me;
    {
        std::lock_guard lock(g_settingsMtx);
        me = g_localName.empty() ? "you" : g_localName;
    }
    const uint32_t v = g_prefsVersion.load();
    if (builtFor != v) {
        builtFor = v;
        long long t = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() - 6 * 60000;
        uint64_t id = 1;
        std::vector<ChatLine> lines;
        lines.reserve(8); // line() hands out pointers into this
        auto line = [&](std::vector<ChatSeg> segs, const std::string& speaker, bool outgoing = false) {
            ChatLine L;
            L.id = id++;
            L.unixMs = (t += 47000);
            L.segs = std::move(segs);
            for (const auto& s : L.segs) L.plain += s.text;
            L.speaker = speaker;
            const size_t c = L.plain.find(": ");
            L.body = c != std::string::npos ? L.plain.substr(c + 2) : L.plain;
            L.outgoing = outgoing;
            lines.push_back(std::move(L));
            return &lines.back();
        };
        const ImVec4 gold(1.0f, 0.67f, 0.0f, 1), white(1, 1, 1, 1), grey(0.67f, 0.67f, 0.67f, 1);
        const ImVec4 aqua(0.33f, 1.0f, 1.0f, 1), green(0.33f, 1.0f, 0.33f, 1), navy(0.0f, 0.0f, 0.67f, 1);
        line({ { "[Server] ", gold }, { "Welcome back! The Kraken event starts in 10 minutes.", white } }, "");
        line({ { "[VIP] ", aqua }, { "Steve", white }, { ": ", grey }, { "hey " + me + ", want to trade diamonds?", white } }, "Steve");
        line({ { "sure, meet me at spawn", white } }, "", true);
        ChatLine* fr = line({ { "Amelie", white }, { ": ", grey }, { "salut tout le monde, quelqu'un veut explorer ?", white } }, "Amelie");
        fr->txStatus = TxStatus::Done;
        fr->txFrom = "fr";
        fr->txText = "hi everyone, does anyone want to explore?";
        line({ { "HyAI > ", green }, { "The Kraken spawns at night near the deep ocean; bring potions.", green } }, "HyAI");
        line({ { "Notch", white }, { ": ", grey }, { "dark colours like this one get lifted so they stay readable - and long "
                 "messages wrap neatly under the timestamp instead of running off the edge.", navy } }, "Notch");
        g_previewView.SetStatic(std::move(lines));
    }
    g_previewView.Draw(ImVec2(0, height), "", true, false);
}

// =============================================================================
// Alerts and the on-disk log
// =============================================================================
static void AppendToDisk(const std::vector<ChatLine>& lines) {
    const std::wstring dir = GetThisModuleDirectory() + L"chatlogs";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::ofstream f;
    std::string openDay;
    for (const ChatLine& L : lines) {
        EnsureTimeZone();
        const time_t t = (time_t)(L.unixMs / 1000);
        struct tm tmv {};
        localtime_s(&tmv, &t);
        char day[16], clock[16];
        strftime(day, sizeof(day), "%Y-%m-%d", &tmv);
        strftime(clock, sizeof(clock), "%H:%M:%S", &tmv);
        if (openDay != day) {
            if (f.is_open()) f.close();
            f.open(dir + L"\\" + Utf8ToWide(day) + L".txt", std::ios::app | std::ios::binary);
            openDay = day;
        }
        if (!f.is_open()) return;
        f << '[' << clock << "] " << (L.outgoing && !L.local ? "> " : "") << CleanText(L.plain);
        if (L.txStatus == TxStatus::Done && !L.txText.empty())
            f << "  [" << L.txFrom << ": " << CleanText(L.txText) << ']';
        f << "\r\n";
    }
}

void Tick() {
    static uint64_t lastId = 0;
    static uint64_t seenVersion = 0;
    const uint64_t v = g_logVersion.load();
    if (v == seenVersion) return;
    seenVersion = v;

    std::vector<ChatLine> fresh;
    CollectNewChatLines(lastId, fresh);
    if (fresh.empty()) return;

    if (g_prefs.saveChatToDisk) AppendToDisk(fresh);

    int mentions = 0;
    for (const ChatLine& L : fresh)
        if (IsMention(L)) ++mentions;
    if (!mentions) return;

    g_unreadMentions += mentions;
    const HWND fg = GetForegroundWindow();
    const bool overlayFocused = fg && fg == g_hwndOverlay && IsWindowVisible(g_hwndOverlay);
    if (g_prefs.alertOnlyUnfocused && overlayFocused) return;
    PlayMentionSound();
    if (g_prefs.mentionFlash) FlashOverlay();
}

void Ping(bool sound, bool flash) {
    if (sound) {
        if (g_prefs.mentionSound != 0) PlayMentionSound();
        else PlaySoundW(L"SystemNotification", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
    }
    if (flash) {
        // Hytale's own taskbar button: that is the window in front while playing.
        HWND h = g_hWnd ? g_hWnd : g_hwndOverlay;
        if (h) {
            FLASHWINFO fi{ sizeof(fi) };
            fi.hwnd = h;
            fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
            fi.uCount = 3;
            FlashWindowEx(&fi);
        }
    }
}

void TestMentionSound() {
    PlayMentionSound();
}

} // namespace ChatUi
