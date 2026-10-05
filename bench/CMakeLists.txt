# Aether benchmark harness (Phase 38 step 1, docs/design/UPGRADE_PLAN.md): a small runner for
# micro and system benchmarks, with JSON reports and a baseline comparison that normalizes by a
# calibration loop (so a slow or noisy machine doesn't fail the gate). aether_bench runs them:
#   aether_bench [--filter text] [--repeat N] [--json report.json] [--baseline file.json] [--tolerance 1.5]
add_library(aether_bench_lib STATIC src/bench.cpp)
target_include_directories(aether_bench_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_bench_lib PUBLIC cxx_std_20)
target_link_libraries(aether_bench_lib PUBLIC Aether::Engine)
add_library(Aether::Bench ALIAS aether_bench_lib)

add_executable(aether_bench src/main.cpp src/bench_engine.cpp)
target_link_libraries(aether_bench PRIVATE Aether::Bench)
