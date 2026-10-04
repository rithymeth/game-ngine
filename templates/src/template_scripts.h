#pragma once

// The Luau controllers the project templates ship (Phase 26 step 2). Each is
// a script class: its top-level fields are the settings the Inspector shows
// (and a scene can override per entity), and OnCreate/OnUpdate drive it.
// They read the bindings the templates create: Move (x right, y forward),
// Look (mouse or right stick; x right, y down) and Jump.

namespace aether::templates {

extern const char* const kFirstPersonScript;
extern const char* const kThirdPersonScript;
extern const char* const kTopDownScript;
extern const char* const kVehicleScript;

} // namespace aether::templates
