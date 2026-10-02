#include "pch.h"
#include "ui.hpp"
#include "common.hpp"
#include "settings.hpp"
#include "autofarm.hpp"
#include "gamesdk.hpp"

#include <imgui_stdlib.h>
#include <shellapi.h>
#include <cmath>

// =============================================================================
// Farm tab
// =============================================================================
// Top to bottom in the order you use it: is everything it needs working, what
// is it doing right now (with the one button that matters for that), which
// farm, what that farm looks like, and - folded away - the per-step tuning.

namespace {

const ImVec4 kAccent{ 0.58f, 0.78f, 1.00f, 1.0f };
const ImVec4 kGood{ 0.45f, 0.95f, 0.55f, 1.0f };
const ImVec4 kWarn{ 0.98f, 0.80f, 0.35f, 1.0f };
const ImVec4 kBad{ 1.00f, 0.45f, 0.40f, 1.0f };
const ImVec4 kRec{ 1.00f, 0.32f, 0.30f, 1.0f };

// Colours for key combinations on the map and in the steps list.
const ImU32 kPalette[] = {
    IM_COL32(110, 190, 255, 255), IM_COL32(120, 230, 140, 255), IM_COL32(255, 190, 90, 255),
    IM_COL32(230, 120, 230, 255), IM_COL32(255, 120, 110, 255), IM_COL32(120, 230, 230, 255),
    IM_COL32(210, 210, 120, 255), IM_COL32(170, 150, 255, 255),
};

std::string Clock(long long ms) {
    char b[32];
    const long long s = ms / 1000;
    if (s >= 3600) snprintf(b, sizeof(b), "%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(b, sizeof(b), "%lld:%02lld", s / 60, s % 60);
    return b;
}

std::string Secs(int ms) {
    char b[32];
    snprintf(b, sizeof(b), "%.1f s", ms / 1000.0);
    return b;
}

std::string HotkeyText(int vk) { return vk > 0 ? FarmKeyName(vk) : std::string("no key set"); }

void Dot(const ImVec4& c) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 5, p.y + h * 0.5f + 1), 4.5f, ImGui::GetColorU32(c));
    ImGui::Dummy(ImVec2(14, h));
    ImGui::SameLine();
}

std::string DateText(long long unix) {
    if (!unix) return "";
    const time_t t = (time_t)unix;
    tm lt{};
    localtime_s(&lt, &t);
    char b[40];
    strftime(b, sizeof(b), "%b %d, %H:%M", &lt);
    return b;
}

// Palette slot for each key combination, in order of first appearance.
std::map<uint32_t, int> ComboColours(const FarmRoute& r) {
    std::map<uint32_t, int> m;
    for (const FarmSample& p : r.s)
        if (!m.count(p.keys)) m[p.keys] = (int)m.size() % (int)(sizeof(kPalette) / sizeof(kPalette[0]));
    return m;
}

const char* Heading(const FarmStep& st) {
    if (st.stopped) return "stopped";
    if (std::fabs(st.dx) < 0.5f && std::fabs(st.dz) < 0.5f) return "-";
    if (std::fabs(st.dx) >= std::fabs(st.dz)) return st.dx > 0 ? "+X" : "-X";
    return st.dz > 0 ? "+Z" : "-Z";
}

