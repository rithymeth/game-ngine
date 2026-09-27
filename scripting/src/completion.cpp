#include "aether/script/completion.h"

#include "aether/ecs/component.h"
#include "aether/reflection/type_info.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

namespace aether::script {

namespace {

using reflect::TypeInfo;

bool IsIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

struct Member {
    const char* name;
    const char* detail;
};

// The engine's script API (script_api.cpp, world_bindings.cpp) and the
// sandboxed Luau libraries (luau_host.cpp).
const std::vector<Member> kInput = {
    {"IsTriggered", "(action: string) -> boolean"}, {"GetAxis1D", "(action: string) -> number"},
    {"GetAxis2D", "(action: string) -> vector"},    {"GetAxis3D", "(action: string) -> vector"},
    {"OnStarted", "(action: string) -> Event"},     {"OnTriggered", "(action: string) -> Event"},
    {"OnCompleted", "(action: string) -> Event"},   {"OnCanceled", "(action: string) -> Event"},
};
const std::vector<Member> kTimer = {
    {"After", "(seconds: number, fn) -> Timer"},
    {"Every", "(seconds: number, fn) -> Timer"},
};
const std::vector<Member> kEventModule = {{"new", "() -> Event"}};
const std::vector<Member> kWorld = {
    {"Spawn", "() -> Entity"},
    {"Destroy", "(entity: Entity)"},
    {"Find", "(guid: string) -> Entity?"},
    {"EntitiesWith", "(component: string) -> {Entity}"},
};
const std::vector<Member> kEntity = {
    {"Get", "(component: string) -> Component?"}, {"Has", "(component: string) -> boolean"},
    {"Add", "(component: string) -> Component"},  {"Remove", "(component: string)"},
    {"IsValid", "() -> boolean"},                 {"Guid", "() -> string"},
};
const std::vector<Member> kEvent = {{"Connect", "(fn) -> Connection"}, {"Fire", "(...)"}};
const std::vector<Member> kConnection = {{"Disconnect", "() -> boolean"}, {"IsConnected", "() -> boolean"}};
const std::vector<Member> kTimerObject = {{"Cancel", "() -> boolean"}};
const std::vector<Member> kVectorFields = {{"x", "number"}, {"y", "number"}, {"z", "number"}};

struct Library {
    const char* name;
    std::vector<const char*> members;
};
const std::vector<Library> kLibraries = {
    {"math", {"abs", "acos", "asin", "atan", "atan2", "ceil", "clamp", "cos", "cosh", "deg", "exp", "floor", "fmod",
              "frexp", "huge", "isfinite", "isinf", "isnan", "ldexp", "lerp", "log", "log10", "map", "max", "min",
              "modf", "noise", "pi", "pow", "rad", "random", "randomseed", "round", "sign", "sin", "sinh", "sqrt", "tan",
              "tanh"}},
    {"string", {"byte", "char", "find", "format", "gmatch", "gsub", "len", "lower", "match", "pack", "packsize", "rep",
                "reverse", "split", "sub", "unpack", "upper"}},
    {"table", {"clear", "clone", "concat", "create", "find", "freeze", "insert", "isfrozen", "maxn", "move", "pack",
               "remove", "sort", "unpack"}},
    {"vector", {"abs", "angle", "ceil", "clamp", "create", "cross", "dot", "floor", "magnitude", "max", "min",
                "normalize", "one", "sign", "zero"}},
    {"bit32", {"arshift", "band", "bnot", "bor", "btest", "bxor", "byteswap", "countlz", "countrz", "extract",
               "lrotate", "lshift", "replace", "rrotate", "rshift"}},
    {"utf8", {"char", "charpattern", "codepoint", "codes", "len", "offset"}},
    {"coroutine", {"close", "create", "isyieldable", "resume", "running", "status", "wrap", "yield"}},
    {"buffer", {"copy", "create", "fill", "fromstring", "len", "readf32", "readf64", "readi32", "readi8", "readstring",
                "readu32", "readu8", "tostring", "writef32", "writef64", "writei32", "writei8", "writestring",
                "writeu32", "writeu8"}},
    {"os", {"clock"}},
};

const char* const kKeywords[] = {"and",    "break", "continue", "do",   "else",  "elseif", "end",
                                 "export", "false", "for",      "function", "if", "in",    "local",
                                 "nil",    "not",   "or",       "repeat", "return", "then", "true",
                                 "type",   "until", "while"};
const char* const kGlobals[] = {"assert", "error",   "getmetatable", "ipairs",   "next",   "pairs", "pcall",
                                "print",  "rawequal", "rawget",      "rawlen",   "rawset", "select", "setmetatable",
                                "tonumber", "tostring", "type",      "typeof",   "unpack", "xpcall"};
const char* const kCallbacks[] = {"OnCreate",     "OnEnable",  "OnStart",   "OnUpdate",         "OnFixedUpdate",
                                  "OnLateUpdate", "OnDisable", "OnDestroy", "OnReload",         "OnCollisionBegin",
                                  "OnCollisionStay", "OnCollisionEnd", "OnTriggerEnter", "OnTriggerExit"};

// ---------------------------------------------------------------------------
// What the cursor is in
// ---------------------------------------------------------------------------

struct Lexical {
    bool in_comment = false;
    bool in_string = false;
    usize string_start = 0; // just after the opening quote
};

// The level of a long bracket opening at `i` ("[[" = 0, "[==[" = 2), or -1.
int LongBracket(std::string_view s, usize i) {
    if (i >= s.size() || s[i] != '[') {
        return -1;
    }
    usize j = i + 1;
    while (j < s.size() && s[j] == '=') {
        ++j;
    }
    return j < s.size() && s[j] == '[' ? static_cast<int>(j - i - 1) : -1;
}

Lexical Scan(std::string_view s, usize cursor) {
    Lexical state;
    usize i = 0;
    while (i < cursor) {
        const char c = s[i];
        if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            const int level = LongBracket(s, i + 2);
            if (level >= 0) {
                const std::string close = "]" + std::string(static_cast<usize>(level), '=') + "]";
                const usize end = s.find(close, i + 4 + static_cast<usize>(level));
                if (end == std::string_view::npos || end + close.size() > cursor) {
                    state.in_comment = true;
                    return state;
                }
                i = end + close.size();
            } else {
                const usize end = s.find('\n', i);
                if (end == std::string_view::npos || end >= cursor) {
                    state.in_comment = true;
                    return state;
                }
                i = end + 1;
            }
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            usize j = i + 1;
            while (j < cursor && s[j] != c && s[j] != '\n') {
                j += s[j] == '\\' ? 2 : 1;
            }
            if (j >= cursor) {
                state.in_string = true;
                state.string_start = i + 1;
                return state;
            }
            i = j + 1;
            continue;
        }
        if (const int level = LongBracket(s, i); level >= 0) {
            const std::string close = "]" + std::string(static_cast<usize>(level), '=') + "]";
            const usize end = s.find(close, i + 2 + static_cast<usize>(level));
            if (end == std::string_view::npos || end + close.size() > cursor) {
                state.in_string = true;
                state.string_start = i + 2 + static_cast<usize>(level);
                return state;
            }
            i = end + close.size();
            continue;
        }
        ++i;
    }
    return state;
}

// ---------------------------------------------------------------------------
// Expression chains: base ident, then .name / :name / (args) segments
// ---------------------------------------------------------------------------

struct Segment {
    char sep = 0;         // '.' or ':'
    std::string name;
    bool called = false;
    std::string string_arg; // the first argument, if it's a string literal
};

struct Chain {
    std::string base;
    bool base_called = false;
    std::vector<Segment> segments;
};

usize SkipSpaces(std::string_view s, usize i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return i;
}

std::string ReadIdent(std::string_view s, usize& i) {
    const usize start = i;
    while (i < s.size() && IsIdent(s[i])) {
        ++i;
    }
    return std::string(s.substr(start, i - start));
}

// Reads "(...)" at s[i]; `arg` gets a leading string literal argument.
bool ReadCall(std::string_view s, usize& i, std::string& arg) {
    if (i >= s.size() || s[i] != '(') {
        return false;
    }
    usize j = SkipSpaces(s, i + 1);
    if (j < s.size() && (s[j] == '"' || s[j] == '\'')) {
        const char quote = s[j];
        const usize end = s.find(quote, j + 1);
        if (end != std::string_view::npos) {
            arg = std::string(s.substr(j + 1, end - j - 1));
        }
    }
    int depth = 0;
    for (; i < s.size(); ++i) {
        if (s[i] == '(') {
            ++depth;
        } else if (s[i] == ')' && --depth == 0) {
            ++i;
            return true;
        }
    }
    return false;
}

// Parses a chain starting at `i`; stops at the first thing that isn't one.
Chain ParseChain(std::string_view s, usize i) {
    Chain chain;
    i = SkipSpaces(s, i);
    if (i >= s.size() || std::isdigit(static_cast<unsigned char>(s[i])) != 0) {
        return chain;
    }
    chain.base = ReadIdent(s, i);
    if (chain.base.empty()) {
        return chain;
    }
    std::string ignored;
    if (ReadCall(s, i, ignored)) {
        chain.base_called = true;
    }
    while (i < s.size() && (s[i] == '.' || s[i] == ':')) {
        if (s[i] == '.' && i + 1 < s.size() && s[i + 1] == '.') {
            break; // concatenation
        }
        Segment segment;
        segment.sep = s[i++];
        segment.name = ReadIdent(s, i);
        if (segment.name.empty()) {
            break;
        }
        segment.called = ReadCall(s, i, segment.string_arg);
        chain.segments.push_back(std::move(segment));
    }
    return chain;
}

// The start of the receiver expression ending just before `end`.
usize ReceiverStart(std::string_view s, usize end) {
    usize i = end;
    while (i > 0) {
        const char c = s[i - 1];
        if (IsIdent(c) || c == '.' || c == ':') {
            --i;
        } else if (c == ')') {
            int depth = 0;
            usize j = i;
            while (j > 0) {
                --j;
                if (s[j] == ')') {
                    ++depth;
                } else if (s[j] == '(' && --depth == 0) {
                    break;
                }
            }
            if (depth != 0) {
                break;
            }
            i = j;
        } else {
            break;
        }
    }
    return i;
}

// ---------------------------------------------------------------------------
// A little type evaluation
// ---------------------------------------------------------------------------

enum class Kind { Unknown, Module, Library, World, Entity, Component, Struct, Vector, Event, Connection, Timer, Self };

struct Type {
    Type() = default;
    Type(Kind k, const TypeInfo* i = nullptr, std::string n = {}) : kind(k), info(i), name(std::move(n)) {}

