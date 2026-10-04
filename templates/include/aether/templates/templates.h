#pragma once

#include "aether/project/project.h"

#include <filesystem>
#include <string>
#include <vector>

// Project templates (Phase 26 step 2, docs/design/PHASE_SPECS.md §26.2).
// "New Project" starts from one of these: a project with a startup scene, a
// controller script, input bindings and a Blueprint, so the first thing you
// see is a game that's wired up, not an empty folder.
//
// Every template is generated in code (no art: the scenes are entities with
// transforms, tags, a camera, a script and a Blueprint), so what a template
// holds is always a legal project of this engine version.

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
// Blank has only the scene. The project's startup scene is Main, and
// `always_cook` lists Input/ (nothing in a scene refers to it).
// False, with `error`, for an unknown or unavailable template, or whatever
// CreateProject refuses (a bad name, a folder that isn't empty).
bool CreateProjectFromTemplate(const std::filesystem::path& parent_dir, const std::string& name,
                               const std::string& template_id, ProjectPaths* out_paths = nullptr,
                               std::string* error = nullptr);

} // namespace aether::templates
