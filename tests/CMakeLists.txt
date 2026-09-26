add_executable(aether_tests
    main.cpp
    test_memory.cpp
    test_math.cpp
    test_filesystem.cpp
    test_job_system.cpp
    test_ecs.cpp
    test_serialization.cpp
)

target_link_libraries(aether_tests PRIVATE Aether::Engine)
target_include_directories(aether_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_sources(aether_tests PRIVATE test_render_graph.cpp)
endif()

if(TARGET Aether::Physics)
    target_sources(aether_tests PRIVATE test_physics.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::Physics)
endif()

add_test(NAME aether_tests COMMAND aether_tests)
