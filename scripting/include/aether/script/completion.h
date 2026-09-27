#pragma once

#include "aether/core/base.h"

#include <string>
#include <string_view>
#include <vector>

namespace aether::script {

enum class CompletionKind {
    Keyword,
    Variable,  // a local, a parameter or a loop variable
    Global,    // print, pairs, ...
    Module,    // Input, Timer, math, ...
    Function,  // a module's function
    Method,    // called with ':'
    Field,
    Component, // a component type name, inside :Get("...")
    Callback,  // a lifecycle callback, after "function Class:"
};

struct CompletionItem {
    std::string label;
    CompletionKind kind = CompletionKind::Variable;
    std::string detail; // a type ("f32") or a signature ("(dt: number)")
};

struct CompletionResult {
    // The text being completed is [replace_from, cursor): an item replaces it.
    usize replace_from = 0;
    std::vector<CompletionItem> items; // sorted by label, prefix-filtered
};

// Code completion for the script editor (Phase 11 step 6,
// docs/design/PHASE_SPECS.md §11.5). No type checker: it reads the text
// around `cursor` (a byte offset into `source`) and knows
//   - the engine API: world, Input, Timer, Event and the Luau libraries;
//   - components from reflection: `:Get("` lists component names, and a
//     component value (`e:Get("Health")`, or a local holding one) lists its
//     reflected fields after '.' and functions after ':'; struct and vector
//     fields go one level further ("t.position.x");
//   - entities (`self.entity`, `world:Spawn()`, ...), events, timers and
//     connections;
//   - `self.` lists the class's fields and `self:` its methods, as they
//     appear in the source; "function Class:" lists the lifecycle callbacks;
//   - otherwise keywords, globals and the locals declared above the cursor.
// Prefix matching ignores case. Nothing is offered inside comments or
// ordinary strings.
CompletionResult CompleteScript(std::string_view source, usize cursor);

} // namespace aether::script