// ---- the tractor's own direction ------------------------------------------------
// The finder walks you through four short steps in game, so while it runs
// this is the biggest thing on the tab, and it beeps at each step for when
// you're looking at the game instead.
void DrawRideWizard() {
    static const int kStepMs[] = { 0, 5000, 3000, 10000, 6000, 2000, 1000 };
    static const char* kTitles[] = { "", "Get ready", "1 of 4  -  Hold still", "2 of 4  -  Drive in circles",
                                     "3 of 4  -  Look around, tractor still", "4 of 4  -  Hold still", "Checking" };
    const int st = (std::max)(0, (std::min)(6, g_gsdkRideStep.load()));
    const long long left = (std::max)(0LL, g_gsdkRideStepEndMs.load() - NowSteadyMs());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.13f, 0.20f, 1.0f));
    ImGui::BeginChild("##ridewiz", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::TextColored(kWarn, "Finding the tractor's direction  -  %s", kTitles[st]);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(GsdkRideStatus().c_str());
    ImGui::PopTextWrapPos();
    const float frac = kStepMs[st] ? 1.0f - (float)left / kStepMs[st] : 1.0f;
    char over[32];
    snprintf(over, sizeof(over), "%.0f s", left / 1000.0);
    ImGui::ProgressBar((std::max)(0.0f, (std::min)(1.0f, frac)), ImVec2(-90, 0), over);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) GsdkCancelRideFacing();
    ImGui::TextDisabled("It beeps once, twice, three times as each step starts - you don't need to watch this.");
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DrawRide(const FarmStatus& s) {
    if (g_gsdkRideFinding.load()) { DrawRideWizard(); return; }
    GsdkRidePath rp;
    { std::lock_guard lock(g_gsdkRideMtx); rp = g_gsdkRide; }
    const bool busy = s.mode != FarmMode::Idle;

    if (!rp.found) {
        Dot(kWarn);
        ImGui::Text("Tractor direction");
        ImGui::SameLine();
        ImGui::TextDisabled("not set up - it's what makes tricky corners reliable");
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || !s.havePos);
        if (ImGui::SmallButton("Find it")) GsdkFindRideFacing();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("About 25 seconds, sitting on the tractor: hold still, drive in circles,\n"
                              "look around with the tractor still. Then laps you record carry the\n"
                              "tractor's direction, and playback lines corners up by it.");
    }
    else {
        if (s.haveRide) {
            Dot(kGood);
            ImGui::Text("Tractor direction");
            ImGui::SameLine();
            ImGui::TextDisabled("%.0f deg - turn the tractor and this should follow", s.ride);
        }
        else {
            Dot(ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
            ImGui::Text("Tractor direction");
            ImGui::SameLine();
            ImGui::TextDisabled(GsdkRideSearching() ? "looking for your tractor..." : "found - get on the tractor to use it");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::SmallButton("Find again")) GsdkFindRideFacing();
        ImGui::SameLine();
        if (ImGui::SmallButton("Forget")) GsdkForgetRide();
        ImGui::EndDisabled();
    }

    const std::string status = GsdkRideStatus();
    if (!status.empty()) {
        ImGui::Indent(22);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", status.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Unindent(22);
    }

    // The runners-up, live, in case the first pick isn't the tractor.
    std::vector<GsdkRideCandidate> cands;
    { std::lock_guard lock(g_gsdkRideCandMtx); cands = g_gsdkRideCandidates; }
    if (rp.found && cands.size() > 1) {
        ImGui::Indent(22);
        if (ImGui::TreeNode("##ridecands", "Not following the tractor? The other possibilities (%zu)", cands.size())) {
            ImGui::TextDisabled("Turn the tractor and pick the one that follows it.");
            for (size_t i = 0; i < cands.size(); ++i) {
                const GsdkRideCandidate& c = cands[i];
                float v = 0;
                const bool ok = GsdkReadRideCandidate(c, v);
                const bool inUse = c.path.anchor == rp.anchor && c.path.derefs == rp.derefs && c.path.offset == rp.offset;
                char label[160];
                snprintf(label, sizeof(label), "%s   %s   %s   swung %.0f deg###rc%zu",
                    ok ? (std::to_string((int)std::lround(v)) + " deg").c_str() : "--",
                    c.agree >= 0 ? ("pointed your way " + std::to_string((int)std::lround(c.agree * 100)) + "%").c_str()
                                 : "not checked against driving",
                    c.path.anchor == 1 ? "in the tractor" : "in you", c.range, i);
                if (ImGui::Selectable(label, inUse)) GsdkAdoptRide(c);
            }
            ImGui::TreePop();
        }
        ImGui::Unindent(22);
    }
}

