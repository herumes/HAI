#include "pch.h"
#include "ui.hpp"
#include "common.hpp"
#include "chat.hpp"
#include "settings.hpp"
#include "shared.hpp"
#include "extras.hpp"
#include "translate.hpp"
#include "chatview.hpp"
#include "chatprefix.hpp"
#include "aiflow.hpp"
#include "aitext.hpp"
#include "aitools.hpp"
#include "autoreply.hpp"
#include "prefab.hpp"
#include "farmroute.hpp"
#include "autofarm.hpp"

#include <imgui_stdlib.h>
#include <commdlg.h>
#include <shellapi.h>

// =============================================================================
// Settings tab
// =============================================================================
// One long scrolling page had grown to ~40 controls in no particular order.
// It is now a list of pages on the left - searchable, since the thing you are
// looking for is usually a word ("font", "sound", "hotkey") rather than a
// category - and one page at a time on the right.

namespace {

std::string g_pageRequest;        // set by UiOpenSettings, consumed by the nav
bool g_selectTabRequest = false;

// ---- small helpers: every control writes under the settings lock ----------
void Tip(const char* tip) {
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tip);
}

bool PrefBool(const char* label, bool& v, const char* tip = nullptr) {
    bool t = v;
    const bool ch = ImGui::Checkbox(label, &t);
    Tip(tip);
    if (ch) { std::lock_guard lock(g_settingsMtx); v = t; MarkPrefsChanged(); }
    return ch;
}

bool PrefFloat(const char* label, float& v, float lo, float hi, const char* fmt, const char* tip = nullptr, float width = 220.0f) {
    float t = v;
    ImGui::SetNextItemWidth(width);
    const bool ch = ImGui::SliderFloat(label, &t, lo, hi, fmt);
    Tip(tip);
    if (ch) { std::lock_guard lock(g_settingsMtx); v = t; MarkPrefsChanged(); }
    return ch;
}

bool PrefInt(const char* label, int& v, int lo, int hi, const char* fmt = "%d", const char* tip = nullptr, float width = 220.0f) {
    int t = v;
    ImGui::SetNextItemWidth(width);
    const bool ch = ImGui::SliderInt(label, &t, lo, hi, fmt);
    Tip(tip);
    if (ch) { std::lock_guard lock(g_settingsMtx); v = t; MarkPrefsChanged(); }
    return ch;
}

bool PrefCombo(const char* label, int& v, const char* const* items, int n, const char* tip = nullptr, float width = 220.0f) {
    int t = v;
    ImGui::SetNextItemWidth(width);
    const bool ch = ImGui::Combo(label, &t, items, n);
    Tip(tip);
    if (ch) { std::lock_guard lock(g_settingsMtx); v = t; MarkPrefsChanged(); }
    return ch;
}

bool PrefColor(const char* label, ImVec4& v, const char* tip = nullptr, bool alpha = true) {
    ImVec4 t = v;
    ImGuiColorEditFlags f = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf;
    if (alpha) f |= ImGuiColorEditFlags_AlphaBar;
    else f |= ImGuiColorEditFlags_NoAlpha;
    const bool ch = ImGui::ColorEdit4(label, &t.x, f);
    Tip(tip);
    if (ch) { std::lock_guard lock(g_settingsMtx); v = t; MarkPrefsChanged(); }
    return ch;
}

// Text fields commit when you leave them, not on every keystroke.
bool PrefText(const char* label, std::string& v, const char* tip = nullptr, float width = 320.0f, float multilineH = 0.0f) {
    // One buffer per field; only the field being typed in keeps its own copy,
    // every other one follows the setting (which may change from elsewhere).
    static std::map<const void*, std::string> buffers;
    static const void* active = nullptr;
    std::string& editing = buffers[&v];
    if (active != &v) editing = v;
    bool committed = false;
    ImGui::PushID(&v);
    if (multilineH > 0.0f) ImGui::InputTextMultiline(label, &editing, ImVec2(width, multilineH));
    else { ImGui::SetNextItemWidth(width); ImGui::InputText(label, &editing); }
    const bool isActive = ImGui::IsItemActive();
    Tip(tip);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::lock_guard lock(g_settingsMtx);
        v = editing;
        MarkPrefsChanged();
        committed = true;
    }
    if (isActive) active = &v;
    else if (active == &v) active = nullptr;
    ImGui::PopID();
    return committed;
}

// Legacy settings kept in char buffers (shared.hpp): same commit-on-leave rule.
void LegacyText(const char* label, char* buf, size_t size, std::string& target, const char* tip = nullptr,
    ImGuiInputTextFlags flags = 0, float width = 320.0f, bool affectsDisplay = false) {
    ImGui::SetNextItemWidth(width);
    ImGui::InputText(label, buf, size, flags);
    Tip(tip);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::lock_guard lock(g_settingsMtx);
        target = buf;
        g_settingsDirty = true;
        if (affectsDisplay) g_prefsVersion.fetch_add(1);
    }
}

bool LegacyBool(const char* label, bool& v, const char* tip = nullptr, bool affectsDisplay = false) {
    bool t;
    { std::lock_guard lock(g_settingsMtx); t = v; }
    const bool ch = ImGui::Checkbox(label, &t);
    Tip(tip);
    if (ch) {
        std::lock_guard lock(g_settingsMtx);
        v = t;
        g_settingsDirty = true;
        if (affectsDisplay) g_prefsVersion.fetch_add(1);
    }
    return ch;
}

void Section(const char* title) {
    ImGui::Spacing();
    ImGui::SeparatorText(title);
}

void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void ResetButton(PrefGroup group, const char* what = "this page") {
    ImGui::Spacing();
    ImGui::Separator();
    static long long armedAt = 0;
    static int armedGroup = -1;
    const long long now = NowSteadyMs();
    const bool armed = armedGroup == (int)group && now - armedAt < 3000;
    const std::string label = armed ? std::string("Click again to reset ") + what
                                     : std::string("Reset ") + what + " to defaults";
    if (armed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.20f, 0.18f, 1.0f));
    const bool clicked = ImGui::Button((label + "###reset").c_str());
    if (armed) ImGui::PopStyleColor();
    if (!clicked) return;
    if (armed) {
        ResetPrefsGroup(group);
        armedGroup = -1;
    }
    else {
        armedGroup = (int)group;
        armedAt = now;
    }
}

std::string BrowseFile(const wchar_t* filter, const wchar_t* title) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwndOverlay;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    return WideToUtf8(file, (int)wcslen(file));
}

void OpenFolder(const std::wstring& path) {
    CreateDirectoryW(path.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- hotkey capture --------------------------------------------------------
std::string KeyName(int vk) {
    if (vk <= 0) return "(none)";
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    // These live on the extended part of the keyboard; without the flag
    // GetKeyNameText reports the numpad key that shares the scan code.
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
    case VK_RCONTROL: case VK_RMENU: case VK_SNAPSHOT:
        sc |= 0x100;
        break;
    default: break;
    }
    wchar_t name[64] = L"";
    if (sc && GetKeyNameTextW((LONG)(sc << 16), name, 64) > 0)
        return WideToUtf8(name, (int)wcslen(name));
    char buf[24];
    snprintf(buf, sizeof(buf), "Key 0x%02X", vk);
    return buf;
}

// A button showing the bound key; click, then press the new key. Esc cancels,
// Backspace or Delete unbinds. Returns true when the binding changed.
const void* g_capturingKey = nullptr;   // the binding waiting for a key press

bool HotkeyField(const char* id, int& vk) {
    const void*& capturing = g_capturingKey;
    static bool down[256];
    ImGui::PushID(id);
    const bool isCapturing = capturing == &vk;
    const std::string label = isCapturing ? "press a key...  (Esc cancels, Backspace clears)" : KeyName(vk);
    if (ImGui::Button((label + "###btn").c_str(), ImVec2(isCapturing ? 330.0f : 160.0f, 0))) {
        capturing = &vk;
        for (int k = 0; k < 256; ++k) down[k] = (GetAsyncKeyState(k) & 0x8000) != 0;
    }
    bool changed = false;
    if (capturing == &vk) {
        for (int k = 8; k < 255; ++k) {
            // Mouse buttons and bare modifiers can't be hotkeys here.
            if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON || k == VK_SHIFT ||
                k == VK_CONTROL || k == VK_MENU || (k >= VK_LSHIFT && k <= VK_RMENU) ||
                k == VK_LWIN || k == VK_RWIN)
                continue;
            const bool now = (GetAsyncKeyState(k) & 0x8000) != 0;
            if (now && !down[k]) {
                capturing = nullptr;
                if (k == VK_ESCAPE) break;
                std::lock_guard lock(g_settingsMtx);
                vk = (k == VK_BACK || k == VK_DELETE) ? 0 : k;
                MarkPrefsChanged();
                changed = true;
                break;
            }
            down[k] = now;
        }
    }
    ImGui::PopID();
    return changed;
}

