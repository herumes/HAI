#include "pch.h"
#include "ui.hpp"
#include "aiflow.hpp"
#include "aicontext.hpp"
#include "aiprompt.hpp"
#include "aitools.hpp"
#include "chat.hpp"
#include "common.hpp"
#include "settings.hpp"
#include "shared.hpp"

#include <imgui_stdlib.h>
#include <commdlg.h>
#include <shellapi.h>

// =============================================================================
// AI Studio
// =============================================================================
// Inbox    - what the bot wants to say, and the buttons to send, edit, retry or drop it
// Prompt   - the system prompt as a stack of preset files, with a live editor
// Context  - exactly what a question would send to the model right now, editable
//
// The logic lives in aiflow.cpp / aiprompt.cpp / aicontext.cpp; this file only draws it.

namespace {

using AiFlow::State;

const ImVec4 kGold(1.0f, 0.80f, 0.35f, 1), kGreen(0.45f, 0.92f, 0.55f, 1), kRed(1.0f, 0.50f, 0.42f, 1);
const ImVec4 kBlue(0.55f, 0.78f, 1.0f, 1), kGrey(0.58f, 0.60f, 0.66f, 1), kViolet(0.78f, 0.66f, 1.0f, 1);

void Tip(const char* tip) {
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tip);
}

void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

bool PrefBool(const char* label, bool& v, const char* tip = nullptr) {
    bool t = v;
    const bool ch = ImGui::Checkbox(label, &t);
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

// Text fields commit when you leave them, so a half-typed template never reaches the model.
bool PrefText(const char* label, std::string& v, const char* tip = nullptr, float width = 420.0f) {
    static std::map<const void*, std::string> buffers;
    static const void* active = nullptr;
    std::string& editing = buffers[&v];
    if (active != &v) editing = v;
    bool committed = false;
    ImGui::PushID(&v);
    ImGui::SetNextItemWidth(width);
    ImGui::InputText(label, &editing);
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

// Two clicks within three seconds: the first arms the button.
bool ConfirmButton(const char* label, const char* armedLabel, const void* key) {
    static const void* armedKey = nullptr;
    static long long armedAt = 0;
    const long long now = NowSteadyMs();
    const bool armed = armedKey == key && now - armedAt < 3000;
    if (armed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.22f, 0.22f, 0.95f));
    const bool clicked = ImGui::Button(armed ? armedLabel : label);
    if (armed) ImGui::PopStyleColor();
    if (!clicked) return false;
    if (armed) { armedKey = nullptr; return true; }
    armedKey = key;
    armedAt = now;
    return false;
}

std::string Ago(long long unixMs) {
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const long long s = (std::max)(0LL, (now - unixMs) / 1000);
    if (s < 60) return std::to_string(s) + "s ago";
    if (s < 3600) return std::to_string(s / 60) + "m ago";
    return std::to_string(s / 3600) + "h ago";
}

std::string Clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t cut = n;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

std::string Comma(long long v) {
    std::string s = std::to_string(v), out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) out += ',';
        out += s[i];
    }
    return out;
}

// A read-only multi-line box that grows with its text, up to a limit.
void ReadOnlyBox(const char* id, const std::string& text, float maxLines = 12.0f) {
    std::string copy = text;
    int lines = 1;
    for (char c : copy) if (c == '\n') ++lines;
    const float lh = ImGui::GetTextLineHeightWithSpacing();
    const float h = (std::min)((float)lines + 1.0f, maxLines) * lh + 6.0f;
    ImGui::InputTextMultiline(id, &copy, ImVec2(-FLT_MIN, h), ImGuiInputTextFlags_ReadOnly);
}

