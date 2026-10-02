#pragma once
// ImGui overlay + external standalone window.
#include "pch.h"

void RenderOverlay();
void ApplyTheme(int preset);
void OverlayThread();
void ToggleCompactChatMode();
void ApplyWindowStyle();

// Farm tab (ui_farm.cpp).
void DrawFarmTab();

// AI Studio tab (ui_aistudio.cpp): inbox, prompt presets, live context.
void DrawAiStudioTab();
// The controls for what is sent with each question; the Studio's Context tab and the
// Settings > AI control page both show them.
void DrawAiContextSettings();
// Bring up the Studio (0 inbox, 1 prompt, 2 context).
void UiOpenStudioPage(int page);
// True once after UiOpenStudioPage, so the tab bar can select the tab.
bool UiTakeStudioTabRequest();

// Build tab: "Make a build with an AI" (ui_aiprefab.cpp) - copy the prompt, paste the
// reply, turn it into a prefab. And the words shown next to a block's rotation number
// ("tall side toward the east") so a stair is never guessed at.
void DrawAiPrefabSection();
std::string BuildRotationLabel(int rotation, const std::string& blockName);
struct Prefab;
void DrawPreview3dRow(const std::shared_ptr<const Prefab>& p);

// Settings tab (ui_settings.cpp).
void DrawSettingsTab();
// Switch the overlay to the Settings tab, on the named page.
void UiOpenSettings(const char* page);
// True once after UiOpenSettings, so the tab bar can select the tab.
bool UiTakeSettingsTabRequest();
// While a hotkey field waits for a key, the global hotkeys stand down.
bool UiIsCapturingHotkey();

// Whether the compact chat window is showing instead of the full overlay.
bool IsCompactMode();