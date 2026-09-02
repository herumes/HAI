#pragma once
#include <string>

namespace AI {

	// Main entry – handles tool calling loop internally (Ollama or OpenAI-compatible).
	// Drop-in replacement for the old OllamaProvider::GetResponse.
	bool GetResponse(const std::string& userPrompt, std::string& response);

	// Optional early load (also called automatically on first request).
	void EnsureKnowledgeLoaded();

	// Request cancel of the in-flight AI HTTP request and skip the reply.
	// Safe to call from the UI thread.
	void RequestCancel();

	// True while a cancel has been requested and the worker has not yet exited.
	bool IsCancelRequested();

} // namespace AI

