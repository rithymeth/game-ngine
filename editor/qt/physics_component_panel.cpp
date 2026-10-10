#include "physics_component_panel.h"

#include "aether/physics/character.h"
#include "aether/physics/components.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/gameplay.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <limits>

namespace aether::editor::qt {
namespace {

struct ComponentChoice {
    const char* label;
    ComponentId (*id)();
};

const ComponentChoice kChoices[] = {
    {"Rigid Body", &GetComponentId<RigidBody>},
    {"Box Collider", &GetComponentId<BoxCollider>},
    {"Sphere Collider", &GetComponentId<SphereCollider>},
    {"Capsule Collider", &GetComponentId<CapsuleCollider>},
    {"Convex Collider", &GetComponentId<ConvexCollider>},
    {"Mesh Collider", &GetComponentId<MeshCollider>},
    {"Character Movement", &GetComponentId<CharacterMovement>},
    {"Collision Layer", &GetComponentId<Layer>},
};

bool IsVector3(const reflect::TypeInfo& type) {
    return type.kind == reflect::TypeKind::Struct && type.serialize_as_array && type.fields.size() == 3 &&
        type.fields[0].type->kind == reflect::TypeKind::Float &&
        type.fields[1].type->kind == reflect::TypeKind::Float &&
        type.fields[2].type->kind == reflect::TypeKind::Float;
}

i64 ReadEnum(const reflect::TypeInfo& type, const void* data) {
    if (!type.underlying) return 0;
    switch (type.underlying->size) {
    case 1: {
        if (type.underlying->kind == reflect::TypeKind::Int) return *static_cast<const i8*>(data);
        return *static_cast<const u8*>(data);
    }
    case 2: {
        if (type.underlying->kind == reflect::TypeKind::Int) return *static_cast<const i16*>(data);
        return *static_cast<const u16*>(data);
    }
    case 4: {
        if (type.underlying->kind == reflect::TypeKind::Int) return *static_cast<const i32*>(data);
        return *static_cast<const u32*>(data);
    }
    default: {
        if (type.underlying->kind == reflect::TypeKind::Int) return *static_cast<const i64*>(data);
        return static_cast<i64>(*static_cast<const u64*>(data));
    }
    }
}

reflect::Any EnumValue(const reflect::TypeInfo& type, i64 raw) {
    reflect::Any value = reflect::Any::DefaultOf(type);
    if (!type.underlying) return value;
    switch (type.underlying->size) {
    case 1: { const u8 v = static_cast<u8>(raw); std::memcpy(value.Data(), &v, sizeof(v)); break; }
    case 2: { const u16 v = static_cast<u16>(raw); std::memcpy(value.Data(), &v, sizeof(v)); break; }
    case 4: { const u32 v = static_cast<u32>(raw); std::memcpy(value.Data(), &v, sizeof(v)); break; }
    default: { const u64 v = static_cast<u64>(raw); std::memcpy(value.Data(), &v, sizeof(v)); break; }
    }
    return value;
}

reflect::Any NumericValue(const reflect::TypeInfo& type, double number) {
    if (type.kind == reflect::TypeKind::Float)
        return type.size == sizeof(f32) ? reflect::Any(static_cast<f32>(number)) : reflect::Any(number);
    switch (type.size) {
    case 1:
        return type.kind == reflect::TypeKind::Int
            ? reflect::Any(static_cast<i8>(number)) : reflect::Any(static_cast<u8>(number));
    case 2:
        return type.kind == reflect::TypeKind::Int
            ? reflect::Any(static_cast<i16>(number)) : reflect::Any(static_cast<u16>(number));
    case 4:
        return type.kind == reflect::TypeKind::Int
            ? reflect::Any(static_cast<i32>(number)) : reflect::Any(static_cast<u32>(number));
    default:
        return type.kind == reflect::TypeKind::Int
            ? reflect::Any(static_cast<i64>(number)) : reflect::Any(static_cast<u64>(number));
    }
}

QDoubleSpinBox* MakeNumberEditor(const reflect::TypeInfo& type, const reflect::Meta& meta, double value,
                                 QWidget* parent) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(meta.HasRange() ? meta.range_min : -1000000.0,
                   meta.HasRange() ? meta.range_max : 1000000.0);
    spin->setDecimals(type.kind == reflect::TypeKind::Float ? 3 : 0);
    spin->setSingleStep(type.kind == reflect::TypeKind::Float ? 0.1 : 1.0);
    if (meta.units) spin->setSuffix(QString(" %1").arg(meta.units));
    spin->setKeyboardTracking(false);
    spin->setValue(std::clamp(value, spin->minimum(), spin->maximum()));
    return spin;
}

} // namespace

