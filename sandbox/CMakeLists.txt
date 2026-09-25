if(WIN32)
    add_executable(aether_sandbox main.cpp)
    target_link_libraries(aether_sandbox PRIVATE Aether::Engine)
endif()