    Kind kind = Kind::Unknown;
    const TypeInfo* info = nullptr; // Component, Struct
    std::string name;               // Module, Library
};

const TypeInfo* ReflectedComponent(const std::string& name) {
    const ComponentId id = FindComponentIdByName(name);
    return id == kInvalidComponentId ? nullptr : GetComponentInfo(id).reflected;
}

Type TypeOfValue(const TypeInfo* info) {
    if (info == nullptr) {
        return {};
    }
    if (std::string_view(info->name) == "Vec3") {
        return {Kind::Vector};
    }
    if (info->kind == reflect::TypeKind::Struct && (!info->fields.empty() || !info->functions.empty())) {
        return {Kind::Struct, info};
    }
    return {};
}

class Evaluator {
public:
    Evaluator(std::string_view source, usize cursor) : source_(source), cursor_(cursor) {}

    Type Evaluate(const Chain& chain, int depth = 0) const {
        Type type = Base(chain, depth);
        for (const Segment& segment : chain.segments) {
            type = Apply(type, segment);
            if (type.kind == Kind::Unknown) {
                break;
            }
        }
        return type;
    }

private:
    Type Base(const Chain& chain, int depth) const {
        const std::string& name = chain.base;
        if (chain.base_called) {
            return {};
        }
        if (name == "world") return {Kind::World};
        if (name == "self") return {Kind::Self};
        if (name == "Input" || name == "Timer" || name == "Event") return {Kind::Module, nullptr, name};
        for (const Library& library : kLibraries) {
            if (name == library.name) return {Kind::Library, nullptr, name};
        }
        // A local: evaluate what it was last set to above the cursor.
        if (depth >= 8) {
            return {};
        }
        const std::regex declaration("\\blocal\\s+" + name + "\\s*(?::[^=]*)?=\\s*");
        usize best = std::string_view::npos;
        const std::string before(source_.substr(0, cursor_));
        for (auto it = std::sregex_iterator(before.begin(), before.end(), declaration); it != std::sregex_iterator();
             ++it) {
            best = static_cast<usize>(it->position() + it->length());
        }
        if (best == std::string_view::npos) {
            return {};
        }
        return Evaluate(ParseChain(source_, best), depth + 1);
    }

