#include "animation_editor_widget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace aether::editor::qt {
namespace {

using aether::assets::GltfAnimation;
using aether::assets::GltfAnimationChannel;
using aether::assets::GltfAnimationInterpolation;
using aether::assets::GltfAnimationPath;
using aether::assets::GltfAnimationPose;
using aether::assets::GltfNodeInstance;
using aether::assets::GltfScene;
using aether::assets::GltfSkin;
using aether::assets::GltfVertex;
using aether::f32;
using aether::usize;
using json = nlohmann::json;

constexpr int kTranslation = 0;
constexpr int kRotation = 1;
constexpr int kScale = 2;
constexpr float kEpsilon = 1e-4f;
constexpr float kPi = 3.14159265358979323846f;
const GltfAnimation& RestAnimation() {
    static const GltfAnimation animation;
    return animation;
}

GltfAnimationPath PathFromRow(int row) {
    return row == kRotation ? GltfAnimationPath::Rotation
         : row == kScale ? GltfAnimationPath::Scale
                         : GltfAnimationPath::Translation;
}

int RowFromPath(GltfAnimationPath path) {
    return path == GltfAnimationPath::Rotation ? kRotation
         : path == GltfAnimationPath::Scale ? kScale
                                            : kTranslation;
}

const char* PathName(GltfAnimationPath path) {
    return path == GltfAnimationPath::Rotation ? "rotation"
         : path == GltfAnimationPath::Scale ? "scale"
                                            : "translation";
}

std::string NodeName(const GltfScene& scene, int node) {
    if (node < 0 || static_cast<usize>(node) >= scene.nodes.size()) return "(no bone selected)";
    const std::string& name = scene.nodes[static_cast<usize>(node)].name;
    return name.empty() ? "Node " + std::to_string(node) : name;
}

QDoubleSpinBox* MakeFloatBox(QWidget* parent, double minimum, double maximum, int decimals = 3) {
    auto* box = new QDoubleSpinBox(parent);
    box->setRange(minimum, maximum);
    box->setDecimals(decimals);
    box->setKeyboardTracking(false);
    box->setButtonSymbols(QAbstractSpinBox::NoButtons);
    box->setMinimumWidth(64);
    return box;
}

QWidget* MakeTripleRow(QWidget* parent, const std::array<QDoubleSpinBox*, 3>& fields,
                       const QStringList& labels) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    for (int i = 0; i < 3; ++i) {
        fields[static_cast<usize>(i)]->setPrefix(labels.value(i) + " ");
        layout->addWidget(fields[static_cast<usize>(i)], 1);
    }
    return row;
}

Vec3 ReadPosition(const aether::Mat4& matrix) {
    const aether::Vec4& c = matrix.cols[3];
    return {c.x, c.y, c.z};
}

} // namespace

struct AnimationEditorWidget::TimelineWidget final : QWidget {
    explicit TimelineWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(138);
        setMinimumWidth(380);
        setMouseTracking(true);
        setAccessibleName("Animation keyframe timeline");
        setToolTip("Click to scrub the playhead; drag a key marker to retime it");
    }

    void SetData(const GltfAnimation* clip, int node, int path, int key, float time, float fps, bool snap) {
        clip_ = clip;
        node_ = node;
        selected_path_ = path;
        selected_key_ = key;
        time_ = time;
        fps_ = std::max(fps, 1.0f);
        snap_ = snap;
        update();
    }

    std::function<void(float)> scrub;
    std::function<void(int)> select_path;
    std::function<void(int, int, float)> move_key;
    std::function<void(int, int)> select_key;
    std::function<void()> begin_key_drag;
    std::function<void()> end_key_drag;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor("#111820"));
        constexpr int left = 58;
        constexpr int header = 29;
        constexpr int lane_height = 34;
        const float duration = clip_ ? std::max(clip_->duration, 0.05f) : 1.0f;
        const int track_width = std::max(width() - left - 12, 1);
        auto x_for = [&](float time) {
            return static_cast<qreal>(left) + std::clamp(time / duration, 0.0f, 1.0f) * track_width;
        };

        painter.setPen(QColor("#71808f"));
        painter.drawText(QRect(8, 3, left - 12, header - 2), Qt::AlignLeft | Qt::AlignVCenter, "TIME");
        const float major_step = duration <= 2.0f ? 0.25f : duration <= 8.0f ? 1.0f : 2.0f;
        painter.setPen(QPen(QColor("#33404c"), 1));
        for (float t = 0.0f; t <= duration + major_step * 0.25f; t += major_step) {
            const qreal x = x_for(t);
            painter.drawLine(QPointF(x, header - 2), QPointF(x, header + lane_height * 3));
            painter.setPen(QColor("#8d9aa7"));
            painter.drawText(QRectF(x + 3, 3, 56, 20), QString::number(t, 'f', 2));
            painter.setPen(QPen(QColor("#33404c"), 1));
        }

        const std::array<QString, 3> names{"Translation", "Rotation", "Scale"};
        const std::array<QColor, 3> colors{QColor("#53c99c"), QColor("#69a8ff"), QColor("#f6b953")};
        for (int lane = 0; lane < 3; ++lane) {
            const int y = header + lane * lane_height;
            painter.fillRect(QRect(left, y, track_width, lane_height - 1),
                             lane == selected_path_ ? QColor("#202c37") : QColor("#19232c"));
            painter.setPen(lane == selected_path_ ? colors[static_cast<usize>(lane)] : QColor("#9aa7b3"));
            painter.drawText(QRect(8, y, left - 12, lane_height - 1), Qt::AlignLeft | Qt::AlignVCenter,
                             names[static_cast<usize>(lane)]);
            if (!clip_ || node_ < 0) continue;
            const GltfAnimationPath path = PathFromRow(lane);
            for (const GltfAnimationChannel& channel : clip_->channels) {
                if (channel.node_index != static_cast<usize>(node_) || channel.path != path) continue;
                for (usize i = 0; i < channel.times.size(); ++i) {
                    const qreal x = x_for(channel.times[i]);
                    const bool selected = lane == selected_path_ && static_cast<int>(i) == selected_key_;
                    const qreal radius = selected ? 6.5 : 4.5;
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(colors[static_cast<usize>(lane)]);
                    painter.drawEllipse(QPointF(x, y + lane_height * 0.5), radius, radius);
                    if (selected) {
                        painter.setBrush(Qt::NoBrush);
                        painter.setPen(QPen(QColor("#f2f6fa"), 1.5));
                        painter.drawEllipse(QPointF(x, y + lane_height * 0.5), radius + 2.5, radius + 2.5);
                    }
                }
            }
        }
        const qreal playhead_x = x_for(time_);
        painter.setPen(QPen(QColor("#ff636f"), 2));
        painter.drawLine(QPointF(playhead_x, header - 1), QPointF(playhead_x, header + lane_height * 3));
        QPolygonF marker;
        marker << QPointF(playhead_x - 6, header - 2) << QPointF(playhead_x + 6, header - 2)
               << QPointF(playhead_x, header + 6);
        painter.setBrush(QColor("#ff636f"));
        painter.setPen(Qt::NoPen);
        painter.drawPolygon(marker);
        painter.setPen(QColor("#758391"));
        painter.drawText(QRect(left, height() - 20, track_width, 16), Qt::AlignLeft | Qt::AlignVCenter,
                         node_ >= 0 ? "Keys for selected rig node · click a lane or drag a key to retime"
                                    : "Select a rig node to edit its animation keys");
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        const float requested_time = TimeFromX(event->position().x());
        const int lane = LaneFromY(event->position().y());
        if (lane < 0 || !clip_ || node_ < 0) {
            if (scrub) scrub(Snap(requested_time));
            event->accept();
            return;
        }
        selected_path_ = lane;
        if (select_path) select_path(lane);
        selected_key_ = FindKeyAt(lane, event->position().x());
        if (selected_key_ >= 0) {
            if (select_key) select_key(lane, selected_key_);
            dragging_key_ = true;
            if (begin_key_drag) begin_key_drag();
            event->accept();
            update();
            return;
        }
        if (scrub) scrub(Snap(requested_time));
        update();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_key_ && (event->buttons() & Qt::LeftButton)) {
            if (move_key) move_key(selected_path_, selected_key_, Snap(TimeFromX(event->position().x())));
            event->accept();
            return;
        }
        const int lane = LaneFromY(event->position().y());
        setCursor(lane >= 0 ? Qt::PointingHandCursor : Qt::IBeamCursor);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && dragging_key_) {
            dragging_key_ = false;
            if (end_key_drag) end_key_drag();
            event->accept();
        }
    }

