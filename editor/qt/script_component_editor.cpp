#include "script_component_editor.h"

#include "aether/assets/asset_database.h"
#include "aether/core/log.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/script_component.h"
#include "aether/script/luau_host.h"
#include "aether/script/script_system.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace aether::editor::qt {
namespace {

QString ToQString(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return file.good();
}

std::string SafeScriptName(std::string name) {
    std::string result;
    result.reserve(name.size());
    for (unsigned char character : name) {
        if (std::isalnum(character) || character == '_') result.push_back(static_cast<char>(character));
    }
    if (result.empty()) result = "NewScript";
    if (std::isdigit(static_cast<unsigned char>(result.front()))) result.insert(result.begin(), '_');
    return result;
}

nlohmann::json ReadOverride(const ScriptComponent& component, const ExposedVariable& variable) {
    if (const ScriptProperty* property = component.FindProperty(variable.name)) {
        try {
            nlohmann::json value = nlohmann::json::parse(property->value);
            if (MatchesKind(variable.kind, value)) return value;
        } catch (...) {
            // A malformed or outdated override falls back to the script default.
        }
    }
    return variable.default_value;
}

void ClearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        if (QLayout* child_layout = item->layout()) {
            ClearLayout(child_layout);
        }
        delete item;
    }
}

} // namespace

ScriptComponentEditor::ScriptComponentEditor(SceneDocument& document, std::function<void()> changed, QWidget* parent)
    : QGroupBox("Custom Script", parent), document_(document), changed_(std::move(changed)),
      host_(std::make_unique<script::LuauHost>()) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 10);
    root->setSpacing(7);

    add_component_ = new QPushButton("+ Add Script Component", this);
    add_component_->setToolTip("Attach a Luau script to the selected entity");
    root->addWidget(add_component_);

    scripts_ = new QComboBox(this);
    scripts_->setToolTip("Script asset attached to this entity");
    root->addWidget(scripts_);

    auto* actions = new QHBoxLayout;
    create_script_ = new QPushButton("New Script…", this);
    edit_script_ = new QPushButton("Edit Source…", this);
    remove_component_ = new QPushButton("Remove", this);
    actions->addWidget(create_script_);
    actions->addWidget(edit_script_);
    actions->addStretch();
    actions->addWidget(remove_component_);
    root->addLayout(actions);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setObjectName("muted");
    root->addWidget(status_);
    variables_ = new QVBoxLayout;
    variables_->setContentsMargins(0, 2, 0, 0);
    variables_->setSpacing(5);
    root->addLayout(variables_);

    QObject::connect(add_component_, &QPushButton::clicked, this, [this] {
        if (entity_.IsNull()) return;
        if (document_.AddComponent(entity_, GetComponentId<ScriptComponent>())) {
            Refresh();
            if (changed_) changed_();
        }
    });
    QObject::connect(scripts_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0 || entity_.IsNull() || !document_.GetWorld().HasComponent<ScriptComponent>(entity_)) return;
        const QString text = scripts_->itemData(index).toString();
        assets::AssetGuid guid{};
        if (!text.isEmpty() && !assets::ParseAssetGuid(text.toStdString(), guid)) return;
        SetScriptAsset(guid);
    });
    QObject::connect(create_script_, &QPushButton::clicked, this, [this] { CreateAndAttachScript(); });
    QObject::connect(edit_script_, &QPushButton::clicked, this, [this] { EditScript(); });
    QObject::connect(remove_component_, &QPushButton::clicked, this, [this] {
        if (entity_.IsNull() || !document_.RemoveComponent(entity_, GetComponentId<ScriptComponent>())) return;
        Refresh();
        if (changed_) changed_();
    });
    Refresh();
}

ScriptComponentEditor::~ScriptComponentEditor() = default;

void ScriptComponentEditor::SetContentRoot(const std::filesystem::path& content_root) {
    if (content_root_ == content_root) {
        RefreshAssets();
        Refresh();
        return;
    }
    content_root_ = content_root;
    database_ = std::make_unique<assets::AssetDatabase>(content_root_);
    const assets::ScanResult scan = database_->Scan();
    for (const std::string& warning : scan.warnings)
        AETHER_LOG_WARN("QtEditor", "%s", warning.c_str());
    RefreshAssets();
    Refresh();
}

