#pragma once

namespace aether {

// Engine version, written into project files so a project knows which engine
// it was last saved with. Matches the published engine release version.
inline constexpr int kEngineVersionMajor = 0;
inline constexpr int kEngineVersionMinor = 27;
inline constexpr int kEngineVersionPatch = 6;
inline constexpr const char* kEngineVersion = "0.27.6";

} // namespace aether
