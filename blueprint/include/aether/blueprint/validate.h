#pragma once

#include "aether/blueprint/nodes.h"

#include <string>
#include <vector>

namespace aether::bp {

// Graph validation (Phase 12 step 1): everything that can be checked
// before compiling. Codes and wording follow the message catalog in
// docs/design/BLUEPRINT_NODES.md §13.

enum class Severity : u8 { Error, Warning };

struct Diagnostic {
    std::string code; // "BP001"
    Severity severity = Severity::Error;
    std::string graph;
    NodeId node = 0; // 0: the graph as a whole
    std::string pin; // "" when not about one pin
    std::string message;
};

struct ValidationResult {
    std::vector<Diagnostic> diagnostics; // grouped by graph
    usize errors = 0;
    usize warnings = 0;

    bool Ok() const { return errors == 0; }
    // The diagnostics for one node (the editor shows them on it).
    std::vector<const Diagnostic*> For(const std::string& graph, NodeId node) const;
    bool Has(const std::string& code) const;
};

ValidationResult ValidateBlueprint(const Blueprint& blueprint);

} // namespace aether::bp