void ScriptComponentEditor::SetEntity(Entity entity) {
    if (entity_ == entity) {
        Refresh();
        return;
    }
    entity_ = entity;
    Refresh();
}

void ScriptComponentEditor::RefreshAssets() {
    if (!scripts_) return;
    const QSignalBlocker blocker(scripts_);
    scripts_->clear();
    scripts_->addItem("Select a Luau script…", QString());
    if (!database_) return;
    for (const assets::AssetRecord* record : database_->All()) {
        if (!record || record->importer != assets::ScriptAsset::kImporter || record->missing || record->IsSubAsset()) continue;
        scripts_->addItem(ToQString(record->path), ToQString(assets::ToString(record->guid)));
    }
}

void ScriptComponentEditor::Refresh() {
    const bool alive = !entity_.IsNull() && document_.GetWorld().IsAlive(entity_);
    const auto* component = alive ? document_.GetWorld().GetComponent<ScriptComponent>(entity_) : nullptr;
    const bool attached = component != nullptr;
    add_component_->setVisible(alive && !attached);
    scripts_->setVisible(alive && attached);
    create_script_->setVisible(alive);
    edit_script_->setVisible(alive && attached && component->script.IsSet());
    remove_component_->setVisible(alive && attached);
    status_->setVisible(alive && attached);

    ClearLayout(variables_);
    if (!alive || !attached) return;

    {
        const QSignalBlocker blocker(scripts_);
        int selected = 0;
        if (component->script.IsSet()) {
            const QString guid = ToQString(assets::ToString(component->script.guid));
            selected = scripts_->findData(guid);
            if (selected < 0) {
                scripts_->addItem("Missing script asset", guid);
                selected = scripts_->count() - 1;
            }
        }
        scripts_->setCurrentIndex(selected);
    }
    if (!component->script.IsSet()) {
        status_->setText("Choose a script asset, or create one for this entity.");
        return;
    }
    if (!database_) {
        status_->setText("Open a project to browse and create script assets.");
        return;
    }
    const assets::AssetRecord* record = database_->Find(component->script.guid);
    if (!record || record->missing) {
        status_->setText("The attached script asset is missing from this project.");
        return;
    }
    const std::string source = ReadText(database_->SourcePath(record->guid));
    if (source.empty()) {
        status_->setText("The script source is empty or couldn't be read.");
        return;
    }
    const ScriptClassInfo info = script::ScriptSystem::DescribeSource(*host_, source, record->path);
    if (!info.ok) {
        status_->setText("Script compile error: " + ToQString(info.error));
        return;
    }
    QString summary = QString("Luau · %1 exposed variable(s)").arg(static_cast<qulonglong>(info.variables.size()));
    if (!info.callbacks.empty()) {
        QStringList callbacks;
        for (const std::string& callback : info.callbacks) callbacks.push_back(ToQString(callback));
        summary += "\nCallbacks: " + callbacks.join(", ");
    }
    status_->setText(summary);
    if (info.variables.empty()) return;

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setVerticalSpacing(5);
    variables_->addLayout(form);
    for (const ExposedVariable& variable : info.variables) {
        const nlohmann::json current = ReadOverride(*component, variable);
        const auto commit = [this, name = variable.name, default_value = variable.default_value](const nlohmann::json& value) {
            SetProperty(name, value, default_value);
        };
        QWidget* editor = nullptr;
        switch (variable.kind) {
        case ExposedVariable::Kind::Number: {
            auto* spin = new QDoubleSpinBox(this);
            spin->setRange(variable.has_range ? variable.range_min : -1000000.0,
                           variable.has_range ? variable.range_max : 1000000.0);
            spin->setDecimals(3);
            spin->setSingleStep(0.1);
            spin->setKeyboardTracking(false);
            spin->setValue(std::clamp(current.get<double>(), spin->minimum(), spin->maximum()));
            if (!variable.tooltip.empty()) spin->setToolTip(ToQString(variable.tooltip));
            QObject::connect(spin, &QDoubleSpinBox::editingFinished, this, [spin, commit] { commit(spin->value()); });
            editor = spin;
            break;
        }
        case ExposedVariable::Kind::Bool: {
            auto* check = new QCheckBox(this);
            check->setChecked(current.get<bool>());
            if (!variable.tooltip.empty()) check->setToolTip(ToQString(variable.tooltip));
            QObject::connect(check, &QCheckBox::toggled, this, [commit](bool value) { commit(value); });
            editor = check;
            break;
        }
        case ExposedVariable::Kind::String: {
            auto* line = new QLineEdit(ToQString(current.get<std::string>()), this);
            if (!variable.tooltip.empty()) line->setToolTip(ToQString(variable.tooltip));
            QObject::connect(line, &QLineEdit::editingFinished, this, [line, commit] { commit(line->text().toStdString()); });
            editor = line;
            break;
        }
        case ExposedVariable::Kind::Vector: {
            auto* row = new QWidget(this);
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            for (int axis = 0; axis < 3; ++axis) {
                auto* spin = new QDoubleSpinBox(row);
                spin->setRange(variable.has_range ? variable.range_min : -1000000.0,
                               variable.has_range ? variable.range_max : 1000000.0);
                spin->setDecimals(3);
                spin->setSingleStep(0.1);
                spin->setPrefix(QString(QChar('X' + axis)) + " ");
                spin->setKeyboardTracking(false);
                spin->setValue(std::clamp(current[static_cast<std::size_t>(axis)].get<double>(),
                                          spin->minimum(), spin->maximum()));
                layout->addWidget(spin);
                QObject::connect(spin, &QDoubleSpinBox::editingFinished, this, [row, commit] {
                    const auto spins = row->findChildren<QDoubleSpinBox*>();
                    if (spins.size() != 3) return;
                    commit(nlohmann::json::array({spins[0]->value(), spins[1]->value(), spins[2]->value()}));
                });
            }
            if (!variable.tooltip.empty()) row->setToolTip(ToQString(variable.tooltip));
            editor = row;
            break;
        }
        }
        form->addRow(ToQString(variable.name), editor);
    }
}