private:
    float TimeFromX(qreal x) const {
        constexpr qreal left = 58.0;
        const qreal track_width = std::max<qreal>(static_cast<qreal>(this->width()) - left - 12.0, 1.0);
        const float duration = clip_ ? std::max(clip_->duration, 0.05f) : 1.0f;
        return std::clamp(static_cast<float>((x - left) / track_width) * duration, 0.0f, duration);
    }
    float Snap(float time) const { return snap_ ? std::round(time * fps_) / fps_ : time; }
    static int LaneFromY(qreal y) {
        constexpr int header = 29;
        constexpr int lane_height = 34;
        if (y < header || y >= header + lane_height * 3) return -1;
        return std::clamp(static_cast<int>((y - header) / lane_height), 0, 2);
    }
    int FindKeyAt(int lane, qreal x) const {
        if (!clip_ || node_ < 0) return -1;
        const GltfAnimationPath path = PathFromRow(lane);
        const GltfAnimationChannel* channel = nullptr;
        for (const GltfAnimationChannel& candidate : clip_->channels) {
            if (candidate.node_index == static_cast<usize>(node_) && candidate.path == path) {
                channel = &candidate;
                break;
            }
        }
        if (!channel) return -1;
        int closest = -1;
        qreal distance = 10.0;
        for (usize i = 0; i < channel->times.size(); ++i) {
            const qreal candidate = std::abs(x_for(channel->times[i]) - x);
            if (candidate <= distance) {
                distance = candidate;
                closest = static_cast<int>(i);
            }
        }
        return closest;
    }
    qreal x_for(float time) const {
        constexpr qreal left = 58.0;
        const qreal track_width = std::max<qreal>(static_cast<qreal>(this->width()) - left - 12.0, 1.0);
        const float duration = clip_ ? std::max(clip_->duration, 0.05f) : 1.0f;
        return left + std::clamp(time / duration, 0.0f, 1.0f) * track_width;
    }

    const GltfAnimation* clip_ = nullptr;
    int node_ = -1;
    int selected_path_ = 0;
    int selected_key_ = -1;
    float time_ = 0.0f;
    float fps_ = 30.0f;
    bool snap_ = true;
    bool dragging_key_ = false;
};

struct AnimationEditorWidget::PreviewWidget final : QWidget {
    explicit PreviewWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(300, 260);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName("Animated character preview");
        setToolTip("Right-drag to orbit · mouse wheel to zoom · click a bone to select it");
    }

    void SetFrame(const GltfScene* scene, const GltfAnimation* animation, float time,
                  int selected_node, int view) {
        scene_ = scene;
        animation_ = animation;
        time_ = time;
        selected_node_ = selected_node;
        if (view != view_) SetView(view);
        update();
    }

    std::function<void(int)> select_node;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QLinearGradient background(rect().topLeft(), rect().bottomRight());
        background.setColorAt(0.0, QColor("#17222d"));
        background.setColorAt(1.0, QColor("#0e151c"));
        painter.fillRect(rect(), background);
        painter.setPen(QColor("#dce7f0"));
        painter.drawText(QRect(14, 8, width() - 28, 20), Qt::AlignLeft | Qt::AlignVCenter,
                         scene_ ? "CHARACTER PREVIEW" : "OPEN A GLTF CHARACTER TO START");
        if (!scene_) return;

        GltfAnimationPose pose;
        aether::assets::EvaluateAnimationPose(*scene_, animation_ ? *animation_ : RestAnimation(), time_, pose);
        DrawGrid(painter);
        DrawMeshes(painter, pose);
        DrawRig(painter, pose);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::RightButton) {
            orbiting_ = true;
            drag_start_ = event->position();
            yaw_start_ = yaw_;
            pitch_start_ = pitch_;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && scene_) {
            int node = -1;
            const qreal distance = FindJoint(event->position(), node);
            if (node >= 0 && distance <= 20.0 && select_node) select_node(node);
            event->accept();
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (orbiting_ && (event->buttons() & Qt::RightButton)) {
            const QPointF delta = event->position() - drag_start_;
            yaw_ = yaw_start_ - static_cast<float>(delta.x()) * 0.008f;
            pitch_ = std::clamp(pitch_start_ + static_cast<float>(delta.y()) * 0.006f, -1.48f, 1.48f);
            view_ = 0;
            update();
            event->accept();
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::RightButton && orbiting_) {
            orbiting_ = false;
            setCursor(Qt::ArrowCursor);
            event->accept();
        }
    }

    void wheelEvent(QWheelEvent* event) override {
        camera_distance_ = std::clamp(camera_distance_ *
            std::pow(0.86f, static_cast<float>(event->angleDelta().y()) / 120.0f), 0.35f, 100.0f);
        update();
        event->accept();
    }

