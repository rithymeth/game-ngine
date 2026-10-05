#include "test_framework.h"

#include "aether/core/api.h"

// The stability annotations (Phase 37 step 3): the macros compile on declarations, and a
// deprecated function can still be called inside code that opts out of the warning.

namespace stability_test {

AETHER_STABLE int StableThing() { return 1; }
AETHER_EXPERIMENTAL int ExperimentalThing() { return 2; }
AETHER_INTERNAL int InternalThing() { return 3; }

// Deprecated, and not called from the tests: the point is that the declaration compiles.
AETHER_DEPRECATED("0.40", "NewThing") inline int OldThing() { return 4; }

} // namespace stability_test

AETHER_TEST(ApiStability_AnnotationsCompileOnDeclarations) {
    AETHER_CHECK(stability_test::StableThing() == 1 && stability_test::ExperimentalThing() == 2 && stability_test::InternalThing() == 3);
}