// =============================================================================
// Pages
// =============================================================================
void PageGeneral() {
    Section("You");
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("Your in-game name", g_localNameBuf, sizeof(g_localNameBuf));
    Tip("Used to tell your own messages apart, to allow owner commands, and for mentions.");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::lock_guard lock(g_settingsMtx);
        g_localName = g_localNameBuf;
        MarkPrefsChanged();
    }

    Section("When the bot answers");
    LegacyBool("Muted (AFK)", g_botMuted, "Ignore wake words and stay silent. Auto replies stop too.");
    LegacyBool("Answer my own messages", g_allowSelfTrigger, "Respond when your own messages contain the wake word.");
    LegacyBool("Ignore lines starting with / or !", g_blockLeadingSlash,
        "Also: a bot reply that would start with / is sent with a > in front, so it can't run a command.");
    LegacyBool("Whitelist mode (only listed players can wake the bot)", g_whitelistMode,
        "Manage the list on the Ignored tab. Ignore still wins if a name is on both.");

    Section("What the AI remembers");
    int mem, ctx;
    bool global;
    { std::lock_guard lock(g_settingsMtx); mem = g_maxMemory; ctx = g_maxChatContext; global = g_includeGlobalChat; }
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Conversation turns", &mem, 1, 8)) { std::lock_guard lock(g_settingsMtx); g_maxMemory = mem; g_settingsDirty = true; }
    Tip("Recent question + answer pairs sent back to the model.");
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Chat lines as context", &ctx, 4, 60)) { std::lock_guard lock(g_settingsMtx); g_maxChatContext = ctx; g_settingsDirty = true; }
    Tip("Recent server chat lines included with each question.");
    if (ImGui::Checkbox("Include server chat as context", &global)) { std::lock_guard lock(g_settingsMtx); g_includeGlobalChat = global; g_settingsDirty = true; }
}

void PageBotReplies() {
    Section("How replies look in game");
    LegacyBool("Colour the reply", g_coloredChat, "Put the colour code below in front of every reply line.");
    LegacyText("Colour code", g_aiColorCodeBuf, sizeof(g_aiColorCodeBuf), g_aiColorCode, "e.g. &a (green), &b (aqua), &e (yellow)", 0, 120);
    LegacyText("Reply prefix", g_aiPrefixBuf, sizeof(g_aiPrefixBuf), g_aiPrefix, "Shown before each reply line, e.g. \"HyAI > \". Dropped while the nickname trick is on.");
    LegacyBool("Speak under a nickname", g_useNickname,
        "Sends the \"nickname on\" command before a reply and the \"off\" command after,\nso replies show under the bot's name instead of yours.");
    ImGui::Indent(16.0f);
    LegacyText("Nickname on command", g_nicknameOnBuf, sizeof(g_nicknameOnBuf), g_nicknameOn);
    LegacyText("Nickname off command", g_nicknameOffBuf, sizeof(g_nicknameOffBuf), g_nicknameOff);
    int delay;
    { std::lock_guard lock(g_settingsMtx); delay = g_nicknameDelayMs; }
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Wait after nickname (ms)", &delay, 0, 2000)) { std::lock_guard lock(g_settingsMtx); g_nicknameDelayMs = delay; g_settingsDirty = true; }
    Tip("Increase if the first reply line still shows your real name (try 600-800).");
    ImGui::Unindent(16.0f);

    Section("Waking the bot");
    LegacyText("Wake word", g_wakeWordBuf, sizeof(g_wakeWordBuf), g_wakeWord, "A message containing this word (as a whole word) gets an AI reply.\nAlso the prefix for owner commands, e.g. \"hyai mute\".");
    LegacyText("Second wake word", g_wakeWord2Buf, sizeof(g_wakeWord2Buf), g_wakeWord2);
    LegacyText("Bot name", g_botNameBuf, sizeof(g_botNameBuf), g_botName, "How the bot recognises its own lines (so it never answers itself).", 0, 320.0f, true);
    LegacyText("Failure message", g_failMessageBuf, sizeof(g_failMessageBuf), g_failMessage, "Sent when the AI request fails.");

    Section("Sending");
    int chunk, gap;
    { std::lock_guard lock(g_settingsMtx); chunk = (int)MAX_CHAT_CHUNK; gap = SEND_GAP_MS; }
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Reply line length", &chunk, 32, 200)) { std::lock_guard lock(g_settingsMtx); MAX_CHAT_CHUNK = (size_t)chunk; g_settingsDirty = true; }
    Tip("Long replies are split into lines of about this many characters.");
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Gap between lines (ms)", &gap, 50, 500)) { std::lock_guard lock(g_settingsMtx); SEND_GAP_MS = gap; g_settingsDirty = true; }
    Tip("Raise it if the server's spam filter drops some of the lines.");
}

void PageAiModel() {
    Section("Provider");
    int prov;
    std::string provider;
    { std::lock_guard lock(g_settingsMtx); provider = g_aiProvider; }
    prov = provider == "openai" ? 1 : 0;
    const char* providers[] = { "Ollama (local)", "OpenAI-compatible" };
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("Provider", &prov, providers, 2)) {
        std::lock_guard lock(g_settingsMtx);
        g_aiProvider = prov == 1 ? "openai" : "ollama";
        g_settingsDirty = true;
    }
    Tip("OpenAI-compatible covers OpenAI, LM Studio, vLLM, LiteLLM, OpenRouter and similar.");
    if (prov == 0) {
        LegacyText("Model", g_ollamaModelBuf, sizeof(g_ollamaModelBuf), g_ollamaModel, "e.g. gemma4:e4b, llama3.1:8b");
        LegacyText("Host", g_ollamaHostBuf, sizeof(g_ollamaHostBuf), g_ollamaHost);
        int port;
        { std::lock_guard lock(g_settingsMtx); port = g_ollamaPort; }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Port", &port)) {
            std::lock_guard lock(g_settingsMtx);
            g_ollamaPort = (std::max)(1, (std::min)(65535, port));
            g_settingsDirty = true;
        }
    }
    else {
        LegacyText("Base URL", g_openaiBaseBuf, sizeof(g_openaiBaseBuf), g_openaiBaseUrl,
            "e.g. https://api.openai.com or http://127.0.0.1:1234\n/v1/chat/completions is added for you.");
        LegacyText("API key", g_openaiKeyBuf, sizeof(g_openaiKeyBuf), g_openaiApiKey, nullptr, ImGuiInputTextFlags_Password);
        LegacyText("Model", g_openaiModelBuf, sizeof(g_openaiModelBuf), g_openaiModel);
    }

    Section("Generation");
    float temp;
    int npred, nctx;
    { std::lock_guard lock(g_settingsMtx); temp = g_ollamaTemperature; npred = g_ollamaNumPredict; nctx = g_ollamaNumCtx; }
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderFloat("Temperature", &temp, 0.0f, 2.0f, "%.2f")) { std::lock_guard lock(g_settingsMtx); g_ollamaTemperature = temp; g_settingsDirty = true; }
    Tip("Higher = more varied, lower = more focused. 0.3-1.0 is typical.");
    PrefFloat("Top-p", g_prefs.topP, 0.05f, 1.0f, "%.2f", "Nucleus sampling cut-off. 0.9-0.95 is typical.");
    PrefInt("Top-k", g_prefs.topK, 0, 200, "%d", "Ollama only. 0 turns it off.");
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Max reply tokens", &npred, 16, 16549)) { std::lock_guard lock(g_settingsMtx); g_ollamaNumPredict = npred; g_settingsDirty = true; }
    Tip("Hard cap on the reply length. 512-768 is plenty for chat.");
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Context window", &nctx, 2048, 131072)) { std::lock_guard lock(g_settingsMtx); g_ollamaNumCtx = nctx; g_settingsDirty = true; }
    Tip("Ollama num_ctx. Higher uses more VRAM. Ignored by cloud providers.");

    Section("Thinking (Ollama reasoning models)");
    bool think;
    std::string lvl;
    { std::lock_guard lock(g_settingsMtx); think = g_thinkEnabled; lvl = ToLower(TrimCopy(g_thinkLevel)); }
    const char* levels[] = { "Off", "On", "Low", "Medium", "High", "Max" };
    int idx = !think ? 0 : lvl == "low" ? 2 : (lvl == "medium" || lvl == "med") ? 3 : lvl == "high" ? 4 : lvl == "max" ? 5 : 1;
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("Think level", &idx, levels, 6)) {
        static const char* values[] = { "false", "true", "low", "medium", "high", "max" };
        std::lock_guard lock(g_settingsMtx);
        g_thinkEnabled = idx != 0;
        g_thinkLevel = values[idx];
        strncpy_s(g_thinkLevelBuf, g_thinkLevel.c_str(), _TRUNCATE);
        g_settingsDirty = true;
    }
    Tip("Levels are model-dependent. Thinking makes replies slower.");

    Section("Web search tool (LangSearch)");
    bool ws, wsSum;
    int wsCount, wsMax;
    { std::lock_guard lock(g_settingsMtx); ws = g_webSearchEnabled; wsSum = g_webSearchSummary; wsCount = g_webSearchCount; wsMax = g_webSearchMaxChars; }
    bool ch = false;
    if (ImGui::Checkbox("Let the model search the web", &ws)) ch = true;
    Tip("When off, the tool isn't offered to the model at all (saves tokens).");
    LegacyText("LangSearch API key", g_webSearchKeyBuf, sizeof(g_webSearchKeyBuf), g_webSearchApiKey, nullptr, ImGuiInputTextFlags_Password);
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Results", &wsCount, 1, 10)) ch = true;
    if (ImGui::Checkbox("Long summaries", &wsSum)) ch = true;
    Tip("Off: short snippets (fewer tokens). On: full summaries.");
    LegacyText("Freshness", g_webSearchFreshBuf, sizeof(g_webSearchFreshBuf), g_webSearchFreshness, "oneDay | oneWeek | oneMonth | oneYear | noLimit", 0, 160);
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Max result characters", &wsMax, 400, 4000)) ch = true;
    if (ch) {
        std::lock_guard lock(g_settingsMtx);
        g_webSearchEnabled = ws;
        g_webSearchSummary = wsSum;
        g_webSearchCount = wsCount;
        g_webSearchMaxChars = wsMax;
        g_settingsDirty = true;
    }
}

