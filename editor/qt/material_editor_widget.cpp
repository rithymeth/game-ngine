#include "material_editor_widget.h"

#include "aether/renderer/material.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QRegularExpression>
#include <QShortcut>
#include <QSplitter>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace aether::editor::qt {
namespace {
constexpr int kMaterialNodeRole = Qt::UserRole + 40;
constexpr int kMaterialPinRole = Qt::UserRole + 41;
constexpr int kMaterialPinOutputRole = Qt::UserRole + 42;
constexpr int kMaterialPinNameRole = Qt::UserRole + 43;

QColor TypeColor(aether::mat::PinType type) {
    using aether::mat::PinType;
    switch (type) {
    case PinType::Float: return QColor("#9adb47");
    case PinType::Float2: return QColor("#4fc3e8");
    case PinType::Float3: return QColor("#f5c542");
    case PinType::Float4: return QColor("#e06cd8");
    case PinType::Texture: return QColor("#e85a3c");
    default: return QColor("#a0a0a0");
    }
}

QColor HeaderColor(const std::string& category) {
    if (category == "Output") return QColor("#70562b");
    if (category == "Parameters" || category == "Constants") return QColor("#315b3f");
    if (category == "Texture") return QColor("#703d33");
    if (category == "Coordinates" || category == "Vectors") return QColor("#304f66");
    if (category == "Functions") return QColor("#4e3e65");
    if (category == "Custom") return QColor("#693a54");
    return QColor("#315b86");
}

class MaterialScene final : public QGraphicsScene {
public:
    explicit MaterialScene(QObject* parent = nullptr) : QGraphicsScene(parent) {}
    std::function<void(QGraphicsItem*, QGraphicsItem*)> link_handler;
    std::function<void(const std::map<aether::mat::NodeId, QPointF>&)> move_handler;
protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        before_.clear();
        for (auto* item : items()) {
            if (item->data(kMaterialNodeRole).isValid())
                before_[item->data(kMaterialNodeRole).toUInt()] = item->pos();
        }
        auto* hit = itemAt(event->scenePos(), QTransform());
        while (hit && !hit->data(kMaterialPinRole).toBool()) hit = hit->parentItem();
        if (hit && event->button() == Qt::LeftButton) {
            drag_pin_ = hit;
            temporary_ = addPath({}, QPen(QColor("#80c8ff"), 2, Qt::DashLine));
            temporary_->setZValue(-2);
            event->accept();
            return;
        }
        QGraphicsScene::mousePressEvent(event);
    }
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
        if (drag_pin_ && temporary_) {
            const QPointF a = drag_pin_->scenePos(), b = event->scenePos();
            QPainterPath path(a);
            const qreal bend = std::max<qreal>(40, std::abs(b.x() - a.x()) * 0.45);
            const qreal direction = drag_pin_->data(kMaterialPinOutputRole).toBool() ? 1 : -1;
            path.cubicTo(a + QPointF(direction * bend, 0), b - QPointF(direction * bend, 0), b);
            temporary_->setPath(path);
            event->accept();
            return;
        }
        QGraphicsScene::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        if (drag_pin_ && event->button() == Qt::LeftButton) {
            auto* end = itemAt(event->scenePos(), QTransform());
            while (end && !end->data(kMaterialPinRole).toBool()) end = end->parentItem();
            if (temporary_) { removeItem(temporary_); delete temporary_; temporary_ = nullptr; }
            auto* start = drag_pin_; drag_pin_ = nullptr;
            if (end && end != start && link_handler) link_handler(start, end);
            event->accept();
            return;
        }
        QGraphicsScene::mouseReleaseEvent(event);
        if (move_handler) {
            std::map<aether::mat::NodeId, QPointF> moved;
            for (auto* item : items()) {
                if (!item->data(kMaterialNodeRole).isValid()) continue;
                const auto id = item->data(kMaterialNodeRole).toUInt();
                if (before_.count(id) && before_[id] != item->pos()) moved[id] = item->pos();
            }
            if (!moved.empty()) move_handler(moved);
        }
    }
private:
    QGraphicsItem* drag_pin_ = nullptr;
    QGraphicsPathItem* temporary_ = nullptr;
    std::map<aether::mat::NodeId, QPointF> before_;
};
}

