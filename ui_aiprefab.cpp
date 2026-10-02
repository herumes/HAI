// The Build tab's "Make a build with an AI" section.
//
// The flow it exists for: describe a build, copy one ready-made prompt (the game's
// block list, the script format and the design rules are all in it), paste it into
// the best AI you have, copy that AI's reply, paste it back here, and start
// building it. Nothing is sent anywhere by this program: it only reads and writes
// the clipboard, so the AI can be any chat window.
//
// The prompt is ai_prefab_prompt.md next to the DLL, made by tools/make_ai_prompt.py
// from the game's own block registry. The block names an AI-written script uses are
// checked against the same list (ai_prefab_blocks.txt) plus the live registry.
#include "pch.h"
#include "ui.hpp"
#include "buildscript.hpp"
#include "buildsession.hpp"
#include "common.hpp"
#include "prefab.hpp"
#include "settings.hpp"

#include <imgui_stdlib.h>
#include <shellapi.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace {

const ImVec4 kGood(0.45f, 0.85f, 0.55f, 1.0f);
const ImVec4 kBad(1.0f, 0.45f, 0.40f, 1.0f);
const ImVec4 kWarn(0.95f, 0.85f, 0.45f, 1.0f);
const ImVec4 kHead(0.58f, 0.78f, 1.0f, 1.0f);

struct State {
    std::string request;                 // what you want built, or what to change
    std::string script;                  // the script that produced `result`
    BuildScript::Result result;
    bool haveResult = false;
    std::string status;
    ImVec4 statusColor = kGood;
    std::string savedPath;               // the prefab written from `result`
    std::string savedNote;
};

State& S() {
    static State s;
    return s;
}

void SetStatus(const std::string& text, const ImVec4& colour) {
    S().status = text;
    S().statusColor = colour;
}

std::string ReadFileText(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::wstring NextToDll(const wchar_t* name) { return GetThisModuleDirectory() + name; }

std::string CountText(size_t n) {
    char buf[32];
    if (n >= 1000) snprintf(buf, sizeof(buf), "%.1fk", (double)n / 1000.0);
    else snprintf(buf, sizeof(buf), "%zu", n);
    return buf;
}

// ---- what goes on the clipboard --------------------------------------------
std::string Notes() {
    std::lock_guard lock(g_settingsMtx);
    return g_prefs.aiPrefabNotes;
}

// prompt (optionally) + script to change (optionally) + your words.
bool BuildClipboardText(bool withScript, std::string& out) {
    out.clear();
    bool newChat, race;
    {
        std::lock_guard lock(g_settingsMtx);
        newChat = g_prefs.aiPrefabNewChat;
        race = g_prefs.aiPrefabRace;
    }
    if (newChat) {
        out = ReadFileText(NextToDll(race ? L"ai_prefab_prompt_race.md" : L"ai_prefab_prompt.md"));
        if (out.empty()) {
            SetStatus(std::string(race ? "ai_prefab_prompt_race.md" : "ai_prefab_prompt.md") +
                      " is missing next to the DLL. Run tools\\make_ai_prompt.py, "
                      "or copy it from tools\\ beside the DLL.", kBad);
            return false;
        }
        if (out.back() != '\n') out += '\n';
    }
    State& s = S();
    if (withScript) {
        out += "Here is my current build script. Change it as I describe below and send back the COMPLETE "
               "updated script.\n\n```json\n" + s.script + "\n```\n\n";
    }
    out += s.request;
    const std::string notes = Notes();
    if (!notes.empty()) out += "\n\nMy standing preferences:\n" + notes;
    out += "\n";
    return true;
}

void CopyForAi(bool withScript) {
    std::string text;
    if (!BuildClipboardText(withScript, text)) return;
    ImGui::SetClipboardText(text.c_str());
    SetStatus("Copied (about " + CountText(text.size() / 4) + " tokens). Paste it into your AI, then copy its reply.", kGood);
}

// ---- checking a reply -------------------------------------------------------
BuildScript::NameBook MakeBook() {
    BuildScript::NameBook book;
    for (const std::string& n : BuildBlockNameList()) book.Add(n);
    // The list the prompt was made from also knows blocks the live registry has
    // not been asked about yet (Rock_Stone_Brick is one).
    const std::string file = ReadFileText(NextToDll(L"ai_prefab_blocks.txt"));
    size_t at = 0;
    while (at < file.size()) {
        size_t end = file.find('\n', at);
        if (end == std::string::npos) end = file.size();
        std::string line = file.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] != '#') book.Add(line);
        at = end + 1;
    }
    return book;
}