    static Type Apply(const Type& type, const Segment& segment) {
        switch (type.kind) {
        case Kind::Module:
            if (type.name == "Event" && segment.name == "new") return {Kind::Event};
            if (type.name == "Timer" && (segment.name == "After" || segment.name == "Every")) return {Kind::Timer};
            if (type.name == "Input" && segment.name.rfind("On", 0) == 0) return {Kind::Event};
            return {};
        case Kind::World:
            if (segment.name == "Spawn" || segment.name == "Find") return {Kind::Entity};
            return {};
        case Kind::Self:
            if (segment.sep == '.' && segment.name == "entity") return {Kind::Entity};
            return {};
        case Kind::Entity:
            if (segment.sep == ':' && (segment.name == "Get" || segment.name == "Add") && segment.called) {
                if (const TypeInfo* info = ReflectedComponent(segment.string_arg)) {
                    return {Kind::Component, info};
                }
            }
            return {};
        case Kind::Event:
            if (segment.name == "Connect") return {Kind::Connection};
            return {};
        case Kind::Component:
        case Kind::Struct:
            if (segment.sep == '.' && !segment.called) {
                if (const reflect::FieldInfo* field = type.info->FindField(segment.name)) {
                    return TypeOfValue(field->type);
                }
            } else if (segment.called) {
                if (const reflect::FunctionInfo* function = type.info->FindFunction(segment.name)) {
                    return TypeOfValue(function->return_type);
                }
            }
            return {};
        default: return {};
        }
    }

