#include "aether/ui/binding.h"

#include "aether/ui/basic.h"
#include "aether/ui/controls.h"

#include <cmath>
#include <cstdio>
#include <functional>

namespace aether::ui {

namespace {

using reflect::Any;
using reflect::Reflect;

// Number conversions for the reflected scalar types.
bool ToNumber(const Any& a, f64& out) {
    if (auto* v = a.TryGet<f32>()) return out = *v, true;
    if (auto* v = a.TryGet<f64>()) return out = *v, true;
    if (auto* v = a.TryGet<i32>()) return out = *v, true;
    if (auto* v = a.TryGet<u32>()) return out = *v, true;
    if (auto* v = a.TryGet<i64>()) return out = static_cast<f64>(*v), true;
    if (auto* v = a.TryGet<u64>()) return out = static_cast<f64>(*v), true;
    if (auto* v = a.TryGet<i16>()) return out = *v, true;
    if (auto* v = a.TryGet<u16>()) return out = *v, true;
    if (auto* v = a.TryGet<i8>()) return out = *v, true;
    if (auto* v = a.TryGet<u8>()) return out = *v, true;
    if (auto* v = a.TryGet<bool>()) return out = *v ? 1.0 : 0.0, true;
    return false;
}

// A value of the field's own type from a number.
bool FromNumber(const reflect::TypeInfo& t, f64 v, Any& out) {
    if (&t == &Reflect<f32>()) return out = Any(static_cast<f32>(v)), true;
    if (&t == &Reflect<f64>()) return out = Any(v), true;
    if (&t == &Reflect<i32>()) return out = Any(static_cast<i32>(std::lround(v))), true;
    if (&t == &Reflect<u32>()) return out = Any(static_cast<u32>(std::max(0.0, std::round(v)))), true;
    if (&t == &Reflect<i64>()) return out = Any(static_cast<i64>(std::llround(v))), true;
    if (&t == &Reflect<u64>()) return out = Any(static_cast<u64>(std::max(0.0, std::round(v)))), true;
    if (&t == &Reflect<bool>()) return out = Any(v != 0.0), true;
    return false;
}

std::string Number(f64 v, i32 precision) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", std::max(precision, 0), v);
    return buf;
}

// Split "a.b.c" at the first dot.
bool Head(const std::string& path, std::string& head, std::string& rest) {
    const usize dot = path.find('.');
    if (dot == std::string::npos) return false;
    head = path.substr(0, dot);
    rest = path.substr(dot + 1);
    return !head.empty() && !rest.empty();
}

// The properties each widget type can bind, and whether they're numbers or text.
bool Supports(const Widget& w, const std::string& p) {
    if (p == "visible" || p == "enabled" || p == "opacity") return true;
    if (p == "text") return dynamic_cast<const Text*>(&w) || dynamic_cast<const TextInput*>(&w);
    if (p == "percent") return dynamic_cast<const ProgressBar*>(&w) != nullptr;
    if (p == "value") return dynamic_cast<const Slider*>(&w) != nullptr;
    if (p == "checked") return dynamic_cast<const Toggle*>(&w) != nullptr;
    if (p == "selected") return dynamic_cast<const Dropdown*>(&w) || dynamic_cast<const ListView*>(&w);
    return false;
}

} // namespace

void DataBinder::AddSource(const std::string& name, void* object, const reflect::TypeInfo& type) { sources_[name] = {object, &type}; }

void DataBinder::RemoveSource(const std::string& name) { sources_.erase(name); }

bool DataBinder::Resolve(const std::string& path, Resolved& out, std::string* error) const {
    std::string head, rest;
    if (!Head(path, head, rest)) {
        if (error != nullptr) *error = "'" + path + "' isn't a <source>.<field> path";
        return false;
    }
    const auto it = sources_.find(head);
    if (it == sources_.end() || it->second.object == nullptr) {
        if (error != nullptr) *error = "no source named '" + head + "'";
        return false;
    }
    void* object = it->second.object;
    const reflect::TypeInfo* type = it->second.type;
    // Walk nested structs down to the leaf field.
    while (true) {
        std::string field_name = rest, remaining;
        const usize dot = rest.find('.');
        if (dot != std::string::npos) field_name = rest.substr(0, dot), remaining = rest.substr(dot + 1);
        const reflect::FieldInfo* field = type->FindField(field_name);
        if (field == nullptr) {
            if (error != nullptr) *error = "'" + std::string(type->name) + "' has no field '" + field_name + "'";
            return false;
        }
        if (remaining.empty()) {
            out = {object, field};
            return true;
        }
        object = field->Ptr(object);
        type = field->type;
        rest = remaining;
    }
}

bool DataBinder::ReadNumber(const std::string& path, f64& out) const {
    Resolved r;
    return Resolve(path, r) && ToNumber(r.field->Get(r.owner), out);
}

bool DataBinder::ReadText(const std::string& path, std::string& out) const {
    Resolved r;
    if (!Resolve(path, r)) return false;
    const Any a = r.field->Get(r.owner);
    if (auto* s = a.TryGet<std::string>()) return out = *s, true;
    f64 v = 0.0;
    if (auto* b = a.TryGet<bool>()) return out = *b ? "true" : "false", true;
    if (ToNumber(a, v)) return out = Number(v, std::floor(v) == v ? 0 : 3), true;
    return false;
}

bool DataBinder::WriteNumber(const std::string& path, f64 value) const {
    Resolved r;
    Any a;
    return Resolve(path, r) && FromNumber(*r.field->type, value, a) && r.field->Set(r.owner, a);
}