private:
    struct Triangle {
        QPolygonF points;
        float depth = 0.0f;
        QColor color;
    };

    void SetView(int view) {
        view_ = std::clamp(view, 0, 3);
        if (view_ == 0) { yaw_ = 0.55f; pitch_ = 0.28f; }
        else if (view_ == 1) { yaw_ = 0.0f; pitch_ = 0.0f; }
        else if (view_ == 2) { yaw_ = kPi * 0.5f; pitch_ = 0.0f; }
        else { yaw_ = 0.0f; pitch_ = 1.44f; }
    }

    Vec3 CameraPosition() const {
        const float horizontal = std::cos(pitch_) * camera_distance_;
        return target_ + Vec3{std::sin(yaw_) * horizontal, std::sin(pitch_) * camera_distance_,
                              -std::cos(yaw_) * horizontal};
    }

    bool Project(const Vec3& world, QPointF& point, float& depth) const {
        const Vec3 camera_position = CameraPosition();
        const Vec3 forward = (target_ - camera_position).Normalized();
        const Vec3 right = forward.Cross({0.0f, 1.0f, 0.0f}).Normalized();
        const Vec3 up = right.Cross(forward).Normalized();
        const Vec3 relative = world - camera_position;
        const float horizontal = relative.Dot(right);
        const float vertical = relative.Dot(up);
        depth = relative.Dot(forward);
        if (view_ == 0) {
            if (depth <= 0.04f) return false;
            const float focal = static_cast<float>(height()) * 0.9f;
            point = QPointF(width() * 0.5 + horizontal * focal / depth,
                            height() * 0.52 - vertical * focal / depth);
        } else {
            const float scale = static_cast<float>(height()) * 0.82f / camera_distance_;
            point = QPointF(width() * 0.5 + horizontal * scale,
                            height() * 0.52 - vertical * scale);
        }
        return true;
    }

    void DrawGrid(QPainter& painter) const {
        painter.setPen(QPen(QColor(75, 96, 115, 45), 1));
        const int center_x = width() / 2;
        const int center_y = height() / 2;
        for (int i = -10; i <= 10; ++i) {
            const int offset = i * 32;
            painter.drawLine(center_x + offset, 34, center_x + offset, height() - 12);
            painter.drawLine(8, center_y + offset, width() - 8, center_y + offset);
        }
        painter.setPen(QPen(QColor("#3bba96"), 1.5));
        painter.drawLine(10, center_y, width() - 10, center_y);
        painter.setPen(QPen(QColor("#e58668"), 1.5));
        painter.drawLine(center_x, 32, center_x, height() - 10);
    }

    QColor MaterialColor(int material) const {
        if (!scene_ || material < 0 || static_cast<usize>(material) >= scene_->materials.size())
            return QColor("#9cb4c5");
        const auto& base = scene_->materials[static_cast<usize>(material)].base_color;
        return QColor::fromRgbF(std::clamp(base[0], 0.0f, 1.0f), std::clamp(base[1], 0.0f, 1.0f),
                                std::clamp(base[2], 0.0f, 1.0f), std::clamp(base[3], 0.15f, 1.0f));
    }

    void DrawMeshes(QPainter& painter, const GltfAnimationPose& pose) {
        if (!scene_) return;
        constexpr usize kTriangleBudget = 42000;
        std::vector<Triangle> triangles;
        triangles.reserve(12000);
        usize source_triangle_count = 0;
        for (const GltfNodeInstance& instance : pose.node_instances) {
            if (instance.mesh_index < scene_->meshes.size())
                for (const auto& primitive : scene_->meshes[instance.mesh_index].primitives)
                    source_triangle_count += primitive.indices.size() / 3;
        }
        const usize triangle_stride = std::max<usize>(1, source_triangle_count / kTriangleBudget);
        usize triangle_number = 0;
        std::vector<aether::Mat4> skin_matrices;
        for (const GltfNodeInstance& instance : pose.node_instances) {
            if (instance.mesh_index >= scene_->meshes.size()) continue;
            const GltfSkin* skin = instance.skin_index >= 0 &&
                static_cast<usize>(instance.skin_index) < scene_->skins.size()
                ? &scene_->skins[static_cast<usize>(instance.skin_index)] : nullptr;
            skin_matrices.clear();
            if (skin) aether::assets::ComputeSkinMatrices(pose, *skin, skin_matrices,
                static_cast<int>(instance.node_index));
            for (const auto& primitive : scene_->meshes[instance.mesh_index].primitives) {
                const QColor color = MaterialColor(primitive.material_index);
                const usize count = primitive.indices.size() / 3;
                for (usize triangle = 0; triangle < count; ++triangle, ++triangle_number) {
                    if (triangle_number % triangle_stride != 0 || triangles.size() >= kTriangleBudget) continue;
                    Triangle draw;
                    draw.color = color;
                    draw.points.reserve(3);
                    bool visible = true;
                    float depth_sum = 0.0f;
                    for (usize corner = 0; corner < 3; ++corner) {
                        const usize index = primitive.indices[triangle * 3 + corner];
                        if (index >= primitive.vertices.size()) { visible = false; break; }
                        const GltfVertex& vertex = primitive.vertices[index];
                        Vec4 local(vertex.position[0], vertex.position[1], vertex.position[2], 1.0f);
                        Vec4 deformed{};
                        float total_weight = 0.0f;
                        if (skin && !skin_matrices.empty() && index < primitive.joint_indices.size() &&
                            index < primitive.joint_weights.size()) {
                            for (usize influence = 0; influence < 4; ++influence) {
                                const float weight = primitive.joint_weights[index][influence];
                                const usize joint = primitive.joint_indices[index][influence];
                                if (weight <= 0.0f || joint >= skin_matrices.size()) continue;
                                const Vec4 transformed = skin_matrices[joint] * local;
                                deformed = deformed + transformed * weight;
                                total_weight += weight;
                            }
                        }
                        if (total_weight > 0.0001f) local = deformed * (1.0f / total_weight);
                        const Vec4 world4 = instance.world_transform * local;
                        QPointF projected;
                        float depth = 0.0f;
                        if (!Project({world4.x, world4.y, world4.z}, projected, depth)) {
                            visible = false;
                            break;
                        }
                        draw.points << projected;
                        depth_sum += depth;
                    }
                    if (visible) {
                        draw.depth = depth_sum / 3.0f;
                        triangles.push_back(std::move(draw));
                    }
                }
            }
        }
        std::sort(triangles.begin(), triangles.end(), [](const Triangle& a, const Triangle& b) {
            return a.depth > b.depth;
        });
        painter.setPen(Qt::NoPen);
        for (const Triangle& triangle : triangles) {
            painter.setBrush(triangle.color);
            painter.drawPolygon(triangle.points);
        }
    }

    int ParentJoint(int node, const std::vector<int>& parents, const std::vector<bool>& joints) const {
        if (node < 0 || static_cast<usize>(node) >= parents.size()) return -1;
        int parent = parents[static_cast<usize>(node)];
        while (parent >= 0) {
            if (joints[static_cast<usize>(parent)]) return parent;
            parent = parents[static_cast<usize>(parent)];
        }
        return -1;
    }

    bool NodePosition(const GltfAnimationPose& pose, int node, Vec3& out) const {
        if (node < 0 || static_cast<usize>(node) >= pose.node_world_transforms.size()) return false;
        out = ReadPosition(pose.node_world_transforms[static_cast<usize>(node)]);
        return true;
    }

    void DrawRig(QPainter& painter, const GltfAnimationPose& pose) {
        if (!scene_ || scene_->nodes.empty()) return;
        std::vector<int> parents(scene_->nodes.size(), -1);
        for (usize parent = 0; parent < scene_->nodes.size(); ++parent)
            for (usize child : scene_->nodes[parent].children)
                if (child < parents.size()) parents[child] = static_cast<int>(parent);
        std::vector<bool> joints(scene_->nodes.size(), scene_->skins.empty());
        if (!scene_->skins.empty()) {
            for (usize joint : scene_->skins.front().joints)
                if (joint < joints.size()) joints[joint] = true;
        }
        for (usize node = 0; node < joints.size(); ++node) {
            if (!joints[node]) continue;
            const int parent = ParentJoint(static_cast<int>(node), parents, joints);
            if (parent < 0) continue;
            Vec3 from, to;
            QPointF a, b;
            float da = 0.0f, db = 0.0f;
            if (!NodePosition(pose, parent, from) || !NodePosition(pose, static_cast<int>(node), to) ||
                !Project(from, a, da) || !Project(to, b, db)) continue;
            painter.setPen(QPen(QColor(215, 229, 240, 155), 2));
            painter.drawLine(a, b);
        }
        for (usize node = 0; node < joints.size(); ++node) {
            if (!joints[node]) continue;
            Vec3 world;
            QPointF screen;
            float depth = 0.0f;
            if (!NodePosition(pose, static_cast<int>(node), world) || !Project(world, screen, depth)) continue;
            const bool selected = static_cast<int>(node) == selected_node_;
            painter.setPen(QPen(selected ? QColor("#fff0a4") : QColor("#1b272f"), selected ? 2.0 : 1.0));
            painter.setBrush(selected ? QColor("#ffc95e") : QColor("#74d7bd"));
            painter.drawEllipse(screen, selected ? 6.5 : 4.0, selected ? 6.5 : 4.0);
            if (selected) {
                painter.setPen(QColor("#f1f5f9"));
                painter.drawText(screen + QPointF(10, -7), QString::fromStdString(scene_->nodes[node].name));
            }
        }
    }

    qreal FindJoint(const QPointF& mouse, int& node_out) const {
        node_out = -1;
        if (!scene_) return std::numeric_limits<qreal>::max();
        GltfAnimationPose pose;
        aether::assets::EvaluateAnimationPose(*scene_, animation_ ? *animation_ : RestAnimation(), time_, pose);
        std::vector<bool> joints(scene_->nodes.size(), scene_->skins.empty());
        if (!scene_->skins.empty())
            for (usize joint : scene_->skins.front().joints)
                if (joint < joints.size()) joints[joint] = true;
        qreal closest = std::numeric_limits<qreal>::max();
        for (usize node = 0; node < joints.size(); ++node) {
            if (!joints[node]) continue;
            Vec3 world;
            QPointF screen;
            float depth = 0.0f;
            if (!NodePosition(pose, static_cast<int>(node), world) || !Project(world, screen, depth)) continue;
            const qreal distance = std::hypot(screen.x() - mouse.x(), screen.y() - mouse.y());
            if (distance < closest) { closest = distance; node_out = static_cast<int>(node); }
        }
        return closest;
    }

    const GltfScene* scene_ = nullptr;
    const GltfAnimation* animation_ = nullptr;
    float time_ = 0.0f;
    int selected_node_ = -1;
    int view_ = -1;
    float yaw_ = 0.55f;
    float pitch_ = 0.28f;
    float camera_distance_ = 4.0f;
    Vec3 target_{0.0f, 1.0f, 0.0f};
    bool orbiting_ = false;
    QPointF drag_start_;
    float yaw_start_ = 0.0f;
    float pitch_start_ = 0.0f;
};