// ---- setup -------------------------------------------------------------------
void DrawSetup(const FarmStatus& s) {
    if (s.havePos) { Dot(kGood); ImGui::Text("Position"); ImGui::SameLine(); ImGui::TextDisabled("%.1f, %.1f, %.1f", s.x, s.y, s.z); }
    else {
        Dot(kBad); ImGui::Text("Position");
        ImGui::SameLine();
        ImGui::TextColored(kBad, "not found - set it up on the Position or Player tab first");
    }

    if (s.haveYaw) {
        Dot(kGood); ImGui::Text("Camera direction"); ImGui::SameLine(); ImGui::TextDisabled("%.0f deg", s.yaw);
    }
    else {
        Dot(ImVec4(0.5f, 0.5f, 0.55f, 1.0f)); ImGui::Text("Camera direction");
        ImGui::SameLine();
        ImGui::TextDisabled("not set up - optional, only needed if you turn the camera during a lap (Player tab)");
    }

    DrawRide(s);

    const int mode = g_prefs.farmInputMode, learned = g_prefs.farmInputLearned;
    const char* text = "";
    ImVec4 col = kGood;
    if (mode == 1) text = "background keys (set on Settings > Auto farm)";
    else if (mode == 2) { text = "direct keys - Hytale has to stay in front"; col = kWarn; }
    else if (learned == 1) text = "background keys work here - use other windows while it farms";
    else if (learned == 2) { text = "Hytale needs to be in front while it farms (it pauses otherwise)"; col = kWarn; }
    else { text = "not tried yet - the first run finds out which way works"; col = ImVec4(0.5f, 0.5f, 0.55f, 1.0f); }
    Dot(col); ImGui::Text("Keys to Hytale"); ImGui::SameLine(); ImGui::TextDisabled("%s", text);
}

// ---- what it is doing ------------------------------------------------------------
void DrawControl(const FarmStatus& s, const std::shared_ptr<const FarmRoute>& sel) {
    const std::string playKey = HotkeyText(g_prefs.keyFarmPlay);
    const std::string recKey = HotkeyText(g_prefs.keyFarmRecord);

    switch (s.mode) {
    case FarmMode::Idle: {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.24f, 0.24f, 1.0f));
        const std::string recLabel = "Record a lap  (" + recKey + ")";
        if (ImGui::Button(recLabel.c_str(), ImVec2(230, 34))) FarmToggleRecord();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::BeginDisabled(!sel);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.45f, 0.26f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.58f, 0.33f, 1.0f));
        const std::string playLabel = sel ? "Start farming \"" + sel->name + "\"  (" + playKey + ")###play"
                                          : "Start farming  (" + playKey + ")###play";
        if (ImGui::Button(playLabel.c_str(), ImVec2((std::max)(260.0f, ImGui::CalcTextSize(playLabel.c_str()).x + 30), 34)))
            FarmTogglePlay(true);
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        ImGui::TextDisabled("In game: %s records a lap, %s starts and stops farming. This window can stay closed.",
            recKey.c_str(), playKey.c_str());
        break;
    }
    case FarmMode::Recording: {
        const bool blink = (s.nowMs / 500) % 2 == 0;
        ImGui::TextColored(blink ? kRec : ImVec4(0.6f, 0.2f, 0.2f, 1.0f), "RECORDING");
        ImGui::SameLine();
        ImGui::Text("%s", Clock(s.recMs).c_str());
        if (!s.recStarted)
            ImGui::TextColored(kWarn, "Waiting for you to start driving...");
        else {
            ImGui::Text("%.0f blocks driven  -  %d key changes  -  %.0f blocks from the start",
                s.recTravelled, s.recKeyChanges, s.recFromStart);
            ImGui::TextDisabled("Holding: %s", s.recKeys.c_str());
            if (g_prefs.farmAutoFinish && s.recTravelled > 40.0f)
                ImGui::TextColored(kAccent, "Drive back to where you started - it finishes by itself there.");
        }
        const std::string stopLabel = "Stop and save  (" + recKey + ")";
        if (ImGui::Button(stopLabel.c_str(), ImVec2(200, 30))) FarmToggleRecord();
        ImGui::SameLine();
        if (ImGui::Button("Throw it away", ImVec2(140, 30))) FarmCancelRecord();
        break;
    }
    case FarmMode::Countdown:
        ImGui::TextColored(kAccent, "Starting \"%s\" in %d...", s.routeName.empty() ? "farm" : s.routeName.c_str(), s.countdownLeft);
        ImGui::TextDisabled("Hytale has been brought to the front. Get on the tractor, on the route.");
        if (ImGui::Button("Cancel", ImVec2(120, 30))) FarmStop();
        break;
    case FarmMode::Playing:
    case FarmMode::Paused: {
        if (s.mode == FarmMode::Paused) {
            ImGui::TextColored(kWarn, "PAUSED");
            ImGui::SameLine();
            ImGui::TextColored(kWarn, "%s", s.pauseReason.c_str());
        }
        else {
            ImGui::TextColored(kGood, "FARMING");
            ImGui::SameLine();
            ImGui::Text("\"%s\"  -  running %s", s.routeName.c_str(), Clock(s.playMs).c_str());
        }
        char over[64];
        snprintf(over, sizeof(over), "lap %d  -  %.0f%%", s.laps + 1, s.progress * 100.0f);
        ImGui::ProgressBar(s.progress, ImVec2(-FLT_MIN, 0), over);
        if (s.laps > 0)
            ImGui::Text("%d lap%s done  -  last %s  -  best %s", s.laps, s.laps == 1 ? "" : "s",
                Secs(s.lastLapMs).c_str(), Secs(s.bestLapMs).c_str());
        std::string what = "Holding " + s.holding;
        if (s.unsticking) what += "  -  nudging on to the next spot";
        else if (s.inStop) what += "  -  stopped, as in the recording";
        else if (s.toNextMs >= 0) {
            char b[48];
            snprintf(b, sizeof(b), "  -  next change in %.1f s", s.toNextMs / 1000.0f);
            what += b;
        }
        ImGui::Text("%s", what.c_str());
        // How closely it is matching the recording, in words.
        const ImVec4 fitCol = s.off < 1.0f ? kGood : s.off < 2.5f ? kWarn : kBad;
        ImGui::TextColored(fitCol, "%s", s.off < 1.0f ? "on the line" : s.off < 2.5f ? "a little off the line" : "well off the line");
        ImGui::SameLine();
        ImGui::TextDisabled("(%.1f blocks)  -  %.0f blocks/s  -  %s%s keys%s%s%s%s", s.off, s.speed,
            s.framesLive ? "frame-locked, " : "",
            s.inputMode == 2 ? "direct" : "background",
            s.verifying ? " (checking they work)" : "",
            s.usingRide ? "  -  lining up by the tractor's direction" : "",
            s.steering ? "  -  turning the camera" : "",
            s.unsticks ? ("  -  " + std::to_string(s.unsticks) + " nudges").c_str() : "");
        if (s.headingTaps)
            ImGui::TextDisabled("Straightened the tractor %d time%s.", s.headingTaps, s.headingTaps == 1 ? "" : "s");
        const std::string stopLabel = "Stop  (" + playKey + ")";
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 0.90f));
        if (ImGui::Button(stopLabel.c_str(), ImVec2(160, 30))) FarmStop();
        ImGui::PopStyleColor();
        break;
    }
    }

    if (!s.message.empty())
        ImGui::TextColored(s.messageIsError ? kBad : kGood, "%s", s.message.c_str());
}

