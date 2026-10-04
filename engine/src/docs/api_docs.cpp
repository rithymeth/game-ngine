#include "aether/docs/api_docs.h"

#include "aether/ecs/component.h"
#include "aether/reflection/registry.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace aether::docs {

namespace stdfs = std::filesystem;

namespace {

bool IsScalar(const reflect::TypeInfo& t) {
    using reflect::TypeKind;
    return t.kind == TypeKind::Bool || t.kind == TypeKind::Int || t.kind == TypeKind::UInt || t.kind == TypeKind::Float ||
           t.kind == TypeKind::String || t.kind == TypeKind::FixedString || t.kind == TypeKind::Array;
}

std::vector<const reflect::TypeInfo*> DocumentedTypes(const ApiDocOptions& options) {
    std::map<std::string, const reflect::TypeInfo*> by_name; // sorted, one per name
    for (const reflect::TypeInfo* t : reflect::TypeRegistry::AllTypes()) {
        if (!t || !t->name || !*t->name) continue;
        if (options.skip_scalars && IsScalar(*t)) continue;
        if (t->asset_type) continue; // AssetRef<T>: written where it is used
        by_name.emplace(t->name, t);
    }
    std::vector<const reflect::TypeInfo*> out;
    for (const auto& [name, t] : by_name) out.push_back(t);
    return out;
}

std::string Anchor(const std::string& name) {
    std::string a;
    for (char c : name) a.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    return a;
}

std::string FlagsText(const reflect::FieldInfo& f) {
    std::vector<std::string> parts;
    if (f.HasFlag(reflect::Field_EditAnywhere)) {
        parts.push_back("editable");
    } else if (f.HasFlag(reflect::Field_ReadOnly)) {
        parts.push_back("read-only");
    } else {
        parts.push_back("internal");
    }
    if (f.HasFlag(reflect::Field_Transient)) parts.push_back("not saved");
    if (f.HasFlag(reflect::Field_EditorOnly)) parts.push_back("editor only");
    if (f.HasFlag(reflect::Field_BlueprintReadWrite)) parts.push_back("Blueprint");
    if (f.HasFlag(reflect::Field_Replicated)) parts.push_back("replicated");
    std::string out;
    for (usize i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
    return out;
}

std::string Number(f64 v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

std::string DetailsText(const reflect::Meta& m) {
    std::string out;
    const auto add = [&](const std::string& s) { out += (out.empty() ? "" : "; ") + s; };
    if (m.tooltip) add(m.tooltip);
    if (m.HasRange()) add("range " + Number(m.range_min) + " to " + Number(m.range_max));
    if (m.units) add(std::string("unit ") + m.units);
    if (m.category) add(std::string("group ") + m.category);
    if (m.is_color) add("a colour");
    return out;
}

std::string Escape(const std::string& text) { // inside a table cell
    std::string out;
    for (char c : text) {
        if (c == '|') out += "\\|";
        else if (c == '\n') out += ' ';
        else out.push_back(c);
    }
    return out;
}

std::string FunctionFlagsText(const reflect::FunctionInfo& f) {
    std::vector<std::string> parts;
    if (f.HasFlag(reflect::Fn_BlueprintCallable)) parts.push_back("Blueprint");
    if (f.HasFlag(reflect::Fn_Pure)) parts.push_back("pure");
    if (f.HasFlag(reflect::Fn_Const)) parts.push_back("const");
    if (f.HasFlag(reflect::Fn_Static)) parts.push_back("static");
    if (f.HasFlag(reflect::Fn_Server)) parts.push_back("server");
    if (f.HasFlag(reflect::Fn_Client)) parts.push_back("client");
    if (f.HasFlag(reflect::Fn_Multicast)) parts.push_back("multicast");
    if (f.HasFlag(reflect::Fn_Unreliable)) parts.push_back("unreliable");
    std::string out;
    for (usize i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
    return out;
}

std::string Signature(const reflect::FunctionInfo& f) {
    std::string out = std::string(f.name) + "(";
    for (usize i = 0; i < f.params.size(); ++i) {
        if (i) out += ", ";
        out += std::string(f.params[i].name) + ": " + (f.params[i].type ? DisplayTypeName(*f.params[i].type) : "?");
    }
    out += ")";
    if (f.return_type) out += " -> " + DisplayTypeName(*f.return_type);
    return out;
}

} // namespace

std::string DisplayTypeName(const reflect::TypeInfo& type) {
    if (type.kind == reflect::TypeKind::Array && type.element) return "Array<" + DisplayTypeName(*type.element) + ">";
    if (type.asset_type) return std::string("AssetRef<") + type.asset_type + ">";
    if (type.kind == reflect::TypeKind::FixedString) return "char[" + std::to_string(type.size) + "]";
    return type.name ? type.name : "?";
}

std::string TypeCategory(const reflect::TypeInfo& type) {
    if (type.kind == reflect::TypeKind::Enum) return "Enum";
    if (type.kind == reflect::TypeKind::Struct && type.name && FindComponentIdByName(type.name) != kInvalidComponentId) {
        return "Component";
    }
    return "Struct";
}

std::string GenerateApiMarkdown(const ApiDocOptions& options) {
    const std::vector<const reflect::TypeInfo*> types = DocumentedTypes(options);
    std::set<std::string> documented;
    for (const reflect::TypeInfo* t : types) documented.insert(t->name);
    const auto type_cell = [&](const reflect::TypeInfo& t) {
        const std::string text = DisplayTypeName(t);
        // The element or the type itself, linked when it has an entry.
        const reflect::TypeInfo* target = (t.kind == reflect::TypeKind::Array && t.element) ? t.element : &t;
        if (target->name && documented.count(target->name)) {
            if (text == target->name) return "[`" + text + "`](#" + Anchor(target->name) + ")";
            return "`" + text + "` ([" + target->name + "](#" + Anchor(target->name) + "))";
        }
        return "`" + text + "`";
    };

    std::ostringstream out;
    out << "# " << options.title << "\n\n";
    out << "Generated from the reflection registry: every type below was registered by the engine, a plugin or the game.\n"
           "Don't edit it by hand; change the type's `AETHER_REFLECT` (tooltips, ranges and units come from there) and regenerate.\n\n";

    std::map<std::string, std::vector<const reflect::TypeInfo*>> groups;
    for (const reflect::TypeInfo* t : types) groups[TypeCategory(*t)].push_back(t);
    out << "## Contents\n\n";
    for (const char* group : {"Component", "Struct", "Enum"}) {
        if (groups[group].empty()) continue;
        out << "- **" << group << "s** (" << groups[group].size() << "): ";
        for (usize i = 0; i < groups[group].size(); ++i) {
            out << (i ? ", " : "") << "[" << groups[group][i]->name << "](#" << Anchor(groups[group][i]->name) << ")";
        }
        out << "\n";
    }
    out << "\n";

    for (const char* group : {"Component", "Struct", "Enum"}) {
        if (groups[group].empty()) continue;
        out << "## " << group << "s\n\n";
        for (const reflect::TypeInfo* t : groups[group]) {
            out << "### " << t->name << "\n\n";
            out << "*" << group << ", " << t->size << " bytes, schema version " << t->version << "*\n\n";
            if (t->kind == reflect::TypeKind::Enum) {
                out << "| Value | Number |\n|---|---|\n";
                for (const reflect::EnumValue& v : t->enum_values) out << "| `" << v.name << "` | " << v.value << " |\n";
                out << "\n";
                continue;
            }
            if (!t->fields.empty()) {
                out << "| Field | Type | Editing | Details |\n|---|---|---|---|\n";
                for (const reflect::FieldInfo& f : t->fields) {
                    out << "| `" << f.name << "` | " << (f.type ? type_cell(*f.type) : "?") << " | " << FlagsText(f) << " | "
                        << Escape(DetailsText(f.meta)) << " |\n";
                }
                out << "\n";
            }
            if (!t->functions.empty()) {
                out << "Functions:\n\n";
                for (const reflect::FunctionInfo& f : t->functions) {
                    out << "- `" << Signature(f) << "`";
                    const std::string flags = FunctionFlagsText(f);
                    if (!flags.empty()) out << " (" << flags << ")";
                    out << "\n";
                }
                out << "\n";
            }
        }
    }
    return out.str();
}

nlohmann::json GenerateApiJson(const ApiDocOptions& options) {
    nlohmann::json types = nlohmann::json::array();
    for (const reflect::TypeInfo* t : DocumentedTypes(options)) {
        nlohmann::json entry = {{"name", t->name}, {"category", TypeCategory(*t)}, {"size", t->size}, {"version", t->version}};
        nlohmann::json fields = nlohmann::json::array();
        for (const reflect::FieldInfo& f : t->fields) {
            nlohmann::json field = {{"name", f.name},
                                    {"type", f.type ? DisplayTypeName(*f.type) : "?"},
                                    {"editing", f.HasFlag(reflect::Field_EditAnywhere) ? "editable"
                                                : f.HasFlag(reflect::Field_ReadOnly)   ? "read-only"
                                                                                       : "internal"},
                                    {"flags", FlagsText(f)}};
            if (f.meta.tooltip) field["tooltip"] = f.meta.tooltip;
            if (f.meta.HasRange()) field["range"] = {f.meta.range_min, f.meta.range_max};
            if (f.meta.units) field["units"] = f.meta.units;
            if (f.meta.category) field["group"] = f.meta.category;
            fields.push_back(std::move(field));
        }
        entry["fields"] = std::move(fields);
        nlohmann::json functions = nlohmann::json::array();
        for (const reflect::FunctionInfo& f : t->functions) {
            functions.push_back({{"name", f.name}, {"signature", Signature(f)}, {"flags", FunctionFlagsText(f)}});
        }
        entry["functions"] = std::move(functions);
        nlohmann::json values = nlohmann::json::array();
        for (const reflect::EnumValue& v : t->enum_values) values.push_back({{"name", v.name}, {"value", v.value}});
        entry["values"] = std::move(values);
        types.push_back(std::move(entry));
    }
    return {{"title", options.title}, {"types", std::move(types)}};
}

bool WriteApiDocs(const stdfs::path& directory, const ApiDocOptions& options, std::string* error) {
    std::error_code ec;
    stdfs::create_directories(directory, ec);
    const auto write = [&](const char* name, const std::string& text) {
        std::ofstream out(directory / name, std::ios::binary);
        if (!out) {
            if (error) *error = "Couldn't write " + (directory / name).string();
            return false;
        }
        out << text;
        return true;
    };
    return write("API.md", GenerateApiMarkdown(options)) && write("api.json", GenerateApiJson(options).dump(2));
}

} // namespace aether::docs