AnimationEditorWidget::AnimationEditorWidget(std::function<void(const QString&)> status, QWidget* parent)
    : QWidget(parent), status_(std::move(status)) {
    BuildUi();
}

void AnimationEditorWidget::BuildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto* toolbar = new QHBoxLayout;
    auto* title = new QLabel("ANIMATION WORKBENCH", this);
    title->setObjectName("section");
    toolbar->addWidget(title);
    auto* open_model = new QPushButton("Open Character…", this);
    open_model->setToolTip("Load a glTF model with a rig and imported animation clips");
    auto* open_clip = new QPushButton("Open Clip…", this);
    auto* new_clip = new QPushButton("+ New Clip", this);
    auto* save_clip = new QPushButton("Save Clip", this);
    toolbar->addStretch();
    toolbar->addWidget(open_model);
    toolbar->addWidget(open_clip);
    toolbar->addWidget(new_clip);
    toolbar->addWidget(save_clip);
    root->addLayout(toolbar);

    auto* clip_row = new QHBoxLayout;
    source_label_ = new QLabel("No character loaded", this);
    source_label_->setObjectName("muted");
    source_label_->setMinimumWidth(160);
    source_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    animation_combo_ = new QComboBox(this);
    animation_combo_->setMinimumWidth(150);
    animation_combo_->setToolTip("Choose an imported clip or a clip you are authoring");
    animation_name_ = new QLineEdit(this);
    animation_name_->setPlaceholderText("Clip name");
    animation_name_->setMaximumWidth(220);
    duration_spin_ = MakeFloatBox(this, 0.05, 3600.0, 3);
    duration_spin_->setSuffix(" s");
    duration_spin_->setValue(1.0);
    duration_spin_->setToolTip("Clip duration in seconds");
    clip_row->addWidget(source_label_, 2);
    clip_row->addWidget(new QLabel("CLIP", this));
    clip_row->addWidget(animation_combo_, 1);
    clip_row->addWidget(animation_name_, 1);
    clip_row->addWidget(new QLabel("DURATION", this));
    clip_row->addWidget(duration_spin_);
    root->addLayout(clip_row);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    auto* left = new QWidget(splitter);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    auto* rig_title = new QLabel("RIG HIERARCHY", left);
    rig_title->setObjectName("section");
    left_layout->addWidget(rig_title);
    rig_tree_ = new QTreeWidget(left);
    rig_tree_->setHeaderHidden(true);
    rig_tree_->setAccessibleName("Animation rig hierarchy");
    rig_tree_->setMinimumWidth(185);
    left_layout->addWidget(rig_tree_, 1);

    auto* center = new QWidget(splitter);
    auto* center_layout = new QVBoxLayout(center);
    center_layout->setContentsMargins(0, 0, 0, 0);
    auto* preview_row = new QHBoxLayout;
    auto* preview_title = new QLabel("PREVIEW", center);
    preview_title->setObjectName("section");
    preview_row->addWidget(preview_title);
    preview_row->addStretch();
    view_combo_ = new QComboBox(center);
    view_combo_->addItems({"Orbit", "Front", "Side", "Top"});
    view_combo_->setToolTip("Choose a preview camera; right-drag the preview to orbit");
    preview_row->addWidget(view_combo_);
    center_layout->addLayout(preview_row);
    preview_ = new PreviewWidget(center);
    center_layout->addWidget(preview_, 4);

    auto* transport = new QHBoxLayout;
    play_button_ = new QPushButton("▶ Play", center);
    play_button_->setToolTip("Play or pause the animation preview");
    loop_button_ = new QPushButton("Loop", center);
    loop_button_->setCheckable(true);
    loop_button_->setChecked(true);
    loop_button_->setToolTip("Loop playback when the playhead reaches the end");
    snap_button_ = new QPushButton("Snap", center);
    snap_button_->setCheckable(true);
    snap_button_->setChecked(true);
    snap_button_->setToolTip("Snap scrubbing and key times to the frame rate");
    fps_spin_ = MakeFloatBox(center, 1.0, 240.0, 1);
    fps_spin_->setValue(30.0);
    fps_spin_->setSuffix(" fps");
    time_spin_ = MakeFloatBox(center, 0.0, 1.0, 3);
    time_spin_->setSuffix(" s");
    time_spin_->setToolTip("Playhead position in seconds");
    transport->addWidget(play_button_);
    transport->addWidget(loop_button_);
    transport->addWidget(snap_button_);
    transport->addWidget(new QLabel("RATE", center));
    transport->addWidget(fps_spin_);
    transport->addStretch();
    transport->addWidget(new QLabel("PLAYHEAD", center));
    transport->addWidget(time_spin_);
    center_layout->addLayout(transport);

    timeline_ = new TimelineWidget(center);
    center_layout->addWidget(timeline_, 2);

    auto* right = new QWidget(splitter);
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);
    selected_node_label_ = new QLabel("Select a bone", right);
    selected_node_label_->setObjectName("section");
    selected_node_label_->setWordWrap(true);
    right_layout->addWidget(selected_node_label_);
    auto* transform_form = new QFormLayout;
    transform_form->setLabelAlignment(Qt::AlignLeft);
    transform_form->setSpacing(7);
    const std::array<QStringList, 3> axis_labels{QStringList{"X", "Y", "Z"},
                                                 QStringList{"X°", "Y°", "Z°"},
                                                 QStringList{"X", "Y", "Z"}};
    const std::array<QString, 3> row_labels{"Translation", "Rotation", "Scale"};
    for (int group = 0; group < 3; ++group) {
        std::array<QDoubleSpinBox*, 3> row_fields{};
        for (int axis = 0; axis < 3; ++axis) {
            const int index = group * 3 + axis;
            const bool rotation = group == 1;
            transform_fields_[index] = MakeFloatBox(right, rotation ? -36000.0 : -100000.0,
                                                     rotation ? 36000.0 : 100000.0, rotation ? 1 : 4);
            transform_fields_[index]->setEnabled(false);
            transform_fields_[index]->setAccessibleName(QString("%1 %2")
                .arg(row_labels[static_cast<usize>(group)], axis_labels[static_cast<usize>(group)][axis]));
            row_fields[static_cast<usize>(axis)] = transform_fields_[index];
        }
        transform_form->addRow(row_labels[static_cast<usize>(group)],
                               MakeTripleRow(right, row_fields, axis_labels[static_cast<usize>(group)]));
    }
    right_layout->addLayout(transform_form);
    interpolation_combo_ = new QComboBox(right);
    interpolation_combo_->addItems({"Linear", "Step"});
    interpolation_combo_->setToolTip("Choose how this bone channel interpolates between keys");
    right_layout->addWidget(new QLabel("INTERPOLATION", right));
    right_layout->addWidget(interpolation_combo_);
    add_key_button_ = new QPushButton("◆  Add / Update Key", right);
    add_key_button_->setToolTip("Record this bone's transform at the playhead");
    delete_key_button_ = new QPushButton("Delete Selected Key", right);
    undo_button_ = new QPushButton("↶  Undo", right);
    redo_button_ = new QPushButton("↷  Redo", right);
    add_key_button_->setEnabled(false);
    delete_key_button_->setEnabled(false);
    undo_button_->setEnabled(false);
    redo_button_->setEnabled(false);
    right_layout->addWidget(add_key_button_);
    right_layout->addWidget(delete_key_button_);
    auto* history_row = new QHBoxLayout;
    history_row->addWidget(undo_button_);
    history_row->addWidget(redo_button_);
    right_layout->addLayout(history_row);
    right_layout->addStretch(1);
    clip_status_ = new QLabel("Open a character model or a saved Aether clip.", right);
    clip_status_->setWordWrap(true);
    clip_status_->setObjectName("muted");
    right_layout->addWidget(clip_status_);

    splitter->addWidget(left);
    splitter->addWidget(center);
    splitter->addWidget(right);
    splitter->setSizes({220, 720, 310});
    root->addWidget(splitter, 1);

    timer_ = new QTimer(this);
    timer_->setInterval(33);
    QObject::connect(timer_, &QTimer::timeout, this, [this] {
        const GltfAnimation* clip = ActiveAnimation();
        if (!playing_ || !clip) return;
        float next = playhead_ + static_cast<float>(timer_->interval()) / 1000.0f;
        if (next >= clip->duration) {
            if (looping_ && clip->duration > 0.0f) next = std::fmod(next, clip->duration);
            else { next = clip->duration; playing_ = false; play_button_->setText("▶ Play"); }
        }
        SetPlayhead(next, false);
    });

    QObject::connect(open_model, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "Open Character Model", QString(),
                                                          "glTF Models (*.gltf)");
        if (!path.isEmpty()) OpenModel(std::filesystem::path(path.toStdWString()));
    });
    QObject::connect(open_clip, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "Open Aether Animation Clip", QString(),
                                                          "Aether Animation Clips (*.aetheranim)");
        if (!path.isEmpty()) OpenClipFile(std::filesystem::path(path.toStdWString()));
    });
    QObject::connect(new_clip, &QPushButton::clicked, this, [this] { CreateAnimation(); });
    QObject::connect(save_clip, &QPushButton::clicked, this, [this] { SaveClip(); });
    QObject::connect(animation_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this](int index) { SelectAnimation(index); });
    QObject::connect(animation_name_, &QLineEdit::editingFinished, this, [this] { SetClipName(); });
    QObject::connect(duration_spin_, &QDoubleSpinBox::editingFinished, this, [this] { SetClipDuration(); });
    QObject::connect(view_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this](int view) { preview_->SetFrame(&scene_, ActiveAnimation(), playhead_, selected_node_, view); });
    QObject::connect(rig_tree_, &QTreeWidget::currentItemChanged, this,
                     [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        SelectNode(current ? current->data(0, Qt::UserRole).toInt() : -1);
    });
    preview_->select_node = [this](int node) {
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> find = [&](QTreeWidgetItem* item) -> QTreeWidgetItem* {
            if (item->data(0, Qt::UserRole).toInt() == node) return item;
            for (int i = 0; i < item->childCount(); ++i)
                if (QTreeWidgetItem* child = find(item->child(i))) return child;
            return nullptr;
        };
        for (int i = 0; i < rig_tree_->topLevelItemCount(); ++i)
            if (QTreeWidgetItem* item = find(rig_tree_->topLevelItem(i))) {
                rig_tree_->setCurrentItem(item);
                return;
            }
    };
    timeline_->scrub = [this](float time) { ScrubTo(time); };
    timeline_->select_path = [this](int path) {
        selected_path_ = path;
        selected_key_ = -1;
        RefreshControls();
        RefreshTimeline();
    };
    timeline_->select_key = [this](int path, int key) {
        selected_path_ = path;
        selected_key_ = key;
        const int channel_index = FindChannel(selected_node_, PathFromRow(path));
        const auto* clip = ActiveAnimation();
        if (clip && channel_index >= 0) {
            const auto& channel = clip->channels[static_cast<usize>(channel_index)];
            if (key >= 0 && static_cast<usize>(key) < channel.times.size()) SetPlayhead(channel.times[static_cast<usize>(key)]);
        }
        RefreshControls();
        RefreshTimeline();
    };
    timeline_->begin_key_drag = [this] {
        if (!recording_key_drag_) { RecordUndo(); recording_key_drag_ = true; }
    };
    timeline_->move_key = [this](int path, int key, float time) { MoveSelectedKey(path, key, time); };
    timeline_->end_key_drag = [this] { recording_key_drag_ = false; };
    QObject::connect(play_button_, &QPushButton::clicked, this, [this] {
        playing_ = !playing_;
        play_button_->setText(playing_ ? "Ⅱ Pause" : "▶ Play");
    });
    QObject::connect(loop_button_, &QPushButton::toggled, this, [this](bool enabled) { looping_ = enabled; });
    QObject::connect(snap_button_, &QPushButton::toggled, this, [this](bool enabled) { snapping_ = enabled; RefreshTimeline(); });
    QObject::connect(fps_spin_, &QDoubleSpinBox::editingFinished, this, [this] { RefreshTimeline(); });
    QObject::connect(time_spin_, &QDoubleSpinBox::editingFinished, this,
                     [this] { SetPlayhead(static_cast<float>(time_spin_->value())); });
    QObject::connect(add_key_button_, &QPushButton::clicked, this, [this] { AddOrUpdateKey(); });
    QObject::connect(delete_key_button_, &QPushButton::clicked, this, [this] { DeleteSelectedKey(); });
    QObject::connect(interpolation_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this](int interpolation) { SetInterpolation(interpolation); });
    QObject::connect(undo_button_, &QPushButton::clicked, this, [this] { Undo(); });
    QObject::connect(redo_button_, &QPushButton::clicked, this, [this] { Redo(); });
    timer_->start();
}

