#pragma once
// Josebot extras: time tool, reminders, echo placeholders, private encrypted chat, AI context dump, web search.
#include "pch.h"
#include <string>
#include <vector>
#include <cstdint>

namespace Extras {

	// ---------------------------------------------------------------------------
	// Time
	// ---------------------------------------------------------------------------
	// query: empty/local, city/country, IANA-ish name, or abbr (UTC, GMT, CST, PST, CET, ...).
	std::string ToolGetTime(const std::string& query);

	// ---------------------------------------------------------------------------
	// Reminders (in-memory; flushed by TickReminders)
	// ---------------------------------------------------------------------------
	std::string ToolSetReminder(const std::string& user, int inSeconds, const std::string& message);
	std::string ToolListReminders(const std::string& userFilter);
	std::string ToolCancelReminder(uint64_t id);

	// Call from WM_TIMER / ProcessPending path ~every 150ms–1s.
	// deliverCb(text) should queue a bot chat line (e.g. QueueBotReply).
	void TickReminders(void (*deliverCb)(const std::string& text));

	// Owner helpers
	std::string FormatReminderList(const std::string& userFilter);
	bool CancelReminderById(uint64_t id);

	// ---------------------------------------------------------------------------
	// Echo placeholders: ${math(...)}, ${time}, ${time:ZONE}, ${uptime}, ${random:N}, ${random:A-B}
	// ---------------------------------------------------------------------------
	std::string ExpandEchoPlaceholders(const std::string& input);

	// ---------------------------------------------------------------------------
// Private encrypted channel (AES-256-GCM via Windows BCrypt)
// Wire format in chat:  ~jb1.<base64url(nonce||ciphertext||tag)>
// ---------------------------------------------------------------------------
	void SetPrivatePassphrase(const std::string& passphrase); // derives key in memory (always trims)
	bool HasPrivateKey();
	void ClearPrivateKey();
	bool IsPrivateCiphertext(const std::string& body);
	// Encrypt plaintext -> ~jb1....  (empty on failure)
	std::string EncryptPrivateMessage(const std::string& plaintext);
	// Decrypt ~jb1.... -> plaintext (empty on failure / wrong key)
	std::string DecryptPrivateMessage(const std::string& wire);
	// Short non-reversible fingerprint of the current key (for comparing two clients)
	std::string PrivateKeyFingerprint();

	// ---------------------------------------------------------------------------
	// AI context dump (matches what GetResponse sends)
	// ---------------------------------------------------------------------------
	std::string BuildAiContextDump();

	// Last composed user message from AI::GetResponse (set by ai.cpp)
	void SetLastComposedPrompt(const std::string& composed);
	std::string GetLastComposedPrompt();

	// ---------------------------------------------------------------------------
	// Web search (LangSearch API) — token-budgeted
	// ---------------------------------------------------------------------------
	// Returns compact multi-line text for the model. Error string on failure.
	// freshnessOverride: optional oneDay|oneWeek|oneMonth|oneYear|noLimit (empty = use setting).
	std::string ToolWebSearch(const std::string& query, const std::string& freshnessOverride = "");

} // namespace Extras

