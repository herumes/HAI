#pragma once
#include <functional>
#include <string>
#include <vector>

namespace AiContext { struct Built; }

namespace AI {

	// What happened to one tool call the model made.
	struct ToolEvent {
		std::string name, args, result;
		int outcome = 0;         // 0 ran, 1 declined by the user, 2 switched off, 3 unknown tool
		long long ms = 0;
	};

	// What one request did, for the Studio's inspector.
	struct Trace {
		std::string provider, model, error;
		std::string requestJson;         // the messages of the first round
		std::vector<ToolEvent> tools;
		int rounds = 0;
		int promptTokens = -1, replyTokens = -1;   // as reported by the server, -1 = not reported
		long long ms = 0;
	};

	enum class ToolVerdict { Run, Deny };

	struct CallOptions {
		std::string speaker;             // who asked, for {speaker} in prompts
		std::string extraNote;           // one-off instruction added for this reply (retry with a note)
		bool useFrozenChat = false;      // reuse `frozenChat` instead of the live chat lines
		std::string frozenChat;
		bool remember = true;            // add the exchange to the conversation memory on success
		// Called for a tool whose rule is "ask". Runs on the worker thread and may block; may edit
		// the arguments. Without it such a tool just runs.
		std::function<ToolVerdict(const std::string& name, std::string& argsJson)> approveTool;
		// Gets what was sent (the system prompt, the chat lines) right after it was worked out.
		std::function<void(const AiContext::Built&)> onContext;
	};

	// Main entry – handles tool calling loop internally (Ollama or OpenAI-compatible).
	// Drop-in replacement for the old OllamaProvider::GetResponse.
	bool GetResponse(const std::string& userPrompt, std::string& response);
	bool GetResponse(const std::string& userPrompt, std::string& response,
		const CallOptions& opt, Trace* trace);

	// Optional early load (also called automatically on first request).
	void EnsureKnowledgeLoaded();

	// Request cancel of the in-flight AI HTTP request and skip the reply.
	// Safe to call from the UI thread.
	void RequestCancel();

	// True while a cancel has been requested and the worker has not yet exited.
	bool IsCancelRequested();

} // namespace AI

