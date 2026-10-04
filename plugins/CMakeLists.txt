# The engine's own plugins (Phase 26 step 1, docs/design/PHASE_SPECS.md
# §26.1): each optional module as a plugin with an .aplugin descriptor and a
# runtime module, built when the module itself is.
foreach(plugin Physics Audio Navigation AI Networking)
    set(_target_Physics Aether::Physics)
    set(_target_Audio Aether::Audio)
    set(_target_Navigation Aether::Nav)
    set(_target_AI Aether::AI)
    set(_target_Networking Aether::Net)
    if(TARGET ${_target_${plugin}})
        add_subdirectory(${plugin})
    endif()
endforeach()
