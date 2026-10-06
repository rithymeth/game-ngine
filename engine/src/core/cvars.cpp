#include "aether/core/cvars.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <sstream>

namespace aether {
namespace cvars {

namespace {

struct RegistryInternals {
    std::mutex mutex;
    std::vector<std::unique_ptr<CVar>> cvars; // stable: never reallocated (std::deque-like via unique_ptr)
    std::vector<std::unique_ptr<ConsoleCommand>> commands;
    std::once_flag builtins_once;
};

RegistryInternals& Registry() {
    static RegistryInternals registry;
    return registry;
}

CVarType ParseType(const char* type) {
    std::string t = type != nullptr ? type : "int";
    if (t == "float" || t == "double") return CVarType::Float;
    if (t == "bool") return CVarType::Bool;
    if (t == "string") return CVarType::String;
    return CVarType::Int;
}

bool ParseBool(const std::string& v, bool& out) {
    if (v == "1" || v == "true" || v == "on" || v == "yes") { out = true; return true; }
    if (v == "0" || v == "false" || v == "off" || v == "no") { out = false; return true; }
    return false;
}

const char* TypeName(CVarType type) {
    switch (type) {
        case CVarType::Int: return "int";
        case CVarType::Float: return "float";
        case CVarType::Bool: return "bool";
        case CVarType::String: return "string";
    }
    return "?";
}

std::string Canonicalize(CVarType type, const std::string& raw) {
    switch (type) {
        case CVarType::Int: {
            char* end = nullptr;
            const long long v = std::strtoll(raw.c_str(), &end, 10);
            if (end == raw.c_str() || *end != '\0') return std::string(); // invalid
            return std::to_string(v);
        }
        case CVarType::Float: {
            char* end = nullptr;
            const double v = std::strtod(raw.c_str(), &end);
            if (end == raw.c_str() || *end != '\0') return std::string(); // invalid
            std::ostringstream os;
            os.precision(6);
            os << v;
            return os.str();
        }
        case CVarType::Bool: {
            bool b = false;
            if (!ParseBool(raw, b)) return std::string();
            return b ? "1" : "0";
        }
        case CVarType::String:
            return raw;
    }
    return std::string();
}

} // namespace

void RegisterCommand(const std::string& name, std::function<void(const std::vector<std::string>&, std::string&)> fn,
                     const std::string& help) {
    RegistryInternals& r = Registry();
    std::lock_guard lock(r.mutex);
    for (auto& c : r.commands) {
        if (c->name == name) {
            c->fn = std::move(fn);
            if (!help.empty()) c->help = help;
            return;
        }
    }
    auto cmd = std::make_unique<ConsoleCommand>();
    cmd->name = name;
    cmd->fn = std::move(fn);
    cmd->help = help;
    r.commands.push_back(std::move(cmd));
}

const ConsoleCommand* FindCommand(const std::string& name) {
    RegistryInternals& r = Registry();
    std::lock_guard lock(r.mutex);
    for (const auto& c : r.commands) {
        if (c->name == name) return c.get();
    }
    return nullptr;
}

std::vector<const ConsoleCommand*> Commands() {
    RegistryInternals& r = Registry();
    std::lock_guard lock(r.mutex);
    std::vector<const ConsoleCommand*> out;
    out.reserve(r.commands.size());
    for (const auto& c : r.commands) out.push_back(c.get());
    return out;
}

CVar* RegisterCVar(const std::string& name, CVarType type, const std::string& initial_value,
                   const std::string& help) {
    RegistryInternals& r = Registry();
    std::lock_guard lock(r.mutex);
    for (auto& c : r.cvars) {
        if (c->name == name) return c.get();
    }
    auto cvar = std::make_unique<CVar>();
    cvar->name = name;
    cvar->type = type;
    cvar->help = help;

    // Apply without firing on_change (registration-side).
    const std::string canonical = Canonicalize(type, initial_value);
    const std::string effective = canonical.empty() ? initial_value : canonical;
    switch (type) {
        case CVarType::Int: {
            const long long v = std::strtoll(effective.c_str(), nullptr, 10);
            cvar->int_value = static_cast<i64>(v);
            break;
        }
        case CVarType::Float:
            cvar->float_value = std::strtod(effective.c_str(), nullptr);
            break;
        case CVarType::Bool: {
            bool b = false;
            ParseBool(effective, b);
            cvar->bool_value = b;
            break;
        }
        case CVarType::String:
            cvar->string_value = effective;
            break;
    }
    r.cvars.push_back(std::move(cvar));
    return r.cvars.back().get();
}

CVar* FindMutable(const std::string& name) {
    RegistryInternals& r = Registry();
    std::lock_guard lock(r.mutex);
    for (auto& c : r.cvars) {
        if (c->name == name) return c.get();
    }
    return nullptr;
}

const CVar* Find(const std::string& name) {
    RegistryInternals& r = Registry();
    std::lock_guard lock(r.mutex);
    for (const auto& c : r.cvars) {
        if (c->name == name) return c.get();
    }
    return nullptr;
}

bool SetString(const std::string& name, const std::string& value) {
    RegistryInternals& r = Registry();
    std::lock_guard lock(r.mutex);
    for (auto& c : r.cvars) {
        if (c->name != name) continue;
        const std::string canonical = Canonicalize(c->type, value);
        if (canonical.empty()) return false;
        switch (c->type) {
            case CVarType::Int: c->int_value = std::strtoll(canonical.c_str(), nullptr, 10); break;
            case CVarType::Float: c->float_value = std::strtod(canonical.c_str(), nullptr); break;
            case CVarType::Bool: c->bool_value = (canonical == "1"); break;
            case CVarType::String: c->string_value = value; break;
        }
        if (c->on_change) c->on_change();
        return true;
    }
    return false;
}

std::string GetString(const std::string& name, const std::string& fallback) {
    const CVar* c = Find(name);
    if (c == nullptr) return fallback;
    switch (c->type) {
        case CVarType::Int: return std::to_string(c->int_value);
        case CVarType::Float: {
            std::ostringstream os;
            os.precision(6);
            os << c->float_value;
            return os.str();
        }
        case CVarType::Bool: return c->bool_value ? "1" : "0";
        case CVarType::String: return c->string_value;
    }
    return fallback;
}

bool SetInt(const std::string& name, i64 value) { return SetString(name, std::to_string(value)); }
bool SetFloat(const std::string& name, f64 value) { return SetString(name, std::to_string(value)); }
bool SetBool(const std::string& name, bool value) { return SetString(name, value ? "1" : "0"); }
bool SetStringCVar(const std::string& name, const std::string& value) { return SetString(name, value); }

i64 GetInt(const std::string& name, i64 fallback) {
    const CVar* c = Find(name);
    return c != nullptr && c->type == CVarType::Int ? c->int_value : fallback;
}
f64 GetFloat(const std::string& name, f64 fallback) {
    const CVar* c = Find(name);
    return c != nullptr && c->type == CVarType::Float ? c->float_value : fallback;
}
bool GetBool(const std::string& name, bool fallback) {
    const CVar* c = Find(name);
    return c != nullptr && c->type == CVarType::Bool ? c->bool_value : fallback;
}
const std::string GetStringValue(const std::string& name, const std::string& fallback) {
    const CVar* c = Find(name);
    return c != nullptr && c->type == CVarType::String ? c->string_value : fallback;
}

std::vector<const CVar*> All() {
    RegistryInternals& r = Registry();
    std::lock_guard lock(r.mutex);
    std::vector<const CVar*> out;
    out.reserve(r.cvars.size());
    for (const auto& c : r.cvars) out.push_back(c.get());
    return out;
}

std::vector<std::string> AllNames() {
    std::vector<std::string> out;
    for (const CVar* c : All()) out.push_back(c->name);
    return out;
}

std::vector<std::string> SplitArguments(const std::string& line) {
    std::vector<std::string> args;
    std::string current;
    bool in_quote = false;
    for (char ch : line) {
        if (ch == '"') {
            in_quote = !in_quote;
        } else if (ch == ' ' && !in_quote) {
            if (!current.empty()) {
                args.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) args.push_back(std::move(current));
    return args;
}

void EnsureBuiltins() {
    RegistryInternals& r = Registry();
    std::call_once(r.builtins_once, [&] {
        // NOTE: these registration callbacks run without the registry mutex held;
        // the lambdas call Registry()/RegisterCommand() which lock internally.
        RegisterCommand("help", [](const std::vector<std::string>&, std::string& out) {
            out = "Aether console. Commands: help, cvars, commands, echo [text], stat <fps|gpu|memory>, physics.debug <0|1>";
        });
        RegisterCommand("cvars", [](const std::vector<std::string>&, std::string& out) {
            for (const CVar* c : All()) {
                out += c->name + " = " + GetString(c->name) + "\n";
            }
        });
        RegisterCommand("commands", [](const std::vector<std::string>&, std::string& out) {
            for (const ConsoleCommand* c : Commands()) out += c->name + "\n";
        });
        RegisterCommand("echo", [](const std::vector<std::string>& args, std::string& out) {
            // args[0] is the command name ("echo"); echo the remaining args.
            for (usize i = 1; i < args.size(); ++i) {
                out += args[i];
                if (i + 1 < args.size()) out += " ";
            }
        });
    });
}

bool RunCommand(const std::string& line, std::string& output) {
    EnsureBuiltins();

    const std::vector<std::string> args = SplitArguments(line);
    if (args.empty()) return false;

    const std::string& name = args[0];

    // A command, if one exists.
    if (const ConsoleCommand* cmd = FindCommand(name)) {
        cmd->fn(args, output);
        return true;
    }

    // A CVar name alone prints its value; with more args, set it.
    const CVar* cvar = Find(name);
    if (cvar != nullptr) {
        if (args.size() == 1) {
            output = GetString(name);
        } else {
            // Join remaining args back into a single value string.
            std::string value;
            for (usize i = 1; i < args.size(); ++i) {
                if (i > 1) value += " ";
                value += args[i];
            }
            if (!SetString(name, value)) {
                output = "Invalid value '" + value + "' for " + name + " (type " + TypeName(cvar->type) + ")";
                return false;
            }
            return true;
        }
        return true;
    }

    output = "Unknown command or CVar: " + name;
    return false;
}

} // namespace cvars
} // namespace aether
