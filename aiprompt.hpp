#pragma once
// The system prompt as a stack of preset files.
//
// CONTEXT.md is the base and always exists in the list; every other preset is a
// prompts\<name>.md next to the DLL. Presets that are switched on are joined,
// in order, with a blank line between them - so "persona", "rules" and "current
// event" can each be a small file that is turned on or off on its own.
// The list and the on/off switches are the aiStack pref.
#include "aitext.hpp"
#include <string>
#include <vector>

namespace AiPrompt {

constexpr const char* kBase = "CONTEXT.md";

struct Item {
    std::string name;       // "CONTEXT.md" for the base, else the file name without ".md"
    bool enabled = false;
    bool base = false;
    bool exists = false;    // the base can be missing until it is first saved
    size_t bytes = 0;
};

// The stack as it is right now: the saved order, files that vanished dropped, new
// files appended switched off. Cheap to call every frame (cached for a second).
std::vector<Item> Items();

bool Read(const std::string& name, std::string& text);
// Writes atomically. For the base this also reloads the system prompt.
bool Write(const std::string& name, const std::string& text, std::string& err);
// Makes prompts\<name>.md (a free name is found when it is taken). The new preset is
// added to the end of the stack, switched off.
bool Create(const std::string& wanted, const std::string& text, std::string& created, std::string& err);
bool Rename(const std::string& name, const std::string& wanted, std::string& err);
bool Remove(const std::string& name, std::string& err);   // the base can't be removed
void SetEnabled(const std::string& name, bool on);
void Move(const std::string& name, int delta);

// The prompt the model gets: every enabled preset, variables filled in. Empty when
// nothing is on or everything is empty. `enabledCount` = presets that contributed.
std::string Assemble(const AiText::Vars& vars, int* enabledCount = nullptr);

std::wstring Dir();     // <DLL folder>\prompts\  (created when first needed)

struct Starter { const char* name; const char* text; };
const std::vector<Starter>& Starters();

// The pieces the Studio shows: (name, text) of each enabled preset after variable expansion.
struct Section { std::string name; std::string text; };
std::vector<Section> Sections(const AiText::Vars& vars);

} // namespace AiPrompt