MaterialEditorWidget::MaterialEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    auto* toolbar = new QHBoxLayout;
    save_ = new QPushButton("Save", this); save_->setObjectName("materialSave");
    auto* undo = new QPushButton("Undo", this); undo->setObjectName("materialUndo");
    auto* redo = new QPushButton("Redo", this); redo->setObjectName("materialRedo");
    auto* save_as = new QPushButton("Save As…", this);
    auto* new_material = new QPushButton("New Material", this);
    toolbar->addWidget(save_); toolbar->addWidget(undo); toolbar->addWidget(redo);
    toolbar->addWidget(save_as); toolbar->addSpacing(12); toolbar->addWidget(new_material);
    status_ = new QLabel("Open a .amat material from the Content Browser", this);
    status_->setObjectName("materialStatus"); toolbar->addWidget(status_, 1);
    root->addLayout(toolbar);

    auto* main_split = new QSplitter(Qt::Horizontal, this);
    auto* left = new QWidget(main_split); auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(4, 4, 4, 4);
    left_layout->addWidget(new QLabel("PARAMETERS", left));
    auto* add_row = new QHBoxLayout;
    for (const auto& entry : std::vector<std::pair<QString, aether::mat::PinType>>{
             {"+ Scalar", aether::mat::PinType::Float}, {"+ Vector", aether::mat::PinType::Float3},
             {"+ Texture", aether::mat::PinType::Texture}}) {
        auto* button = new QPushButton(entry.first, left);
        QObject::connect(button, &QPushButton::clicked, this, [this, type = entry.second] {
            const auto name = document_.AddParameter(type); Refresh();
            for (int i = 0; i < parameters_->count(); ++i)
                if (parameters_->item(i)->text() == QString::fromStdString(name)) parameters_->setCurrentRow(i);
        });
        add_row->addWidget(button);
    }
    left_layout->addLayout(add_row);
    parameters_ = new QListWidget(left); parameters_->setObjectName("materialParameters"); left_layout->addWidget(parameters_, 1);
    auto* add_param_node = new QPushButton("Add selected parameter to graph", left); left_layout->addWidget(add_param_node);

    auto* center = new QWidget(main_split); auto* center_layout = new QVBoxLayout(center);
    center_layout->setContentsMargins(4, 4, 4, 4);
    auto* palette_row = new QHBoxLayout;
    palette_filter_ = new QLineEdit(center); palette_filter_->setPlaceholderText("Search material nodes…");
    palette_filter_->setObjectName("materialNodeSearch"); palette_row->addWidget(palette_filter_, 1);
    auto* add_node = new QPushButton("Add Node", center); palette_row->addWidget(add_node); center_layout->addLayout(palette_row);
    palette_ = new QListWidget(center); palette_->setMaximumHeight(110); palette_->setObjectName("materialNodePalette"); center_layout->addWidget(palette_);
    scene_ = new MaterialScene(this); scene_->setBackgroundBrush(QColor("#151b22"));
    view_ = new QGraphicsView(scene_, center); view_->setObjectName("materialGraphView");
    view_->setRenderHint(QPainter::Antialiasing); view_->setDragMode(QGraphicsView::RubberBandDrag);
    view_->setBackgroundBrush(QColor("#151b22")); center_layout->addWidget(view_, 1);

    auto* right = new QWidget(main_split); auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(4, 4, 4, 4); right_layout->addWidget(new QLabel("DETAILS", right));
    details_ = new QWidget(right); details_form_ = new QFormLayout(details_); right_layout->addWidget(details_);
    auto* tabs = new QTabWidget(right);
    diagnostics_ = new QPlainTextEdit(tabs); diagnostics_->setReadOnly(true); diagnostics_->setObjectName("materialDiagnostics");
    shader_ = new QPlainTextEdit(tabs); shader_->setReadOnly(true); shader_->setObjectName("materialGeneratedHlsl");
    tabs->addTab(diagnostics_, "Diagnostics"); tabs->addTab(shader_, "Generated HLSL"); right_layout->addWidget(tabs, 1);
    main_split->addWidget(left); main_split->addWidget(center); main_split->addWidget(right);
    main_split->setStretchFactor(1, 1); main_split->setSizes({220, 700, 300}); root->addWidget(main_split, 1);

    QObject::connect(save_, &QPushButton::clicked, this, [this] { SaveCurrent(); });
    QObject::connect(save_as, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, "Save Material As", QDir::homePath(), "Aether Materials (*.amat)");
        if (path.isEmpty()) return;
        std::string error;
        if (document_.SaveAs(std::filesystem::path(path.toStdWString()), &error)) { Refresh(); SetStatus("Saved material"); }
        else SetStatus(QString::fromStdString(error), true);
    });
    QObject::connect(undo, &QPushButton::clicked, this, [this] { UndoCurrent(); });
    QObject::connect(redo, &QPushButton::clicked, this, [this] { RedoCurrent(); });
    QObject::connect(new_material, &QPushButton::clicked, this, [this] {
        if (!ConfirmDiscardChanges(this)) return;
        aether::mat::Material material; material.Add("Material.Output");
        document_ = aether::editor::MaterialDocument(std::move(material));
        document_.Edit("Create material", [](aether::mat::Material&) {});
        Refresh();
        SetStatus("New material · choose Save As to create the asset");
    });
    QObject::connect(palette_filter_, &QLineEdit::textChanged, this, [this] { RefreshPalette(); });
    QObject::connect(palette_, &QListWidget::itemDoubleClicked, this, [this] { AddSelectedNode(); });
    QObject::connect(add_node, &QPushButton::clicked, this, [this] { AddSelectedNode(); });
    QObject::connect(parameters_, &QListWidget::currentRowChanged, this, [this] { RefreshDetails(); });
    QObject::connect(scene_, &QGraphicsScene::selectionChanged, this, [this] {
        if (refreshing_) return;
        selected_node_id_ = 0;
        for (auto* item : scene_->selectedItems()) {
            if (item->data(kMaterialNodeRole).isValid()) {
                selected_node_id_ = item->data(kMaterialNodeRole).toUInt();
                break;
            }
        }
        RefreshDetails();
    });
    QObject::connect(add_param_node, &QPushButton::clicked, this, [this] {
        auto* item = parameters_->currentItem(); if (!item) return;
        const QString name = item->text().section(" · ", 0, 0);
        const auto* p = document_.Get().FindParameter(name.toStdString()); if (!p) return;
        document_.Edit("Add parameter node", [&](aether::mat::Material& m) {
            const auto node = m.Add(aether::editor::MaterialDocument::ParameterNodeType(*p), {},
                static_cast<float>(view_->mapToScene(view_->viewport()->rect().center()).x()),
                static_cast<float>(view_->mapToScene(view_->viewport()->rect().center()).y()));
            Q_UNUSED(node);
        }); Refresh();
    });
    auto* material_scene = static_cast<MaterialScene*>(scene_);
    material_scene->link_handler = [this](auto* a, auto* b) { ConnectPins(a, b); };
    material_scene->move_handler = [this](const auto& positions) { CommitPositions(positions); };
    auto* save_shortcut = new QShortcut(QKeySequence::Save, this); QObject::connect(save_shortcut, &QShortcut::activated, this, [this] { SaveCurrent(); });
    auto* undo_shortcut = new QShortcut(QKeySequence::Undo, this); QObject::connect(undo_shortcut, &QShortcut::activated, this, [this] { UndoCurrent(); });
    auto* redo_shortcut = new QShortcut(QKeySequence::Redo, this); QObject::connect(redo_shortcut, &QShortcut::activated, this, [this] { RedoCurrent(); });
    Refresh();
}

