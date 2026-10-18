#include <aether/templates/templates.h>

#include <filesystem>
#include <iostream>
#include <string>

int main() {
    namespace fs = std::filesystem;
    using namespace aether::templates;

    const fs::path root = fs::temp_directory_path() / "aether-installed-sdk-templates";
    std::error_code ec;
    fs::remove_all(root, ec);
    if (ec || !fs::create_directories(root, ec) || ec) {
        std::cerr << "Could not prepare the SDK template output directory: " << ec.message() << '\n';
        return 1;
    }

    bool created_any = false;
    for (const ProjectTemplate& project_template : ProjectTemplates()) {
        if (!project_template.available) continue;
        aether::ProjectPaths paths;
        std::string error;
        if (!CreateProjectFromTemplate(root, project_template.id, project_template.id, &paths, &error)) {
            std::cerr << "Installed SDK could not create template '" << project_template.id << "': " << error << '\n';
            fs::remove_all(root, ec);
            return 1;
        }
        if (!fs::exists(paths.file, ec) || ec) {
            std::cerr << "Template '" << project_template.id << "' did not create its project file\n";
            fs::remove_all(root, ec);
            return 1;
        }
        created_any = true;
    }

    fs::remove_all(root, ec);
    if (ec || !created_any) {
        std::cerr << "No available templates were created, or the output directory could not be cleaned\n";
        return 1;
    }
    return 0;
}