    std::string_view source_;
    usize cursor_;
};

// ---------------------------------------------------------------------------
// Collecting items
// ---------------------------------------------------------------------------

std::string Signature(const reflect::FunctionInfo& function) {
    std::string text = "(";
    for (usize i = 0; i < function.params.size(); ++i) {
        if (i > 0) {
            text += ", ";
        }
        text += function.params[i].name;
        if (function.params[i].type != nullptr) {
            text += std::string(": ") + function.params[i].type->name;
        }
    }
    text += ")";
    if (function.return_type != nullptr) {
        text += std::string(" -> ") + function.return_type->name;
    }
    return text;
}

class Collector {
public:
    explicit Collector(std::string_view prefix) : prefix_(Lower(prefix)) {}

    void Add(const std::string& label, CompletionKind kind, const std::string& detail = "") {
        if (label.empty() || Lower(label).rfind(prefix_, 0) != 0 || !seen_.insert(label).second) {
            return;
        }
        items_.push_back({label, kind, detail});
    }
    void AddAll(const std::vector<Member>& members, CompletionKind kind) {
        for (const Member& member : members) {
            Add(member.name, kind, member.detail);
        }
    }

    std::vector<CompletionItem> Take() {
        std::sort(items_.begin(), items_.end(), [](const CompletionItem& a, const CompletionItem& b) {
            const std::string la = Lower(a.label), lb = Lower(b.label);
            return la != lb ? la < lb : a.label < b.label;
        });
        return std::move(items_);
    }

private:
    std::string prefix_;
    std::set<std::string> seen_;
    std::vector<CompletionItem> items_;
};

void AddMatches(Collector& out, std::string_view source, const std::regex& pattern, CompletionKind kind,
                const std::string& detail = "") {
    const std::string text(source);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it) {
        out.Add((*it)[1].str(), kind, detail);
    }
}

