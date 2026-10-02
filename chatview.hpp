#pragma once
// The chat log as a text view you can select and copy from.
//
// ImGui has no multi-colour selectable text, so the log is laid out and drawn
// by hand: every message is one string plus colour spans, wrapped against the
// panel width, and selection is a pair of (message id, byte offset) positions.
// That is what makes click-drag across coloured runs and across messages work,
// and it is also what the copy formats (plain, colour codes, rich text,
// Discord) are built from.
#include "pch.h"
#include "chat.hpp"
#include <set>
#include <string>

namespace ChatUi {

// Does this line mention you (your name or one of the highlight words)?
// Shared by the log view, the compact overlay and the alert sound.
bool IsMention(const ChatLine& line);

// 0 show, 1 dim, 2 hide - from the hide patterns and the ignored-players option.
int LineVisibility(const ChatLine& line, const std::set<std::string>& ignoredLower);

// A game colour adjusted for the dark panel (brightness floor, mono mode).
ImVec4 DisplayColor(const ImVec4& c);

// Every frame, whether or not the overlay is visible: mention alerts and the
// on-disk chat log.
void Tick();

// The whole Chat tab: toolbar, log and input line.
void DrawChatTab();

// The Settings preview: a fixed sample conversation drawn with the live style,
// selectable like the real log.
void DrawStylePreview(float height);

// Put text on the clipboard. `html` / `rtf` may be empty; when given, apps that
// understand them (Word, Google Docs, Outlook) paste the colours too.
bool SetClipboard(const std::string& plainUtf8, const std::string& htmlFragment = {},
    const std::string& rtf = {});

// Focus the chat input on the next frame (hotkey).
void FocusInput();

// Mentions that arrived since the Chat tab was last drawn (for its label).
int UnreadMentions();

// Play the configured mention sound now (Settings "Test" button).
void TestMentionSound();

// A sound and / or a taskbar flash for something that needs attention (an AI draft to approve).
// The sound is the mention sound if one is set, else the system notification sound.
void Ping(bool sound, bool flash);

} // namespace ChatUi
