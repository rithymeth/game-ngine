#include "aether/gameplay/tag_query.h"

namespace aether::gas {

using reflect::Json;

namespace {

const char* OpName(TagQuery::Op op) {
    switch (op) {
    case TagQuery::Op::Any: return "any";
    case TagQuery::Op::All: return "all";
    case TagQuery::Op::None: return "none";
    case TagQuery::Op::And: return "and";
    case TagQuery::Op::Or: return "or";
    }
    return "all";
}

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

constexpr int kMaxDepth = 32;

bool Read(const Json& j, TagQuery& out, std::string* error, int depth) {
    if (depth > kMaxDepth) return Fail(error, "the query is nested too deeply");
    if (!j.is_object() || !j.contains("op") || !j["op"].is_string()) return Fail(error, "a query is an object with an 'op'");
    const std::string op = j["op"].get<std::string>();
    TagQuery q;
    bool leaf = true;
    if (op == "any") q.op = TagQuery::Op::Any;
    else if (op == "all") q.op = TagQuery::Op::All;
    else if (op == "none") q.op = TagQuery::Op::None;
    else if (op == "and") q.op = TagQuery::Op::And, leaf = false;
    else if (op == "or") q.op = TagQuery::Op::Or, leaf = false;
    else return Fail(error, "unknown query op '" + op + "'");
    if (leaf) {
        if (j.contains("tags")) {
            if (!j["tags"].is_array()) return Fail(error, "'tags' is a list of tag names");
            for (const Json& t : j["tags"]) {
                if (!t.is_string() || !GameplayTag::ValidName(t.get<std::string>())) return Fail(error, "'" + (t.is_string() ? t.get<std::string>() : t.dump()) + "' isn't a valid tag name");
                q.tags.push_back(GameplayTag{t.get<std::string>()});
            }
        }
    } else if (j.contains("children")) {
        if (!j["children"].is_array()) return Fail(error, "'children' is a list of queries");
        for (const Json& c : j["children"]) {
            TagQuery child;
            if (!Read(c, child, error, depth + 1)) return false;
            q.children.push_back(std::move(child));
        }
    }
    out = std::move(q);
    return true;
}

} // namespace

bool TagQuery::Matches(const TagContainer& container) const {
    switch (op) {
    case Op::Any: return container.HasAny(tags);
    case Op::All: return container.HasAll(tags);
    case Op::None: return container.HasNone(tags);
    case Op::And:
        for (const TagQuery& c : children) {
            if (!c.Matches(container)) return false;
        }
        return true;
    case Op::Or:
        for (const TagQuery& c : children) {
            if (c.Matches(container)) return true;
        }
        return false;
    }
    return false;
}

Json TagQuery::ToJson() const {
    Json j = {{"op", OpName(op)}};
    if (op == Op::And || op == Op::Or) {
        Json list = Json::array();
        for (const TagQuery& c : children) list.push_back(c.ToJson());
        j["children"] = std::move(list);
    } else {
        Json list = Json::array();
        for (const GameplayTag& t : tags) list.push_back(t.name);
        j["tags"] = std::move(list);
    }
    return j;
}

bool TagQuery::FromJson(const Json& j, TagQuery& out, std::string* error) { return Read(j, out, error, 0); }

} // namespace aether::gas