bool AnimationEditorWidget::ConfirmDiscard(QWidget* parent) const {
    if (!dirty_) return true;
    QMessageBox prompt(QMessageBox::Warning, "Unsaved Animation Clip",
                       "Save changes to the animation clip before continuing?",
                       QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                       parent ? parent : const_cast<AnimationEditorWidget*>(this));
    const auto choice = static_cast<QMessageBox::StandardButton>(prompt.exec());
    if (choice == QMessageBox::Discard) return true;
    if (choice == QMessageBox::Cancel) return false;
    return const_cast<AnimationEditorWidget*>(this)->SaveClip();
}

bool AnimationEditorWidget::ConfirmDiscardChanges(QWidget* parent) {
    return ConfirmDiscard(parent);
}

void AnimationEditorWidget::EmitStatus(const QString& message) const {
    if (status_) status_(message);
}

bool AnimationEditorWidget::OpenModel(const std::filesystem::path& path, bool confirm) {
    if (confirm && !ConfirmDiscard()) return false;
    GltfScene candidate;
    if (!aether::assets::LoadGltf(path.string(), candidate)) {
        QMessageBox::critical(this, "Couldn't open character", "The glTF model could not be loaded.");
        return false;
    }
    scene_ = std::move(candidate);
    source_model_path_ = path;
    clip_file_path_.clear();
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    animation_index_ = scene_.animations.empty() ? -1 : 0;
    selected_node_ = -1;
    selected_key_ = -1;
    playhead_ = 0.0f;
    source_label_->setText(QString::fromStdWString(path.filename().wstring()));
    RebuildRigTree();
    RebuildAnimationList(animation_index_);
    preview_->SetFrame(&scene_, ActiveAnimation(), playhead_, selected_node_, view_combo_->currentIndex());
    if (scene_.animations.empty())
        clip_status_->setText("Rig loaded. Create an animation clip, then select a bone and add keys.");
    else
        clip_status_->setText(QString("Rig loaded · %1 imported clip(s)").arg(scene_.animations.size()));
    EmitStatus(QString("Animation editor opened %1").arg(QString::fromStdWString(path.filename().wstring())));
    return true;
}