void ScriptComponentEditor::CreateAndAttachScript() {
    if (entity_.IsNull() || content_root_.empty()) {
        QMessageBox::information(this, "Open a project", "Open a project before creating a script asset.");
        return;
    }
    bool accepted = false;
    const QString entered = QInputDialog::getText(this, "Create Luau Script", "Script name:",
                                                   QLineEdit::Normal, "NewScript", &accepted).trimmed();
    if (!accepted || entered.isEmpty()) return;
    const std::string name = SafeScriptName(entered.toStdString());
    const std::filesystem::path folder = content_root_ / "Scripts";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        QMessageBox::critical(this, "Couldn't create script folder", ToQString(ec.message()));
        return;
    }
    const std::filesystem::path path = folder / (name + ".luau");
    if (std::filesystem::exists(path)) {
        QMessageBox::warning(this, "Script already exists", "A script with that name already exists in Content/Scripts.");
        return;
    }
    const std::string source = "local " + name + " = {}\n\n--@tooltip Movement speed\n--@range 0 20\n" +
        name + ".speed = 6.0\n\nfunction " + name + ":OnStart()\n" +
        "    -- Physics.AddImpulse(self.entity, vector.create(0, 5, 0))\nend\n\n" +
        "function " + name + ":OnUpdate(dt)\n" +
        "    -- local hit, other, point, normal, distance = Physics.Raycast(origin, direction, 100, self.entity)\nend\n\n" +
        "function " + name + ":OnCollisionBegin(other, approachSpeed)\nend\n\nreturn " + name + "\n";
    if (!WriteText(path, source)) {
        QMessageBox::critical(this, "Couldn't create script", "The script source couldn't be written to disk.");
        return;
    }
    database_ = std::make_unique<assets::AssetDatabase>(content_root_);
    database_->Scan();
    const std::filesystem::path relative = std::filesystem::relative(path, content_root_, ec);
    const std::string asset_path = ec ? (std::string("Scripts/") + name + ".luau") : relative.generic_string();
    const assets::AssetRecord* record = database_->FindByPath(asset_path);
    if (!record) {
        QMessageBox::critical(this, "Script index failed", "The script was written, but its asset GUID could not be indexed.");
        RefreshAssets();
        return;
    }
    RefreshAssets();
    if (!document_.GetWorld().HasComponent<ScriptComponent>(entity_))
        document_.AddComponent(entity_, GetComponentId<ScriptComponent>());
    SetScriptAsset(record->guid);
}

