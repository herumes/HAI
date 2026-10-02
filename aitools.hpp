#pragma once
// The tools the AI can call: what exists, which ones the model is offered, how
// each one is run. The rules (off / ask / auto per tool) live in the prefs.
#include "aitext.hpp"
#include <string>
#include <vector>

namespace AiTools {

struct Def {
    const char* name;
    const char* label;      // short, for the settings page
    const char* what;       // one line: what it does
    bool sideEffect;        // changes something or costs something (reminders, web search)
    const char* json;       // the schema sent to the model
};

const std::vector<Def>& All();
const Def* Find(const std::string& name);

// Off / Ask / Auto for one tool, from the per-tool rule or else the general policy.
// web_search also counts as Off while the web-search switch is off.
AiText::ToolMode ModeFor(const std::string& name);
// Set or (clear = true) remove the per-tool rule.
void SetRule(const std::string& name, AiText::ToolMode mode, bool clear);

// The "tools" array for a request: everything not Off. "[]" when nothing is offered.
std::string ToolsJson();

// Runs one tool and returns its text result (an error text for a bad call).
std::string Execute(const std::string& name, const std::string& argsJson);

} // namespace AiTools
