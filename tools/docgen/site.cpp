#include "site.h"

#include "site_assets.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace aether::docs {
namespace {

struct SitePage {
    std::string path;
    std::string content;
    std::string title;
};

bool ReadFile(const std::filesystem::path& path, std::string& content) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::ostringstream stream;
    stream << input.rdbuf();
    if (input.bad()) return false;
    content = stream.str();
    return true;
}

std::string ExtractTitle(const std::string& markdown, const std::string& fallback) {
    std::istringstream input(markdown);
    for (std::string line; std::getline(input, line);) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] != '#') continue;
        const auto text = line.find_first_not_of("# \t", first);
        if (text != std::string::npos) return line.substr(text);
    }
    return fallback;
}

bool WriteFile(const std::filesystem::path& path, const std::string& content,
               std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "Couldn't create " + path.parent_path().string() + ": " + ec.message();
        return false;
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        if (error) *error = "Couldn't write " + path.string();
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        if (error) *error = "Couldn't finish writing " + path.string();
        return false;
    }
    return true;
}

} // namespace

bool WriteDocumentationSite(const std::filesystem::path& docs_directory,
                            const std::filesystem::path& output_directory,
                            std::string* error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(docs_directory, ec) || ec) {
        if (error) *error = "Documentation source directory does not exist: " + docs_directory.string();
        return false;
    }
    std::filesystem::create_directories(output_directory, ec);
    if (ec) {
        if (error) *error = "Couldn't create site directory " + output_directory.string() + ": " + ec.message();
        return false;
    }

    std::vector<SitePage> pages;
    std::filesystem::recursive_directory_iterator it(docs_directory,
        std::filesystem::directory_options::skip_permission_denied, ec), end;
    if (ec) {
        if (error) *error = "Couldn't scan documentation source directory: " + ec.message();
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            if (error) *error = "Couldn't scan documentation source directory: " + ec.message();
            return false;
        }
        if (!it->is_regular_file(ec) || ec || it->path().extension() != ".md") continue;
        const auto relative = std::filesystem::relative(it->path(), docs_directory, ec);
        if (ec) {
            if (error) *error = "Couldn't resolve a documentation path: " + ec.message();
            return false;
        }
        const std::string path = "docs/" + relative.generic_string();
        std::string content;
        if (!ReadFile(it->path(), content)) {
            if (error) *error = "Couldn't read " + it->path().string();
            return false;
        }
        pages.push_back({path, std::move(content), {}});
        pages.back().title = ExtractTitle(pages.back().content, relative.generic_string());
    }

    std::string api;
    if (!ReadFile(output_directory / "api" / "API.md", api)) {
        if (error) *error = "API reference is missing; generate it in <site>/api before building the site";
        return false;
    }
    pages.push_back({"api/API.md", std::move(api), "API reference"});
    std::stable_sort(pages.begin(), pages.end(), [](const SitePage& a, const SitePage& b) {
        const auto rank = [](const std::string& path) {
            if (path == "docs/manual/README.md") return 0;
            if (path.rfind("docs/manual/", 0) == 0) return 1;
            if (path == "api/API.md") return 2;
            if (path == "docs/README.md") return 3;
            return 4;
        };
        const int rank_a = rank(a.path), rank_b = rank(b.path);
        return rank_a == rank_b ? a.path < b.path : rank_a < rank_b;
    });

    nlohmann::ordered_json data = nlohmann::ordered_json::object();
    for (const SitePage& page : pages) {
        data[page.path] = {{"path", page.path}, {"title", page.title}, {"content", page.content}};
    }
    if (!WriteFile(output_directory / "site-data.js", "window.AETHER_DOCS=" + data.dump() + ";\n", error) ||
        !WriteFile(output_directory / "site.js", site_assets::kJavaScript, error) ||
        !WriteFile(output_directory / "site.css", site_assets::kStylesheet, error)) return false;

    static const std::string index = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="description" content="Aether Engine manual and generated API reference">
  <title>Aether Documentation</title>
  <link rel="stylesheet" href="site.css">
  <script src="site-data.js"></script>
  <script defer src="site.js"></script>
</head>
<body>
  <header class="top"><a class="brand" href="#/docs/manual/README.md">Aether Engine</a><small>Documentation</small></header>
  <div class="layout">
    <aside aria-label="Documentation navigation">
      <label class="visually-hidden" for="doc-search">Search documentation</label>
      <input id="doc-search" type="search" placeholder="Search documentation…" autocomplete="off">
      <nav id="doc-nav" aria-label="Pages"></nav>
    </aside>
    <main>
      <div id="page-title"></div>
      <article id="doc-content"></article>
    </main>
  </div>
</body>
</html>
)HTML";
    if (!WriteFile(output_directory / "index.html", index, error)) return false;
    return true;
}

} // namespace aether::docs
