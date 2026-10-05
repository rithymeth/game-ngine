#include "aether/core/cvar.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace aether {

const char* CVarTypeName(CVarType type) {
    switch (type) {
    case CVarType::Bool: return "bool";
    case CVarType::Int: return "int";
    case CVarType::Float: return "float";
    case CVarType::String: return "string";
    }
    return "?";
}

std::string FormatCVarFloat(f64 value) {
    char buf[64];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, value);
        if (std::strtod(buf, nullptr) == value) break;
    }
    return buf;
}

namespace {

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool ParseBool(std::string_view text, bool& out) {
    const std::string t = Lower(text);
    if (t == "1" || t == "true" || t == "on" || t == "yes") return out = true, true;
    if (t == "0" || t == "false" || t == "off" || t == "no") return out = false, true;
    return false;
}

bool ParseFloat(std::string_view text, f64& out) {
    if (text.empty()) return false;
    const std::string s(text);
    char* end = nullptr;
    errno = 0;
    const f64 v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || errno == ERANGE || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool ParseInt(std::string_view text, i64& out) {
    if (text.empty()) return false;
    const std::string s(text);
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s.c_str(), &end, 0); // 10, 0x1F, 017
    if (end == s.c_str() + s.size() && errno != ERANGE) return out = v, true;
    f64 f; // "2.0" and "1e3" are fine when whole
    if (ParseFloat(text, f) && f == std::floor(f) && std::fabs(f) < 9.2e18) return out = static_cast<i64>(f), true;
    return false;
}

} // namespace

CVar::CVar(std::string name, CVarType type, std::string help, u32 flags, f64 min, f64 max)
    : name_(std::move(name)), help_(std::move(help)), type_(type), flags_(flags), min_(min), max_(max) {}

bool CVar::GetBool() const {
    switch (type_) {
    case CVarType::Bool: return bool_.load();
    case CVarType::Int: return int_.load() != 0;
    case CVarType::Float: return float_.load() != 0.0;
    case CVarType::String: {
        bool b = false;
        return ParseBool(GetString(), b) && b;
    }
    }
    return false;
}

i64 CVar::GetInt() const {
    switch (type_) {
    case CVarType::Bool: return bool_.load() ? 1 : 0;
    case CVarType::Int: return int_.load();
    case CVarType::Float: return static_cast<i64>(float_.load());
    case CVarType::String: {
        i64 v = 0;
        return ParseInt(GetString(), v) ? v : 0;
    }
    }
    return 0;
}

f64 CVar::GetFloat() const {
    switch (type_) {
    case CVarType::Bool: return bool_.load() ? 1.0 : 0.0;
    case CVarType::Int: return static_cast<f64>(int_.load());
    case CVarType::Float: return float_.load();
    case CVarType::String: {
        f64 v = 0;
        return ParseFloat(GetString(), v) ? v : 0.0;
    }
    }
    return 0.0;
}

std::string CVar::GetString() const {
    if (type_ != CVarType::String) return ValueString();
    std::lock_guard<std::mutex> lock(mutex_);
    return string_;
}

std::string CVar::ValueString() const {
    switch (type_) {
    case CVarType::Bool: return bool_.load() ? "1" : "0";
    case CVarType::Int: return std::to_string(int_.load());
    case CVarType::Float: return FormatCVarFloat(float_.load());
    case CVarType::String: return GetString();
    }
    return {};
}

bool CVar::SetFromString(std::string_view text, std::string* error) {
    const auto fail = [&](const char* what) {
        if (error) *error = std::string("'") + std::string(text) + "' isn't " + what;
        return false;
    };
    switch (type_) {
    case CVarType::Bool: {
        bool b = false;
        if (!ParseBool(text, b)) return fail("a bool (1/0, true/false, on/off)");
        SetBool(b);
        return true;
    }
    case CVarType::Int: {
        i64 v = 0;
        if (!ParseInt(text, v)) return fail("a whole number");
        SetInt(v);
        return true;
    }
    case CVarType::Float: {
        f64 v = 0;
        if (!ParseFloat(text, v)) return fail("a number");
        SetFloat(v);
        return true;
    }
    case CVarType::String: SetString(std::string(text)); return true;
    }
    return false;
}