PhysicsComponentPanel::PhysicsComponentPanel(SceneDocument& document, std::function<void()> changed, QWidget* parent)
    : QWidget(parent), document_(document), changed_(std::move(changed)) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 8, 0, 0);
    root->setSpacing(8);
    auto* add = new QPushButton("+ Add Physics Component", this);
    add->setAccessibleName("Add physics component to selected entity");
    add->setToolTip("Add a rigid body, collider, or character movement component");
    root->addWidget(add);
    cards_ = new QVBoxLayout;
    cards_->setSpacing(8);
    root->addLayout(cards_);
    QObject::connect(add, &QPushButton::clicked, this, [this, add] {
        if (entity_.IsNull()) return;
        QMenu menu(add);
        for (const ComponentChoice& choice : kChoices) {
            const ComponentId id = choice.id();
            if (document_.GetWorld().HasComponentRaw(entity_, id)) continue;
            QAction* action = menu.addAction(QString::fromUtf8(choice.label));
            QObject::connect(action, &QAction::triggered, this, [this, id] {
                if (!document_.AddComponent(entity_, id)) return;
                Refresh();
                if (changed_) changed_();
            });
        }
        if (menu.isEmpty()) menu.addAction("All physics components are attached")->setEnabled(false);
        menu.exec(add->mapToGlobal(QPoint(0, add->height())));
    });
}

void PhysicsComponentPanel::SetEntity(Entity entity) {
    if (entity_ == entity) return;
    entity_ = entity;
    Refresh();
}

void PhysicsComponentPanel::SetLayerNames(const std::vector<std::string>& names) {
    layer_names_.clear();
    for (const std::string& name : names) {
        if (layer_names_.size() == kMaxLayers) break;
        layer_names_.push_back(name);
    }
    if (layer_names_.empty() || layer_names_.front() != "Default") layer_names_.insert(layer_names_.begin(), "Default");
    if (layer_names_.size() > kMaxLayers) layer_names_.resize(kMaxLayers);
    Refresh();
}