void AnimationEditorWidget::SelectAnimation(int index) {
    if (index < 0 || static_cast<usize>(index) >= scene_.animations.size()) {
        animation_index_ = -1;
        RefreshPlayhead();
        return;
    }
    if (index == animation_index_) return;
    if (dirty_ && !ConfirmDiscard()) {
        const QSignalBlocker blocker(animation_combo_);
        animation_combo_->setCurrentIndex(animation_index_);
        return;
    }
    animation_index_ = index;
    clip_file_path_.clear();
    dirty_ = false;
    undo_.clear();
    redo_.clear();
    selected_key_ = -1;
    SetPlayhead(0.0f, false);
    RefreshPlayhead();
}

void AnimationEditorWidget::CreateAnimation() {
    if (scene_.nodes.empty()) {
        QMessageBox::information(this, "Load a Character First", "Open a glTF model to use its rig before creating an animation.");
        return;
    }
    if (!ConfirmDiscard()) return;
    GltfAnimation animation;
    animation.name = "New Animation";
    animation.duration = 1.0f;
    scene_.animations.push_back(std::move(animation));
    animation_index_ = static_cast<int>(scene_.animations.size()) - 1;
    clip_file_path_.clear();
    dirty_ = true;
    undo_.clear();
    redo_.clear();
    selected_key_ = -1;
    playhead_ = 0.0f;
    RebuildAnimationList(animation_index_);
    RefreshPlayhead();
    EmitStatus("Created a new animation clip");
}

void AnimationEditorWidget::RebuildAnimationList(int selected) {
    const QSignalBlocker blocker(animation_combo_);
    animation_combo_->clear();
    for (usize i = 0; i < scene_.animations.size(); ++i) {
        const auto& animation = scene_.animations[i];
        const QString name = animation.name.empty()
            ? QString("Animation %1").arg(i + 1) : QString::fromStdString(animation.name);
        animation_combo_->addItem(name, static_cast<int>(i));
    }
    if (selected >= 0 && selected < animation_combo_->count()) animation_combo_->setCurrentIndex(selected);
    animation_index_ = selected >= 0 && selected < static_cast<int>(scene_.animations.size()) ? selected : -1;
    RefreshPlayhead();
}

void AnimationEditorWidget::RebuildRigTree() {
    const QSignalBlocker blocker(rig_tree_);
    rig_tree_->clear();
    if (scene_.nodes.empty()) return;
    std::vector<bool> is_joint(scene_.nodes.size(), scene_.skins.empty());
    if (!scene_.skins.empty())
        for (usize joint : scene_.skins.front().joints)
            if (joint < is_joint.size()) is_joint[joint] = true;
    std::vector<bool> inserted(scene_.nodes.size(), false);
    std::function<void(QTreeWidgetItem*, usize)> add_children = [&](QTreeWidgetItem* parent, usize node) {
        if (node >= scene_.nodes.size() || inserted[node]) return;
        inserted[node] = true;
        const auto& source = scene_.nodes[node];
        QString name = source.name.empty() ? QString("Node %1").arg(node) : QString::fromStdString(source.name);
        if (is_joint[node]) name += "  ·  JOINT";
        auto* item = parent ? new QTreeWidgetItem(parent, {name}) : new QTreeWidgetItem(rig_tree_, {name});
        item->setData(0, Qt::UserRole, static_cast<int>(node));
        for (usize child : source.children) add_children(item, child);
    };
    for (usize root : scene_.root_nodes) add_children(nullptr, root);
    for (usize node = 0; node < scene_.nodes.size(); ++node)
        if (!inserted[node]) add_children(nullptr, node);
    rig_tree_->expandToDepth(2);
}

void AnimationEditorWidget::SelectNode(int node) {
    selected_node_ = node >= 0 && static_cast<usize>(node) < scene_.nodes.size() ? node : -1;
    selected_path_ = std::clamp(selected_path_, 0, 2);
    selected_key_ = -1;
    RefreshControls();
    RefreshTimeline();
    RefreshPreview();
}

void AnimationEditorWidget::RefreshPlayhead(bool update_controls) {
    const GltfAnimation* clip = ActiveAnimation();
    const float duration = clip ? std::max(clip->duration, 0.05f) : 1.0f;
    playhead_ = std::clamp(playhead_, 0.0f, duration);
    {
        const QSignalBlocker blocker(time_spin_);
        time_spin_->setRange(0.0, duration);
        time_spin_->setValue(playhead_);
    }
    {
        const QSignalBlocker blocker(duration_spin_);
        duration_spin_->setValue(clip ? clip->duration : 1.0);
    }
    {
        const QSignalBlocker blocker(animation_name_);
        animation_name_->setText(clip ? QString::fromStdString(clip->name) : QString());
    }
    play_button_->setEnabled(clip != nullptr);
    loop_button_->setEnabled(clip != nullptr);
    add_key_button_->setEnabled(clip != nullptr && selected_node_ >= 0);
    if (update_controls) RefreshControls();
    RefreshTimeline();
    RefreshPreview();
}

void AnimationEditorWidget::RefreshTimeline() {
    timeline_->SetData(ActiveAnimation(), selected_node_, selected_path_, selected_key_, playhead_,
                       static_cast<float>(fps_spin_->value()), snapping_);
    delete_key_button_->setEnabled(ActiveAnimation() && selected_node_ >= 0 && selected_key_ >= 0);
    undo_button_->setEnabled(!undo_.empty());
    redo_button_->setEnabled(!redo_.empty());
}

void AnimationEditorWidget::RefreshPreview() {
    preview_->SetFrame(&scene_, ActiveAnimation(), playhead_, selected_node_, view_combo_->currentIndex());
}

void AnimationEditorWidget::RefreshControls() {
    const GltfAnimation* clip = ActiveAnimation();
    const bool valid = clip && selected_node_ >= 0 && static_cast<usize>(selected_node_) < scene_.nodes.size();
    selected_node_label_->setText(valid ? QString::fromStdString(NodeName(scene_, selected_node_)) : "Select a bone");
    if (!valid) {
        for (QDoubleSpinBox* field : transform_fields_) field->setEnabled(false);
        add_key_button_->setEnabled(false);
        interpolation_combo_->setEnabled(false);
        return;
    }
    aether::assets::GltfAnimationPose pose;
    aether::assets::EvaluateAnimationPose(scene_, *clip, playhead_, pose);
    const auto& node = pose.animated_nodes[static_cast<usize>(selected_node_)];
    const std::array<double, 9> values{
        node.translation.x, node.translation.y, node.translation.z,
        0.0, 0.0, 0.0,
        node.scale.x, node.scale.y, node.scale.z};
    const auto& q = node.rotation;
    const float sin_roll = 2.0f * (q.w * q.x + q.y * q.z);
    const float cos_roll = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const float roll = std::atan2(sin_roll, cos_roll);
    const float sin_pitch = std::clamp(2.0f * (q.w * q.y - q.z * q.x), -1.0f, 1.0f);
    const float pitch = std::asin(sin_pitch);
    const float sin_yaw = 2.0f * (q.w * q.z + q.x * q.y);
    const float cos_yaw = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    const double degrees = 180.0 / static_cast<double>(kPi);
    const std::array<double, 3> euler{roll * degrees, pitch * degrees, std::atan2(sin_yaw, cos_yaw) * degrees};
    std::array<double, 9> displayed = values;
    for (int i = 0; i < 3; ++i) displayed[static_cast<usize>(3 + i)] = euler[static_cast<usize>(i)];
    for (int i = 0; i < 9; ++i) {
        const QSignalBlocker blocker(transform_fields_[i]);
        transform_fields_[i]->setEnabled(true);
        transform_fields_[i]->setValue(displayed[static_cast<usize>(i)]);
    }
    const int channel_index = FindChannel(selected_node_, PathFromRow(selected_path_));
    interpolation_combo_->setEnabled(channel_index >= 0);
    add_key_button_->setEnabled(true);
    if (channel_index >= 0) {
        const auto& channel = clip->channels[static_cast<usize>(channel_index)];
        const QSignalBlocker blocker(interpolation_combo_);
        interpolation_combo_->setCurrentIndex(channel.interpolation == GltfAnimationInterpolation::Step ? 1 : 0);
    } else {
        const QSignalBlocker blocker(interpolation_combo_);
        interpolation_combo_->setCurrentIndex(0);
    }
}

