#pragma once

#include "aether/core/base.h"
#include "aether/ecs/entity.h"
#include "aether/reflection/reflection.h"

#include <string>

// Blueprint function library for attributes (Phase 30 step 2, §30.2), acting on
// AttributeSystem::Active(); without one the getters return the default and the
// setters do nothing. `Event.OnAttributeChanged` (name, old, new) is what an
// entity's Blueprint hears when one of its attributes' current value changes.

namespace aether::gas {

struct Attributes {
    static f32 GetAttribute(const Entity& target, const std::string& name, f32 fallback);
    static f32 GetAttributeBase(const Entity& target, const std::string& name, f32 fallback);
    static f32 GetAttributeMin(const Entity& target, const std::string& name, f32 fallback);
    static f32 GetAttributeMax(const Entity& target, const std::string& name, f32 fallback);
    static bool HasAttribute(const Entity& target, const std::string& name);
    // Creates (or redefines) an attribute with its bounds; false on a bad name.
    static bool DefineAttribute(const Entity& target, const std::string& name, f32 base, f32 min, f32 max);
    static bool SetAttributeBase(const Entity& target, const std::string& name, f32 value);
    static bool AddAttributeBase(const Entity& target, const std::string& name, f32 delta);
};

} // namespace aether::gas

AETHER_REFLECT(aether::gas::Attributes, 1,
    AETHER_METHOD(GetAttribute, Fn_BlueprintCallable | Fn_Pure, {"target", "name", "default"}),
    AETHER_METHOD(GetAttributeBase, Fn_BlueprintCallable | Fn_Pure, {"target", "name", "default"}),
    AETHER_METHOD(GetAttributeMin, Fn_BlueprintCallable | Fn_Pure, {"target", "name", "default"}),
    AETHER_METHOD(GetAttributeMax, Fn_BlueprintCallable | Fn_Pure, {"target", "name", "default"}),
    AETHER_METHOD(HasAttribute, Fn_BlueprintCallable | Fn_Pure, {"target", "name"}),
    AETHER_METHOD(DefineAttribute, Fn_BlueprintCallable, {"target", "name", "base", "min", "max"}),
    AETHER_METHOD(SetAttributeBase, Fn_BlueprintCallable, {"target", "name", "value"}),
    AETHER_METHOD(AddAttributeBase, Fn_BlueprintCallable, {"target", "name", "delta"}))