void AddMembers(Collector& out, const Type& type, char sep, std::string_view source) {
    const bool colon = sep == ':';
    switch (type.kind) {
    case Kind::Module:
        if (!colon) {
            out.AddAll(type.name == "Input" ? kInput : type.name == "Timer" ? kTimer : kEventModule,
                       CompletionKind::Function);
        }
        break;
    case Kind::Library:
        if (!colon) {
            for (const Library& library : kLibraries) {
                if (type.name == library.name) {
                    for (const char* member : library.members) {
                        out.Add(member, CompletionKind::Function);
                    }
                }
            }
        }
        break;
    case Kind::World:
        if (colon) out.AddAll(kWorld, CompletionKind::Method);
        break;
    case Kind::Entity:
        if (colon) out.AddAll(kEntity, CompletionKind::Method);
        break;
    case Kind::Event:
        if (colon) out.AddAll(kEvent, CompletionKind::Method);
        break;
    case Kind::Connection:
        if (colon) out.AddAll(kConnection, CompletionKind::Method);
        break;
    case Kind::Timer:
        if (colon) out.AddAll(kTimerObject, CompletionKind::Method);
        break;
    case Kind::Vector:
        if (!colon) out.AddAll(kVectorFields, CompletionKind::Field);
        break;
    case Kind::Component:
    case Kind::Struct:
        if (colon) {
            for (const reflect::FunctionInfo& function : type.info->functions) {
                out.Add(function.name, CompletionKind::Method, Signature(function));
            }
        } else {
            for (const reflect::FieldInfo& field : type.info->fields) {
                out.Add(field.name, CompletionKind::Field, field.type != nullptr ? field.type->name : "");
            }
        }
        break;
    case Kind::Self:
        if (colon) {
            AddMatches(out, source, std::regex("function\\s+[A-Za-z_]\\w*:([A-Za-z_]\\w*)"), CompletionKind::Method);
        } else {
            out.Add("entity", CompletionKind::Field, "Entity");
            AddMatches(out, source, std::regex("\\bself\\.([A-Za-z_]\\w*)"), CompletionKind::Field);
            // Class fields: "Class.speed = 180" (the class is a local table).
            const std::string text(source);
            const std::regex table("\\blocal\\s+([A-Za-z_]\\w*)\\s*=\\s*\\{\\s*\\}");
            for (auto it = std::sregex_iterator(text.begin(), text.end(), table); it != std::sregex_iterator();
                 ++it) {
                AddMatches(out, source, std::regex("\\b" + (*it)[1].str() + "\\.([A-Za-z_]\\w*)\\s*="),
                           CompletionKind::Field);
            }
        }
        break;
    case Kind::Unknown: break;
    }
}

void AddScope(Collector& out, std::string_view source, usize cursor) {
    for (const char* keyword : kKeywords) {
        out.Add(keyword, CompletionKind::Keyword);
    }
    for (const char* global : kGlobals) {
        out.Add(global, CompletionKind::Global);
    }
    out.Add("world", CompletionKind::Module, "World");
    out.Add("Input", CompletionKind::Module);
    out.Add("Timer", CompletionKind::Module);
    out.Add("Event", CompletionKind::Module);
    for (const Library& library : kLibraries) {
        out.Add(library.name, CompletionKind::Module);
    }
    const std::string_view before = source.substr(0, cursor);
    const std::string text(before);
    // local a, b = ... / local function f / function f(a, b) / for i, v in
    const std::regex locals("\\blocal\\s+(?!function\\b)([A-Za-z_]\\w*(?:\\s*:\\s*[^,=\\n]+)?(?:\\s*,\\s*[A-Za-z_]\\w*"
                            "(?:\\s*:\\s*[^,=\\n]+)?)*)");
    const std::regex name("([A-Za-z_]\\w*)(?:\\s*:\\s*[^,=\\n]+)?");
    auto add_list = [&](const std::string& list) {
        for (auto it = std::sregex_iterator(list.begin(), list.end(), name); it != std::sregex_iterator(); ++it) {
            out.Add((*it)[1].str(), CompletionKind::Variable);
        }
    };
    for (auto it = std::sregex_iterator(text.begin(), text.end(), locals); it != std::sregex_iterator(); ++it) {
        add_list((*it)[1].str());
    }
    AddMatches(out, before, std::regex("\\bfunction\\s+([A-Za-z_]\\w*)\\s*\\("), CompletionKind::Variable);
    const std::regex params("\\bfunction\\b[^(\\n]*\\(([^)]*)\\)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), params); it != std::sregex_iterator(); ++it) {
        add_list((*it)[1].str());
    }
    const std::regex loops("\\bfor\\s+([A-Za-z_]\\w*(?:\\s*,\\s*[A-Za-z_]\\w*)*)\\s*(?:=|in\\b)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), loops); it != std::sregex_iterator(); ++it) {
        add_list((*it)[1].str());
    }
    if (std::regex_search(text, std::regex("\\bfunction\\s+[A-Za-z_]\\w*:"))) {
        out.Add("self", CompletionKind::Variable);
    }
}