bool MaterialEditorWidget::OpenFile(const std::filesystem::path& path) {
    if (!ConfirmDiscardChanges(this)) return false;
    std::string error;
    aether::editor::MaterialDocument next;
    if (!next.Load(path, &error)) { SetStatus(QString::fromStdString(error), true); return false; }
    document_ = std::move(next); Refresh(); SetStatus(QString("Opened %1").arg(QString::fromStdString(document_.Name()))); return true;
}

void MaterialEditorWidget::ClearDocument() {
    aether::mat::Material material; material.Add("Material.Output");
    document_ = aether::editor::MaterialDocument(std::move(material));
    Refresh(); SetStatus("Open or create a material asset");
}

bool MaterialEditorWidget::ConfirmDiscardChanges(QWidget* parent) const {
    if (!document_.Dirty()) return true;
    return QMessageBox::question(parent, "Unsaved Material", "Discard unsaved material changes?",
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Discard;
}

void MaterialEditorWidget::SaveCurrent() {
    if (document_.Path().empty()) { SetStatus("Save As is needed for a new material", true); return; }
    std::string error;
    if (!document_.Save(&error)) SetStatus(QString::fromStdString(error), true);
    else { SetStatus("Saved " + QString::fromStdString(document_.Name())); Refresh(); }
}
void MaterialEditorWidget::UndoCurrent() { if (document_.Undo()) Refresh(); }
void MaterialEditorWidget::RedoCurrent() { if (document_.Redo()) Refresh(); }

void MaterialEditorWidget::Refresh() {
    if (refreshing_) return;
    refreshing_ = true; scene_->clear();
    std::map<std::pair<aether::mat::NodeId, std::string>, QGraphicsItem*> pins;
    const auto& material = document_.Get(); const auto analysis = document_.Analysis();
    for (const auto& node : material.nodes) {
        std::string error;
        const auto signature = aether::mat::ResolveNode(material, node, &error);
        const QString title = signature ? QString::fromStdString(signature->title) : QString::fromStdString(node.type);
        const auto ins = signature ? signature->inputs : std::vector<aether::mat::PinDesc>{};
        const auto outs = signature ? signature->outputs : std::vector<aether::mat::PinDesc>{};
        const int rows = std::max<int>(1, static_cast<int>(std::max(ins.size(), outs.size())));
        const qreal width = std::max<qreal>(190, title.size() * 8.5 + 36);
        const qreal height = 36 + rows * 25;
        auto* box = scene_->addRect(QRectF(0, 0, width, height), QPen(QColor("#526170"), 1.2), QColor("#202a34"));
        box->setPos(node.x, node.y); box->setZValue(1); box->setData(kMaterialNodeRole, node.id);
        box->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
        box->setSelected(node.id == selected_node_id_);
        auto* header = scene_->addRect(QRectF(0, 0, width, 29), Qt::NoPen,
                                       signature ? HeaderColor(signature->category) : QColor("#7e3337"));
        header->setParentItem(box); header->setAcceptedMouseButtons(Qt::NoButton);
        auto* text = scene_->addSimpleText(title, QFont("Segoe UI", 9, QFont::DemiBold));
        text->setBrush(Qt::white); text->setPos(9, 5); text->setParentItem(box); text->setAcceptedMouseButtons(Qt::NoButton);
        auto add_pins = [&](const std::vector<aether::mat::PinDesc>& list, bool output) {
            for (int i = 0; i < static_cast<int>(list.size()); ++i) {
                const auto& pin = list[static_cast<size_t>(i)]; const qreal y = 44 + i * 25;
                auto* dot = scene_->addEllipse(QRectF(-5, -5, 10, 10), QPen(QColor("#111820"), 1), TypeColor(analysis.TypeOf({node.id, pin.name}) == aether::mat::PinType::None ? pin.type : analysis.TypeOf({node.id, pin.name})));
                dot->setPos(output ? width - 5 : -5, y); dot->setParentItem(box); dot->setZValue(3);
                dot->setData(kMaterialPinRole, true); dot->setData(kMaterialNodeRole, node.id);
                dot->setData(kMaterialPinOutputRole, output); dot->setData(kMaterialPinNameRole, QString::fromStdString(pin.name));
                dot->setToolTip(QString("%1 · %2").arg(QString::fromStdString(pin.name), QString::fromLatin1(aether::mat::TypeName(pin.type))));
                auto* label = scene_->addSimpleText(QString::fromStdString(pin.name), QFont("Segoe UI", 8));
                label->setBrush(QColor("#c2ccd5")); label->setPos(output ? width - label->boundingRect().width() - 12 : 11, y - 9);
                label->setParentItem(box); label->setAcceptedMouseButtons(Qt::NoButton); pins[{node.id, pin.name}] = dot;
            }
        };
        add_pins(ins, false); add_pins(outs, true); if (!error.empty()) box->setToolTip(QString::fromStdString(error));
    }
    for (const auto& link : material.links) {
        auto from = pins.find({link.from.node, link.from.pin}), to = pins.find({link.to.node, link.to.pin});
        if (from == pins.end() || to == pins.end()) continue;
        const auto a = from->second->scenePos(), b = to->second->scenePos();
        QPainterPath path(a); const qreal bend = std::max<qreal>(45, (b.x() - a.x()) * 0.45);
        path.cubicTo(a + QPointF(bend, 0), b - QPointF(bend, 0), b);
        auto* wire = scene_->addPath(path, QPen(QColor("#94a9bb"), 2)); wire->setZValue(-1); wire->setAcceptedMouseButtons(Qt::NoButton);
    }
    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-150, -120, 150, 120));
    parameters_->clear();
    for (const auto& p : material.parameters)
        parameters_->addItem(QString::fromStdString(p.name) + " · " + QString::fromLatin1(aether::mat::TypeName(p.type)));
    QStringList messages;
    for (const auto& d : analysis.diagnostics)
        messages << QString("%1 %2: %3").arg(d.severity == aether::mat::Severity::Error ? "Error" : "Warning",
            QString::fromStdString(d.code), QString::fromStdString(d.message));
    if (messages.empty()) messages << (document_.Generated().ok ? "Material graph is valid." : "No validation details.");
    diagnostics_->setPlainText(messages.join('\n')); shader_->setPlainText(QString::fromStdString(document_.Generated().hlsl));
    save_->setText(document_.Dirty() ? "Save*" : "Save");
    if (!document_.Path().empty()) status_->setText(QString::fromStdString(document_.Path().string()));
    RefreshPalette(); RefreshDetails(); refreshing_ = false;
}

