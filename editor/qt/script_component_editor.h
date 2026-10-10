#pragma once

#include "core/scene_document.h"

#include <QGroupBox>

#include <filesystem>
#include <memory>
#include <functional>

namespace aether::assets { class AssetDatabase; }
namespace aether::script { class LuauHost; }
class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace aether::editor::qt {

class ScriptComponentEditor final : public QGroupBox {
public:
    explicit ScriptComponentEditor(SceneDocument& document, std::function<void()> changed, QWidget* parent = nullptr);
    ~ScriptComponentEditor() override;

    void SetContentRoot(const std::filesystem::path& content_root);
    void SetEntity(Entity entity);

private:
    void Refresh();
    void RefreshAssets();
    void CreateAndAttachScript();
    void EditScript();
    void SetScriptAsset(const assets::AssetGuid& guid);
    void SetProperty(const std::string& name, const nlohmann::json& value,
                     const nlohmann::json& default_value);

    SceneDocument& document_;
    std::function<void()> changed_;
    Entity entity_ = kNullEntity;
    std::filesystem::path content_root_;
    std::unique_ptr<assets::AssetDatabase> database_;
    std::unique_ptr<script::LuauHost> host_;
    QComboBox* scripts_ = nullptr;
    QPushButton* add_component_ = nullptr;
    QPushButton* create_script_ = nullptr;
    QPushButton* edit_script_ = nullptr;
    QPushButton* remove_component_ = nullptr;
    QLabel* status_ = nullptr;
    QVBoxLayout* variables_ = nullptr;
};

} // namespace aether::editor::qt
