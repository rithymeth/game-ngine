#pragma once

#include "aether/blueprint/bytecode.h"
#include "aether/blueprint/validate.h"

#include <memory>

namespace aether::bp {

struct CompileResult {
    std::shared_ptr<const CompiledBlueprint> blueprint; // null if there were errors
    ValidationResult diagnostics;                       // validation plus compiler errors (BP002, BP012)
    bool Ok() const { return blueprint != nullptr; }
};

// Validates, then lowers each event and function graph to bytecode (Phase
// 12 step 2, docs/ROADMAP_DETAILS.md §C.3):
// - exec links are walked from each entry; Branch becomes JMPF, Sequence
//   runs its outputs in order, and an exec wire back into a node already on
//   the path becomes a loop (the instruction budget guards it);
// - pure nodes are emitted right before the impure node that reads them,
//   and cached for the rest of that node's evaluation;
// - implicit conversions (int -> float, float -> int, anything -> string)
//   become CONV instructions.
CompileResult CompileBlueprint(const Blueprint& blueprint);

} // namespace aether::bp
