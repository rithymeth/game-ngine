#pragma once

#include "aether/project/project.h"

#include <filesystem>
#include <string>
#include <vector>

// Project templates (Phase 26 step 2).
// "New Project" starts from one of these: a project with a startup scene, a
// controller script, input bindings and a Blueprint, so the first thing you
// see is a game that's wired up, not an empty folder.
//
// Every template is generated in code, its art included (the floor, the player
// and the pickups are one generated cube in three colours), so what a template
// holds is always a legal project of this engine version and shows something
// when it runs.

namespace aether::templates {

struct ProjectTemplate {
    std::string id;          // "first_person"
    std::string name;        // "First Person"
    std::string genre;       // "Shooter"
    std::string description;
    // What's in it, one line each ("WASD and mouse look", ...).
    std::vector<std::string> features;
    // The ones that need an engine feature that isn't built yet say what
    // they wait for; CreateProjectFromTemplate refuses them.
    bool available = true;
    std::string unavailable_reason;
};

// Blank, First Person, Third Person, Top Down, Vehicle, 2D Platformer.
const std::vector<ProjectTemplate>& ProjectTemplates();
const ProjectTemplate* FindProjectTemplate(const std::string& id);

// CreateProject(parent_dir, name), then the template's files and settings:
//   Content/Scenes/Main.ascene          the startup scene
//   Content/Scripts/<Controller>.luau   the controller (see the template)
//   Content/Blueprints/BP_Pickup.abp    a spinning pickup, placed in the scene
//   Content/Input/*.aaction, Gameplay.amapping   Move, Look and Jump bindings
//   Content/Models/Floor|Player|Pickup.gltf, Cube.bin   the greybox models the scene shows
// Blank has only the scene. The project's startup scene is Main, and
// `always_cook` lists Input/ (nothing in a scene refers to it).
// False, with `error`, for an unknown or unavailable template, or whatever
// CreateProject refuses (a bad name, a folder that isn't empty).
bool CreateProjectFromTemplate(const std::filesystem::path& parent_dir, const std::string& name,
                               const std::string& template_id, ProjectPaths* out_paths = nullptr,
                               std::string* error = nullptr);

// The Third Person template's controller and bindings alone, for a project that has its own scene (the Procedural World baker uses it to make a baked
// world playable): under `content`, Scripts/ThirdPersonController.luau and Input/Move.aaction, Look.aaction, Jump.aaction and Gameplay.amapping (WASD and the
// left stick move relative to the camera, the mouse and right stick orbit it, Space and the A button jump, Shift and the stick click sprint). A file that is
// already there is left as it is, so a user's edits to the script or the bindings stay. `script_path`, when given, is the script's path relative to `content`.
// The scene it goes with needs an entity tagged "Player" carrying a ScriptComponent for the script and a CharacterMovement, and one with a Camera. The project must
// cook Input/ (nothing in a scene refers to it): list it in the project's always-cook folders. False, with `error`, when a file can't be written.
bool InstallThirdPersonKit(const std::filesystem::path& content, std::string* error = nullptr, std::string* script_path = nullptr);

} // namespace aether::templates