void PageChatDisplay() {
    Hint("Everything here changes the chat log right away - including this preview. "
         "Select text in it with the mouse; right-click for the copy options.");
    ChatUi::DrawStylePreview(210.0f);

    Section("Text");
    PrefFloat("Text size", g_prefs.chatFontSize, 0.0f, 32.0f, g_prefs.chatFontSize < 8.0f ? "same as UI" : "%.0f px",
        "Size of the chat log text only. Leave at the left end to follow the UI font size.");
    if (g_prefs.chatFontSize > 0.0f && g_prefs.chatFontSize < 8.0f) { std::lock_guard lock(g_settingsMtx); g_prefs.chatFontSize = 0.0f; }
    {
        float gap = g_chatLineGap;
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("Space between messages", &gap, 0.0f, 16.0f, "%.0f px")) {
            std::lock_guard lock(g_settingsMtx);
            g_chatLineGap = gap;
            MarkPrefsChanged();
        }
    }
    PrefBool("Indent wrapped lines under the text", g_prefs.hangingIndent, "Continuation rows start after the timestamp, not under it.");

    Section("Timestamps");
    LegacyBool("Show timestamps", g_showTimestamps, nullptr, true);
    const char* formats[] = { "3:07 PM", "15:07", "15:07:42", "3:07:42 PM", "Custom" };
    PrefCombo("Format", g_prefs.timeFormat, formats, 5);
    if (g_prefs.timeFormat == 4) {
        ImGui::Indent(16.0f);
        PrefText("Pattern", g_prefs.timeCustom, "strftime codes: %H hour, %M minute, %S second, %I 12-hour, %p AM/PM,\n%d day, %b month name, %a weekday. Unknown codes are shown as typed.", 160);
        ImGui::SameLine();
        ImGui::TextDisabled("now: %s", FormatChatTime(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()).c_str());
        ImGui::Unindent(16.0f);
    }
    const char* zones[] = { "US Pacific (PST/PDT)", "This PC's time zone", "UTC" };
    if (PrefCombo("Time zone", g_prefs.timeZone, zones, 3, "Used for chat timestamps, the saved chat log and the debug log."))
        SetTimeZoneMode(g_prefs.timeZone);

    Section("Colours");
    PrefBool("Use the game's colours", g_prefs.gameColors, "Off: every message in the text colour below.");
    PrefFloat("Minimum brightness", g_prefs.minBrightness, 0.0f, 0.9f, "%.2f",
        "Dark game colours (black, navy, dark red) are lifted to at least this brightness\nso they stay readable on a dark panel. 0 shows them exactly as sent.");
    PrefColor("Text", g_prefs.colText, "Lines without colour information, and everything when game colours are off.", false);
    ImGui::SameLine(0, 20); PrefColor("Your messages", g_prefs.colOutgoing, nullptr, false);
    ImGui::SameLine(0, 20); PrefColor("Timestamp", g_prefs.colTimestamp, nullptr, false);
    PrefColor("Translation", g_prefs.colTranslation, nullptr, false);
    ImGui::SameLine(0, 20); PrefColor("Selection", g_prefs.colSelection);
    ImGui::SameLine(0, 20); PrefColor("Search match", g_prefs.colSearchHit);

    Section("Decoration");
    PrefBool("Coloured bar beside each message", g_prefs.accentBars);
    if (g_prefs.accentBars) {
        ImGui::Indent(16.0f);
        PrefColor("Yours", g_prefs.colBarOutgoing);
        ImGui::SameLine(0, 20); PrefColor("Bot", g_prefs.colBarBot);
        ImGui::SameLine(0, 20); PrefColor("Others", g_prefs.colBarOther);
        ImGui::SameLine(0, 20); PrefColor("Mentions", g_prefs.colBarMention);
        ImGui::Unindent(16.0f);
    }
    PrefBool("Striped rows", g_prefs.zebra);
    if (g_prefs.zebra) { ImGui::SameLine(); PrefColor("##zebra", g_prefs.colZebra); }
    LegacyBool("Show my messages with their game formatting", g_showOwnFormatted, "Off: your messages use the \"Your messages\" colour.", true);
    PrefBool("Show the \"tr\" translate button", g_prefs.showTranslateChip, "After lines that haven't been translated.");

    Section("What gets shown");
    const char* ign[] = { "Show normally", "Dim them", "Hide them" };
    PrefCombo("Messages from ignored players", g_prefs.ignoredDisplay, ign, 3, "Ignoring someone always stops the AI answering them; this is only about the log.");
    PrefText("Hide lines containing", g_prefs.hidePatterns,
        "One phrase per line, not case sensitive. Matching lines are kept (search, AI context, copy)\n"
        "but not drawn - good for vote reminders, ads and other server spam.", 420, 80);
    PrefInt("History kept", g_prefs.maxLogLines, 100, 5000, "%d lines", "How many lines stay in memory for scrolling back and searching.");
    PrefBool("Follow new messages", g_prefs.autoScroll, "While you're scrolled to the bottom. Scroll up to read history and it stays put.");

    ResetButton(PrefGroup::ChatDisplay);
}

void PageHighlights() {
    Hint("A mention is a line that contains your name or one of the words below. It gets a tinted "
         "background, and can play a sound. Highlighting a player (right-click them in the log) "
         "also highlights everything they say.");
    Section("What counts as a mention");
    PrefBool("My name", g_prefs.highlightMyName, "Uses \"Your in-game name\" from General.");
    PrefText("Also these words or names", g_prefs.highlightWords, "Separate with commas or new lines.", 420, 70);
    PrefBool("Whole words only", g_prefs.highlightWholeWord, "Off: \"art\" also matches \"party\".");
    PrefColor("Background", g_prefs.colMentionBg);
    ImGui::SameLine(0, 20);
    PrefColor("Bar", g_prefs.colBarMention);

    Section("Alerts");
    const char* sounds[] = { "No sound", "Windows notification sound", "Short beep", "A .wav file" };
    PrefCombo("Sound", g_prefs.mentionSound, sounds, 4);
    if (g_prefs.mentionSound == 3) {
        ImGui::Indent(16.0f);
        PrefText("File", g_prefs.mentionSoundPath, nullptr, 360);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            const std::string f = BrowseFile(L"Sounds (*.wav)\0*.wav\0", L"Choose a mention sound");
            if (!f.empty()) { std::lock_guard lock(g_settingsMtx); g_prefs.mentionSoundPath = f; MarkPrefsChanged(); }
        }
        ImGui::Unindent(16.0f);
    }
    if (g_prefs.mentionSound != 0) {
        ImGui::SameLine();
        if (ImGui::Button("Test")) ChatUi::TestMentionSound();
    }
    PrefBool("Flash the taskbar button", g_prefs.mentionFlash);
    PrefBool("Not while I'm looking at the overlay", g_prefs.alertOnlyUnfocused);

    ResetButton(PrefGroup::Highlights);
}

void PageCopy() {
    Hint("Select text in the chat log with the mouse (double-click a word, triple-click a message, "
         "Shift+click to extend, Ctrl+A for everything). Ctrl+C copies; right-click offers every format.");
    Section("Ctrl+C");
    const char* modes[] = { "Plain text", "Plain text + colours for Word / Docs / email", "Text with colour codes" };
    PrefCombo("Ctrl+C copies", g_prefs.ctrlCMode, modes, 3,
        "The middle option puts both on the clipboard: Notepad and the game get plain text,\n"
        "Word, Google Docs and Outlook keep the colours.", 340);
    const char* codes[] = { "&c   (nearest of the 16 classic colours)", "&#RRGGBB   (exact colour)",
                            "\xC2\xA7" "c   (section sign, nearest)", "<#RRGGBB>   (tags)" };
    PrefCombo("Colour code style", g_prefs.copyCodeFormat, codes, 4, "Used by \"Text with colour codes\".", 340);

    Section("Line and log copies");
    PrefBool("Include timestamps", g_prefs.copyTimestamps, "For \"Copy line\", \"Copy whole log\" and exports. A selection always copies exactly what's selected.");
    PrefBool("Include translations", g_prefs.copyTranslations);
    PrefBool("Rich text on a dark background", g_prefs.richDarkBackground,
        "Game colours are made for a dark panel. On: pasted text sits on a dark block.\nOff: light colours are darkened to read on white paper.");

    Section("Saving");
    PrefBool("Save all chat to disk", g_prefs.saveChatToDisk, "One file per day in the chatlogs folder next to the DLL.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Open chatlogs folder")) OpenFolder(GetThisModuleDirectory() + L"chatlogs");
    Hint("Right-click the chat log > Export log... saves what's on screen as a web page (with colours) or plain text.");

    ResetButton(PrefGroup::CopyLog);
}