void RunScript(const std::string& text) {
    State& s = S();
    BuildScript::Options opt;
    {
        std::lock_guard lock(g_settingsMtx);
        // 0 (the default) = no limit: a huge race world is a normal build here
        opt.maxBlocks = g_prefs.aiPrefabMaxBlocks > 0 ? (size_t)g_prefs.aiPrefabMaxBlocks : (std::numeric_limits<size_t>::max)();
    }
    const BuildScript::NameBook book = MakeBook();
    BuildScript::Expand(text, book, opt, s.result);
    s.haveResult = true;
    s.savedPath.clear();
    s.savedNote.clear();
    const std::string json = BuildScript::ExtractJson(text);
    s.script = json.empty() ? text : json;
    if (s.result.Ok()) SetStatus("Understood: " + std::to_string(s.result.blocks.size()) + " blocks.", kGood);
    else if (!s.result.errors.empty()) SetStatus("That script could not be used - see below.", kBad);
    else SetStatus("The script works, but some block names do not exist in the game - see below.", kWarn);
}

void PasteReply() {
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip) {
        SetStatus("The clipboard is empty. Copy the AI's whole reply first.", kBad);
        return;
    }
    RunScript(clip);
}

// ---- saving -----------------------------------------------------------------
std::string ScriptFolder() { return PrefabGetFolder() + "\\ai_scripts"; }

