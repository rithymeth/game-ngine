cmake_minimum_required(VERSION 3.24)
project(AetherTemplateSdkConsumer LANGUAGES CXX)

find_package(Aether CONFIG REQUIRED)

add_executable(aether_template_sdk_consumer main.cpp)
target_link_libraries(aether_template_sdk_consumer PRIVATE Aether::Templates)