void PageInput() {
    Section("Chat box");
    PrefInt("Remembered messages", g_prefs.inputHistory, 0, 500, "%d", "Up / Down in the chat box walks back through what you sent.");
    PrefText("Whisper command", g_prefs.whisperTemplate, "Used by right-click > Whisper. {name} becomes the player.", 200);
    Hint("Tab completes player names from recent chat (press again to cycle). Messages longer than one chat "
         "line are split at spaces and sent as several.");
    ResetButton(PrefGroup::Input);
}

void PageAutoReplies() {
    static std::vector<AutoReplyRule> rules;
    static uint64_t loadedGen = 0;
    if (loadedGen != AutoReply::Generation()) {
        rules = AutoReply::GetRules();
        loadedGen = AutoReply::Generation();
    }
    {
        // Fire counts change underneath us; everything else is ours to edit.
        const auto live = AutoReply::GetRules();
        for (size_t i = 0; i < rules.size() && i < live.size(); ++i) {
            rules[i].fired = live[i].fired;
            rules[i].error = live[i].error;
        }
    }

    Hint("When a chat line matches, the reply is sent - once per cooldown, never to the bot's own lines, "
         "and never while muted. The first matching rule wins.");
    ImGui::TextDisabled("Placeholders: {speaker}  {me}  {bot}  {message}  {1}..{9} (regex groups)  ${time}  ${random:1-6}  ${math(2*21)}");

    bool changed = false;
    int remove = -1, moveUp = -1;
    const char* matchNames[] = { "contains", "starts with", "is exactly", "regex" };
    if (ImGui::BeginTable("##rules", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("On");
        ImGui::TableSetupColumn("When a line");
        ImGui::TableSetupColumn("Pattern", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Reply", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Cooldown");
        ImGui::TableSetupColumn("Options");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rules.size(); ++i) {
            AutoReplyRule& r = rules[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##on", &r.enabled)) changed = true;
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(100);
            if (ImGui::Combo("##m", &r.match, matchNames, 4)) changed = true;
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##p", &r.pattern);
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            if (!r.error.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "bad regex");
                Tip(r.error.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##r", &r.reply);
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(70);
            if (ImGui::DragInt("##cd", &r.cooldownSec, 0.5f, 0, 3600, "%ds")) changed = true;
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("Aa", &r.caseSensitive)) changed = true;
            Tip("Case sensitive");
            ImGui::SameLine();
            bool others = r.skipOwn;
            if (ImGui::Checkbox("not mine", &others)) { r.skipOwn = others; changed = true; }
            Tip("Don't react to lines you sent yourself");
            ImGui::TableNextColumn();
            if (i > 0) { if (ImGui::ArrowButton("##up", ImGuiDir_Up)) moveUp = (int)i; }
            else ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = (int)i;
            if (r.fired) { ImGui::SameLine(); ImGui::TextDisabled("%llu", (unsigned long long)r.fired); Tip("Times this rule has fired this session"); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) { rules.erase(rules.begin() + remove); changed = true; }
    if (moveUp > 0) { std::swap(rules[(size_t)moveUp], rules[(size_t)moveUp - 1]); changed = true; }
    if (ImGui::Button("Add rule")) {
        AutoReplyRule r;
        r.pattern = "";
        r.reply = "";
        r.enabled = false;
        rules.push_back(r);
        changed = true;
    }
    if (changed) {
        AutoReply::SetRules(rules);
        loadedGen = AutoReply::Generation();
    }

    Section("Try it");
    static std::string sample = "[+] Steve joined the game";
    ImGui::SetNextItemWidth(420);
    ImGui::InputTextWithHint("##sample", "A chat line, e.g.  Steve: hello there", &sample);
    // A handful of rules against one line - cheap enough to redo every frame,
    // which keeps it right however the rules were changed.
    ImGui::TextWrapped("%s", AutoReply::Test(sample, "").c_str());

    Section("Chat minigames (solved locally, no AI)");
    struct Game { const char* name; bool* show; bool* reply; const char* tip; };
    Game games[] = {
        { "Word value (A=1 ... Z=26)", &g_wordValueShow, &g_wordValueAuto, "\"What is WORD worth?\"" },
        { "Morse code", &g_morseShow, &g_morseAuto, "\"Decode this Morse: ...\"" },
        { "Caesar cipher", &g_caesarShow, &g_caesarAuto, "\"Caesar cipher, shift N - decode: TEXT\"" },
        { "Count the letters", &g_countLettersShow, &g_countLettersAuto, "\"How many letters are in WORD?\"" },
        { "Number pattern", &g_patternShow, &g_patternAuto, "\"What comes next: 81, 27, 9, 3, ?\"" },
    };
    if (ImGui::BeginTable("##games", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Game");
        ImGui::TableSetupColumn("Show answer");
        ImGui::TableSetupColumn("Send answer");
        ImGui::TableHeadersRow();
        for (Game& g : games) {
            ImGui::PushID(g.name);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(g.name);
            Tip(g.tip);
            ImGui::TableNextColumn();
            LegacyBool("##show", *g.show);
            ImGui::TableNextColumn();
            LegacyBool("##send", *g.reply, "Sends the answer in chat (respects mute and a 2.5 s cooldown).");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void PageTranslation() {
    Section("Incoming");
    LegacyBool("Translate incoming chat automatically", g_liveTranslate,
        "Lines that look foreign are sent to Google Translate. Others keep a \"tr\" button.");
    struct L { const char* code; const char* name; };
    static const L targets[] = {
        { "en", "English" }, { "es", "Spanish" }, { "fr", "French" }, { "de", "German" }, { "it", "Italian" },
        { "pt", "Portuguese" }, { "nl", "Dutch" }, { "pl", "Polish" }, { "ru", "Russian" }, { "uk", "Ukrainian" },
        { "tr", "Turkish" }, { "sv", "Swedish" }, { "da", "Danish" }, { "no", "Norwegian" }, { "fi", "Finnish" },
        { "cs", "Czech" }, { "hu", "Hungarian" }, { "ro", "Romanian" }, { "el", "Greek" }, { "ar", "Arabic" },
        { "ja", "Japanese" }, { "ko", "Korean" }, { "zh-CN", "Chinese (Simplified)" }, { "zh-TW", "Chinese (Traditional)" },
    };
    const char* cur = "English";
    for (const auto& t : targets) if (ToLower(g_prefs.txTarget) == ToLower(t.code)) cur = t.name;
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("Translate into", cur)) {
        for (const auto& t : targets) {
            if (ImGui::Selectable(t.name, ToLower(g_prefs.txTarget) == ToLower(t.code))) {
                std::lock_guard lock(g_settingsMtx);
                g_prefs.txTarget = t.code;
                MarkPrefsChanged();
            }
        }
        ImGui::EndCombo();
    }
    Tip("Your language. Lines already in it are left alone.");
    PrefBool("Show the \"tr\" button on untranslated lines", g_prefs.showTranslateChip);

    Section("Outgoing");
    std::string out;
    { std::lock_guard lock(g_settingsMtx); out = g_outgoingLang; }
    const char* preview = out.empty() ? "As-is" : LangName(out);
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("Send my messages in", preview ? preview : "As-is")) {
        for (const auto& l : OUT_LANGS) {
            if (ImGui::Selectable(l.name, out == l.code)) {
                std::lock_guard lock(g_settingsMtx);
                g_outgoingLang = l.code;
                g_settingsDirty = true;
            }
        }
        ImGui::EndCombo();
    }
    Hint("Start a message with /fr, /de, /es ... to send just that one in another language, or /en to send it as typed.");
}

// The compact chat's name prefixes, shown on a fixed sample so every switch has a
// visible effect. The sample goes through the same code the compact window uses.
void DrawPrefixPreview() {
    const ImVec4 white(0.92f, 0.92f, 0.92f, 1);
    struct Sample { std::vector<ChatSeg> segs; std::string speaker; };
    static const std::vector<Sample> samples = {
        { { { "[P. 69 \xC2\xB7" " Lvl. 7415] ", white }, { "[", white }, { "Head Builder", ImVec4(1.0f, 0.45f, 0.10f, 1) }, { "] ", white },
            { "Beaverplzr", ImVec4(1.0f, 0.67f, 0.0f, 1) }, { ": ", white }, { "wb death o/", ImVec4(1.0f, 0.55f, 0.20f, 1) } }, "Beaverplzr" },
        { { { "[P. 6 \xC2\xB7" " Lvl. 841] ", white }, { "[", white }, { "Titan", ImVec4(0.30f, 0.30f, 1.0f, 1) }, { "] ", white },
            { "Ro4", white }, { ": ", white }, { "u made ur own sun ???", ImVec4(0.33f, 1.0f, 1.0f, 1) } }, "Ro4" },
        { { { "[P. 130 \xC2\xB7" " Lvl. 3905] ", white }, { "[", white }, { "Divine", ImVec4(0.40f, 1.0f, 0.55f, 1) }, { "] ", white },
            { "Warden \xE2\xB8\xBA" " ", ImVec4(0.20f, 0.65f, 0.40f, 1) }, { "HitShift_MC", ImVec4(1.0f, 0.35f, 0.35f, 1) },
            { ": ", white }, { "a", white } }, "HitShift_MC" },
        { { { "[Lvl. 12] ", white }, { "Newbie", white }, { ": ", white }, { "how do i get a pet?", ImVec4(0.85f, 1.0f, 0.35f, 1) } }, "Newbie" },
        { { { "[!] ", ImVec4(1.0f, 0.67f, 0.0f, 1) }, { "No one answered correctly!", ImVec4(1.0f, 0.33f, 0.33f, 1) } }, "" },
    };

    const ChatPrefix::Options o = ChatPrefix::FromPrefs(g_prefs);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, g_prefs.compactBg);
    ImGui::BeginChild("##pfxprev", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * (float)samples.size() + 16.0f),
        ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleColor();
    for (const Sample& sm : samples) {
        ChatLine L;
        L.segs = sm.segs;
        L.speaker = sm.speaker;
        std::vector<ChatSeg> shown;
        const std::vector<ChatSeg>& segs = ChatPrefix::Simplify(L, o, shown) ? shown : L.segs;
        bool first = true;
        for (const ChatSeg& g : segs) {
            if (g.text.empty()) continue;
            if (!first) ImGui::SameLine(0.0f, 0.0f);
            first = false;
            ImGui::PushStyleColor(ImGuiCol_Text, ChatUi::DisplayColor(g.color));
            ImGui::TextUnformatted(g.text.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
}

// A sentence through the reply clean-up as it is set right now.
void DrawCleanupPreview() {
    AiText::PostOptions o;
    o.lowercase = g_prefs.aiPostLower; o.stripEmoji = g_prefs.aiPostStripEmoji; o.stripMarkdown = g_prefs.aiPostStripMarkdown;
    o.maxChars = g_prefs.aiPostMaxChars; o.blocklist = g_prefs.aiPostBlocklist; o.blockAction = g_prefs.aiPostBlockAction;
    o.prefix = g_prefs.aiPostPrefix; o.suffix = g_prefs.aiPostSuffix;
    const std::string sample = "**Hey Steve!** \xF0\x9F\x98\x80 The forge is at spawn - go north for the best iron. Darn, that is a long way.";
    const AiText::PostResult r = AiText::PostProcess(sample, o);
    ImGui::TextDisabled("Example in:");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", sample.c_str());
    ImGui::TextDisabled("Example out:");
    ImGui::SameLine();
    if (r.blocked) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1), "held for review (%s)", r.notes.empty() ? "blocked word" : r.notes.back().c_str());
    else ImGui::TextWrapped("%s", r.text.c_str());
}

void PageAiControl() {
    Hint("How the bot's replies are handled, what it may do, and what it is sent. The live views - inbox, prompt editor and "
         "the exact context - are on the AI Studio tab.");
    if (ImGui::Button("Open AI Studio")) UiOpenStudioPage(0);

    Section("When the bot has an answer");
    static const char* modes[] = { "Send it by itself", "Ask me first (a draft waits for my OK)", "Dry run (never send, just keep drafts)" };
    PrefCombo("Reply mode", g_prefs.aiReplyMode, modes, 3,
        "Ask-me-first drafts appear on the AI Studio tab, in the compact chat and as a line in the chat log.\n"
        "Dry run lets you try a persona on a live server without it ever speaking.", 320.0f);
    PrefBool("Ask before the model is called at all", g_prefs.aiAskBeforeModel,
        "Every wake waits for your OK first, so nothing is sent to the model (or billed) until you say so.");
    PrefBool("Replies to whitelisted players still go out by themselves", g_prefs.aiAutoWhitelist,
        "Only in \"ask me first\". The player list is on the Ignored tab.");
    PrefInt("A draft nobody answers is handled after", g_prefs.aiDraftTimeoutSec, 0, 600, g_prefs.aiDraftTimeoutSec == 0 ? "never" : "%d s",
        "Keeps a forgotten draft from going stale.");
    if (g_prefs.aiDraftTimeoutSec > 0) {
        static const char* acts[] = { "Discard it", "Send it" };
        ImGui::Indent(16.0f);
        PrefCombo("then", g_prefs.aiDraftTimeoutSend, acts, 2);
        ImGui::Unindent(16.0f);
    }
    PrefInt("Wakes kept while the AI is busy", g_prefs.aiQueueMax, 0, 10, g_prefs.aiQueueMax == 0 ? "none (skipped)" : "%d",
        "0 = a wake while it is still answering is ignored, as before.");

    Section("Telling me something is waiting");
    PrefBool("A line in the chat log", g_prefs.aiNoteInChat, "\"[AI] reply to Steve: ...  -  F6 send, F7 discard\" - visible in the compact chat too.");
    PrefBool("A sound", g_prefs.aiDraftSound, "The mention sound if you set one, else the Windows notification sound.");
    PrefBool("Flash Hytale's taskbar button", g_prefs.aiDraftFlash);
    Hint("The send / discard / retry keys are on the Hotkeys page.");

    Section("Who can wake it, and how often");
    PrefBool("The wake word only counts at the start of a message", g_prefs.aiWakeAtStart,
        "\"hyai what is X\" and \"hey hyai, ...\" wake it; \"does hyai know X\" doesn't.");
    PrefInt("Gap between any two answers", g_prefs.aiCooldownSec, 0, 300, g_prefs.aiCooldownSec == 0 ? "none" : "%d s");
    PrefInt("Gap between answers to one player", g_prefs.aiPlayerCooldownSec, 0, 600, g_prefs.aiPlayerCooldownSec == 0 ? "none" : "%d s");
    PrefInt("At most this many answers a minute", g_prefs.aiMaxPerMinute, 0, 60, g_prefs.aiMaxPerMinute == 0 ? "no limit" : "%d");
    if (ImGui::SmallButton("Reset the counters")) AiFlow::ResetLimits();

    Section("Tools");
    static const char* policies[] = { "Run by themselves", "Ask me before tools with side effects or costs", "Ask me before every tool" };
    PrefCombo("When the model wants a tool", g_prefs.aiToolPolicy, policies, 3,
        "Side effects: setting or cancelling reminders, and web searches (which cost API calls).", 340.0f);
    PrefInt("A tool question I don't answer counts as \"no\" after", g_prefs.aiToolWaitSec, 5, 300, "%d s");
    Hint("A rule on a single tool beats the setting above. Off = the model isn't even told the tool exists.");
    {
        std::string rulesText;
        { std::lock_guard lock(g_settingsMtx); rulesText = g_prefs.aiToolRules; }
        const AiText::ToolRules rules = AiText::ParseToolRules(rulesText);
        static const char* ruleNames[] = { "Follow the setting", "Off", "Ask me", "Auto" };
        for (const auto& d : AiTools::All()) {
            const auto it = rules.byName.find(d.name);
            int idx = it == rules.byName.end() ? 0 : it->second == AiText::ToolMode::Off ? 1 : it->second == AiText::ToolMode::Ask ? 2 : 3;
            ImGui::PushID(d.name);
            ImGui::SetNextItemWidth(160);
            if (ImGui::Combo("##rule", &idx, ruleNames, 4))
                AiTools::SetRule(d.name, idx == 1 ? AiText::ToolMode::Off : idx == 2 ? AiText::ToolMode::Ask : AiText::ToolMode::Auto, idx == 0);
            ImGui::SameLine();
            const AiText::ToolMode eff = AiTools::ModeFor(d.name);
            ImGui::TextColored(eff == AiText::ToolMode::Off ? ImVec4(0.6f, 0.6f, 0.65f, 1) : eff == AiText::ToolMode::Ask ? ImVec4(1, 0.8f, 0.35f, 1) : ImVec4(0.45f, 0.9f, 0.55f, 1),
                "%-6s", eff == AiText::ToolMode::Off ? "off" : eff == AiText::ToolMode::Ask ? "asks" : "auto");
            ImGui::SameLine();
            ImGui::Text("%s%s", d.label, d.sideEffect ? " *" : "");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", d.what);
            ImGui::PopID();
        }
        Hint("* has side effects or costs something. Web search also needs its switch on the AI model page.");
    }

    Section("Reply clean-up");
    Hint("Applied to every reply before you see or send it. The unchanged reply stays visible in the draft's details.");
    PrefBool("All lowercase", g_prefs.aiPostLower, "A-Z only.");
    PrefBool("Remove emoji", g_prefs.aiPostStripEmoji);
    PrefBool("Remove markdown (**bold**, `code`, # headings, - bullets)", g_prefs.aiPostStripMarkdown);
    PrefInt("Longest reply", g_prefs.aiPostMaxChars, 0, 600, g_prefs.aiPostMaxChars == 0 ? "no limit" : "%d characters",
        "Cut at the end of a sentence or word.");
    PrefText("Words it must not say", g_prefs.aiPostBlocklist, "Separate with commas or new lines. Whole words, any case.", 420.0f, 60.0f);
    static const char* blockActs[] = { "Replace them with ****", "Hold the reply for my review" };
    PrefCombo("When one appears", g_prefs.aiPostBlockAction, blockActs, 2, "Held replies wait for you even in \"send it by itself\" mode.", 260.0f);
    PrefText("Put in front of every reply", g_prefs.aiPostPrefix, nullptr, 260.0f);
    PrefText("Put after every reply", g_prefs.aiPostSuffix, nullptr, 260.0f);
    DrawCleanupPreview();

    Section("What is sent with each question");
    Hint("See it live on the AI Studio > Context tab.");
    DrawAiContextSettings();

    Section("History");
    PrefBool("Save every exchange to ailogs\\<date>.jsonl", g_prefs.aiSaveHistory, "One JSON line per wake: who, what, the reply, tools, timing.");
    PrefInt("Exchanges kept in the Studio", g_prefs.aiHistoryMax, 10, 500);
    if (ImGui::Button("Open the log folder")) OpenFolder(GetThisModuleDirectory() + L"ailogs");
    ResetButton(PrefGroup::Ai, "AI control");
}

void PageCompact() {
    Hint("A small see-through chat window for playing with. F8 (Hotkeys page) switches between it and this window. "
         "Drag its top edge to move it and its edges to resize; right-click it for quick options.");
    bool compact = IsCompactMode();
    if (ImGui::Checkbox("Use the compact chat window now", &compact) && compact != IsCompactMode())
        ToggleCompactChatMode();

    Section("Look");
    {
        float op, fs, pad;
        int ml;
        { std::lock_guard lock(g_settingsMtx); op = g_compactOpacity; fs = g_compactFontSize; pad = g_compactPadX; ml = g_compactMaxLines; }
        auto commit = [&]() {
            std::lock_guard lock(g_settingsMtx);
            g_compactOpacity = op; g_compactFontSize = fs; g_compactPadX = pad; g_compactMaxLines = ml;
            MarkPrefsChanged();
            g_windowStyleDirty = true;
        };
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("Panel opacity", &op, 0.10f, 1.0f, "%.2f")) commit();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("Text size##c", &fs, 10.0f, 28.0f, "%.0f px")) commit();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("Side padding", &pad, 4.0f, 32.0f, "%.0f px")) commit();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("Lines kept", &ml, 20, 300)) commit();
    }
    PrefColor("Panel colour", g_prefs.compactBg, nullptr, false);
    PrefFloat("Corner radius", g_prefs.compactRadius, 0.0f, 30.0f, "%.0f px");
    PrefText("Font", g_prefs.compactFont, "Any installed font name, e.g. Segoe UI, Consolas, Verdana, Bahnschrift.", 200);
    PrefBool("Text outline", g_prefs.compactShadow, "A dark edge around the text - readable over snow and sky.");
    PrefBool("Timestamps##c", g_prefs.compactTimestamps);
    PrefBool("Emoji for :heart: <3 :) and friends", g_prefs.compactEmoji);
    PrefBool("Show coordinates", g_prefs.compactShowPos, "When the Position tab has found them.");

    Section("Before player names");
    Hint("Some servers put a level, a rank and a title in front of every name. Pick what the compact chat shows; "
         "the full window, copies and the saved log always keep the whole line.");
    DrawPrefixPreview();
    if (ImGui::Button("Just names")) {
        std::lock_guard lock(g_settingsMtx);
        g_prefs.compactShowPrestige = g_prefs.compactShowLevel = g_prefs.compactShowTitle = false;
        g_prefs.compactRankMode = 1;
        MarkPrefsChanged();
    }
    ImGui::SameLine();
    if (ImGui::Button("Show everything")) {
        std::lock_guard lock(g_settingsMtx);
        g_prefs.compactShowPrestige = g_prefs.compactShowLevel = g_prefs.compactShowTitle = true;
        g_prefs.compactRankMode = 0;
        MarkPrefsChanged();
    }
    PrefBool("Prestige   [P. 69 ...]##pfx", g_prefs.compactShowPrestige,
        "The prestige half of an island tag. Tags that only have a level (nobody prestiged yet) are unaffected.");
    PrefBool("Level   [... Lvl. 7415]##pfx", g_prefs.compactShowLevel);
    static const char* const rankModes[] = {
        "Show all", "Hide all", "Hide all except the ones listed", "Hide only the ones listed" };
    PrefCombo("Rank   [Titan]", g_prefs.compactRankMode, rankModes, 4,
        "The tag in brackets that isn't a level. \"Hide all except\" is handy for keeping staff ranks visible.", 260.0f);
    if (g_prefs.compactRankMode >= 2)
        PrefText("Ranks##pfx", g_prefs.compactRankList,
            "Rank names separated by commas, e.g. Head Mod, Head Builder, Owner. Capitals don't matter.", 300);
    PrefBool("Title   (Warden \xE2\x80\x94)", g_prefs.compactShowTitle,
        "A title before the name that ends in a dash or a symbol.");

    Section("When the mouse isn't over it");
    PrefBool("Hide the panel background", g_prefs.compactHideBgIdle, "Only the text floats over the game, like vanilla chat.");
    PrefInt("Hide lines older than", g_prefs.compactFadeSec, 0, 600, g_prefs.compactFadeSec == 0 ? "never" : "%d s",
        "Old lines come back while the mouse is over the window.");

    Section("Position");
    if (ImGui::Button("Put it back in the bottom-left corner")) {
        std::lock_guard lock(g_settingsMtx);
        g_prefs.compactX = g_prefs.compactY = -1;
        g_prefs.compactW = 440;
        g_prefs.compactH = 210;
        MarkPrefsChanged();
        if (IsCompactMode()) { ToggleCompactChatMode(); ToggleCompactChatMode(); }
    }
    ResetButton(PrefGroup::Compact);
}

void PageWindow() {
    Section("Window");
    bool top;
    float opacity;
    { std::lock_guard lock(g_settingsMtx); top = g_alwaysOnTop; opacity = g_windowOpacity; }
    if (ImGui::Checkbox("Always on top", &top)) { std::lock_guard lock(g_settingsMtx); g_alwaysOnTop = top; g_settingsDirty = true; g_windowStyleDirty = true; }
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderFloat("Opacity", &opacity, 0.30f, 1.00f, "%.2f")) { std::lock_guard lock(g_settingsMtx); g_windowOpacity = opacity; g_settingsDirty = true; g_windowStyleDirty = true; }
    PrefBool("Start hidden (show with the overlay hotkey)", g_prefs.startHidden);

    Section("Theme");
    int theme;
    { std::lock_guard lock(g_settingsMtx); theme = g_themePreset; }
    const char* themes[] = { "Slate", "Purple", "Green", "Blue", "Crimson", "Custom" };
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("Theme", &theme, themes, 6)) { std::lock_guard lock(g_settingsMtx); g_themePreset = theme; g_themeDirty = true; g_settingsDirty = true; }
    if (theme == 5) {
        ImGui::Indent(16.0f);
        bool ch = ImGui::ColorEdit3("Accent colour", g_customAccent, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel);
        Tip("Every element of the window is generated from this one colour.");
        ch |= ImGui::Checkbox("Darker background", &g_customAccentDark);
        if (ch) { g_themeDirty = true; g_settingsDirty = true; }
        ImGui::Unindent(16.0f);
    }
    {
        float fs = g_fontSize, us = g_uiScale, ur = g_uiRounding;
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("Font size", &fs, 11.0f, 26.0f, "%.0f px")) { g_fontSize = fs; g_fontDirty = true; g_settingsDirty = true; g_prefsVersion.fetch_add(1); }
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("UI scale", &us, 0.75f, 1.60f, "%.2f")) { g_uiScale = us; g_fontDirty = true; g_settingsDirty = true; g_prefsVersion.fetch_add(1); }
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("Corner rounding", &ur, 0.0f, 12.0f, "%.0f")) { g_uiRounding = ur; g_themeDirty = true; g_settingsDirty = true; }
    }

    Section("Font");
    if (PrefText("Font file", g_prefs.uiFontPath, "A .ttf / .otf / .ttc file. Empty = Segoe UI.", 360)) g_fontDirty = true;
    ImGui::SameLine();
    if (ImGui::Button("Browse...##font")) {
        const std::string f = BrowseFile(L"Fonts (*.ttf;*.otf;*.ttc)\0*.ttf;*.otf;*.ttc\0", L"Choose the UI font");
        if (!f.empty()) { std::lock_guard lock(g_settingsMtx); g_prefs.uiFontPath = f; MarkPrefsChanged(); g_fontDirty = true; }
    }
    if (PrefBool("Fallback fonts for symbols, emoji, Chinese, Japanese and Korean", g_prefs.extraFonts,
        "Without them those characters show as boxes. Costs some memory the first time they appear."))
        g_fontDirty = true;

    Section("Tabs");
    Hint("Hide the tabs you never use. Chat and Settings always stay.");
    struct T { const char* name; bool* v; };
    T tabs[] = {
        { "Ignored", &g_prefs.tabIgnored }, { "AI & Translation", &g_prefs.tabAi }, { "Test Chat", &g_prefs.tabTest },
        { "Tools", &g_prefs.tabTools }, { "Position", &g_prefs.tabPosition }, { "Player", &g_prefs.tabPlayer },
        { "Packets", &g_prefs.tabPackets }, { "Shapes", &g_prefs.tabShapes }, { "Build", &g_prefs.tabBuild },
        { "Copy", &g_prefs.tabCopy }, { "Farm", &g_prefs.tabFarm }, { "AI Studio", &g_prefs.tabStudio },
    };
    for (size_t i = 0; i < sizeof(tabs) / sizeof(tabs[0]); ++i) {
        if (i % 5) ImGui::SameLine(0, 16);
        PrefBool(tabs[i].name, *tabs[i].v);
    }
    ResetButton(PrefGroup::Window, "window options");
}

