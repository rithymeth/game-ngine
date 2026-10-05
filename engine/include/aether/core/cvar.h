#pragma once

#include "aether/core/base.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace aether {

// Console variables (Phase 23 step 1, docs/design/PHASE_SPECS.md §23.1):
// named, typed settings any module declares where it uses them and anyone
// can change at runtime from the console, a config file or the command
// line - `r.shadows.cascades 4`, `physics.debug 1`.
//
//     static AutoCVar<int> cascades("r.shadows.cascades", 4, "Shadow cascades", CVar_Archive, 1, 4);
//     ... cascades.Get() ...
//
// Names are case-insensitive and dotted by system ("r.", "physics.",
// "net."). Numbers are clamped to their range; reads are atomic, so any
// thread may read while the console writes.

enum class CVarType : u8 { Bool, Int, Float, String };
const char* CVarTypeName(CVarType type);

enum CVarFlags : u32 {
    CVar_None = 0,
    CVar_ReadOnly = 1u << 0,        // shown, never changed from outside
    CVar_Cheat = 1u << 1,           // changed only while cheats are allowed
    CVar_Archive = 1u << 2,         // saved to the user's config when not at its default
    CVar_RequiresRestart = 1u << 3, // takes effect on the next start (the console says so)
};

class CVar {
public:
    CVar(std::string name, CVarType type, std::string help, u32 flags, f64 min, f64 max);

    const std::string& Name() const { return name_; }
    const std::string& Help() const { return help_; }
    CVarType Type() const { return type_; }
    u32 Flags() const { return flags_; }
    bool HasFlag(CVarFlags flag) const { return (flags_ & flag) != 0; }
    bool HasRange() const { return min_ < max_; }
    f64 Min() const { return min_; }
    f64 Max() const { return max_; }

    bool GetBool() const;
    i64 GetInt() const;
    f64 GetFloat() const;
    std::string GetString() const;
    std::string ValueString() const;
    const std::string& DefaultString() const { return default_; }
    bool IsDefault() const { return ValueString() == DefaultString(); }

    // Parses `text` for this type ("1/0/true/false/on/off/yes/no" for
    // bools), clamps numbers to the range, and stores it. Flags are the
    // caller's business (the console checks ReadOnly and Cheat). False,
    // with a reason, when the text doesn't parse.
    bool SetFromString(std::string_view text, std::string* error = nullptr);
    void SetBool(bool value);
    void SetInt(i64 value);
    void SetFloat(f64 value);
    void SetString(std::string value);
    void Reset() { SetFromString(default_); }
    // Sets the default too (registration does this once).
    void SetDefault(std::string_view text);

    // Called after every change (on the changing thread). Returns an id for RemoveOnChange.
    int OnChange(std::function<void(const CVar&)> fn);
    void RemoveOnChange(int id);
    // Bumped on every change, for cheap polling.
    u64 Generation() const { return generation_.load(std::memory_order_acquire); }

private:
    void Changed();
    std::string name_, help_, default_;
    CVarType type_;
    u32 flags_;
    f64 min_, max_;
    std::atomic<i64> int_{0};
    std::atomic<f64> float_{0.0};
    std::atomic<bool> bool_{false};
    mutable std::mutex mutex_; // the string value and the callbacks
    std::string string_;
    std::vector<std::pair<int, std::function<void(const CVar&)>>> callbacks_;
    int next_callback_ = 1;
    std::atomic<u64> generation_{0};
};

class Console;
using ConsoleCommandFn = std::function<void(const std::vector<std::string>& args, Console& console)>;

std::string FormatCVarFloat(f64 value); // shortest form that reads back the same ("0.1", not "0.10000000000000001")

struct ConsoleCommand {
    std::string name, help;
    u32 flags = CVar_None; // CVar_Cheat applies
    ConsoleCommandFn fn;
};

// Every CVar and console command in the process.
class CVarRegistry {
public:
    static CVarRegistry& Get();

    // Registers a variable, or returns the one already registered under
    // that name if it has the same type (its value is kept). Null on a type clash.
    CVar* Register(std::string_view name, CVarType type, std::string_view default_value, std::string_view help,
                   u32 flags = CVar_None, f64 min = 0.0, f64 max = 0.0);
    CVar* Find(std::string_view name) const;
    std::vector<CVar*> All() const; // by name
    bool Unregister(std::string_view name);

    bool RegisterCommand(std::string_view name, std::string_view help, ConsoleCommandFn fn, u32 flags = CVar_None);
    const ConsoleCommand* FindCommand(std::string_view name) const;
    std::vector<const ConsoleCommand*> Commands() const; // by name
    bool UnregisterCommand(std::string_view name);

    static std::string Key(std::string_view name); // lowercased

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<CVar>> vars_;
    std::map<std::string, std::unique_ptr<ConsoleCommand>> commands_;
};

// A CVar declared where it's used. T is bool, an integer type, a floating
// type or std::string.
template <typename T>
class AutoCVar {
public:
    AutoCVar(std::string_view name, const T& default_value, std::string_view help, u32 flags = CVar_None, f64 min = 0.0,
             f64 max = 0.0) {
        var_ = CVarRegistry::Get().Register(name, TypeOf(), ToText(default_value), help, flags, min, max);
    }
    T Get() const {
        if constexpr (std::is_same_v<T, bool>) return var_->GetBool();
        else if constexpr (std::is_integral_v<T>) return static_cast<T>(var_->GetInt());
        else if constexpr (std::is_floating_point_v<T>) return static_cast<T>(var_->GetFloat());
        else return var_->GetString();
    }
    void Set(const T& value) {
        if constexpr (std::is_same_v<T, bool>) var_->SetBool(value);
        else if constexpr (std::is_integral_v<T>) var_->SetInt(static_cast<i64>(value));
        else if constexpr (std::is_floating_point_v<T>) var_->SetFloat(static_cast<f64>(value));
        else var_->SetString(value);
    }
    CVar& Raw() const { return *var_; }
    explicit operator bool() const { return var_ != nullptr; }

private:
    static constexpr CVarType TypeOf() {
        if constexpr (std::is_same_v<T, bool>) return CVarType::Bool;
        else if constexpr (std::is_integral_v<T>) return CVarType::Int;
        else if constexpr (std::is_floating_point_v<T>) return CVarType::Float;
        else return CVarType::String;
    }
    static std::string ToText(const T& v) {
        if constexpr (std::is_same_v<T, bool>) return v ? "1" : "0";
        else if constexpr (std::is_integral_v<T>) return std::to_string(static_cast<i64>(v));
        else if constexpr (std::is_floating_point_v<T>) return FormatCVarFloat(static_cast<f64>(v));
        else return std::string(v);
    }
    CVar* var_ = nullptr;
};

// A console command declared where it's implemented.
struct AutoConsoleCommand {
    AutoConsoleCommand(std::string_view name, std::string_view help, ConsoleCommandFn fn, u32 flags = CVar_None) {
        CVarRegistry::Get().RegisterCommand(name, help, std::move(fn), flags);
    }
};

} // namespace aether