void MaterialEditorWidget::RefreshPalette() {
    const QString query = palette_filter_->text().trimmed(); palette_->clear();
    for (const auto& entry : aether::mat::ListNodeTypes(document_.Get())) {
        const QString label = QString::fromStdString(entry.title + "  ·  " + entry.category);
        if (!query.isEmpty() && !label.contains(query, Qt::CaseInsensitive) &&
            !QString::fromStdString(entry.id).contains(query, Qt::CaseInsensitive)) continue;
        auto* item = new QListWidgetItem(label, palette_); item->setData(Qt::UserRole, QString::fromStdString(entry.id));
    }
}

void MaterialEditorWidget::AddSelectedNode() {
    auto* item = palette_->currentItem(); if (!item) return;
    const QString type = item->data(Qt::UserRole).toString();
    const QPointF center = view_->mapToScene(view_->viewport()->rect().center());
    document_.Edit("Add " + type.toStdString(), [&](aether::mat::Material& m) {
        m.Add(type.toStdString(), nlohmann::json::object(), static_cast<float>(center.x()), static_cast<float>(center.y()));
    }); Refresh();
}

void MaterialEditorWidget::ConnectPins(QGraphicsItem* a, QGraphicsItem* b) {
    QGraphicsItem* out = a->data(kMaterialPinOutputRole).toBool() ? a : b;
    QGraphicsItem* in = out == a ? b : a;
    if (!out->data(kMaterialPinOutputRole).toBool() || in->data(kMaterialPinOutputRole).toBool()) return;
    const aether::mat::PinRef from{out->data(kMaterialNodeRole).toUInt(), out->data(kMaterialPinNameRole).toString().toStdString()};
    const aether::mat::PinRef to{in->data(kMaterialNodeRole).toUInt(), in->data(kMaterialPinNameRole).toString().toStdString()};
    std::string message;
    document_.Edit("Connect material pins", [&](aether::mat::Material& m) {
        auto candidate = m; candidate.Connect(from.node, from.pin, to.node, to.pin);
        const auto check = aether::mat::Analyze(candidate);
        const auto bad = std::find_if(check.diagnostics.begin(), check.diagnostics.end(), [](const auto& d) {
            return d.severity == aether::mat::Severity::Error && (d.code == "MT003" || d.code == "MT004" || d.code == "MT005");
        });
        if (bad == check.diagnostics.end()) m = std::move(candidate); else message = bad->message;
    });
    if (!message.empty()) { document_.CancelEdit(); SetStatus(QString::fromStdString(message), true); }
    Refresh();
}

