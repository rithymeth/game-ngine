#pragma once

#include "aether/reflection/reflection.h"
#include "aether/ui/widget.h"

#include <map>
#include <string>
#include <vector>

namespace aether::ui {

// Data binding (Phase 18 step 3, §18.3): a widget property follows a
// reflected field, by path from a named source ("player.health",
// "player.stats.mana").
struct Binding {
    std::string widget{};   // by name, under the bound root
    std::string property{}; // text, visible, enabled, opacity, percent, value, checked, selected
    std::string source{};   // "<source>.<field>[.<field>...]"
    std::string divide_by{}; // a second path: the property is source / divide_by (a health bar's percent)
    std::string format{};   // text: "{}" becomes the value ("HP {} / 100"); "" is just the value
    i32 precision = 0;    // digits after the point for numbers in text
    bool invert = false;  // bool properties: the opposite
    bool two_way = false; // controls write back: a slider's value, a toggle's check, a text box's text, a dropdown's choice
};

// Holds the sources and the bindings, and copies values across each Update.
class DataBinder {
public:
    // The object must outlive the binder (or be removed first).
    void AddSource(const std::string& name, void* object, const reflect::TypeInfo& type);
    template <typename T>
    void AddSource(const std::string& name, T& object) {
        AddSource(name, &object, reflect::Reflect<T>());
    }
    void RemoveSource(const std::string& name);

    // Binds under `root` (replacing earlier bindings); problems (a missing
    // widget or field, a property the widget doesn't have, a field that
    // can't become it) come back and the binding is skipped.
    std::vector<std::string> Bind(Widget& root, const std::vector<Binding>& bindings);
    // Source values into widgets (those that changed since the last Update).
    void Update();

    // Reads a path as text or a number (tests and the designer).
    bool ReadNumber(const std::string& path, f64& out) const;
    bool ReadText(const std::string& path, std::string& out) const;
    bool WriteNumber(const std::string& path, f64 value) const;
    bool WriteText(const std::string& path, const std::string& value) const;

private:
    struct Source {
        void* object = nullptr;
        const reflect::TypeInfo* type = nullptr;
    };
    struct Resolved {
        void* owner = nullptr;
        const reflect::FieldInfo* field = nullptr;
    };
    struct Live {
        Binding spec;
        Widget* widget = nullptr;
        std::string last; // what was last shown (so unchanged values don't touch the widget)
    };
    bool Resolve(const std::string& path, Resolved& out, std::string* error = nullptr) const;
    void Push(Live& live);
    std::map<std::string, Source> sources_;
    std::vector<Live> live_;
};

} // namespace aether::ui
