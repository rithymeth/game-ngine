# aether_soak (Phase 47 step 5): repeated cycles of long-running work (entity churn, streaming a world in and out,
# replication with spawns and despawns) with a leak gate on live heap bytes. It replaces global operator new/delete
# for itself only, so it is a program of its own. AETHER_SOAK_SCALE=N runs N times the cycles (default 1).
add_executable(aether_soak main.cpp)
target_link_libraries(aether_soak PRIVATE Aether::Engine Aether::Streaming Aether::Net)
add_test(NAME soak COMMAND aether_soak)
