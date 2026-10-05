# Aether Loc (Phase 29, docs/design/PHASE_SPECS.md §29): localization.
# LocText (a key plus source text), string tables with CSV import/export,
# language fallback chains, and plural-aware message formatting. Engine-only
# and dependency-free, so it builds and tests headless on every platform.
add_library(aether_loc STATIC
    src/language.cpp
    src/string_table.cpp
    src/message_format.cpp
    src/localizer.cpp
    src/localization.cpp
    src/localize.cpp
    src/gather.cpp
    src/merge.cpp
    src/po.cpp
    src/pseudo.cpp
    src/localized_path.cpp
    src/project_sync.cpp
)
target_include_directories(aether_loc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_loc PUBLIC cxx_std_20)
target_link_libraries(aether_loc PUBLIC Aether::Engine)
add_library(Aether::Loc ALIAS aether_loc)