void PageHotkeys() {
    Hint("Click a key, then press the new one. Backspace clears it.");
    Section("Overlay");
    ImGui::AlignTextToFramePadding(); ImGui::Text("Show / hide this window");  ImGui::SameLine(260); HotkeyField("ov", g_prefs.keyToggleOverlay);
    ImGui::AlignTextToFramePadding(); ImGui::Text("Compact chat on / off");    ImGui::SameLine(260); HotkeyField("cp", g_prefs.keyToggleCompact);
    ImGui::AlignTextToFramePadding(); ImGui::Text("Open chat and start typing"); ImGui::SameLine(260); HotkeyField("fc", g_prefs.keyFocusChat);
    PrefBool("Only while Hytale or this overlay is the active window", g_prefs.hotkeysNeedFocus,
        "Off: the keys work everywhere - including while you type in a browser.");

    Section("AI review");
    ImGui::AlignTextToFramePadding(); ImGui::Text("Send the draft / allow the tool");  ImGui::SameLine(260); HotkeyField("aiy", g_prefs.keyAiApprove);
    ImGui::AlignTextToFramePadding(); ImGui::Text("Discard the draft / deny the tool"); ImGui::SameLine(260); HotkeyField("ain", g_prefs.keyAiDiscard);
    ImGui::AlignTextToFramePadding(); ImGui::Text("Ask again (newest reply)");        ImGui::SameLine(260); HotkeyField("air", g_prefs.keyAiRetry);
    Hint("They act on the oldest thing waiting for you, and only when something is - otherwise the key passes through to the game.");

    Section("Build guide");
    bool on = g_buildHotkeys.load();
    if (ImGui::Checkbox("Build guide keys", &on)) g_buildHotkeys.store(on);
    struct K { const char* label; std::atomic<int>* key; };
    K keys[] = { { "Placed it - next block", &g_buildKeyAdvance }, { "Undo", &g_buildKeyUndo }, { "Skip block", &g_buildKeySkip } };
    for (const K& k : keys) {
        int vk = k.key->load();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", k.label);
        ImGui::SameLine(260);
        if (HotkeyField(k.label, vk)) { k.key->store(vk); g_settingsDirty = true; }
    }

    Section("Auto farm");
    ImGui::AlignTextToFramePadding(); ImGui::Text("Start / stop farming");     ImGui::SameLine(260); HotkeyField("fp", g_prefs.keyFarmPlay);
    ImGui::AlignTextToFramePadding(); ImGui::Text("Record a lap / finish it"); ImGui::SameLine(260); HotkeyField("fr", g_prefs.keyFarmRecord);
    ResetButton(PrefGroup::Hotkeys, "all hotkeys");
}

