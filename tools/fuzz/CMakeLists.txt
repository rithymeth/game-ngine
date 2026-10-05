# Fuzzing (Phase 47 step 1, docs/design/UPGRADE_PLAN.md). Each target is a libFuzzer entry point
# (LLVMFuzzerTestOneInput). Two builds of each:
#   aether_replay_<name>  always built: runs the checked-in corpus (tools/fuzz/corpus/<name>) as a
#                         regression test on any compiler (ctest: fuzz_replay_<name>).
#   aether_fuzz_<name>    with -DAETHER_BUILD_FUZZERS=ON and Clang: the libFuzzer binary (address
#                         sanitizer on); run it as  aether_fuzz_<name> -max_total_time=60 corpus/<name>
option(AETHER_BUILD_FUZZERS "Build the libFuzzer fuzz targets (needs Clang)" OFF)

function(aether_fuzz_target name source)
    set(libs ${ARGN})
    add_executable(aether_replay_${name} ${source} replay_main.cpp)
    target_include_directories(aether_replay_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(aether_replay_${name} PRIVATE ${libs})
    if(TARGET aether_inventory)
        target_compile_definitions(aether_replay_${name} PRIVATE AETHER_FUZZ_INVENTORY=1)
    else()
        target_compile_definitions(aether_replay_${name} PRIVATE AETHER_FUZZ_INVENTORY=0)
    endif()
    if(TARGET aether_quests)
        target_compile_definitions(aether_replay_${name} PRIVATE AETHER_FUZZ_QUESTS=1)
    else()
        target_compile_definitions(aether_replay_${name} PRIVATE AETHER_FUZZ_QUESTS=0)
    endif()
    add_test(NAME fuzz_replay_${name} COMMAND aether_replay_${name} ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name})
    if(AETHER_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_executable(aether_fuzz_${name} ${source})
        target_include_directories(aether_fuzz_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(aether_fuzz_${name} PRIVATE ${libs})
        target_compile_options(aether_fuzz_${name} PRIVATE -fsanitize=fuzzer-no-link,address -fno-omit-frame-pointer)
        target_link_options(aether_fuzz_${name} PRIVATE -fsanitize=fuzzer,address)
        target_compile_definitions(aether_fuzz_${name} PRIVATE
            AETHER_FUZZ_INVENTORY=$<TARGET_EXISTS:aether_inventory> AETHER_FUZZ_QUESTS=$<TARGET_EXISTS:aether_quests>)
    endif()
endfunction()

set(gameplay_libs Aether::Gameplay)
if(TARGET aether_inventory)
    list(APPEND gameplay_libs Aether::Inventory)
endif()
if(TARGET aether_quests)
    list(APPEND gameplay_libs Aether::Quests)
endif()

aether_fuzz_target(pak fuzz_pak.cpp Aether::Engine)
aether_fuzz_target(scene fuzz_scene.cpp Aether::Engine)
aether_fuzz_target(blueprint fuzz_blueprint.cpp Aether::Blueprint)
aether_fuzz_target(gameplay fuzz_gameplay.cpp ${gameplay_libs})
