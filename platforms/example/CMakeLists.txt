# The example platform plugin: a template for a real one (see
# platforms/README.md). Built when listed in AETHER_PLATFORM_PLUGINS.
aether_platform_plugin(NAME example
    SOURCES example_platform.cpp
    DEFINITIONS AETHER_HAS_EXAMPLE_PLATFORM)