void AnimationEditorWidget::SetPlayhead(float time, bool snap_to_frame) {
    const GltfAnimation* clip = ActiveAnimation();
    const float duration = clip ? std::max(clip->duration, 0.05f) : 1.0f;
    if (snap_to_frame || snapping_) time = std::round(time * static_cast<float>(fps_spin_->value())) /
                                               static_cast<float>(fps_spin_->value());
    playhead_ = std::clamp(time, 0.0f, duration);
    RefreshPlayhead();
}

void AnimationEditorWidget::ScrubTo(float time) { SetPlayhead(time); }

void AnimationEditorWidget::AddOrUpdateKey() {
    GltfAnimation* clip = ActiveAnimation();
    if (!clip || selected_node_ < 0) return;
    const GltfAnimationPath path = PathFromRow(selected_path_);
    const int channel_index = FindChannel(selected_node_, path);
    float time = playhead_;
    const float fps = static_cast<float>(fps_spin_->value());
    if (snapping_) time = std::round(time * fps) / fps;
    const usize components = path == GltfAnimationPath::Rotation ? 4 : 3;
    std::vector<float> value;
    if (path == GltfAnimationPath::Rotation) {
        constexpr float to_radians = kPi / 180.0f;
        const auto x = aether::Quaternion::FromAxisAngle({1, 0, 0}, static_cast<float>(transform_fields_[3]->value()) * to_radians);
        const auto y = aether::Quaternion::FromAxisAngle({0, 1, 0}, static_cast<float>(transform_fields_[4]->value()) * to_radians);
        const auto z = aether::Quaternion::FromAxisAngle({0, 0, 1}, static_cast<float>(transform_fields_[5]->value()) * to_radians);
        const auto q = (z * y * x).Normalized();
        value = {q.x, q.y, q.z, q.w};
    } else {
        const int offset = path == GltfAnimationPath::Scale ? 6 : 0;
        value = {static_cast<float>(transform_fields_[offset]->value()),
                 static_cast<float>(transform_fields_[offset + 1]->value()),
                 static_cast<float>(transform_fields_[offset + 2]->value())};
    }
    if (value.size() != components) return;
    RecordUndo();
    int key = -1;
    if (channel_index >= 0) {
        auto& channel = clip->channels[static_cast<usize>(channel_index)];
        for (usize i = 0; i < channel.times.size(); ++i)
            if (std::abs(channel.times[i] - time) <= 0.5f / fps) { key = static_cast<int>(i); break; }
        if (key >= 0) {
            channel.times[static_cast<usize>(key)] = time;
            for (usize component = 0; component < components; ++component)
                channel.values[static_cast<usize>(key) * components + component] = value[component];
        } else {
            const auto it = std::lower_bound(channel.times.begin(), channel.times.end(), time);
            key = static_cast<int>(std::distance(channel.times.begin(), it));
            channel.times.insert(it, time);
            channel.values.insert(channel.values.begin() + static_cast<std::ptrdiff_t>(key * components),
                                  value.begin(), value.end());
        }
    } else {
        GltfAnimationChannel channel;
        channel.node_index = static_cast<usize>(selected_node_);
        channel.path = path;
        channel.times.push_back(time);
        channel.values = std::move(value);
        clip->channels.push_back(std::move(channel));
        key = 0;
    }
    clip->duration = std::max(clip->duration, time);
    selected_key_ = key;
    dirty_ = true;
    SetPlayhead(time, false);
    clip_status_->setText("Clip has unsaved changes · add/update key stored at the playhead.");
    RefreshPlayhead();
}

void AnimationEditorWidget::DeleteSelectedKey() {
    GltfAnimation* clip = ActiveAnimation();
    const int channel_index = FindChannel(selected_node_, PathFromRow(selected_path_));
    if (!clip || channel_index < 0) return;
    auto& channel = clip->channels[static_cast<usize>(channel_index)];
    if (selected_key_ < 0 || static_cast<usize>(selected_key_) >= channel.times.size()) return;
    RecordUndo();
    const usize key = static_cast<usize>(selected_key_);
    const usize components = channel.ComponentsPerKey();
    channel.times.erase(channel.times.begin() + static_cast<std::ptrdiff_t>(key));
    channel.values.erase(channel.values.begin() + static_cast<std::ptrdiff_t>(key * components),
                         channel.values.begin() + static_cast<std::ptrdiff_t>((key + 1) * components));
    if (channel.times.empty()) clip->channels.erase(clip->channels.begin() + channel_index);
    selected_key_ = -1;
    dirty_ = true;
    clip_status_->setText("Key deleted · clip has unsaved changes.");
    RefreshPlayhead();
}

void AnimationEditorWidget::MoveSelectedKey(int path, int key, float time) {
    GltfAnimation* clip = ActiveAnimation();
    const int channel_index = FindChannel(selected_node_, PathFromRow(path));
    if (!clip || channel_index < 0) return;
    auto& channel = clip->channels[static_cast<usize>(channel_index)];
    if (key < 0 || static_cast<usize>(key) >= channel.times.size()) return;
    float minimum = 0.0f;
    float maximum = clip->duration;
    if (key > 0) minimum = channel.times[static_cast<usize>(key - 1)] + kEpsilon;
    if (static_cast<usize>(key + 1) < channel.times.size())
        maximum = channel.times[static_cast<usize>(key + 1)] - kEpsilon;
    const float moved = std::clamp(time, minimum, std::max(minimum, maximum));
    channel.times[static_cast<usize>(key)] = moved;
    dirty_ = true;
    selected_key_ = key;
    playhead_ = moved;
    RefreshPlayhead();
}

void AnimationEditorWidget::SetInterpolation(int interpolation) {
    GltfAnimation* clip = ActiveAnimation();
    const int channel = FindChannel(selected_node_, PathFromRow(selected_path_));
    if (!clip || channel < 0) return;
    const auto desired = interpolation == 1 ? GltfAnimationInterpolation::Step : GltfAnimationInterpolation::Linear;
    auto& value = clip->channels[static_cast<usize>(channel)].interpolation;
    if (value == desired) return;
    RecordUndo();
    value = desired;
    dirty_ = true;
    RefreshTimeline();
}

void AnimationEditorWidget::SetClipName() {
    GltfAnimation* clip = ActiveAnimation();
    if (!clip) return;
    const std::string name = animation_name_->text().trimmed().toStdString();
    if (name.empty() || name == clip->name) return;
    RecordUndo();
    clip->name = name;
    dirty_ = true;
    RebuildAnimationList(animation_index_);
    clip_status_->setText("Clip renamed · unsaved changes.");
}

void AnimationEditorWidget::SetClipDuration() {
    GltfAnimation* clip = ActiveAnimation();
    if (!clip) return;
    const float duration = static_cast<float>(duration_spin_->value());
    if (std::abs(duration - clip->duration) < 0.0005f) return;
    float last_key = 0.0f;
    for (const auto& channel : clip->channels)
        if (!channel.times.empty()) last_key = std::max(last_key, channel.times.back());
    const float safe_duration = std::max(duration, last_key);
    if (safe_duration != duration) {
        const QSignalBlocker blocker(duration_spin_);
        duration_spin_->setValue(safe_duration);
    }
    RecordUndo();
    clip->duration = safe_duration;
    dirty_ = true;
    RefreshPlayhead();
}

void AnimationEditorWidget::RecordUndo() {
    const GltfAnimation* clip = ActiveAnimation();
    if (!clip) return;
    undo_.push_back(*clip);
    if (undo_.size() > 100) undo_.erase(undo_.begin());
    redo_.clear();
    RefreshTimeline();
}

