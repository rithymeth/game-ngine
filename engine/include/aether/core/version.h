#pragma once

namespace aether {

// Engine version, written into project files so a project knows which engine
// it was last saved with. Bumped per roadmap phase: 0.<phase>.<patch>.
inline constexpr int kEngineVersionMajor = 0;
inline constexpr int kEngineVersionMinor = 7;
inline constexpr int kEngineVersionPatch = 0;
inline constexpr const char* kEngineVersion = "0.7.0";

} // namespace aether