// ---- the map ------------------------------------------------------------------------
void DrawMap(const FarmRoute& r, const FarmStatus& s, bool playingThis) {
    if (r.s.size() < 2) return;
    const std::map<uint32_t, int> colours = ComboColours(r);

    float minX = r.s[0].x, maxX = minX, minZ = r.s[0].z, maxZ = minZ;
    for (const FarmSample& p : r.s) {
        minX = (std::min)(minX, p.x); maxX = (std::max)(maxX, p.x);
        minZ = (std::min)(minZ, p.z); maxZ = (std::max)(maxZ, p.z);
    }
    // You, if you're anywhere near it, so you can see which way to go to get on.
    const bool showMe = s.havePos && s.x > minX - 150 && s.x < maxX + 150 && s.z > minZ - 150 && s.z < maxZ + 150;
    if (showMe) {
        minX = (std::min)(minX, s.x); maxX = (std::max)(maxX, s.x);
        minZ = (std::min)(minZ, s.z); maxZ = (std::max)(maxZ, s.z);
    }
    const float spanX = (std::max)(4.0f, maxX - minX), spanZ = (std::max)(4.0f, maxZ - minZ);

    const float w = (std::max)(200.0f, ImGui::GetContentRegionAvail().x - 4);
    const float h = 280.0f, pad = 14.0f;

    // A farm 48 wide and 1200 long drawn to scale is a single line, so past a
    // certain shape each axis is fitted on its own and the map says so.
    float sx = (w - 2 * pad) / spanX, sz = (h - 2 * pad) / spanZ;
    const bool stretched = (std::max)(sx, sz) / (std::min)(sx, sz) > 4.0f;
    if (!stretched) sx = sz = (std::min)(sx, sz);

    // Which way up it is: smaller z at the top.
    ImGui::TextDisabled("-Z at the top   x %.0f to %.0f   z %.0f to %.0f%s", minX, maxX, minZ, maxZ,
        stretched ? "   (stretched to fit)" : "");
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##farmmap", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(16, 18, 26, 255), 6.0f);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(60, 66, 86, 255), 6.0f);

    const float offX = p0.x + (w - spanX * sx) * 0.5f, offZ = p0.y + (h - spanZ * sz) * 0.5f;
    auto to = [&](float x, float z) { return ImVec2(offX + (x - minX) * sx, offZ + (z - minZ) * sz); };

    for (int j = 0; j + 1 < (int)r.s.size(); ++j) {
        const auto it = colours.find(r.s[j].keys);
        const ImU32 c = kPalette[it != colours.end() ? it->second : 0];
        dl->AddLine(to(r.s[j].x, r.s[j].z), to(r.s[j + 1].x, r.s[j + 1].z), c, 2.0f);
    }
    for (int a = 0; a < (int)r.stopEnd.size(); ++a) {
        if (r.stopEnd[a] < 0) continue;
        const ImVec2 q = to(r.s[a].x, r.s[a].z);
        dl->AddRectFilled(ImVec2(q.x - 4, q.y - 4), ImVec2(q.x + 4, q.y + 4), IM_COL32(255, 200, 80, 255));
    }
    // Where you actually went on the last run, over the recording: where the
    // two part is where a lap went wrong.
    const FarmTrail trail = FarmGetTrail();
    const bool haveTrail = trail.routeFile == r.file && trail.points.size() > 1;
    if (haveTrail) {
        for (size_t i = 1; i < trail.points.size(); ++i) {
            const auto& a = trail.points[i - 1];
            const auto& b = trail.points[i];
            if (std::fabs(a.first - b.first) + std::fabs(a.second - b.second) > 40.0f) continue;   // a teleport
            dl->AddLine(to(a.first, a.second), to(b.first, b.second), IM_COL32(255, 255, 255, 110), 1.0f);
        }
    }
    const bool haveFail = trail.routeFile == r.file && trail.haveFail;
    if (haveFail) {
        const ImVec2 f = to(trail.failX, trail.failZ), w2 = to(trail.wantX, trail.wantZ);
        dl->AddLine(f, w2, IM_COL32(255, 90, 80, 200), 1.5f);
        dl->AddCircle(w2, 6.0f, IM_COL32(255, 200, 80, 255), 16, 2.0f);
        dl->AddLine(ImVec2(f.x - 6, f.y - 6), ImVec2(f.x + 6, f.y + 6), IM_COL32(255, 80, 70, 255), 3.0f);
        dl->AddLine(ImVec2(f.x - 6, f.y + 6), ImVec2(f.x + 6, f.y - 6), IM_COL32(255, 80, 70, 255), 3.0f);
    }

    const ImVec2 st = to(r.s[0].x, r.s[0].z);
    dl->AddCircle(st, 7.0f, IM_COL32(120, 255, 140, 255), 16, 2.0f);
    if (playingThis) {
        const ImVec2 c = to(s.cursorX, s.cursorZ);
        dl->AddCircle(c, 9.0f, IM_COL32(255, 220, 90, 255), 16, 2.0f);
    }
    if (showMe) {
        const ImVec2 me = to(s.x, s.z);
        dl->AddCircleFilled(me, 5.0f, IM_COL32(255, 255, 255, 255));
        dl->AddCircle(me, 6.5f, IM_COL32(0, 0, 0, 200), 16, 1.5f);
    }


    if (hovered) {
        // The nearest point of the lap to the mouse, and what is held there.
        const ImVec2 m = ImGui::GetIO().MousePos;
        int best = -1;
        float bd = 1e30f;
        for (int j = 0; j < (int)r.s.size(); ++j) {
            const ImVec2 q = to(r.s[j].x, r.s[j].z);
            const float d = (q.x - m.x) * (q.x - m.x) + (q.y - m.y) * (q.y - m.y);
            if (d < bd) { bd = d; best = j; }
        }
        if (best >= 0 && bd < 30 * 30) {
            const FarmSample& p = r.s[best];
            dl->AddCircle(to(p.x, p.z), 5.0f, IM_COL32(255, 255, 255, 180));
            ImGui::SetTooltip("%s\n%.1f, %.1f, %.1f\n%.0f blocks into the lap, %.1f s", FarmKeysText(r, r.eff[best]).c_str(),
                p.x, p.y, p.z, r.cum[best], p.t / 1000.0);
        }
    }

    // Legend
    bool first = true;
    for (const auto& [keys, idx] : colours) {
        if (!first) ImGui::SameLine(0, 16);
        first = false;
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const float lh = ImGui::GetTextLineHeight();
        dl->AddRectFilled(ImVec2(q.x, q.y + lh * 0.5f - 2), ImVec2(q.x + 16, q.y + lh * 0.5f + 2), kPalette[idx]);
        ImGui::Dummy(ImVec2(18, lh));
        ImGui::SameLine(0, 4);
        ImGui::TextDisabled("%s", FarmKeysText(r, keys).c_str());
    }
    ImGui::TextDisabled("green ring = start   yellow = stop in the lap   white dot = you%s%s",
        playingThis ? "   yellow ring = where it thinks you are" : "",
        haveTrail ? "   thin white = where you went" : "");
    if (haveFail) {
        ImGui::TextColored(kBad, "Red X: where the last run gave up (%s). The orange ring is where the recording "
            "had you then - hover the map near it to see the keys, and nudge that step below.", trail.failText.c_str());
    }
}

