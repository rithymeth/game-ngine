#pragma once

#include "core/scene_document.h"

#include <QWidget>

#include <functional>
#include <string>
#include <vector>

class QVBoxLayout;

namespace aether::editor::qt {

class PhysicsComponentPanel final : public QWidget {
public:
    explicit PhysicsComponentPanel(SceneDocument& document, std::function<void()> changed, QWidget* parent = nullptr);

    void SetEntity(Entity entity);
    void SetLayerNames(const std::vector<std::string>& names);

private:
    void Refresh();

    SceneDocument& document_;
    std::function<void()> changed_;
    Entity entity_ = kNullEntity;
    std::vector<std::string> layer_names_{"Default"};
    QVBoxLayout* cards_ = nullptr;
};

} // namespace aether::editor::qt