bool AnimationEditorWidget::Undo() {
    GltfAnimation* clip = ActiveAnimation();
    if (!clip || undo_.empty()) return false;
    redo_.push_back(*clip);
    *clip = std::move(undo_.back());
    undo_.pop_back();
    dirty_ = true;
    selected_key_ = -1;
    RefreshPlayhead();
    return true;
}

bool AnimationEditorWidget::Redo() {
    GltfAnimation* clip = ActiveAnimation();
    if (!clip || redo_.empty()) return false;
    undo_.push_back(*clip);
    *clip = std::move(redo_.back());
    redo_.pop_back();
    dirty_ = true;
    selected_key_ = -1;
    RefreshPlayhead();
    return true;
}

int AnimationEditorWidget::FindChannel(int node, GltfAnimationPath path) const {
    const GltfAnimation* clip = ActiveAnimation();
    if (!clip || node < 0) return -1;
    for (usize i = 0; i < clip->channels.size(); ++i)
        if (clip->channels[i].node_index == static_cast<usize>(node) && clip->channels[i].path == path)
            return static_cast<int>(i);
    return -1;
}

GltfAnimation* AnimationEditorWidget::ActiveAnimation() {
    return animation_index_ >= 0 && static_cast<usize>(animation_index_) < scene_.animations.size()
        ? &scene_.animations[static_cast<usize>(animation_index_)] : nullptr;
}

const GltfAnimation* AnimationEditorWidget::ActiveAnimation() const {
    return animation_index_ >= 0 && static_cast<usize>(animation_index_) < scene_.animations.size()
        ? &scene_.animations[static_cast<usize>(animation_index_)] : nullptr;
}

bool AnimationEditorWidget::OpenClipFile(const std::filesystem::path& path, bool confirm) {
    if (confirm && !ConfirmDiscard()) return false;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("The clip file could not be read.");
        json document;
        input >> document;
        if (document.value("format", std::string()) != "AetherAnimationClip" ||
            document.value("version", 0) != 1) throw std::runtime_error("Unsupported Aether clip format.");
        const std::string source = document.value("source_model", std::string());
        if (source.empty()) throw std::runtime_error("This clip does not refer to a source character model.");
        std::filesystem::path model_path(source);
        if (model_path.is_relative()) model_path = path.parent_path() / model_path;
        if (!OpenModel(model_path, false)) return false;

        const json& encoded = document.at("animation");
        GltfAnimation clip;
        clip.name = encoded.value("name", std::string("Animation"));
        clip.duration = std::max(encoded.value("duration", 0.0f), 0.05f);
        for (const json& item : encoded.at("channels")) {
            GltfAnimationChannel channel;
            channel.node_index = item.value("node", static_cast<usize>(0));
            if (item.contains("node_name")) {
                const std::string node_name = item.value("node_name", std::string());
                if (channel.node_index >= scene_.nodes.size() ||
                    (!node_name.empty() && scene_.nodes[channel.node_index].name != node_name)) {
                    const auto found = std::find_if(scene_.nodes.begin(), scene_.nodes.end(),
                        [&](const auto& node) { return node.name == node_name; });
                    if (found == scene_.nodes.end()) continue;
                    channel.node_index = static_cast<usize>(std::distance(scene_.nodes.begin(), found));
                }
            }
            const std::string channel_path = item.value("path", std::string("translation"));
            channel.path = channel_path == "rotation" ? GltfAnimationPath::Rotation
                : channel_path == "scale" ? GltfAnimationPath::Scale : GltfAnimationPath::Translation;
            channel.interpolation = item.value("interpolation", std::string("LINEAR")) == "STEP"
                ? GltfAnimationInterpolation::Step : GltfAnimationInterpolation::Linear;
            channel.times = item.at("times").get<std::vector<float>>();
            channel.values = item.at("values").get<std::vector<float>>();
            const usize components = channel.path == GltfAnimationPath::Rotation ? 4 : 3;
            if (channel.values.size() != channel.times.size() * components ||
                !std::is_sorted(channel.times.begin(), channel.times.end())) continue;
            clip.duration = std::max(clip.duration, channel.times.empty() ? 0.0f : channel.times.back());
            clip.channels.push_back(std::move(channel));
        }
        const int source_index = document.value("source_animation", -1);
        if (source_index >= 0 && static_cast<usize>(source_index) < scene_.animations.size()) {
            scene_.animations[static_cast<usize>(source_index)] = std::move(clip);
            animation_index_ = source_index;
        } else {
            scene_.animations.push_back(std::move(clip));
            animation_index_ = static_cast<int>(scene_.animations.size()) - 1;
        }
        clip_file_path_ = path;
        dirty_ = false;
        undo_.clear();
        redo_.clear();
        RebuildAnimationList(animation_index_);
        RefreshPlayhead();
        clip_status_->setText("Aether animation clip loaded · edit keys and save to update this sidecar.");
        return true;
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Couldn't open animation clip", QString::fromUtf8(error.what()));
        return false;
    }
}

bool AnimationEditorWidget::SaveClip(bool save_as) {
    GltfAnimation* clip = ActiveAnimation();
    if (!clip || source_model_path_.empty()) {
        QMessageBox::information(this, "No Animation Clip", "Open a character model and select or create an animation clip first.");
        return false;
    }
    std::filesystem::path path = save_as ? std::filesystem::path{} : clip_file_path_;
    if (path.empty()) {
        const QString suggested = QString::fromStdWString(source_model_path_.parent_path().wstring()) + "/" +
            QString::fromStdString(clip->name.empty() ? "Animation" : clip->name) + ".aetheranim";
        const QString selected = QFileDialog::getSaveFileName(this, "Save Aether Animation Clip", suggested,
                                                              "Aether Animation Clips (*.aetheranim)");
        if (selected.isEmpty()) return false;
        path = std::filesystem::path(selected.toStdWString());
    }
    try {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) throw std::runtime_error(ec.message());
        std::filesystem::path relative_model = std::filesystem::relative(source_model_path_, path.parent_path(), ec);
        if (ec) { ec.clear(); relative_model = std::filesystem::absolute(source_model_path_, ec); }
        json channels = json::array();
        for (const auto& channel : clip->channels) {
            const std::string node_name = channel.node_index < scene_.nodes.size()
                ? scene_.nodes[channel.node_index].name : std::string();
            channels.push_back({
                {"node", channel.node_index}, {"node_name", node_name}, {"path", PathName(channel.path)},
                {"interpolation", channel.interpolation == GltfAnimationInterpolation::Step ? "STEP" : "LINEAR"},
                {"times", channel.times}, {"values", channel.values}});
        }
        json document = {
            {"format", "AetherAnimationClip"}, {"version", 1},
            {"source_model", relative_model.generic_string()},
            {"source_animation", animation_index_ < static_cast<int>(scene_.animations.size())
                ? animation_index_ : -1},
            {"animation", {{"name", clip->name}, {"duration", clip->duration}, {"channels", channels}}}
        };
        QSaveFile file(QString::fromStdWString(path.wstring()));
        if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error(file.errorString().toStdString());
        const QByteArray bytes = QByteArray::fromStdString(document.dump(2) + "\n");
        if (file.write(bytes) != bytes.size() || !file.commit())
            throw std::runtime_error(file.errorString().toStdString());
        clip_file_path_ = path;
        dirty_ = false;
        clip_status_->setText(QString("Saved %1 · %2 channels · %3 keys")
            .arg(QString::fromStdWString(path.filename().wstring()))
            .arg(clip->channels.size()).arg(std::accumulate(clip->channels.begin(), clip->channels.end(), usize{0},
                [](usize count, const GltfAnimationChannel& channel) { return count + channel.times.size(); })));
        EmitStatus(QString("Animation clip saved: %1").arg(QString::fromStdWString(path.filename().wstring())));
        return true;
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Couldn't save animation clip", QString::fromUtf8(error.what()));
        return false;
    }
}

} // namespace aether::editor::qt