// ---- steps ---------------------------------------------------------------------------
void DrawSteps(const FarmRoute& r, const FarmStatus& s, bool playingThis) {
    const std::vector<FarmStep> steps = FarmSteps(r);
    char header[64];
    snprintf(header, sizeof(header), "Steps (%zu) - fine-tune a corner###farmsteps", steps.size());
    if (!ImGui::CollapsingHeader(header)) return;
    ImGui::TextDisabled(
        "Each step is a set of keys held from one moment of the lap to the next. If one corner goes wrong\n"
        "the same way every lap, start that step a little sooner (-) or later (+), in milliseconds. One\n"
        "game frame is about 17 ms at 60 fps - start with a frame or two.");
    const std::map<uint32_t, int> colours = ComboColours(r);

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingFixedFit;
    const float h = (std::min)(320.0f, ImGui::GetFrameHeightWithSpacing() * (float)(steps.size() + 1) + 8);
    if (!ImGui::BeginTable("##steps", 6, flags, ImVec2(0, h))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Going");
    ImGui::TableSetupColumn("Length");
    ImGui::TableSetupColumn("Time");
    ImGui::TableSetupColumn("Sooner / later (ms)");
    ImGui::TableHeadersRow();

    ImGuiListClipper clip;
    clip.Begin((int)steps.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const FarmStep& st = steps[i];
            ImGui::TableNextRow();
            if (playingThis && s.segment >= st.first && s.segment < st.last)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(70, 90, 40, 160));
            ImGui::TableNextColumn();
            ImGui::Text("%d", i + 1);
            ImGui::TableNextColumn();
            const auto it = colours.find(st.keys);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPalette[it != colours.end() ? it->second : 0]),
                "%s", FarmKeysText(r, st.keys).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Heading(st));
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", st.length);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f s", st.ms / 1000.0);
            ImGui::TableNextColumn();
            if (i == 0) { ImGui::TextDisabled("start of the lap"); continue; }
            const auto n = r.nudges.find(st.first);
            float v = n != r.nudges.end() ? n->second : 0.0f;
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(120);
            // Typing commits when you leave the box; the +/- buttons at once.
            const bool changed = ImGui::InputFloat("##nudge", &v, 5.0f, 20.0f, "%+.0f");
            if ((changed && !ImGui::IsItemActive()) || ImGui::IsItemDeactivatedAfterEdit())
                FarmSetNudge(r.file, st.first, (std::max)(-500.0f, (std::min)(500.0f, v)));
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