// The record list as removable chips, plus "press the key you want to add".
// Left and right Shift/Ctrl/Alt go in as plain Shift/Ctrl/Alt, which is what
// the game treats them as and matches either physical key.
void FarmKeyChips() {
    const std::vector<int> keys = FarmParseKeyList(g_prefs.farmKeys);
    int remove = -1;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) ImGui::SameLine(0, 6);
        const std::string label = FarmKeyName(keys[i]) + "  x##fk" + std::to_string(i);
        if (ImGui::SmallButton(label.c_str())) remove = (int)i;
        Tip("Remove it");
    }
    if (!keys.empty()) ImGui::SameLine(0, 12);

    static bool capturing = false;
    static bool down[256];
    if (!capturing) {
        if (ImGui::SmallButton("+ add a key")) {
            capturing = true;
            for (int k = 0; k < 256; ++k) down[k] = (GetAsyncKeyState(k) & 0x8000) != 0;
        }
    }
    else {
        ImGui::TextColored(ImVec4(0.98f, 0.80f, 0.35f, 1.0f), "press the key...  (Esc cancels)");
        for (int k = 8; k < 255; ++k) {
            if (k == VK_SHIFT || k == VK_CONTROL || k == VK_MENU || k == VK_LWIN || k == VK_RWIN) continue;
            const bool now = (GetAsyncKeyState(k) & 0x8000) != 0;
            const bool hit = now && !down[k];
            down[k] = now;
            if (!hit) continue;
            capturing = false;
            if (k == VK_ESCAPE) break;
            int vk = k;
            if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
            else if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
            else if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
            std::vector<int> next = keys;
            if (std::find(next.begin(), next.end(), vk) == next.end() && next.size() < 32) next.push_back(vk);
            std::lock_guard lock(g_settingsMtx);
            g_prefs.farmKeys = FarmFormatKeyList(next);
            MarkPrefsChanged();
            break;
        }
    }
    if (remove >= 0) {
        std::vector<int> next = keys;
        next.erase(next.begin() + remove);
        std::lock_guard lock(g_settingsMtx);
        g_prefs.farmKeys = FarmFormatKeyList(next);
        MarkPrefsChanged();
    }
}