void MaterialEditorWidget::CommitPositions(const std::map<aether::mat::NodeId, QPointF>& positions) {
    document_.Edit("Move nodes", [&](aether::mat::Material& m) {
        for (const auto& [id, pos] : positions) if (auto* node = m.Find(id)) {
            node->x = static_cast<float>(pos.x()); node->y = static_cast<float>(pos.y());
        }
    }, "material-move"); Refresh();
}

void MaterialEditorWidget::RefreshDetails() {
    while (details_form_->rowCount()) details_form_->removeRow(0);
    if (selected_node_id_ != 0) {
        const auto* node = document_.Get().Find(selected_node_id_);
        if (node) {
            const auto node_id = node->id;
            const auto node_type = node->type;
            details_form_->addRow("Node", new QLabel(QString::fromStdString(node_type), details_));
            auto edit_config = [this, node_id](const char* key, nlohmann::json value, const std::string& label) {
                document_.Edit(label, [=](aether::mat::Material& material) {
                    if (auto* target = material.Find(node_id)) target->config[key] = value;
                }, "material-node-config:" + std::to_string(node_id) + ":" + key);
                Refresh();
            };
            if (node_type.rfind("Const.Float", 0) == 0) {
                const int count = node_type == "Const.Float" ? 1 : std::clamp(node_type.back() - '0', 2, 4);
                nlohmann::json initial = node->config.value("value", nlohmann::json::array());
                auto component = [&](int index) -> double {
                    if (initial.is_number()) return index == 0 ? initial.get<double>() : 0.0;
                    if (initial.is_array() && index < static_cast<int>(initial.size()) && initial[index].is_number()) return initial[index].get<double>();
                    return 0.0;
                };
                for (int index = 0; index < count; ++index) {
                    auto* value = new QDoubleSpinBox(details_); value->setRange(-100000.0, 100000.0);
                    value->setDecimals(4); value->setSingleStep(0.05); value->setValue(component(index));
                    details_form_->addRow(count == 1 ? "Value" : QString("Value %1").arg(index + 1), value);
                    QObject::connect(value, &QDoubleSpinBox::editingFinished, this, [this, node_id, node_type, index, count, value] {
                        const double next = value->value();
                        document_.Edit("Edit material constant", [=](aether::mat::Material& material) {
                            auto* target = material.Find(node_id); if (!target) return;
                            auto config_value = target->config.value("value", nlohmann::json::array());
                            if (count == 1) target->config["value"] = next;
                            else {
                                nlohmann::json values = nlohmann::json::array();
                                for (int i = 0; i < count; ++i) {
                                    double part = 0.0;
                                    if (config_value.is_array() && i < static_cast<int>(config_value.size()) && config_value[i].is_number()) part = config_value[i].get<double>();
                                    values.push_back(i == index ? next : part);
                                }
                                target->config["value"] = std::move(values);
                            }
                        }, "material-node-config:" + std::to_string(node_id) + ":value:" + std::to_string(index));
                        Refresh();
                    });
                }
            } else if (node_type == "Input.TexCoord") {
                auto* index = new QSpinBox(details_); index->setRange(0, 3); index->setValue(static_cast<int>(node->config.value("index", 0)));
                details_form_->addRow("UV channel", index);
                QObject::connect(index, &QSpinBox::editingFinished, this, [edit_config, index] { edit_config("index", index->value(), "Set texture coordinate channel"); });
            } else if (node_type == "Math.ComponentMask") {
                auto* channels = new QLineEdit(QString::fromStdString(node->config.value("channels", std::string("r"))), details_);
                channels->setValidator(new QRegularExpressionValidator(QRegularExpression("[rgba]{1,4}"), channels));
                details_form_->addRow("Channels", channels);
                QObject::connect(channels, &QLineEdit::editingFinished, this, [edit_config, channels] { edit_config("channels", channels->text().toStdString(), "Edit component mask"); });
            } else if (node_type == "Math.Noise") {
                auto* octaves = new QSpinBox(details_); octaves->setRange(1, 8); octaves->setValue(static_cast<int>(node->config.value("octaves", 1)));
                details_form_->addRow("Octaves", octaves);
                QObject::connect(octaves, &QSpinBox::editingFinished, this, [edit_config, octaves] { edit_config("octaves", octaves->value(), "Set noise octaves"); });
            } else if (node_type == "Custom.HLSL") {
                auto* output = new QComboBox(details_);
                for (const auto type : {aether::mat::PinType::Float, aether::mat::PinType::Float2, aether::mat::PinType::Float3, aether::mat::PinType::Float4})
                    output->addItem(QString::fromLatin1(aether::mat::TypeName(type)), QString::fromLatin1(aether::mat::TypeName(type)));
                output->setCurrentIndex(output->findData(QString::fromStdString(node->config.value("output", std::string("float")))));
                details_form_->addRow("Output type", output);
                QObject::connect(output, qOverload<int>(&QComboBox::currentIndexChanged), this, [edit_config, output](int) { edit_config("output", output->currentData().toString().toStdString(), "Set custom node output type"); });
                auto* code = new QPlainTextEdit(QString::fromStdString(node->config.value("code", std::string("return 0.0;"))), details_);
                code->setMaximumHeight(140); details_form_->addRow("HLSL body", code);
                auto* apply_code = new QPushButton("Apply HLSL", details_); details_form_->addRow(apply_code);
                QObject::connect(apply_code, &QPushButton::clicked, this, [this, node_id, code] {
                    const auto value = code->toPlainText().toStdString();
                    document_.Edit("Edit custom HLSL", [=](aether::mat::Material& material) {
                        if (auto* target = material.Find(node_id)) target->config["code"] = value;
                    }, "material-node-config:" + std::to_string(node_id) + ":code");
                    Refresh();
                });

                details_form_->addRow("Inputs", new QLabel("Define the typed pins exposed to the HLSL body.", details_));
                auto* inputs = new QTableWidget(details_); inputs->setColumnCount(2);
                inputs->setHorizontalHeaderLabels({"Name", "Type"}); inputs->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
                inputs->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents); inputs->setMaximumHeight(150);
                const auto input_config = node->config.contains("inputs") ? node->config["inputs"] : nlohmann::json::array();
                if (input_config.is_array()) {
                    for (const auto& entry : input_config) {
                        const int row = inputs->rowCount(); inputs->insertRow(row);
                        inputs->setItem(row, 0, new QTableWidgetItem(entry.is_object() ? QString::fromStdString(entry.value("name", std::string{})) : QString{}));
                        auto* input_type = new QComboBox(inputs);
                        for (const auto type : {aether::mat::PinType::Float, aether::mat::PinType::Float2, aether::mat::PinType::Float3, aether::mat::PinType::Float4, aether::mat::PinType::Texture})
                            input_type->addItem(QString::fromLatin1(aether::mat::TypeName(type)), QString::fromLatin1(aether::mat::TypeName(type)));
                        const auto type_name = entry.is_object() ? entry.value("type", std::string("float")) : std::string("float");
                        const int type_index = input_type->findData(QString::fromStdString(type_name));
                        if (type_index >= 0) input_type->setCurrentIndex(type_index);
                        inputs->setCellWidget(row, 1, input_type);
                    }
                } else {
                    auto* invalid = new QLabel("Input signature is invalid. Clear or replace it to restore valid pins.", details_);
                    invalid->setWordWrap(true); details_form_->addRow(invalid);
                }
                details_form_->addRow(inputs);
                auto* input_buttons = new QWidget(details_); auto* input_buttons_layout = new QHBoxLayout(input_buttons);
                input_buttons_layout->setContentsMargins(0, 0, 0, 0);
                auto* add_input = new QPushButton("Add Input", input_buttons); auto* remove_input = new QPushButton("Remove Selected", input_buttons);
                auto* apply_inputs = new QPushButton("Apply Inputs", input_buttons);
                input_buttons_layout->addWidget(add_input); input_buttons_layout->addWidget(remove_input); input_buttons_layout->addWidget(apply_inputs);
                details_form_->addRow(input_buttons);
                QObject::connect(add_input, &QPushButton::clicked, this, [inputs] {
                    const int row = inputs->rowCount(); inputs->insertRow(row);
                    inputs->setItem(row, 0, new QTableWidgetItem(QString("input%1").arg(row + 1)));
                    auto* input_type = new QComboBox(inputs);
                    for (const auto type : {aether::mat::PinType::Float, aether::mat::PinType::Float2, aether::mat::PinType::Float3, aether::mat::PinType::Float4, aether::mat::PinType::Texture})
                        input_type->addItem(QString::fromLatin1(aether::mat::TypeName(type)), QString::fromLatin1(aether::mat::TypeName(type)));
                    inputs->setCellWidget(row, 1, input_type);
                });
                QObject::connect(remove_input, &QPushButton::clicked, this, [inputs] {
                    const int row = inputs->currentRow(); if (row >= 0) inputs->removeRow(row);
                });
                QObject::connect(apply_inputs, &QPushButton::clicked, this, [this, node_id, inputs] {
                    nlohmann::json input_list = nlohmann::json::array();
                    for (int row = 0; row < inputs->rowCount(); ++row) {
                        const auto* name = inputs->item(row, 0);
                        const auto* type = qobject_cast<QComboBox*>(inputs->cellWidget(row, 1));
                        if (!name || !type) continue;
                        input_list.push_back({{"name", name->text().trimmed().toStdString()}, {"type", type->currentData().toString().toStdString()}});
                    }
                    document_.Edit("Edit custom HLSL inputs", [=](aether::mat::Material& material) {
                        if (auto* target = material.Find(node_id)) target->config["inputs"] = input_list;
                    }, "material-node-config:" + std::to_string(node_id) + ":inputs");
                    Refresh();
                });
            }

            std::string resolve_error;
            const auto signature = aether::mat::ResolveNode(document_.Get(), *node, &resolve_error);
            if (signature) {
                for (const auto& input : signature->inputs) {
                    if (input.type == aether::mat::PinType::Any || input.type == aether::mat::PinType::Texture) continue;
                    const bool linked = std::any_of(document_.Get().links.begin(), document_.Get().links.end(), [&](const auto& link) {
                        return link.to.node == node_id && link.to.pin == input.name;
                    });
                    if (linked) continue;
                    const auto count = static_cast<int>(aether::mat::Components(input.type));
                    if (count < 1 || count > 4) continue;
                    aether::Vec4 defaults = input.default_value;
                    const auto stored = node->defaults.find(input.name);
                    if (stored != node->defaults.end()) {
                        if (stored->is_number()) defaults.x = stored->get<float>();
                        else if (stored->is_array()) {
                            float* values[] = {&defaults.x, &defaults.y, &defaults.z, &defaults.w};
                            for (int i = 0; i < count && i < static_cast<int>(stored->size()); ++i)
                                if ((*stored)[i].is_number()) *values[i] = (*stored)[i].get<float>();
                        }
                    }
                    const float values[] = {defaults.x, defaults.y, defaults.z, defaults.w};
                    const std::array<double, 4> baseline{values[0], values[1], values[2], values[3]};
                    for (int component_index = 0; component_index < count; ++component_index) {
                        auto* value = new QDoubleSpinBox(details_); value->setRange(-100000.0, 100000.0); value->setDecimals(4); value->setSingleStep(0.05); value->setValue(values[component_index]);
                        const QString label = count == 1 ? QString::fromStdString(input.name) : QString("%1 %2").arg(QString::fromStdString(input.name)).arg(component_index + 1);
                        details_form_->addRow(label, value);
                        QObject::connect(value, &QDoubleSpinBox::editingFinished, this, [this, node_id, pin = input.name, component_index, count, baseline, value] {
                            const double next = value->value();
                            document_.Edit("Edit material input default", [=](aether::mat::Material& material) {
                                auto* target = material.Find(node_id); if (!target) return;
                                auto current = target->defaults.value(pin, nlohmann::json::array());
                                if (count == 1) target->defaults[pin] = next;
                                else {
                                    nlohmann::json updated = nlohmann::json::array();
                                    for (int i = 0; i < count; ++i) {
                                        double part = baseline[static_cast<size_t>(i)];
                                        if (current.is_array() && i < static_cast<int>(current.size()) && current[i].is_number()) part = current[i].get<double>();
                                        updated.push_back(i == component_index ? next : part);
                                    }
                                    target->defaults[pin] = std::move(updated);
                                }
                            }, "material-node-default:" + std::to_string(node_id) + ":" + pin + ":" + std::to_string(component_index));
                            Refresh();
                        });
                    }
                }
            }
            return;
        }
        selected_node_id_ = 0;
    }
    const int row = parameters_->currentRow();
    if (row >= 0 && row < static_cast<int>(document_.Get().parameters.size())) {
        const QString name = QString::fromStdString(document_.Get().parameters[static_cast<size_t>(row)].name);
        auto* name_edit = new QLineEdit(name, details_); details_form_->addRow("Parameter", name_edit);
        auto* type = new QComboBox(details_);
        for (auto t : {aether::mat::PinType::Float, aether::mat::PinType::Float2, aether::mat::PinType::Float3, aether::mat::PinType::Float4, aether::mat::PinType::Texture})
            type->addItem(QString::fromLatin1(aether::mat::TypeName(t)), static_cast<int>(t));
        const auto& parameter = document_.Get().parameters[static_cast<size_t>(row)]; type->setCurrentIndex(type->findData(static_cast<int>(parameter.type)));
        details_form_->addRow("Type", type);
        if (parameter.type == aether::mat::PinType::Texture) {
            auto* texture = new QLineEdit(QString::fromStdString(parameter.texture), details_); details_form_->addRow("Default texture GUID", texture);
            QObject::connect(texture, &QLineEdit::editingFinished, this, [this, name, texture] {
                const auto value = texture->text().toStdString(); document_.Edit("Edit texture default", [=](aether::mat::Material& m) {
                    if (auto* p = const_cast<aether::mat::Parameter*>(m.FindParameter(name.toStdString()))) p->texture = value;
                }); Refresh();
            });
        } else {
            for (int i = 0; i < static_cast<int>(aether::mat::Components(parameter.type)); ++i) {
                auto* value = new QDoubleSpinBox(details_); value->setRange(-100000, 100000); value->setDecimals(4); value->setSingleStep(0.05);
                const float components[] = {parameter.default_value.x, parameter.default_value.y, parameter.default_value.z, parameter.default_value.w}; value->setValue(components[i]);
                details_form_->addRow(QString("Default %1").arg(i + 1), value);
                QObject::connect(value, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, name, i](double v) {
                    document_.Edit("Edit parameter default", [=](aether::mat::Material& m) {
                        if (auto* p = const_cast<aether::mat::Parameter*>(m.FindParameter(name.toStdString()))) {
                            float* c[] = {&p->default_value.x, &p->default_value.y, &p->default_value.z, &p->default_value.w}; *c[i] = static_cast<float>(v);
                        }
                    }, "param:" + name.toStdString() + ":" + std::to_string(i));
                });
            }
        }
        QObject::connect(name_edit, &QLineEdit::editingFinished, this, [this, name, name_edit] {
            if (name_edit->text() == name) return;
            std::string error;
            if (!document_.RenameParameter(name.toStdString(), name_edit->text().toStdString(), &error)) SetStatus(QString::fromStdString(error), true);
            Refresh();
        });
        QObject::connect(type, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, name, type](int) {
            document_.SetParameterType(name.toStdString(), static_cast<aether::mat::PinType>(type->currentData().toInt())); Refresh();
        });
        auto* remove = new QPushButton("Remove parameter", details_); details_form_->addRow(remove);
        QObject::connect(remove, &QPushButton::clicked, this, [this, name] { document_.RemoveParameter(name.toStdString()); Refresh(); });
    } else {
        const auto& m = document_.Get();
        auto* shading = new QComboBox(details_); shading->addItems({"Default Lit", "Unlit"}); shading->setCurrentIndex(static_cast<int>(m.shading)); details_form_->addRow("Shading", shading);
        QObject::connect(shading, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
            document_.Edit("Change shading", [=](aether::mat::Material& material) { material.shading = static_cast<aether::mat::ShadingModel>(i); }); Refresh();
        });
        auto* blend = new QComboBox(details_); blend->addItems({"Opaque", "Masked", "Translucent", "Additive"}); blend->setCurrentIndex(static_cast<int>(m.blend)); details_form_->addRow("Blend mode", blend);
        QObject::connect(blend, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
            document_.Edit("Change blend mode", [=](aether::mat::Material& material) { material.blend = static_cast<aether::mat::BlendMode>(i); }); Refresh();
        });
        auto* sides = new QComboBox(details_); sides->addItems({"One sided", "Two sided"}); sides->setCurrentIndex(m.two_sided ? 1 : 0); details_form_->addRow("Faces", sides);
        QObject::connect(sides, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
            document_.Edit("Change face mode", [=](aether::mat::Material& material) { material.two_sided = i != 0; }); Refresh();
        });
    }
}

void MaterialEditorWidget::SetStatus(const QString& text, bool error) {
    status_->setText(text); status_->setStyleSheet(error ? "color: #ff7777" : "color: #a8b6c4");
}

} // namespace aether::editor::qt