void PhysicsComponentPanel::Refresh() {
    while (QLayoutItem* item = cards_->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
    if (entity_.IsNull() || !document_.GetWorld().IsAlive(entity_)) return;

    for (const ComponentChoice& choice : kChoices) {
        const ComponentId component_id = choice.id();
        if (!document_.GetWorld().HasComponentRaw(entity_, component_id)) continue;
        const ComponentInfo& component = GetComponentInfo(component_id);
        if (!component.reflected) continue;
        auto* group = new QGroupBox(QString::fromUtf8(choice.label), this);
        auto* group_layout = new QVBoxLayout(group);
        group_layout->setContentsMargins(10, 8, 10, 8);
        auto* header = new QHBoxLayout;
        header->addStretch();
        auto* remove = new QToolButton(group);
        remove->setText("Remove");
        remove->setToolTip("Remove this component. Undo restores its settings.");
        header->addWidget(remove);
        group_layout->addLayout(header);
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setHorizontalSpacing(10);
        form->setVerticalSpacing(5);
        group_layout->addLayout(form);
        void* component_data = document_.GetWorld().GetComponentRaw(entity_, component_id);
        for (const reflect::FieldInfo& field : component.reflected->fields) {
            if (field.HasFlag(reflect::Field_Transient) ||
                (!field.HasFlag(reflect::Field_EditAnywhere) && !field.HasFlag(reflect::Field_ReadOnly))) continue;
            const reflect::Any current = field.Get(component_data);
            const bool read_only = field.HasFlag(reflect::Field_ReadOnly);
            QWidget* editor = nullptr;
            const QString label = QString::fromUtf8(field.name);
            const auto changed = [this, component_id, field_name = std::string(field.name)](const reflect::Any& value) {
                if (!document_.SetComponentField(entity_, component_id, field_name, value, true)) return;
                if (changed_) changed_();
            };
            if (component_id == GetComponentId<Layer>() && std::strcmp(field.name, "index") == 0) {
                auto* combo = new QComboBox(group);
                const u8 selected = current.Get<u8>();
                for (std::size_t layer = 0; layer < layer_names_.size(); ++layer)
                    combo->addItem(QString::fromStdString(layer_names_[layer]), static_cast<int>(layer));
                if (selected >= layer_names_.size())
                    combo->addItem(QString("Missing layer %1").arg(selected), static_cast<int>(selected));
                combo->setCurrentIndex(combo->findData(static_cast<int>(selected)));
                combo->setEnabled(!read_only);
                QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [combo, changed, field_type = field.type](int index) {
                        if (index >= 0) changed(NumericValue(*field_type, combo->itemData(index).toDouble()));
                    });
                editor = combo;
            } else if (field.type->kind == reflect::TypeKind::Bool) {
                auto* check = new QCheckBox(group);
                check->setChecked(current.Get<bool>());
                check->setEnabled(!read_only);
                QObject::connect(check, &QCheckBox::toggled, this, [changed](bool value) {
                    changed(reflect::Any(value));
                });
                editor = check;
            } else if (field.type->kind == reflect::TypeKind::Enum) {
                auto* combo = new QComboBox(group);
                const i64 selected = ReadEnum(*field.type, current.Data());
                int current_index = -1;
                for (const reflect::EnumValue& option : field.type->enum_values) {
                    combo->addItem(QString::fromUtf8(option.name), QVariant::fromValue<qlonglong>(option.value));
                    if (option.value == selected) current_index = combo->count() - 1;
                }
                combo->setCurrentIndex(current_index);
                combo->setEnabled(!read_only);
                QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [combo, changed, field_type = field.type](int index) {
                        if (index >= 0) changed(EnumValue(*field_type, combo->itemData(index).toLongLong()));
                    });
                editor = combo;
            } else if (field.type->kind == reflect::TypeKind::Int || field.type->kind == reflect::TypeKind::UInt ||
                       field.type->kind == reflect::TypeKind::Float) {
                const double number = reflect::ToJson(*field.type, current.Data()).get<double>();
                auto* spin = MakeNumberEditor(*field.type, field.meta, number, group);
                spin->setEnabled(!read_only);
                QObject::connect(spin, &QDoubleSpinBox::editingFinished, this, [spin, field_type = field.type, changed] {
                    changed(NumericValue(*field_type, spin->value()));
                });
                editor = spin;
            } else if (IsVector3(*field.type)) {
                const nlohmann::json vector = reflect::ToJson(*field.type, current.Data());
                auto* row = new QWidget(group);
                auto* row_layout = new QHBoxLayout(row);
                row_layout->setContentsMargins(0, 0, 0, 0);
                for (int axis = 0; axis < 3; ++axis) {
                    auto* spin = MakeNumberEditor(reflect::Reflect<f32>(), field.meta,
                                                  vector[axis].get<double>(), row);
                    spin->setPrefix(QString(QChar('X' + axis)) + " ");
                    spin->setEnabled(!read_only);
                    row_layout->addWidget(spin);
                    QObject::connect(spin, &QDoubleSpinBox::editingFinished, this,
                        [row, changed] {
                            const auto spins = row->findChildren<QDoubleSpinBox*>();
                            if (spins.size() != 3) return;
                            changed(reflect::Any(Vec3{static_cast<f32>(spins[0]->value()),
                                static_cast<f32>(spins[1]->value()), static_cast<f32>(spins[2]->value())}));
                        });
                }
                editor = row;
            } else if (field.type->kind == reflect::TypeKind::Array) {
                editor = new QLabel(QString("%1 items").arg(field.type->array_size(current.Data())), group);
                editor->setEnabled(false);
            } else {
                editor = new QLabel("Read only", group);
                editor->setEnabled(false);
            }
            if (field.meta.tooltip) editor->setToolTip(QString::fromUtf8(field.meta.tooltip));
            form->addRow(label, editor);
        }
        QObject::connect(remove, &QToolButton::clicked, this, [this, component_id] {
            if (!document_.RemoveComponent(entity_, component_id)) return;
            Refresh();
            if (changed_) changed_();
        });
        cards_->addWidget(group);
    }
}

} // namespace aether::editor::qt
