#pragma once

#include "aether/core/base.h"

#include <functional>
#include <string>
#include <vector>

// CVars + console commands (Phase 23). The engine publishes a registry of
// named, typed configuration values and commands; gameplay/editor tools read
// and set them. Lazy-initialized, thread-safe reads, so it stays out of hot
// paths unless used. Mirrors the cvars_style of other engines: an IntCVar
// reads as an int, a BoolCVar as a bool, etc.

namespace aether {
namespace cvars {

enum class CVarType : u8 { Int, Float, Bool, String };

struct CVar {
    std::string name;
    CVarType type = CVarType::Int;
    i64 int_value = 0;
    f64 float_value = 0.0;
    bool bool_value = false;
    std::string string_value;
    std::function<void()> on_change; // called after a successful SetString
    std::string help;
};

// Returns the value as a CVar's native type; does not convert.
inline i64 CVarInt(const CVar& c) { return c.int_value; }
inline f64 CVarFloat(const CVar& c) { return c.float_value; }
inline bool CVarBool(const CVar& c) { return c.bool_value; }
inline const std::string& CVarString(const CVar& c) { return c.string_value; }

struct ConsoleCommand {
    std::string name;
    std::function<void(const std::vector<std::string>&, std::string&)> fn;
    std::string help;
};

// Registers a CVar (or no-op) and returns a stable pointer.
CVar* RegisterCVar(const std::string& name, CVarType type, const std::string& initial_value,
                   const std::string& help = "");
// Stable mutable pointer to a registered CVar (e.g. to attach on_change), or
// nullptr. Prefer the Find(...) const accessors for reading.
CVar* FindMutable(const std::string& name);
const CVar* Find(const std::string& name);

// String-based set/get. SetString canonicalizes numeric/bool values.
bool SetString(const std::string& name, const std::string& value);
std::string GetString(const std::string& name, const std::string& fallback = "");

bool SetInt(const std::string& name, i64 value);
bool SetFloat(const std::string& name, f64 value);
bool SetBool(const std::string& name, bool value);
bool SetStringCVar(const std::string& name, const std::string& value); // sets string_value directly

i64  GetInt(const std::string& name, i64  fallback = 0);
f64  GetFloat(const std::string& name, f64  fallback = 0.0);
bool GetBool(const std::string& name, bool fallback = false);
const std::string GetStringValue(const std::string& name, const std::string& fallback = "");

std::vector<const CVar*> All();
std::vector<std::string> AllNames();

// Command registration + execution.
void RegisterCommand(const std::string& name,
                     std::function<void(const std::vector<std::string>&, std::string&)> fn,
                     const std::string& help = "");
const ConsoleCommand* FindCommand(const std::string& name);
std::vector<const ConsoleCommand*> Commands();

// Splits a command line into args respecting double quotes.
std::vector<std::string> SplitArguments(const std::string& line);
// Runs a line: a command with arguments, or a bare "name value" CVar set, or
// "name" alone prints a CVar's value. Returns true if handled.
bool RunCommand(const std::string& line, std::string& output);

} // namespace cvars
} // namespace aether