void CVar::SetBool(bool value) {
    if (type_ != CVarType::Bool) {
        SetFromString(value ? "1" : "0");
        return;
    }
    bool_.store(value);
    Changed();
}

void CVar::SetInt(i64 value) {
    if (type_ == CVarType::Float) return SetFloat(static_cast<f64>(value));
    if (type_ == CVarType::Bool) return SetBool(value != 0);
    if (type_ == CVarType::String) return SetString(std::to_string(value));
    if (HasRange()) value = std::clamp(value, static_cast<i64>(std::ceil(min_)), static_cast<i64>(std::floor(max_)));
    int_.store(value);
    Changed();
}

void CVar::SetFloat(f64 value) {
    if (type_ == CVarType::Int) return SetInt(static_cast<i64>(std::llround(value)));
    if (type_ == CVarType::Bool) return SetBool(value != 0.0);
    if (type_ == CVarType::String) return SetString(FormatCVarFloat(value));
    if (!std::isfinite(value)) return;
    if (HasRange()) value = std::clamp(value, min_, max_);
    float_.store(value);
    Changed();
}

void CVar::SetString(std::string value) {
    if (type_ != CVarType::String) {
        SetFromString(value);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        string_ = std::move(value);
    }
    Changed();
}

void CVar::SetDefault(std::string_view text) {
    if (SetFromString(text)) default_ = ValueString();
}

int CVar::OnChange(std::function<void(const CVar&)> fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back({next_callback_, std::move(fn)});
    return next_callback_++;
}

void CVar::RemoveOnChange(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(callbacks_, [&](const auto& c) { return c.first == id; });
}

void CVar::Changed() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    std::vector<std::function<void(const CVar&)>> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, fn] : callbacks_) callbacks.push_back(fn);
    }
    for (const auto& fn : callbacks) fn(*this);
}

// ---------------------------------------------------------------- registry

CVarRegistry& CVarRegistry::Get() {
    static CVarRegistry registry;
    return registry;
}

std::string CVarRegistry::Key(std::string_view name) { return Lower(name); }

CVar* CVarRegistry::Register(std::string_view name, CVarType type, std::string_view default_value, std::string_view help,
                             u32 flags, f64 min, f64 max) {
    if (name.empty() || name.find_first_of(" \t;\"") != std::string_view::npos) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = Key(name);
    if (commands_.count(key)) return nullptr; // a command has the name
    auto it = vars_.find(key);
    if (it != vars_.end()) return it->second->Type() == type ? it->second.get() : nullptr;
    auto var = std::make_unique<CVar>(std::string(name), type, std::string(help), flags, min, max);
    var->SetDefault(default_value);
    CVar* raw = var.get();
    vars_[key] = std::move(var);
    return raw;
}

CVar* CVarRegistry::Find(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = vars_.find(Key(name));
    return it == vars_.end() ? nullptr : it->second.get();
}

std::vector<CVar*> CVarRegistry::All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CVar*> out;
    for (const auto& [key, var] : vars_) out.push_back(var.get());
    return out;
}

bool CVarRegistry::Unregister(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return vars_.erase(Key(name)) > 0;
}

bool CVarRegistry::RegisterCommand(std::string_view name, std::string_view help, ConsoleCommandFn fn, u32 flags) {
    if (name.empty() || !fn || name.find_first_of(" \t;\"") != std::string_view::npos) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = Key(name);
    if (vars_.count(key)) return false;
    auto cmd = std::make_unique<ConsoleCommand>();
    cmd->name = std::string(name), cmd->help = std::string(help), cmd->flags = flags, cmd->fn = std::move(fn);
    commands_[key] = std::move(cmd); // re-registering replaces
    return true;
}

const ConsoleCommand* CVarRegistry::FindCommand(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = commands_.find(Key(name));
    return it == commands_.end() ? nullptr : it->second.get();
}

std::vector<const ConsoleCommand*> CVarRegistry::Commands() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<const ConsoleCommand*> out;
    for (const auto& [key, cmd] : commands_) out.push_back(cmd.get());
    return out;
}

bool CVarRegistry::UnregisterCommand(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return commands_.erase(Key(name)) > 0;
}

} // namespace aether