bool DataBinder::WriteText(const std::string& path, const std::string& value) const {
    Resolved r;
    if (!Resolve(path, r) || r.field->type != &Reflect<std::string>()) return false;
    return r.field->Set(r.owner, Any(value));
}

std::vector<std::string> DataBinder::Bind(Widget& root, const std::vector<Binding>& bindings) {
    live_.clear();
    std::vector<std::string> problems;
    for (const Binding& b : bindings) {
        const std::string who = b.widget + "." + b.property + " <- " + b.source + ": ";
        Widget* w = root.Find(b.widget);
        if (w == nullptr) {
            problems.push_back(who + "no widget named '" + b.widget + "'");
            continue;
        }
        if (!Supports(*w, b.property)) {
            problems.push_back(who + "a " + w->TypeName() + " has no bindable '" + b.property + "'");
            continue;
        }
        Resolved r;
        std::string error;
        if (!Resolve(b.source, r, &error) || (!b.divide_by.empty() && !Resolve(b.divide_by, r, &error))) {
            problems.push_back(who + error);
            continue;
        }
        const usize index = live_.size();
        live_.push_back({b, w, {}});
        if (!b.two_way) continue;
        // Controls write back what the player changes, then call what was there before.
        const std::string path = b.source;
        if (auto* s = dynamic_cast<Slider*>(w); s != nullptr && b.property == "value") {
            auto prev = s->on_changed;
            s->on_changed = [this, path, prev](f32 v) {
                WriteNumber(path, v);
                if (prev) prev(v);
            };
        } else if (auto* t = dynamic_cast<Toggle*>(w); t != nullptr && b.property == "checked") {
            auto prev = t->on_changed;
            const bool invert = b.invert;
            t->on_changed = [this, path, prev, invert](bool v) {
                WriteNumber(path, (v != invert) ? 1.0 : 0.0);
                if (prev) prev(v);
            };
        } else if (auto* ti = dynamic_cast<TextInput*>(w); ti != nullptr && b.property == "text") {
            auto prev = ti->on_changed;
            ti->on_changed = [this, path, prev](const std::string& v) {
                if (!WriteText(path, v)) {
                    // A number field: write what parses.
                    char* end = nullptr;
                    const f64 n = std::strtod(v.c_str(), &end);
                    if (end != v.c_str()) WriteNumber(path, n);
                }
                if (prev) prev(v);
            };
        } else if (auto* d = dynamic_cast<Dropdown*>(w); d != nullptr && b.property == "selected") {
            auto prev = d->on_changed;
            d->on_changed = [this, path, prev](i32 v) {
                WriteNumber(path, v);
                if (prev) prev(v);
            };
        } else {
            problems.push_back(who + "'" + b.property + "' on a " + w->TypeName() + " can't write back");
            live_[index].spec.two_way = false;
        }
    }
    return problems;
}

void DataBinder::Push(Live& l) {
    const Binding& b = l.spec;
    Widget& w = *l.widget;
    f64 v = 0.0;
    const bool numeric = ReadNumber(b.source, v);
    if (numeric && !b.divide_by.empty()) {
        f64 d = 0.0;
        v = ReadNumber(b.divide_by, d) && d != 0.0 ? v / d : 0.0;
    }
    // Compare what would be shown, so unchanged values leave the widget alone.
    std::string shown;
    if (b.property == "text") {
        // Strings and bools as themselves; numbers (and ratios) formatted.
        std::string value;
        Resolved r;
        if (!Resolve(b.source, r)) return;
        const Any a = r.field->Get(r.owner);
        const bool plain = a.TryGet<std::string>() != nullptr || a.TryGet<bool>() != nullptr;
        if (plain && b.divide_by.empty()) {
            if (!ReadText(b.source, value)) return;
        } else if (numeric) {
            // Whole numbers without a point unless a precision is asked for.
            value = b.precision > 0 ? Number(v, b.precision) : std::floor(v) == v ? Number(v, 0) : Number(v, 2);
        } else {
            return;
        }
        shown = value;
        if (!b.format.empty()) {
            shown = b.format;
            const usize at = shown.find("{}");
            if (at != std::string::npos) shown.replace(at, 2, value);
        }
    } else {
        if (!numeric) return;
        shown = Number(v, 6);
    }
    if (shown == l.last) return;
    // A text box being typed in keeps what the player is typing; the value lands once they leave it.
    if (auto* ti = dynamic_cast<TextInput*>(&w); ti != nullptr && ti->IsFocused() && b.property == "text") return;
    l.last = shown;
    const bool flag = (v != 0.0) != b.invert;
    if (b.property == "text") {
        if (auto* t = dynamic_cast<Text*>(&w)) t->text = shown, t->text_key.clear();
        else if (auto* ti = dynamic_cast<TextInput*>(&w)) ti->SetText(shown);
    } else if (b.property == "visible") {
        w.visibility = flag ? Visibility::Visible : Visibility::Collapsed;
    } else if (b.property == "enabled") {
        w.enabled = flag;
    } else if (b.property == "opacity") {
        w.opacity = static_cast<f32>(v);
    } else if (b.property == "percent") {
        static_cast<ProgressBar&>(w).percent = static_cast<f32>(v);
    } else if (b.property == "value") {
        static_cast<Slider&>(w).value = static_cast<f32>(v); // set directly: no on_changed back to the source
    } else if (b.property == "checked") {
        static_cast<Toggle&>(w).checked = flag;
    } else if (b.property == "selected") {
        if (auto* d = dynamic_cast<Dropdown*>(&w)) d->selected = static_cast<i32>(std::lround(v));
        else static_cast<ListView&>(w).selected = static_cast<i64>(std::llround(v));
    }
}

void DataBinder::Update() {
    for (Live& l : live_) Push(l);
}

} // namespace aether::ui
