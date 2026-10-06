#include "aether/gameplay/attribute_library.h"

#include "aether/gameplay/attribute_system.h"

namespace aether::gas {

f32 Attributes::GetAttribute(const Entity& target, const std::string& name, f32 fallback) {
    const AttributeSystem* s = AttributeSystem::Active();
    return s ? s->Get(target, name, fallback) : fallback;
}
f32 Attributes::GetAttributeBase(const Entity& target, const std::string& name, f32 fallback) {
    const AttributeSystem* s = AttributeSystem::Active();
    return s ? s->GetBase(target, name, fallback) : fallback;
}
f32 Attributes::GetAttributeMin(const Entity& target, const std::string& name, f32 fallback) {
    const AttributeSystem* s = AttributeSystem::Active();
    return s ? s->GetMin(target, name, fallback) : fallback;
}
f32 Attributes::GetAttributeMax(const Entity& target, const std::string& name, f32 fallback) {
    const AttributeSystem* s = AttributeSystem::Active();
    return s ? s->GetMax(target, name, fallback) : fallback;
}
bool Attributes::HasAttribute(const Entity& target, const std::string& name) {
    const AttributeSystem* s = AttributeSystem::Active();
    return s && s->Has(target, name);
}
bool Attributes::DefineAttribute(const Entity& target, const std::string& name, f32 base, f32 min, f32 max) {
    AttributeSystem* s = AttributeSystem::Active();
    return s && s->Define(target, name, base, min, max);
}
bool Attributes::SetAttributeBase(const Entity& target, const std::string& name, f32 value) {
    AttributeSystem* s = AttributeSystem::Active();
    return s && s->SetBase(target, name, value);
}
bool Attributes::AddAttributeBase(const Entity& target, const std::string& name, f32 delta) {
    AttributeSystem* s = AttributeSystem::Active();
    return s && s->AddBase(target, name, delta);
}

} // namespace aether::gas
