#pragma once

#include "aether/blueprint/validate.h"

#include <map>

namespace aether::bp {

// Macro inlining (Phase 12 step 4, part 5): every "Macro:Name" instance node
// is replaced by a copy of the macro graph's nodes, wired through its input
// and output tunnels. Each copy gets fresh node IDs, so stateful nodes inside
// a macro have separate state per instance.

// BP016 for macros that contain themselves (directly or through others),
// reported on the instance node that closes the loop.
void CheckMacroCycles(const Blueprint& blueprint, ValidationResult& result);

// Inlines every macro instance in `graph` (nested ones too). `origin` maps
// each generated node to the instance node (in the original graph) it came
// from, for diagnostics.
void ExpandMacros(const Blueprint& blueprint, Graph& graph, std::map<NodeId, NodeId>& origin);

} // namespace aether::bp
