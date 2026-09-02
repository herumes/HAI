#pragma once
// ImGui overlay + external standalone window.
#include "pch.h"

void RenderOverlay();
void ApplyTheme(int preset);
void OverlayThread();
void ToggleCompactChatMode();
void ApplyWindowStyle();

// Optional (if still used elsewhere)
bool CreateOverlayWindow();
void DestroyOverlayWindow();