void PageAutoFarm() {
    Hint("Record a lap on the Farm tab and it drives that lap for you. Everything here changes how.");

    Section("Keys it records and presses");
    FarmKeyChips();
    PrefText("Keys", g_prefs.farmKeys,
        "Separated by spaces. Letters and digits as themselves, and Space, Shift, Ctrl, Alt, Tab,\n"
        "Up, Down, Left, Right, LMB, RMB, MMB, Mouse4, Mouse5, F1-F24.", 360);
    Hint("Only these go into a recording, and only these are ever pressed for you. Laps you already recorded "
         "keep the keys they were recorded with.");

    Section("How keys reach Hytale");
    const char* modes[] = { "Automatic", "Background - Hytale can be behind other windows",
                            "Direct - Hytale has to be the active window" };
    PrefCombo("Send keys", g_prefs.farmInputMode, modes, 3,
        "Background posts the keys to Hytale's window, so you can use other windows while it farms.\n"
        "Direct types them like a keyboard, which every game understands, but only the window in front gets them.\n"
        "Automatic tries background first and switches to direct if holding the keys doesn't move you.", 380);
    const int learned = g_prefs.farmInputLearned;
    ImGui::TextDisabled("%s", learned == 1 ? "Found: background keys work on this PC."
                            : learned == 2 ? "Found: Hytale needs direct keys, so it has to stay in front."
                                           : "Not found out yet - the first run will.");
    if (learned) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Find out again")) { std::lock_guard lock(g_settingsMtx); g_prefs.farmInputLearned = 0; MarkPrefsChanged(); }
    }

    Section("Camera");
    const char* steer[] = { "Turn it when the recorded lap did", "Always face the recorded direction", "Never move the mouse" };
    PrefCombo("Camera", g_prefs.farmSteer, steer, 3,
        "Needs the camera direction set up on the Player tab. Turning it moves the mouse,\n"
        "which only works with Hytale in front.", 380);
    if (g_prefs.farmDegPerCount != 0.0f) {
        ImGui::TextDisabled("Mouse measured at %.3f degrees of turn per count.", std::fabs(g_prefs.farmDegPerCount));
        ImGui::SameLine();
        if (ImGui::SmallButton("Measure again")) { std::lock_guard lock(g_settingsMtx); g_prefs.farmDegPerCount = 0; MarkPrefsChanged(); }
        Tip("Do this after changing your mouse sensitivity.");
    }
    else ImGui::TextDisabled("Mouse sensitivity is measured the first time a lap needs the camera turned.");

    Section("Following the recording");
    PrefBool("Line corners up by the tractor's direction", g_prefs.farmUseRide,
        "Once \"Find tractor direction\" has been done on the Farm tab (and the lap recorded after it):\n"
        "spins end where yours did, and a tractor left a little crooked gets straightened up.");
    PrefInt("Where you are  <->  the recorded timing", g_prefs.farmTiming, 0, 100, "%d",
        "How playback decides which moment of the lap it's at. Left trusts your position more\n"
        "(corrects drift fast, can twitch); right trusts the recording's timing more (smoother\n"
        "through slow corners, slower to notice you're off). 50 suits most farms.", 300);
    PrefFloat("Extra lead", g_prefs.farmExtraLeadMs, -40.0f, 60.0f, "%+.0f ms",
        "Keys are already sent a frame ahead, lined up with the game's frames. Only change this\n"
        "if EVERY turn is late (+) or early (-). One bad corner is better fixed in that farm's steps list.");

    Section("When it stops by itself");
    PrefFloat("Off the route by", g_prefs.farmMaxOff, 1.0f, 20.0f, "%.1f blocks",
        "How far from the recorded line counts as lost. Also how close you must be to start.");
    PrefInt("No progress for", g_prefs.farmStuckSec, 1, 30, "%d s", "Stuck against something, or the tractor stopped.");
    PrefBool("When I press one of its keys myself", g_prefs.farmStopOnKeys,
        "Grab the controls - or open chat or the menu (T, Enter, /, Esc) - and it lets go.\n"
        "Only counts while Hytale is the active window, so you can chat from this overlay while it farms.");
    const char* focus[] = { "Pause until it's back in front", "Stop" };
    PrefCombo("If Hytale isn't in front", g_prefs.farmFocusLoss, focus, 2,
        "Only matters with direct keys or camera turning - background keys don't care.", 260);
    PrefInt("After laps", g_prefs.farmLaps, 0, 500, g_prefs.farmLaps ? "%d" : "never");
    PrefInt("After minutes", g_prefs.farmMinutes, 0, 600, g_prefs.farmMinutes ? "%d" : "never");

    Section("Recording");
    PrefBool("Finish by itself when I'm back where I started", g_prefs.farmAutoFinish,
        "Off: keep recording until you press the record key again. A recording that doesn't end\n"
        "where it started is driven once, not looped.");

    Section("Letting you know");
    PrefBool("Sounds", g_prefs.farmSounds, "Rising for start, falling for stop, low for trouble.");
    PrefBool("Flash Hytale's taskbar button when it stops by itself", g_prefs.farmFlash);
    PrefBool("Show what it's doing in the compact chat window", g_prefs.farmCompact);
    PrefInt("Countdown from the Start button", g_prefs.farmCountdown, 0, 10, "%d s",
        "Time to get your hand off the mouse. The hotkey in game starts straight away.");

    Section("Hotkeys");
    ImGui::AlignTextToFramePadding(); ImGui::Text("Start / stop farming");     ImGui::SameLine(260); HotkeyField("fp2", g_prefs.keyFarmPlay);
    ImGui::AlignTextToFramePadding(); ImGui::Text("Record a lap / finish it"); ImGui::SameLine(260); HotkeyField("fr2", g_prefs.keyFarmRecord);

    ResetButton(PrefGroup::Farm, "auto farm settings");
}