bool EndsWithWord(std::string_view text, std::string_view word) {
    usize end = text.size();
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    if (end < word.size() || text.substr(end - word.size(), word.size()) != word) {
        return false;
    }
    return end == word.size() || !IsIdent(text[end - word.size() - 1]);
}

} // namespace

CompletionResult CompleteScript(std::string_view source, usize cursor) {
    cursor = std::min(cursor, source.size());
    CompletionResult result;
    result.replace_from = cursor;

    const Lexical lexical = Scan(source, cursor);
    if (lexical.in_comment) {
        return result;
    }
    if (lexical.in_string) {
        // Component names inside :Get("..."), :Has, :Add, :Remove, :EntitiesWith.
        usize i = lexical.string_start - 1; // the quote
        while (i > 0 && (source[i - 1] == ' ' || source[i - 1] == '\t')) {
            --i;
        }
        if (i == 0 || source[i - 1] != '(') {
            return result;
        }
        usize end = i - 1;
        usize start = end;
        while (start > 0 && IsIdent(source[start - 1])) {
            --start;
        }
        const std::string_view method = source.substr(start, end - start);
        const bool component_method = method == "Get" || method == "Has" || method == "Add" || method == "Remove" ||
                                      method == "EntitiesWith";
        if (!component_method || start == 0 || (source[start - 1] != ':' && source[start - 1] != '.')) {
            return result;
        }
        result.replace_from = lexical.string_start;
        Collector out(source.substr(lexical.string_start, cursor - lexical.string_start));
        for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
            const ComponentInfo& info = GetComponentInfo(id);
            if (info.reflected != nullptr && info.name != nullptr && info.name[0] != '\0') {
                out.Add(info.name, CompletionKind::Component, "component");
            }
        }
        result.items = out.Take();
        return result;
    }

    usize from = cursor;
    while (from > 0 && IsIdent(source[from - 1])) {
        --from;
    }
    result.replace_from = from;
    Collector out(source.substr(from, cursor - from));
    if (from < cursor && std::isdigit(static_cast<unsigned char>(source[from])) != 0) {
        return result; // a number
    }

    const char sep = from > 0 ? source[from - 1] : '\0';
    const bool member = (sep == '.' || sep == ':') && !(sep == '.' && from > 1 && source[from - 2] == '.');
    if (member) {
        const usize start = ReceiverStart(source, from - 1);
        if (start == from - 1) {
            return result; // nothing before the '.'
        }
        if (sep == ':' && EndsWithWord(source.substr(0, start), "function")) {
            for (const char* callback : kCallbacks) {
                out.Add(callback, CompletionKind::Callback);
            }
            result.items = out.Take();
            return result;
        }
        if (EndsWithWord(source.substr(0, start), "function")) {
            return result;
        }
        const Chain chain = ParseChain(source.substr(0, from - 1), start);
        const Type type = Evaluator(source, cursor).Evaluate(chain);
        AddMembers(out, type, sep, source);
        result.items = out.Take();
        return result;
    }

    // A name being declared needs no suggestions.
    if (EndsWithWord(source.substr(0, from), "local") || EndsWithWord(source.substr(0, from), "function")) {
        return result;
    }
    AddScope(out, source, cursor);
    result.items = out.Take();
    return result;
}

} // namespace aether::script