void OpenFolder(const std::wstring& path) {
    CreateDirectoryW(path.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// =============================================================================
// Inbox
// =============================================================================
ImVec4 StateColor(State s) {
    switch (s) {
    case State::Ready: case State::AwaitingRun: case State::AwaitingTool: return kGold;
    case State::Thinking: case State::Queued: return kBlue;
    case State::Sent: return kGreen;
    case State::Failed: return kRed;
    case State::Discarded: return kGrey;
    }
    return kGrey;
}

void DrawToolTrail(const AiFlow::Detail& d) {
    for (const AI::ToolEvent& t : d.tools) {
        const char* what = t.outcome == 0 ? "ran" : t.outcome == 1 ? "declined" : t.outcome == 2 ? "switched off" : "unknown";
        ImGui::TextColored(t.outcome == 0 ? kGreen : kRed, "%s", what);
        ImGui::SameLine();
        ImGui::Text("%s", t.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s  (%lld ms)", Clip(t.args, 90).c_str(), t.ms);
        if (!t.result.empty()) {
            ImGui::Indent(16.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", Clip(t.result, 400).c_str());
            ImGui::PopStyleColor();
            ImGui::Unindent(16.0f);
        }
    }
}

void DrawDetails(const AiFlow::DraftView& v) {
    AiFlow::Detail d;
    if (!AiFlow::Get(v.id, d)) return;
    if (!ImGui::TreeNode("##details", "Details")) return;
    ImGui::TextDisabled("%s  -  %lld ms  -  %d %s", d.view.model.empty() ? "no model yet" : d.view.model.c_str(), d.view.ms,
        d.view.tries, d.view.tries == 1 ? "try" : "tries");
    if (d.view.promptTokens >= 0 || d.view.replyTokens >= 0)
        ImGui::TextDisabled("tokens: %d in, %d out (as reported by the server)", d.view.promptTokens, d.view.replyTokens);
    if (!d.notes.empty()) {
        std::string n;
        for (const auto& s : d.notes) n += (n.empty() ? "" : ", ") + s;
        ImGui::TextDisabled("clean-up: %s", n.c_str());
    }
    if (!d.tools.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kBlue, "Tools");
        DrawToolTrail(d);
    }
    if (!d.attempts.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kBlue, "Earlier replies");
        for (size_t i = 0; i < d.attempts.size(); ++i) {
            ImGui::PushID((int)i);
            ImGui::TextWrapped("%s", d.attempts[i].c_str());
            if (v.state == State::Ready || v.state == State::Discarded || v.state == State::Failed) {
                if (ImGui::SmallButton("Use this one")) AiFlow::UseAttempt(v.id, i);
            }
            ImGui::PopID();
        }
    }
    if (!d.raw.empty() && d.raw != d.view.reply) {
        ImGui::Spacing();
        ImGui::TextColored(kBlue, "What the model wrote");
        ImGui::TextWrapped("%s", d.raw.c_str());
    }
    if (ImGui::TreeNode("##saw", "What the model saw")) {
        if (!d.systemPrompt.empty()) {
            ImGui::TextDisabled("system prompt");
            ReadOnlyBox("##sys", d.systemPrompt, 8.0f);
        }
        if (!d.chatBlock.empty()) {
            ImGui::TextDisabled("chat lines");
            ReadOnlyBox("##chat", d.chatBlock, 8.0f);
        }
        if (!d.requestJson.empty()) {
            if (ImGui::SmallButton("Copy request JSON")) ImGui::SetClipboardText(d.requestJson.c_str());
            Tip("The messages array of the first request - paste it into any OpenAI-style playground.");
        }
        ImGui::TreePop();
    }
    ImGui::TreePop();
}

void DrawDraft(const AiFlow::DraftView& v, uint64_t& activeEdit, std::map<uint64_t, std::string>& edits) {
    ImGui::PushID((int)v.id);
    std::string head = std::string("[") + AiFlow::StateName(v.state) + "]  " + (v.speaker.empty() ? "?" : v.speaker) + "  -  " +
                       Clip(v.prompt.substr(v.prompt.find(": ") != std::string::npos && v.prompt.find(": ") < 40 ? v.prompt.find(": ") + 2 : 0), 70) +
                       "###draft";
    ImGui::PushStyleColor(ImGuiCol_Text, StateColor(v.state));
    const bool open = ImGui::CollapsingHeader(head.c_str(), AiFlow::NeedsMe(v.state) ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::PopStyleColor();
    if (!open) { ImGui::PopID(); return; }

    ImGui::Indent(10.0f);
    ImGui::TextDisabled("%s  -  %s", Ago(v.whenMs).c_str(),
        v.source == AiFlow::Source::Studio ? "asked from the Studio" : v.source == AiFlow::Source::Retry ? "retried" : "wake word");
    ImGui::TextWrapped("%s", v.prompt.c_str());
    ImGui::Spacing();

    auto keyHint = [](int vk) { const std::string k = AiFlow::KeyLabel(vk); return k.empty() ? std::string() : "  (" + k + ")"; };
    int keyA, keyD, keyR;
    { std::lock_guard lock(g_settingsMtx); keyA = g_prefs.keyAiApprove; keyD = g_prefs.keyAiDiscard; keyR = g_prefs.keyAiRetry; }

    switch (v.state) {
    case State::Queued:
        ImGui::TextColored(kBlue, "waiting for the AI to be free");
        if (ImGui::Button("Skip")) AiFlow::Discard(v.id);
        break;
    case State::Thinking:
        ImGui::TextColored(kBlue, "thinking...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) AiFlow::Discard(v.id);
        break;
    case State::AwaitingRun:
        ImGui::TextColored(kGold, "Waiting for your OK before the model is asked.");
        if (ImGui::Button(("Ask the AI" + keyHint(keyA)).c_str())) AiFlow::Approve(v.id);
        ImGui::SameLine();
        if (ImGui::Button(("Skip" + keyHint(keyD)).c_str())) AiFlow::Discard(v.id);
        break;
    case State::AwaitingTool: {
        ImGui::TextColored(kGold, "The AI wants to run a tool:");
        ImGui::Text("%s", v.askTool.c_str());
        static std::map<uint64_t, std::string> argEdits;
        static uint64_t activeArgs = 0;
        std::string& args = argEdits[v.id];
        if (activeArgs != v.id) args = v.askArgs;
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##args", &args);
        if (ImGui::IsItemActive()) activeArgs = v.id; else if (activeArgs == v.id) activeArgs = 0;
        Tip("The tool's arguments. Edit them before allowing if you want it to run with something else.");
        if (ImGui::Button(("Allow" + keyHint(keyA)).c_str())) AiFlow::AnswerTool(v.id, true, args);
        ImGui::SameLine();
        if (ImGui::Button("Always allow")) { AiTools::SetRule(v.askTool, AiText::ToolMode::Auto, false); AiFlow::AnswerTool(v.id, true, args); }
        Tip("Sets this tool to \"auto\" on the AI control page.");
        ImGui::SameLine();
        if (ImGui::Button(("Deny" + keyHint(keyD)).c_str())) AiFlow::AnswerTool(v.id, false, "");
        ImGui::SameLine();
        if (ImGui::Button("Never")) { AiTools::SetRule(v.askTool, AiText::ToolMode::Off, false); AiFlow::AnswerTool(v.id, false, ""); }
        Tip("Switches this tool off completely.");
        break;
    }
    case State::Ready: case State::Sent: case State::Discarded: case State::Failed: {
        if (v.state == State::Failed) ImGui::TextColored(kRed, "%s", v.error.c_str());
        if (!v.note.empty()) ImGui::TextDisabled("%s", v.note.c_str());
        const bool editable = v.state != State::Sent;
        if (editable) {
            std::string& buf = edits[v.id];
            if (activeEdit != v.id) buf = v.reply;
            const int rows = (int)(std::min<size_t>(6, 1 + v.reply.size() / 70));
            if (ImGui::InputTextMultiline("##reply", &buf, ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * (float)(rows + 1) + 6.0f)))
                AiFlow::SetReply(v.id, buf);
            if (ImGui::IsItemActive()) activeEdit = v.id; else if (activeEdit == v.id) activeEdit = 0;
            const size_t chunk = MAX_CHAT_CHUNK ? MAX_CHAT_CHUNK : 100;
            ImGui::TextDisabled("%zu characters, %zu chat line(s)", buf.size(), (buf.size() + chunk - 1) / chunk);
        }
        else {
            ImGui::TextWrapped("%s", v.reply.c_str());
        }
        ImGui::Spacing();
        if (v.state == State::Ready) {
            if (ImGui::Button(("Send" + keyHint(keyA)).c_str())) AiFlow::Approve(v.id);
            ImGui::SameLine();
        }
        else if (v.state == State::Discarded && !v.reply.empty()) {
            if (ImGui::Button("Send anyway")) AiFlow::Approve(v.id);
            ImGui::SameLine();
        }
        if (ImGui::Button(("Retry" + keyHint(keyR)).c_str())) AiFlow::Retry(v.id);
        Tip("Ask again with the same chat lines the first try saw.");
        ImGui::SameLine();
        if (ImGui::Button("Retry with a note...")) ImGui::OpenPopup("note");
        if (ImGui::BeginPopup("note")) {
            static std::string note;
            static bool fresh = false;
            ImGui::TextDisabled("An instruction for this one reply only:");
            ImGui::SetNextItemWidth(360);
            const bool enter = ImGui::InputTextWithHint("##note", "shorter, funnier, in French...", &note, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Checkbox("Use the latest chat instead of what it saw", &fresh);
            if (enter || ImGui::Button("Retry")) {
                AiFlow::Retry(v.id, note, fresh);
                note.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (v.state == State::Ready) {
            if (ImGui::Button(("Discard" + keyHint(keyD)).c_str())) AiFlow::Discard(v.id);
        }
        else if (ImGui::Button("Remove")) AiFlow::Forget(v.id);
        break;
    }
    }
    ImGui::Spacing();
    DrawDetails(v);
    ImGui::Unindent(10.0f);
    ImGui::Spacing();
    ImGui::PopID();
}

void DrawModeStrip() {
    int mode;
    bool askFirst;
    { std::lock_guard lock(g_settingsMtx); mode = g_prefs.aiReplyMode; askFirst = g_prefs.aiAskBeforeModel; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("When someone says the wake word");
    ImGui::SameLine();
    int m = mode;
    ImGui::RadioButton("Reply by itself", &m, 0); Tip("Every reply goes into chat as soon as it is ready - how it always worked.");
    ImGui::SameLine();
    ImGui::RadioButton("Ask me first", &m, 1); Tip("Replies wait here as drafts until you send them.");
    ImGui::SameLine();
    ImGui::RadioButton("Dry run", &m, 2); Tip("Never speaks. Drafts are only kept here, so you can try a persona on a live server.");
    if (m != mode) { std::lock_guard lock(g_settingsMtx); g_prefs.aiReplyMode = m; MarkPrefsChanged(); }

    PrefBool("Ask before calling the model", g_prefs.aiAskBeforeModel,
        "Each wake waits for your OK before anything is sent to the model - for paid APIs.");
    (void)askFirst;
    ImGui::SameLine(0, 24);
    if (g_aiInProgress.load()) {
        ImGui::TextColored(kBlue, "thinking...");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) CancelAI();
    }
    else ImGui::TextDisabled("idle");
    ImGui::SameLine(0, 24);
    if (ImGui::SmallButton("Clear finished")) AiFlow::ClearFinished();
    ImGui::SameLine();
    if (ImGui::SmallButton("Settings...")) UiOpenSettings("AI control");
    const auto skips = AiFlow::RecentSkips();
    if (!skips.empty()) {
        std::string s = "skipped: ";
        for (size_t i = skips.size() > 3 ? skips.size() - 3 : 0; i < skips.size(); ++i) s += (i + 1 < skips.size() ? skips[i] + "; " : skips[i]);
        ImGui::TextDisabled("%s", s.c_str());
    }
}

void DrawAskBox() {
    if (!ImGui::CollapsingHeader("Ask the AI yourself")) return;
    static std::string who, msg;
    if (who.empty()) { std::lock_guard lock(g_settingsMtx); who = g_localName.empty() ? "You" : g_localName; }
    ImGui::TextDisabled("Runs the real thing - your prompts, memory, chat context and tools - but nothing is said in game until you press Send.");
    ImGui::SetNextItemWidth(140);
    ImGui::InputText("As", &who);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-90);
    const bool enter = ImGui::InputTextWithHint("##ask", "what is a kweebec?", &msg, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Ask") || enter) && !TrimCopy(msg).empty()) {
        AiFlow::Submit((who.empty() ? std::string("You") : who) + ": " + msg, who, AiFlow::Source::Studio);
        msg.clear();
        ImGui::SetKeyboardFocusHere(-1);
    }
}

void DrawInbox() {
    DrawModeStrip();
    ImGui::Separator();
    DrawAskBox();

    static uint64_t activeEdit = 0;
    static std::map<uint64_t, std::string> edits;
    const std::vector<AiFlow::DraftView> list = AiFlow::List();
    for (auto it = edits.begin(); it != edits.end();) {
        bool live = false;
        for (const auto& v : list) if (v.id == it->first) live = true;
        it = live ? std::next(it) : edits.erase(it);
    }

    if (list.empty()) {
        ImGui::Spacing();
        Hint("Nothing yet. When someone says the wake word, what the bot wants to say shows up here - and in \"Ask me first\" "
             "mode it waits for you. With the overlay hidden, the compact chat shows it and the send / discard keys work.");
        return;
    }
    ImGui::BeginChild("##draftlist", ImVec2(0, 0), ImGuiChildFlags_None);
    // Things that need me first, then the rest, newest first.
    for (int pass = 0; pass < 2; ++pass)
        for (auto it = list.rbegin(); it != list.rend(); ++it)
            if ((pass == 0) == AiFlow::NeedsMe(it->state)) DrawDraft(*it, activeEdit, edits);
    ImGui::EndChild();
}

// =============================================================================
// Prompt
// =============================================================================
struct Buf {
    std::string text, disk;
    bool loaded = false;
    long long lastEditMs = 0, checkedMs = 0;
    bool Dirty() const { return loaded && text != disk; }
};
std::map<std::string, Buf> g_bufs;
std::string g_sel = AiPrompt::kBase;
std::string g_status;
long long g_statusMs = 0;

void SetStatus(const std::string& s) { g_status = s; g_statusMs = NowSteadyMs(); }

Buf& BufFor(const std::string& name) {
    Buf& b = g_bufs[name];
    const long long now = NowSteadyMs();
    if (!b.loaded) {
        std::string t;
        AiPrompt::Read(name, t);
        b.text = b.disk = t;
        b.loaded = true;
        b.checkedMs = now;
    }
    else if (!b.Dirty() && now - b.checkedMs > 1500) {          // pick up outside edits (a text editor, a copy)
        b.checkedMs = now;
        std::string t;
        if (AiPrompt::Read(name, t) && t != b.disk) { b.text = b.disk = t; }
    }
    return b;
}

bool SaveBuf(const std::string& name) {
    Buf& b = g_bufs[name];
    std::string err;
    if (!AiPrompt::Write(name, b.text, err)) { SetStatus("Could not save: " + err); return false; }
    b.disk = b.text;
    SetStatus("Saved " + name);
    return true;
}

std::string PickFile() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwndOverlay;
    ofn.lpstrFilter = L"Prompt files (*.md;*.txt)\0*.md;*.txt\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Import a prompt";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    return WideToUtf8(file, (int)wcslen(file));
}

void DrawPromptList(const std::vector<AiPrompt::Item>& items) {
    int on = 0;
    size_t bytes = 0;
    for (const auto& it : items) if (it.enabled) { ++on; bytes += it.bytes; }
    ImGui::TextDisabled("%d of %zu presets in use, about %d tokens", on, items.size(), (int)(bytes / 4));

    for (const auto& it : items) {
        ImGui::PushID(it.name.c_str());
        bool en = it.enabled;
        if (ImGui::Checkbox("##on", &en)) AiPrompt::SetEnabled(it.name, en);
        Tip("In use: its text is part of the system prompt, in the order listed here.");
        ImGui::SameLine();
        const Buf* b = g_bufs.count(it.name) ? &g_bufs[it.name] : nullptr;
        std::string label = it.name + (it.base ? "  (base)" : "") + (b && b->Dirty() ? "  *" : "");
        if (!it.exists) label += "  (empty)";
        if (!it.enabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(label.c_str(), g_sel == it.name, 0, ImVec2(ImGui::GetContentRegionAvail().x - 78.0f, 0))) g_sel = it.name;
        if (!it.enabled) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("^")) AiPrompt::Move(it.name, -1);
        Tip("Earlier in the prompt");
        ImGui::SameLine();
        if (ImGui::SmallButton("v")) AiPrompt::Move(it.name, 1);
        Tip("Later in the prompt");
        ImGui::PopID();
    }
}

void DrawPromptTab() {
    const std::vector<AiPrompt::Item> items = AiPrompt::Items();
    bool found = false;
    for (const auto& it : items) if (it.name == g_sel) found = true;
    if (!found) g_sel = AiPrompt::kBase;
    const AiPrompt::Item* cur = nullptr;
    for (const auto& it : items) if (it.name == g_sel) cur = &it;

    const float listW = 300.0f;
    ImGui::BeginChild("##presets", ImVec2(listW, 0), ImGuiChildFlags_Borders);
    DrawPromptList(items);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    static std::string nameBuf;
    if (ImGui::Button("New")) { nameBuf.clear(); ImGui::OpenPopup("newpreset"); }
    if (ImGui::BeginPopup("newpreset")) {
        ImGui::TextDisabled("Name of the new preset:");
        ImGui::SetNextItemWidth(240);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##nn", &nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((enter || ImGui::Button("Create")) && !TrimCopy(nameBuf).empty()) {
            std::string made, err;
            if (AiPrompt::Create(nameBuf, "Describe how the bot should behave here.\n", made, err)) { g_sel = made; AiPrompt::SetEnabled(made, true); SetStatus("Created " + made); }
            else SetStatus("Could not create: " + err);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Starters")) ImGui::OpenPopup("starters");
    Tip("Ready-made presets to start from: a persona, a style, rules of the room.");
    if (ImGui::BeginPopup("starters")) {
        ImGui::TextDisabled("Adds a copy you can edit (switched on).");
        for (const auto& s : AiPrompt::Starters()) {
            if (ImGui::Selectable(s.name)) {
                std::string made, err;
                if (AiPrompt::Create(s.name, s.text, made, err)) { AiPrompt::SetEnabled(made, true); g_sel = made; SetStatus("Added " + made); }
                else SetStatus("Could not add: " + err);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Import...")) {
        const std::string path = PickFile();
        if (!path.empty()) {
            std::ifstream f(Utf8ToWide(path), std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string stem = path.substr(path.find_last_of("\\/") == std::string::npos ? 0 : path.find_last_of("\\/") + 1);
            const size_t dot = stem.find_last_of('.');
            if (dot != std::string::npos) stem.resize(dot);
            std::string made, err;
            if (AiPrompt::Create(stem, text, made, err)) { g_sel = made; SetStatus("Imported " + made); }
            else SetStatus("Could not import: " + err);
        }
    }

    if (cur && !cur->base) {
        if (ImGui::Button("Duplicate")) {
            std::string made, err;
            if (AiPrompt::Create(cur->name + " copy", g_bufs[cur->name].text, made, err)) { g_sel = made; SetStatus("Duplicated as " + made); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Rename")) { nameBuf = cur->name; ImGui::OpenPopup("renamepreset"); }
        if (ImGui::BeginPopup("renamepreset")) {
            ImGui::SetNextItemWidth(240);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("##rn", &nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
            if (enter || ImGui::Button("Rename")) {
                std::string err;
                const std::string old = cur->name;
                if (AiPrompt::Rename(old, nameBuf, err)) {
                    const std::string neu = AiText::SafePresetName(nameBuf);
                    g_bufs[neu] = g_bufs[old];
                    g_bufs.erase(old);
                    g_sel = neu;
                    SetStatus("Renamed");
                }
                else SetStatus("Could not rename: " + err);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ConfirmButton("Delete", "Really delete?", cur)) {
            std::string err;
            const std::string old = cur->name;
            if (AiPrompt::Remove(old, err)) { g_bufs.erase(old); g_sel = AiPrompt::kBase; SetStatus("Deleted " + old); }
            else SetStatus("Could not delete: " + err);
        }
    }
    if (ImGui::Button("Open folder")) OpenFolder(AiPrompt::Dir());
    Tip("prompts\\ next to the DLL. Any .md file you drop there shows up in this list (switched off).");
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##editor", ImVec2(0, 0), ImGuiChildFlags_None);
    if (!cur) { ImGui::EndChild(); return; }
    Buf& b = BufFor(cur->name);

    ImGui::TextColored(kBlue, "%s", cur->name.c_str());
    ImGui::SameLine();
    if (b.Dirty()) ImGui::TextColored(kGold, "unsaved changes");
    else if (!g_status.empty() && NowSteadyMs() - g_statusMs < 6000) ImGui::TextDisabled("%s", g_status.c_str());
    else ImGui::TextDisabled("%s", cur->enabled ? "in use" : "switched off");

    const bool saveKey = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false);
    ImGui::BeginDisabled(!b.Dirty());
    if (ImGui::Button("Save") || (saveKey && b.Dirty())) SaveBuf(cur->name);
    Tip("Ctrl+S. Writes the file; the next question already uses it.");
    ImGui::SameLine();
    if (ImGui::Button("Revert")) { b.text = b.disk; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool autoSave;
    { std::lock_guard lock(g_settingsMtx); autoSave = g_prefs.aiAutoSavePrompt; }
    PrefBool("Save as I type", g_prefs.aiAutoSavePrompt, "Saves a moment after you stop typing.");
    (void)autoSave;

    const float footer = 26.0f * 1.0f + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    float editorH = ImGui::GetContentRegionAvail().y * 0.52f;
    if (editorH < 140.0f) editorH = 140.0f;
    if (ImGui::InputTextMultiline("##text", &b.text, ImVec2(-FLT_MIN, editorH), ImGuiInputTextFlags_AllowTabInput))
        b.lastEditMs = NowSteadyMs();
    {
        bool as;
        { std::lock_guard lock(g_settingsMtx); as = g_prefs.aiAutoSavePrompt; }
        if (as && b.Dirty() && b.lastEditMs && NowSteadyMs() - b.lastEditMs > 1200 && !ImGui::IsItemActive()) SaveBuf(cur->name);
        else if (as && b.Dirty() && b.lastEditMs && NowSteadyMs() - b.lastEditMs > 1800) SaveBuf(cur->name);
    }
    ImGui::TextDisabled("about %d tokens, %zu characters", AiText::EstimateTokens(b.text), b.text.size());
    ImGui::TextDisabled("Variables:");
    static const char* vars[] = { "me", "bot", "speaker", "time", "date", "weekday", "pos", "players", "wake" };
    for (const char* v : vars) {
        // Wrap onto the next line instead of running off the edge.
        const float w = ImGui::CalcTextSize(v).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::GetContentRegionAvail().x < w + 4.0f) ImGui::NewLine();
        else ImGui::SameLine();
        if (ImGui::SmallButton(v)) { b.text += std::string("{") + v + "}"; b.lastEditMs = NowSteadyMs(); }
        Tip("Adds this variable to the end of the text. It is filled in when the prompt is sent:\n"
            "{me} your name, {bot} the bot's name, {speaker} whoever woke it, {time} {date} {weekday},\n"
            "{pos} your coordinates, {players} who spoke lately, {wake} the wake word. Write \\{ for a literal brace.");
    }
    (void)footer;

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("What the model gets (all presets in use, variables filled in)", ImGuiTreeNodeFlags_DefaultOpen)) {
        static std::string sampleSpeaker = "Steve";
        ImGui::SetNextItemWidth(140);
        ImGui::InputText("as if asked by", &sampleSpeaker);
        const AiText::Vars vars2 = AiContext::LiveVars(sampleSpeaker);
        const auto sections = AiPrompt::Sections(vars2);
        if (sections.empty()) {
            ImGui::TextColored(kGold, "Nothing is in use: the model gets no system prompt.");
        }
        else {
            int total = 0;
            std::string all;
            for (const auto& s : sections) {
                total += AiText::EstimateTokens(s.text);
                if (!all.empty()) all += "\n\n";
                all += s.text;
            }
            ImGui::TextDisabled("%zu section(s), about %d tokens", sections.size(), total);
            ReadOnlyBox("##assembled", all, 14.0f);
        }
    }
    ImGui::EndChild();
}

// =============================================================================
// Context
// =============================================================================
struct BarPart { const char* name; int tokens; ImVec4 col; };

void DrawBudgetBar(const AiContext::Built& b) {
    const int reserve = b.numPredict;
    const int ctx = (std::max)(1, b.numCtx);
    const BarPart parts[] = {
        { "system", b.tokSystem, ImVec4(0.45f, 0.62f, 0.95f, 1) },
        { "memory", b.tokMemory, ImVec4(0.70f, 0.55f, 0.95f, 1) },
        { "chat", b.tokChat, ImVec4(0.40f, 0.80f, 0.60f, 1) },
        { "question", b.tokUser, ImVec4(0.95f, 0.75f, 0.35f, 1) },
        { "tools", b.tokTools, ImVec4(0.90f, 0.50f, 0.50f, 1) },
        { "answer room", reserve, ImVec4(0.45f, 0.48f, 0.55f, 1) },
    };
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 16), IM_COL32(30, 32, 42, 255), 4.0f);
    float x = p.x;
    for (const BarPart& bp : parts) {
        const float pw = w * (float)bp.tokens / (float)ctx;
        if (pw < 0.5f) continue;
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2((std::min)(x + pw, p.x + w), p.y + 16), ImGui::ColorConvertFloat4ToU32(bp.col));
        x += pw;
    }
    ImGui::Dummy(ImVec2(w, 18));
    for (const BarPart& bp : parts) {
        ImGui::ColorButton(bp.name, bp.col, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(10, 10));
        ImGui::SameLine(0, 4);
        ImGui::TextDisabled("%s %d", bp.name, bp.tokens);
        ImGui::SameLine(0, 12);
    }
    ImGui::NewLine();
    const int used = b.tokTotal + reserve;
    const float pct = 100.0f * (float)used / (float)ctx;
    if (used > ctx) ImGui::TextColored(kRed, "About %s of %s tokens - over the context window. The model would cut the start off; lower the chat lines or memory.", Comma(used).c_str(), Comma(ctx).c_str());
    else ImGui::TextDisabled("About %s of %s tokens of the context window (%.0f%%), counting the room kept for the answer. Estimates: 4 characters per token.", Comma(used).c_str(), Comma(ctx).c_str(), pct);
}

void DrawContextTab();

} // namespace

// Also used by the Settings > AI control page.
void DrawAiContextSettings() {
    bool includeGlobal;
    int mem, ctx;
    { std::lock_guard lock(g_settingsMtx); includeGlobal = g_includeGlobalChat; mem = g_maxMemory; ctx = g_maxChatContext; }
    if (ImGui::Checkbox("Include server chat in the context", &includeGlobal)) { std::lock_guard lock(g_settingsMtx); g_includeGlobalChat = includeGlobal; g_settingsDirty = true; MarkPrefsChanged(); }
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Chat lines", &ctx, 4, 60)) { std::lock_guard lock(g_settingsMtx); g_maxChatContext = ctx; g_settingsDirty = true; MarkPrefsChanged(); }
    Tip("How many recent chat lines come with each question.");
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Remembered turns", &mem, 0, 20, mem == 0 ? "none" : "%d")) { std::lock_guard lock(g_settingsMtx); g_maxMemory = mem; g_settingsDirty = true; MarkPrefsChanged(); }
    Tip("Earlier question + answer pairs sent back. 0 = every question stands alone.");
    PrefInt("Leave out chat older than", g_prefs.aiChatMaxAgeMin, 0, 120, g_prefs.aiChatMaxAgeMin == 0 ? "never" : "%d min",
        "Old lines make the bot answer things that are long settled.");
    PrefInt("Shorten chat lines to", g_prefs.aiChatMaxLineChars, 0, 500, g_prefs.aiChatMaxLineChars == 0 ? "whole line" : "%d characters");
    PrefBool("Include the bot's own earlier replies", g_prefs.aiChatIncludeBot);
    PrefBool("Include announcements and other lines without a name", g_prefs.aiChatIncludeSystem);
    PrefText("A chat line looks like", g_prefs.aiChatLineFmt,
        "{speaker} {body} {plain} {time}. Lines without a speaker are always sent as they were.");
    PrefText("Text before the chat lines", g_prefs.aiChatIntro, "Variables work here too: {me} {bot} {speaker} {time} ...");
    PrefText("Text before the question", g_prefs.aiChatOutro, "Empty leaves it out.");
    if (ImGui::SmallButton("Back to the default wording")) {
        ChatPrefs d;
        std::lock_guard lock(g_settingsMtx);
        g_prefs.aiChatLineFmt = d.aiChatLineFmt; g_prefs.aiChatIntro = d.aiChatIntro; g_prefs.aiChatOutro = d.aiChatOutro;
        MarkPrefsChanged();
    }
}

namespace {

void DrawContextTab() {
    static std::string speaker = "Steve", question = "hyai what is a kweebec?";
    static std::string note;

    ImGui::TextDisabled("Exactly what the next question would send. Change anything here and it changes for real.");
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("asked by", &speaker);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##q", &question);

    AiContext::Request rq;
    rq.speaker = speaker;
    rq.userPrompt = speaker + ": " + question;
    rq.extraNote = note;
    const AiContext::Built b = AiContext::Build(rq);

    ImGui::Spacing();
    DrawBudgetBar(b);
    ImGui::Spacing();
    ImGui::BeginChild("##ctxscroll", ImVec2(0, 0), ImGuiChildFlags_None);

    // ---- system prompt ----------------------------------------------------------
    {
        char head[96];
        snprintf(head, sizeof(head), "System prompt   about %d tokens   (%d preset%s)###sys", b.tokSystem, b.presets, b.presets == 1 ? "" : "s");
        if (ImGui::CollapsingHeader(head)) {
            if (b.systemPrompt.empty()) ImGui::TextColored(kGold, "No system prompt: every preset is off or empty.");
            else ReadOnlyBox("##sysp", b.systemPrompt, 10.0f);
            if (ImGui::Button("Edit prompts...")) UiOpenStudioPage(1);
        }
    }

    // ---- memory -----------------------------------------------------------------
    {
        char head[96];
        const int memMsgs = (int)std::count_if(b.parts.begin(), b.parts.end(), [](const AiContext::Part& p) { return p.kind == AiContext::Part::Memory; });
        snprintf(head, sizeof(head), "Memory   about %d tokens   (%d message%s)###mem", b.tokMemory, memMsgs, memMsgs == 1 ? "" : "s");
        if (ImGui::CollapsingHeader(head, ImGuiTreeNodeFlags_DefaultOpen)) {
            bool any = false;
            size_t deleteIndex = (size_t)-1;
            for (const AiContext::Part& p : b.parts) {
                if (p.kind != AiContext::Part::Memory) continue;
                any = true;
                ImGui::PushID((int)p.memIndex);
                ImGui::TextColored(p.role == "user" ? kBlue : kGreen, "%s", p.role == "user" ? "asked" : "bot");
                ImGui::SameLine();
                ImGui::TextWrapped("%s", Clip(p.text, 300).c_str());
                if (ImGui::SmallButton("Forget this one")) deleteIndex = p.memIndex;
                ImGui::PopID();
            }
            if (deleteIndex != (size_t)-1) {
                std::lock_guard lock(g_mtx);
                if (deleteIndex < g_memory.size()) g_memory.erase(g_memory.begin() + (long long)deleteIndex);
            }
            if (!any) ImGui::TextDisabled("Empty: the next question stands alone.");
            else if (ConfirmButton("Forget everything", "Really forget?", &g_memory)) { std::lock_guard lock(g_mtx); g_memory.clear(); }
        }
    }

    // ---- chat context -----------------------------------------------------------
    {
        char head[96];
        snprintf(head, sizeof(head), "Chat lines   about %d tokens   (%zu of the last lines)###chat", b.tokChat, b.chatLines.size());
        if (ImGui::CollapsingHeader(head, ImGuiTreeNodeFlags_DefaultOpen)) {
            struct Row { uint64_t id; std::string text; bool hidden; bool fromBot; bool included; };
            std::vector<Row> rows;
            {
                std::lock_guard lock(g_mtx);
                for (const GlobalChatEntry& e : g_globalChat) {
                    Row r;
                    r.id = e.id;
                    r.text = e.speaker.empty() ? e.plain : e.speaker + ": " + e.body;
                    r.hidden = e.hidden;
                    r.fromBot = e.fromBot;
                    r.included = false;
                    rows.push_back(std::move(r));
                }
            }
            for (Row& r : rows) for (const auto& cl : b.chatLines) if (cl.id == r.id) r.included = true;
            if (rows.empty()) ImGui::TextDisabled("No chat has been seen yet.");
            for (const Row& r : rows) {
                ImGui::PushID((int)(r.id & 0x7fffffff));
                bool on = !r.hidden;
                if (ImGui::Checkbox("##inc", &on)) {
                    std::lock_guard lock(g_mtx);
                    for (auto& e : g_globalChat) if (e.id == r.id) e.hidden = !on;
                }
                Tip("Untick to keep this line out of the context.");
                ImGui::SameLine();
                const bool dim = r.hidden || !r.included;
                if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextUnformatted(Clip(r.text, 140).c_str());
                if (dim) ImGui::PopStyleColor();
                if (!r.hidden && !r.included) { ImGui::SameLine(); ImGui::TextDisabled("(left out by a filter below)"); }
                ImGui::PopID();
            }
            if (!rows.empty() && ImGui::SmallButton("Clear chat context")) {
                std::lock_guard lock(g_mtx);
                g_globalChat.clear();
            }
            Tip("Forgets the lines the bot has collected. New chat fills it again.");
        }
    }

    // ---- the question -----------------------------------------------------------
    if (ImGui::CollapsingHeader("The question, as sent###ques", ImGuiTreeNodeFlags_DefaultOpen)) {
        ReadOnlyBox("##quesbox", b.parts.empty() ? std::string() : b.parts.back().text, 12.0f);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##onenote", "one-off instruction for this preview (\"answer in French\")", &note);
        Tip("Shown as its own system message - what \"Retry with a note\" adds.");
    }

    // ---- tools ----------------------------------------------------------------------
    {
        int offered = 0;
        for (const auto& d : AiTools::All()) if (AiTools::ModeFor(d.name) != AiText::ToolMode::Off) ++offered;
        char head[96];
        snprintf(head, sizeof(head), "Tools offered   %d   about %d tokens###tools", offered, b.tokTools);
        if (ImGui::CollapsingHeader(head)) {
            for (const auto& d : AiTools::All()) {
                const AiText::ToolMode m = AiTools::ModeFor(d.name);
                const char* ms = m == AiText::ToolMode::Off ? "off" : m == AiText::ToolMode::Ask ? "asks you" : "auto";
                ImGui::TextColored(m == AiText::ToolMode::Off ? kGrey : m == AiText::ToolMode::Ask ? kGold : kGreen, "%-9s", ms);
                ImGui::SameLine();
                ImGui::Text("%s", d.label);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", d.what);
            }
            if (ImGui::Button("Change tool rules...")) UiOpenSettings("AI control");
        }
    }

    // ---- the customizing controls ---------------------------------------------------------
    if (ImGui::CollapsingHeader("Change what is included###settings")) DrawAiContextSettings();

    // ---- exact request -------------------------------------------------------------------
    if (ImGui::CollapsingHeader("The exact request (JSON)###json")) {
        const std::string json = AiContext::MessagesJson(b);
        if (ImGui::SmallButton("Copy messages JSON")) ImGui::SetClipboardText(json.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy tools JSON")) ImGui::SetClipboardText(b.toolsJson.c_str());
        ReadOnlyBox("##jsonbox", json, 10.0f);
    }
    ImGui::EndChild();
}

int g_subRequest = -1;          // set by UiOpenStudioPage
bool g_tabRequest = false;

} // namespace

void UiOpenStudioPage(int page) { g_subRequest = page; g_tabRequest = true; }

bool UiTakeStudioTabRequest() {
    const bool r = g_tabRequest;
    g_tabRequest = false;
    return r;
}

void DrawAiStudioTab() {
    const int waiting = AiFlow::WaitingCount();
    char inbox[48];
    if (waiting > 0) snprintf(inbox, sizeof(inbox), "Inbox (%d)###studio_inbox", waiting);
    else snprintf(inbox, sizeof(inbox), "Inbox###studio_inbox");

    if (ImGui::BeginTabBar("##studio")) {
        if (ImGui::BeginTabItem(inbox, nullptr, g_subRequest == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            DrawInbox();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Prompt###studio_prompt", nullptr, g_subRequest == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            DrawPromptTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Context###studio_context", nullptr, g_subRequest == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
            DrawContextTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    g_subRequest = -1;
}
