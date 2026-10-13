# Lantern Run (Phase 48 step 3): a small game written against the frozen API only, as a third party would. CTest runs
# it; tools/check_api_stability.py --consumer samples/frozen_api_game fails if it names a header that is not stable.
add_executable(frozen_api_game lantern_run.cpp main.cpp)
target_link_libraries(frozen_api_game PRIVATE Aether::Engine)
add_test(NAME frozen_api_game COMMAND frozen_api_game)