void ScriptComponentEditor::EditScript() {
    if (entity_.IsNull() || !database_) return;
    const auto* component = document_.GetWorld().GetComponent<ScriptComponent>(entity_);
    if (!component || !component->script.IsSet()) return;
    const assets::AssetRecord* record = database_->Find(component->script.guid);
    if (!record || record->missing) return;
    const std::filesystem::path path = database_->SourcePath(record->guid);
    const std::string original = ReadText(path);
    QDialog dialog(this);
    dialog.setWindowTitle("Edit " + ToQString(record->path));
    dialog.resize(760, 620);
    auto* layout = new QVBoxLayout(&dialog);
    auto* editor = new QPlainTextEdit(&dialog);
    editor->setObjectName("luauSourceEditor");
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont font("Consolas");
    font.setStyleHint(QFont::Monospace);
    editor->setFont(font);
    editor->setPlainText(ToQString(original));
    layout->addWidget(editor, 1);
    auto* validation = new QLabel("Luau script source", &dialog);
    validation->setWordWrap(true);
    layout->addWidget(validation);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto* validate = buttons->addButton("Validate", QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    const auto validate_source = [this, editor, validation, path] {
        const ScriptClassInfo info = script::ScriptSystem::DescribeSource(
            *host_, editor->toPlainText().toStdString(), path.filename().string());
        validation->setText(info.ok ? QString("Valid · %1 exposed variable(s), %2 callback(s)")
            .arg(static_cast<qulonglong>(info.variables.size())).arg(static_cast<qulonglong>(info.callbacks.size()))
            : "Compile error: " + ToQString(info.error));
        validation->setStyleSheet(info.ok ? "color: #77cf9b" : "color: #ff8b83");
        return info.ok;
    };
    QObject::connect(validate, &QPushButton::clicked, &dialog, validate_source);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!WriteText(path, editor->toPlainText().toStdString())) {
            QMessageBox::critical(&dialog, "Couldn't save script", "The source file couldn't be written.");
            return;
        }
        if (database_) database_->Scan();
        dialog.accept();
    });
    validate_source();
    if (dialog.exec() == QDialog::Accepted) {
        RefreshAssets();
        Refresh();
        if (changed_) changed_();
    }
}

void ScriptComponentEditor::SetScriptAsset(const assets::AssetGuid& guid) {
    if (entity_.IsNull() || !document_.GetWorld().HasComponent<ScriptComponent>(entity_)) return;
    if (!guid.IsNull()) {
        const assets::AssetRecord* record = database_ ? database_->Find(guid) : nullptr;
        if (!record || record->importer != assets::ScriptAsset::kImporter) return;
    }
    if (!document_.SetComponentField(entity_, GetComponentId<ScriptComponent>(), "script",
                                    reflect::Any(assets::AssetRef<assets::ScriptAsset>{guid}), true)) return;
    Refresh();
    if (changed_) changed_();
}

void ScriptComponentEditor::SetProperty(const std::string& name, const nlohmann::json& value,
                                        const nlohmann::json& default_value) {
    if (entity_.IsNull()) return;
    const auto* current = document_.GetWorld().GetComponent<ScriptComponent>(entity_);
    if (!current) return;
    ScriptComponent updated = *current;
    if (value == default_value) updated.ResetProperty(name);
    else updated.SetProperty(name, value);
    if (!document_.SetComponentField(entity_, GetComponentId<ScriptComponent>(), "properties",
                                    reflect::Any(updated.properties), true)) return;
    if (changed_) changed_();
}

} // namespace aether::editor::qt