void PagePrivate() {
    Hint("A passphrase shared with friends who also run HyAI. Messages go out as AES-256-GCM ciphertext "
         "(~hy1....) that only people with the same passphrase can read. Sending via /msg means only that "
         "player even sees the ciphertext.");
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("Passphrase", g_privatePassBuf, sizeof(g_privatePassBuf), ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    if (ImGui::Button("Set key")) {
        const std::string pass = TrimCopy(g_privatePassBuf);
        Extras::SetPrivatePassphrase(pass);
        LogToFile(Extras::HasPrivateKey() ? "Private key set from UI  fp=" + Extras::PrivateKeyFingerprint()
                                          : std::string("Private key set failed from UI"));
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear key")) {
        Extras::ClearPrivateKey();
        g_privateMode = false;
        g_privatePassBuf[0] = 0;
    }
    if (Extras::HasPrivateKey())
        ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f), "Key loaded   fingerprint %s", Extras::PrivateKeyFingerprint().c_str());
    else
        ImGui::TextDisabled("No key loaded");
    Tip("Compare fingerprints with a friend: equal means the same passphrase.");
    LegacyBool("Show decrypted messages in the log", g_privateDecryptDisplay);
    LegacyBool("Send via /msg", g_privateViaMsg);

    Section("Send");
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##privtgt", "player (for /msg)", g_privateTargetBuf, sizeof(g_privateTargetBuf));
    ImGui::SetNextItemWidth(360);
    ImGui::InputTextWithHint("##privmsg", "private message...", g_privateMsgBuf, sizeof(g_privateMsgBuf));
    ImGui::SameLine();
    if (ImGui::Button("Send private") && g_privateMsgBuf[0]) {
        if (!Extras::HasPrivateKey())
            PushLocalNote("Set a passphrase first.", ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
        else {
            const std::string tgt = TrimCopy(g_privateTargetBuf);
            if (QueuePrivateEncrypted(g_privateMsgBuf, tgt, g_privateViaMsg && !tgt.empty()))
                g_privateMsgBuf[0] = 0;
            else
                PushLocalNote("Couldn't send: too long for one chat line, or encryption failed.", ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
        }
    }
}

void PageMaintenance() {
    Section("Now");
    if (ImGui::Button("Save settings")) SaveSettings();
    ImGui::SameLine();
    if (ImGui::Button("Reload settings from disk")) {
        LoadSettings();
        LoadIgnored();
        LoadWhitelist();
        AutoReply::Load();
        ReloadSystemPrompt(true);
        g_windowStyleDirty = true;
        g_themeDirty = true;
        g_fontDirty = true;
        std::lock_guard lock(g_settingsMtx);
        strncpy_s(g_localNameBuf, g_localName.c_str(), _TRUNCATE);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload CONTEXT.md")) ReloadSystemPrompt(true);
    if (ImGui::Button("Clear AI memory")) {
        std::lock_guard lock(g_mtx);
        g_memory.clear();
        g_globalChat.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear chat log")) ClearChatLog();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.20f, 0.20f, 0.90f));
    if (ImGui::Button("Cancel AI + send queue")) CancelAI();
    ImGui::PopStyleColor();

    Section("Files");
    if (ImGui::Button("Open the HyAI folder")) OpenFolder(GetThisModuleDirectory());
    Tip("Settings, CONTEXT.md, ignore lists, auto replies and chat logs live here.");
    ImGui::SameLine();
    if (ImGui::Button("Open the debug log")) ShellExecuteW(nullptr, L"open", L"C:\\temp\\hyai_log.txt", nullptr, nullptr, SW_SHOWNORMAL);

    Section("Commands you can type in chat");
    std::string p = "hyai";
    { std::lock_guard lock(g_settingsMtx); if (!g_wakeWord.empty()) p = g_wakeWord; }
    const char* cmds[] = {
        "help", "mute  (toggle)", "cancel  (stop the AI + clear the send queue)", "ignore <name> / unignore <name> / ignorelist",
        "whitelist <name> / unwhitelist <name>", "clearmemory", "reloadprompt", "live  (toggle auto translate)",
        "color  (toggle coloured replies)", "nick  (toggle the nickname trick)", "think  (toggle thinking)",
        "sendlang <code|off>", "echo <text with ${math(1+1)} ${time} ${uptime}>", "context  (write the AI prompt to a file)",
        "reminders / cancelrem <id>", "privkey <passphrase> / privsend <player> <msg> / privsend public <msg>",
    };
    for (const char* c : cmds) ImGui::BulletText("%s %s", p.c_str(), c);
}

struct Page {
    const char* name;
    const char* keywords;
    void (*draw)();
};

const Page kPages[] = {
    { "General",           "name identity mute afk whitelist memory context self trigger slash", PageGeneral },
    { "Bot replies",       "colour color prefix nickname wake word bot name failure chunk gap send", PageBotReplies },
    { "AI model",          "ollama openai provider model key url temperature tokens top think web search langsearch", PageAiModel },
    { "Chat display",      "font size text colour color timestamp time zone format brightness bar stripe zebra indent hide spam ignored history scroll", PageChatDisplay },
    { "Highlights",        "mention name highlight words sound beep wav flash alert notify", PageHighlights },
    { "Copy & saving",     "copy clipboard ctrl+c colour codes rich html discord export save disk log", PageCopy },
    { "Chat box",          "input history whisper msg tab complete", PageInput },
    { "Auto replies",      "auto reply rules regex welcome wb gg minigame morse caesar pattern word value", PageAutoReplies },
    { "Translation",       "translate language google incoming outgoing", PageTranslation },
    { "AI control",        "ai approve review draft ask first dry run mode tool ask allow deny cooldown rate limit wake start clean-up lowercase emoji markdown blocklist context memory chat lines template history log queue notify", PageAiControl },
    { "Compact chat",      "compact overlay small see-through transparent opacity fade font outline emoji coordinates prefix tag rank level prestige title name hide clutter", PageCompact },
    { "Window & theme",    "theme colour accent font size scale rounding opacity on top tabs hide start hidden cjk emoji", PageWindow },
    { "Auto farm",         "farm auto tractor record lap route macro keys afk crop pumpkin camera mouse stop f9 f10", PageAutoFarm },
    { "Hotkeys",           "hotkey key keybind insert f8 shortcut build farm f9 f10", PageHotkeys },
    { "Private chat",      "encrypted private passphrase aes key", PagePrivate },
    { "Maintenance",       "save reload clear memory log cancel folder commands help", PageMaintenance },
};
const int kPageCount = (int)(sizeof(kPages) / sizeof(kPages[0]));

} // namespace

bool UiIsCapturingHotkey() { return g_capturingKey != nullptr; }

void UiOpenSettings(const char* page) {
    g_pageRequest = page ? page : "";
    g_selectTabRequest = true;
}

bool UiTakeSettingsTabRequest() {
    const bool r = g_selectTabRequest;
    g_selectTabRequest = false;
    return r;
}

void DrawSettingsTab() {
    static int current = 0;
    static char search[64] = "";
    if (!g_pageRequest.empty()) {
        for (int i = 0; i < kPageCount; ++i)
            if (g_pageRequest == kPages[i].name) current = i;
        g_pageRequest.clear();
        search[0] = 0;
    }

    const float navW = 180.0f;
    ImGui::BeginChild("##setnav", ImVec2(navW, 0), ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##setsearch", "Find a setting...", search, sizeof(search));
    ImGui::Spacing();
    const std::string q = ToLower(TrimCopy(search));
    int firstMatch = -1;
    bool currentShown = false;
    for (int i = 0; i < kPageCount; ++i) {
        if (!q.empty()) {
            const std::string hay = ToLower(std::string(kPages[i].name) + " " + kPages[i].keywords);
            bool all = true;
            std::istringstream ss(q);
            std::string w;
            while (ss >> w) if (hay.find(w) == std::string::npos) { all = false; break; }
            if (!all) continue;
        }
        if (firstMatch < 0) firstMatch = i;
        if (i == current) currentShown = true;
        if (ImGui::Selectable(kPages[i].name, current == i)) current = i;
    }
    if (firstMatch < 0) ImGui::TextDisabled("Nothing matches.");
    else if (!currentShown) current = firstMatch; // follow the search
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##setpage", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "%s", kPages[current].name);
    kPages[current].draw();
    ImGui::Dummy(ImVec2(0, 12));
    ImGui::EndChild();
}