void SavePrefab() {
    State& s = S();
    if (!s.haveResult || s.result.blocks.empty()) return;
    const std::string folder = PrefabGetFolder();
    CreateDirectoryW(Utf8ToWide(folder).c_str(), nullptr);
    CreateDirectoryW(Utf8ToWide(ScriptFolder()).c_str(), nullptr);

    const std::string slug = BuildScript::Slug(s.result.name);
    std::string path, scriptPath;
    for (int n = 1; n < 1000; ++n) {
        const std::string tail = (n == 1 ? "" : "_" + std::to_string(n)) + ".json";
        path = folder + "\\" + slug + tail;
        scriptPath = ScriptFolder() + "\\" + slug + tail;
        if (GetFileAttributesW(Utf8ToWide(path).c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    std::string err;
    if (!WriteFileAtomic(Utf8ToWide(path), BuildScript::ToPrefabJson(s.result), err, false)) {
        SetStatus("Could not save the prefab: " + err, kBad);
        return;
    }
    // Keep the script too: it is what an AI needs to change the build later.
    std::string scriptErr;
    WriteFileAtomic(Utf8ToWide(scriptPath), s.script, scriptErr, false);
    PrefabScanFolder();
    s.savedPath = path;
    s.savedNote = "Saved " + path;
    SetStatus("Saved. Start building it below.", kGood);
}

// Scripts kept from earlier builds, newest first.
std::vector<std::string> SavedScripts() {
    std::vector<std::pair<FILETIME, std::string>> found;
    WIN32_FIND_DATAW fd{};
    const std::wstring dir = Utf8ToWide(ScriptFolder());
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            found.emplace_back(fd.ftLastWriteTime, WideToUtf8Path(dir + L"\\" + fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return CompareFileTime(&a.first, &b.first) > 0;
    });
    std::vector<std::string> out;
    for (const auto& f : found) out.push_back(f.second);
    return out;
}

std::string ShortName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ---- the report ---------------------------------------------------------------
void DrawReport() {
    State& s = S();
    if (!s.haveResult) return;
    const BuildScript::Result& r = s.result;

    if (!r.blocks.empty()) {
        ImGui::TextColored(r.Ok() ? kGood : kWarn, "%s%s%zu blocks  -  %d wide, %d tall, %d deep",
            r.name.empty() ? "" : r.name.c_str(), r.name.empty() ? "" : "  -  ",
            r.blocks.size(), r.SizeX(), r.SizeY(), r.SizeZ());
    }
    if (!r.errors.empty()) {
        ImGui::TextColored(kBad, "The script has problems:");
        for (const std::string& e : r.errors) ImGui::TextWrapped("  - %s", e.c_str());
    }
    if (!r.unknown.empty()) {
        ImGui::TextColored(kBad, "These blocks do not exist in the game:");
        for (const BuildScript::Unknown& u : r.unknown) {
            std::string line = "  - " + u.name + "  (x" + std::to_string(u.count) + ")";
            if (!u.suggestions.empty()) {
                line += "   did you mean: ";
                for (size_t i = 0; i < u.suggestions.size(); ++i) line += (i ? ", " : "") + u.suggestions[i];
            }
            ImGui::TextWrapped("%s", line.c_str());
        }
    }
    for (const std::string& w : r.warnings) ImGui::TextColored(kWarn, "  ! %s", w.c_str());

    if (!r.materials.empty() && ImGui::TreeNode("Materials to gather")) {
        std::string all;
        char buf[256];
        for (const auto& m : r.materials) {
            snprintf(buf, sizeof(buf), "%zu  %s\n", m.second, m.first.c_str());
            all += buf;
        }
        if (ImGui::SmallButton("Copy this list")) ImGui::SetClipboardText(all.c_str());
        ImGui::BeginChild("##aimaterials", ImVec2(0, (std::min)(180.0f, 22.0f + 18.0f * (float)r.materials.size())), true);
        for (const auto& m : r.materials) ImGui::Text("%6zu  %s", m.second, m.first.c_str());
        ImGui::EndChild();
        ImGui::TreePop();
    }
}

// ---- the 3D preview -----------------------------------------------------------------------------
// preview\preview.html (next to the DLL) draws a prefab with the game's own block shapes and textures.
// A page opened from disk cannot read files by itself, so the prefab is written beside it as a script,
// prefab.js, and the page is opened. The page and its block pack come from tools\build_preview_pack.py.
std::wstring PreviewDir() { return GetThisModuleDirectory() + L"preview\\"; }

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool PreviewInstalled() {
    return FileExists(PreviewDir() + L"preview.html") && FileExists(PreviewDir() + L"pack.js") &&
           FileExists(PreviewDir() + L"mesher.js") && FileExists(PreviewDir() + L"viewer.js") &&
           FileExists(PreviewDir() + L"collide.js");
}

// Builds above this go to bigview.html (typed arrays, chunk streaming, far points, a map); viewer.js keeps every block as a
// JavaScript object and cannot hold millions.
constexpr size_t kBigPreviewBlocks = 250000;

bool BigPreviewInstalled() {
    return FileExists(PreviewDir() + L"bigview.html") && FileExists(PreviewDir() + L"bigview.js") &&
           FileExists(PreviewDir() + L"bigcore.js") && FileExists(PreviewDir() + L"mesher.js") && FileExists(PreviewDir() + L"pack.js");
}

struct PvBlock { int x, y, z; const std::string* name; int rot; };

std::string PreviewScript(const std::string& title, const std::vector<PvBlock>& blocks) {
    std::string js;
    js.reserve(blocks.size() * 56 + 128);
    js += "window.HYPREFAB={\"__name\":\"" + JsonEscape(title) + "\",\"blocks\":[";
    char head[96];
    for (size_t i = 0; i < blocks.size(); ++i) {
        const PvBlock& b = blocks[i];
        snprintf(head, sizeof(head), "%s{\"x\":%d,\"y\":%d,\"z\":%d,\"name\":\"", i ? "," : "", b.x, b.y, b.z);
        js += head;
        js += JsonEscape(*b.name);
        js += "\"";
        if (b.rot >= 0) {
            snprintf(head, sizeof(head), ",\"rotation\":%d", b.rot);
            js += head;
        }
        js += "}";
    }
    js += "]};\n";
    return js;
}

// The compact form for bigview.js: a list of names and 9 bytes per block (int16 x y z, uint16 name id, uint8 rotation, 255 = none),
// base64 in one JavaScript string - about 12 bytes per block instead of 56 for the JSON of viewer.js.
std::string PreviewBigScript(const std::string& title, const std::vector<PvBlock>& blocks) {
    static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::unordered_map<std::string, int> ids;
    std::vector<const std::string*> names;
    std::vector<uint8_t> bytes;
    bytes.reserve(blocks.size() * 9);
    auto put16 = [&](int v) { bytes.push_back((uint8_t)(v & 255)); bytes.push_back((uint8_t)((v >> 8) & 255)); };
    for (const PvBlock& b : blocks) {
        auto it = ids.find(*b.name);
        if (it == ids.end()) { it = ids.emplace(*b.name, (int)names.size()).first; names.push_back(&it->first); }
        put16((std::max)(-32768, (std::min)(32767, b.x)));
        put16((std::max)(-32768, (std::min)(32767, b.y)));
        put16((std::max)(-32768, (std::min)(32767, b.z)));
        put16(it->second);
        bytes.push_back(b.rot < 0 || b.rot > 254 ? 255 : (uint8_t)b.rot);
    }
    std::string js;
    js.reserve(bytes.size() / 3 * 4 + 4096);
    js += "window.HYBIG={\"name\":\"" + JsonEscape(title) + "\",\"n\":" + std::to_string(blocks.size()) + ",\"names\":[";
    for (size_t i = 0; i < names.size(); ++i) { if (i) js += ","; js += "\"" + JsonEscape(*names[i]) + "\""; }
    js += "],\"data\":\"";
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = ((uint32_t)bytes[i] << 16) | ((uint32_t)bytes[i + 1] << 8) | bytes[i + 2];
        js += kB64[(v >> 18) & 63]; js += kB64[(v >> 12) & 63]; js += kB64[(v >> 6) & 63]; js += kB64[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const uint32_t v = (uint32_t)bytes[i] << 16;
        js += kB64[(v >> 18) & 63]; js += kB64[(v >> 12) & 63]; js += "==";
    } else if (i + 2 == bytes.size()) {
        const uint32_t v = ((uint32_t)bytes[i] << 16) | ((uint32_t)bytes[i + 1] << 8);
        js += kB64[(v >> 18) & 63]; js += kB64[(v >> 12) & 63]; js += kB64[(v >> 6) & 63]; js += "=";
    }
    js += "\"};\n";
    return js;
}

std::string g_previewNote;
ImVec4 g_previewNoteColour = kGood;

bool OpenPreview(const std::string& title, const std::vector<PvBlock>& blocks) {
    if (blocks.size() > kBigPreviewBlocks) {
        if (!PreviewInstalled() || !BigPreviewInstalled()) {
            g_previewNote = "The big-build viewer is not installed: run  python tools\\build_preview_pack.py --install  once (it copies bigview.html).";
            g_previewNoteColour = kWarn;
            return false;
        }
        std::string err;
        if (!WriteFileAtomic(PreviewDir() + L"prefab_big.js", PreviewBigScript(title, blocks), err, false)) {
            g_previewNote = "Could not write the preview: " + err;
            g_previewNoteColour = kBad;
            return false;
        }
        if (GetEnvironmentVariableW(L"HYAI_NO_OPEN", nullptr, 0) == 0)
            ShellExecuteW(nullptr, L"open", (PreviewDir() + L"bigview.html").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        g_previewNote = "Opened in your browser (big-build viewer: fly with W A S D, click the map to jump).";
        g_previewNoteColour = kGood;
        return true;
    }
    if (!PreviewInstalled()) {
        g_previewNote = "The 3D preview is not set up yet: run  python tools\\build_preview_pack.py --install  once.";
        g_previewNoteColour = kWarn;
        return false;
    }
    std::string err;
    if (!WriteFileAtomic(PreviewDir() + L"prefab.js", PreviewScript(title, blocks), err, false)) {
        g_previewNote = "Could not write the preview: " + err;
        g_previewNoteColour = kBad;
        return false;
    }
    // HYAI_NO_OPEN lets the UI tests click the button without a browser window appearing.
    if (GetEnvironmentVariableW(L"HYAI_NO_OPEN", nullptr, 0) == 0)
        ShellExecuteW(nullptr, L"open", (PreviewDir() + L"preview.html").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    g_previewNote = "Opened in your browser.";
    g_previewNoteColour = kGood;
    return true;
}

// The button, greyed with the reason when the preview is not set up.
bool PreviewButton(const char* label, const ImVec2& size) {
    const bool ready = PreviewInstalled();
    ImGui::BeginDisabled(!ready);
    const bool clicked = ImGui::Button(label, size);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(ready ? "Opens your browser with the build drawn from the game's own\n"
                                  "block shapes and textures: turn it, zoom it, hide materials."
                                : "One-time setup: python tools\\build_preview_pack.py --install\n"
                                  "(reads the blocks and textures out of your Hytale Assets.zip)");
    }
    return clicked && ready;
}

} // namespace

// What the guide shows beside a block: the rotation number, and in words which way
// it points where that is understood. Only what real exports confirmed.
std::string BuildRotationLabel(int rotation, const std::string& blockName) {
    std::string s;
    if (rotation > 0) s = "  -  rotation " + std::to_string(rotation);
    const std::string hint = BuildScript::RotationHint(blockName, rotation, g_buildRotation.load());
    if (!hint.empty()) s += "  -  " + hint;
    return s;
}

void DrawAiPrefabSection() {
    State& s = S();
    ImGui::SeparatorText("Make a build with an AI");
    if (!ImGui::TreeNodeEx("Design one with a strong AI, then build it here###aiprefab",
            SessionHasActive() ? 0 : ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::TextDisabled(
        "Copy one prompt (the block list, the format and the design rules are in it), paste it into\n"
        "any AI chat, copy the reply, paste it back here. Nothing is sent from this program.");

    // ---- 1 ---------------------------------------------------------------------
    ImGui::TextColored(kHead, "1. Say what you want");
    ImGui::InputTextMultiline("##airequest", &s.request, ImVec2(-1, 66));
    if (s.request.empty() && !ImGui::IsItemActive()) {
        const ImVec2 min = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + 6, min.y + 4), ImGui::GetColorU32(ImGuiCol_TextDisabled),
            "e.g. a medieval tavern, stone ground floor, dark timber upstairs, about 20 x 14, with a cellar door");
    }

    {
        bool newChat;
        {
            std::lock_guard lock(g_settingsMtx);
            newChat = g_prefs.aiPrefabNewChat;
        }
        if (ImGui::Checkbox("New chat: include the full prompt", &newChat)) {
            std::lock_guard lock(g_settingsMtx);
            g_prefs.aiPrefabNewChat = newChat;
            MarkPrefsChanged();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("On: what you copy is the whole prompt plus your request - for a fresh chat.\n"
                              "Off: just your request - for a chat that already has the prompt.");

        bool race;
        {
            std::lock_guard lock(g_settingsMtx);
            race = g_prefs.aiPrefabRace;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Race track (the server's kart blocks)", &race)) {
            std::lock_guard lock(g_settingsMtx);
            g_prefs.aiPrefabRace = race;
            MarkPrefsChanged();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copies the prompt that also knows the race server's custom blocks (asphalt, curbs,\n"
                              "barriers, start grid, podiums...) and how to lay out a track with the road op.\n"
                              "Made by tools\\make_ai_prompt.py from tools\\race_blocks.json.");
    }

    if (ImGui::TreeNode("Standing preferences (added to every request)")) {
        static std::string editing;
        static bool active = false;
        if (!active) editing = Notes();
        ImGui::InputTextMultiline("##ainotes", &editing, ImVec2(-1, 50));
        active = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            std::lock_guard lock(g_settingsMtx);
            g_prefs.aiPrefabNotes = editing;
            MarkPrefsChanged();
        }
        ImGui::TextDisabled("e.g. \"survival blocks only, no Prototype_ blocks; I prefer dark wood and mossy stone\"");
        int maxBlocks;
        {
            std::lock_guard lock(g_settingsMtx);
            maxBlocks = g_prefs.aiPrefabMaxBlocks;
        }
        ImGui::SetNextItemWidth(130);
        if (ImGui::InputInt("refuse scripts larger than (blocks, 0 = never)", &maxBlocks, 100000, 1000000)) {
            std::lock_guard lock(g_settingsMtx);
            g_prefs.aiPrefabMaxBlocks = (std::max)(0, maxBlocks);
            MarkPrefsChanged();
        }
        ImGui::TreePop();
    }

    if (ImGui::Button("Copy for the AI", ImVec2(160, 0))) CopyForAi(false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Puts the prompt and your request on the clipboard.");
    if (!s.script.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Copy to change the last build")) CopyForAi(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Your words above are the change you want (\"taller tower, add a well\").\n"
                              "The current script goes along with them.");
    }

    // ---- 2 ---------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::TextColored(kHead, "2. Paste what it sends back");
    if (ImGui::Button("Paste the AI's reply", ImVec2(160, 0))) PasteReply();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Copy the AI's whole reply first (prose around the script is fine).");

    const std::vector<std::string> older = SavedScripts();
    if (!older.empty()) {
        ImGui::SameLine();
        static int pick = -1;
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("##aiolder", pick >= 0 && pick < (int)older.size() ? ShortName(older[(size_t)pick]).c_str() : "an earlier script...")) {
            for (int i = 0; i < (int)older.size(); ++i)
                if (ImGui::Selectable(ShortName(older[(size_t)i]).c_str(), i == pick)) {
                    pick = i;
                    const std::string text = ReadFileText(Utf8ToWide(older[(size_t)i]));
                    if (!text.empty()) RunScript(text);
                }
            ImGui::EndCombo();
        }
    }

    if (!s.status.empty()) ImGui::TextColored(s.statusColor, "%s", s.status.c_str());
    DrawReport();

    // ---- 3 ---------------------------------------------------------------------
    if (s.haveResult) {
        const BuildScript::Result& r = s.result;
        ImGui::Spacing();
        if (!r.Ok()) {
            if (ImGui::Button("Copy the problems for the AI", ImVec2(230, 0))) {
                ImGui::SetClipboardText(BuildScript::FixRequest(r).c_str());
                SetStatus("Copied. Paste it to the AI - it will send a corrected script.", kGood);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Lists every problem with the closest real block names, and asks for the whole script again.");
        }
        if (!r.blocks.empty()) {
            if (PreviewButton("Preview in 3D", ImVec2(160, 0))) {
                std::vector<PvBlock> pv;
                pv.reserve(r.blocks.size());
                for (const BuildScript::Block& b : r.blocks) pv.push_back({ b.x, b.y, b.z, &b.name, b.rotation });
                OpenPreview(r.name.empty() ? "AI build" : r.name, pv);
            }
            if (!g_previewNote.empty()) { ImGui::SameLine(); ImGui::TextColored(g_previewNoteColour, "%s", g_previewNote.c_str()); }
        }
        const bool onlyUnknown = r.errors.empty() && !r.blocks.empty();
        if (!r.Ok() && onlyUnknown) {
            ImGui::SameLine();
            if (ImGui::Button("Save it anyway")) SavePrefab();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The unknown blocks stay as written; you would swap them in the guide.");
        }
        if (r.Ok() && s.savedPath.empty()) {
            ImGui::TextColored(kHead, "3. Save it");
            if (ImGui::Button("Save as a prefab", ImVec2(160, 0))) SavePrefab();
        }
        if (!s.savedPath.empty()) {
            ImGui::TextColored(kGood, "%s", s.savedNote.c_str());
            const bool busy = g_sessionBusy.load();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Start building it", ImVec2(160, 0))) SessionStartNew(s.savedPath);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("It appears at your feet; turn it with Home, move it with the arrows.");
        }
    }
    ImGui::TreePop();
}

// The row under the session list: look at the open build from any side before putting it up.
void DrawPreview3dRow(const std::shared_ptr<const Prefab>& p) {
    if (!p) return;
    ImGui::Spacing();
    if (PreviewButton("Preview it in 3D", ImVec2(160, 0))) {
        std::vector<PvBlock> pv;
        pv.reserve(p->blocks.size());
        for (const PrefabBlock& b : p->blocks) {
            if (b.nameId < 0 || (size_t)b.nameId >= p->names.size()) continue;
            pv.push_back({ b.x, b.y, b.z, &p->names[(size_t)b.nameId], b.rotation });
        }
        OpenPreview(p->name, pv);
    }
    ImGui::SameLine();
    if (!g_previewNote.empty()) ImGui::TextColored(g_previewNoteColour, "%s", g_previewNote.c_str());
    else ImGui::TextDisabled("see how it looks built, with the game's own blocks and textures");
}