// ---- the list of farms ------------------------------------------------------------
void DrawLibrary(const std::vector<std::shared_ptr<const FarmRoute>>& routes,
    const std::shared_ptr<const FarmRoute>& sel, bool busy) {
    if (routes.empty()) {
        ImGui::TextDisabled(
            "Nothing recorded yet. Get on your tractor anywhere on the loop, press Record, and drive one\n"
            "lap exactly the way you normally would - glitch and all. It stops by itself when you're back\n"
            "where you started.");
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##farms", 6, flags)) {
        ImGui::TableSetupColumn("Farm", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Lap");
        ImGui::TableSetupColumn("Length");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Keys");
        ImGui::TableSetupColumn("Recorded");
        ImGui::TableHeadersRow();
        for (const auto& r : routes) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(r.get());
            const bool isSel = sel && sel->file == r->file;
            ImGui::BeginDisabled(busy);
            if (ImGui::Selectable(r->name.c_str(), isSel, ImGuiSelectableFlags_SpanAllColumns)) FarmSelect(r->file);
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::Text("%s", Secs(r->durationMs).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.0f blocks", r->length);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r->loop ? (r->hasRide ? "loop, tractor dir" : "loop")
                                           : (r->hasRide ? "one way, tractor dir" : "one way"));
            ImGui::TableNextColumn();
            uint32_t used = 0;
            for (const FarmSample& p : r->s) used |= p.keys;
            ImGui::TextUnformatted(FarmKeysText(*r, used).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", DateText(r->createdUnix).c_str());
        }
        ImGui::EndTable();
    }

    if (!sel) return;
    static std::wstring nameFor;
    static std::string nameBuf;
    if (nameFor != sel->file) { nameFor = sel->file; nameBuf = sel->name; }
    ImGui::SetNextItemWidth(240);
    ImGui::InputText("Name##farmname", &nameBuf);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (TrimCopy(nameBuf).empty() || !FarmRename(sel->file, nameBuf)) nameBuf = sel->name;
    }
    else if (!ImGui::IsItemActive() && nameBuf != sel->name) nameBuf = sel->name;

    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Delete...")) ImGui::OpenPopup("Delete farm?");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) {
        CreateDirectoryW(FarmFolder().c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", FarmFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Settings")) UiOpenSettings("Auto farm");

    if (ImGui::BeginPopupModal("Delete farm?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete \"%s\"?", sel->name.c_str());
        ImGui::TextDisabled("It goes to the farms\\deleted folder, so you can get it back from there.");
        if (ImGui::Button("Delete", ImVec2(100, 0))) { FarmDelete(sel->file); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Keep it", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (!sel->hasYaw)
        ImGui::TextDisabled("Camera direction wasn't recorded for this one, so playback never moves your mouse.");
    else if (FarmYawSwing(*sel) > 20.0f)
        ImGui::TextDisabled("This lap turns the camera, so playback turns it too - Hytale has to stay in front.");

    if (sel->hasRide) {
        if (sel->turnPlus >= 0)
            ImGui::TextDisabled("Tractor direction recorded. %s and %s turn it (about %.0f deg/s), so playback can "
                "straighten it up when a corner leaves it a little off.", FarmKeyName(sel->vks[sel->turnPlus]).c_str(),
                FarmKeyName(sel->vks[sel->turnMinus]).c_str(), sel->turnRate);
        else
            ImGui::TextDisabled("Tractor direction recorded - corners line up by it.");
    }
    else {
        GsdkRidePath rp;
        { std::lock_guard lock(g_gsdkRideMtx); rp = g_gsdkRide; }
        if (rp.found)
            ImGui::TextColored(kWarn, "Recorded before the tractor's direction was found. Record this lap again to "
                "have corners line up by it.");
    }
    if (!sel->perFrame)
        ImGui::TextDisabled("Recorded with the older recorder. It plays, but a fresh recording times corners better.");
}

} // namespace

void DrawFarmTab() {
    const FarmStatus s = FarmGetStatus();
    const auto routes = FarmList();
    const auto sel = FarmSelected();
    const bool busy = s.mode != FarmMode::Idle;
    const bool playingThis = (s.mode == FarmMode::Playing || s.mode == FarmMode::Paused) && sel && s.routeFile == sel->file;

    ImGui::TextColored(kAccent, "Auto farm");
    ImGui::TextDisabled(
        "Record one lap of your farm, then let it drive that lap for you, over and over. It keeps lining\n"
        "itself up with your recording - where you are, how fast you're going, which way the tractor faces -\n"
        "and presses keys in step with the game's frames, so corners happen the way you drove them.");
    ImGui::Spacing();
    DrawSetup(s);

    ImGui::Spacing();
    ImGui::SeparatorText("Now");
    DrawControl(s, sel);

    ImGui::Spacing();
    ImGui::SeparatorText("Your farms");
    DrawLibrary(routes, sel, busy);

    if (sel) {
        ImGui::Spacing();
        ImGui::SeparatorText("Map");
        DrawMap(*sel, s, playingThis);
        ImGui::Spacing();
        DrawSteps(*sel, s, playingThis);
    }
}
