add_executable(aether_editor main.cpp)
target_link_libraries(aether_editor PRIVATE Aether::Engine Aether::Physics ImGui::ImGui)
