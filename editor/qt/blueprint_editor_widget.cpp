#include "blueprint_editor_widget.h"

#include "aether/blueprint/nodes.h"

#include <QComboBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFormLayout>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSaveFile>
#include <QScrollArea>
#include <QSplitter>
#include <QStringList>
#include <QVBoxLayout>
#include <QTabWidget>

#include <algorithm>
#include <map>
#include <set>

namespace aether::editor::qt {
namespace {

constexpr int kNodeIdRole = Qt::UserRole;
constexpr int kPinNameRole = Qt::UserRole + 1;
constexpr int kPinOutputRole = Qt::UserRole + 2;
constexpr int kIsPinRole = Qt::UserRole + 3;
constexpr int kMemberKindRole = Qt::UserRole + 4;
constexpr int kCommentIndexRole = Qt::UserRole + 5;
constexpr int kMemberVariable = 1;
constexpr int kMemberGraph = 2;

QColor NodeColor(const std::string& category, aether::bp::NodeKind kind) {
    if (kind == aether::bp::NodeKind::Event) return QColor("#7b3440");
    if (category == "Flow Control") return QColor("#58616c");
    if (kind == aether::bp::NodeKind::Pure) return QColor("#39734f");
    if (kind == aether::bp::NodeKind::Latent) return QColor("#9a682e");
    return QColor("#315b86");
}

QColor PinColor(const aether::bp::PinType& type) {
    using aether::bp::ValueType;
    if (type.IsExec()) return QColor("#e5e9ed");
    switch (type.type) {
    case ValueType::Bool: return QColor("#d06060");
    case ValueType::Int: return QColor("#43b8a7");
    case ValueType::Float: return QColor("#9bc95a");
    case ValueType::String: return QColor("#d765c6");
    case ValueType::Vec3: return QColor("#e5bd49");
    case ValueType::Quat: return QColor("#8bb8f0");
    case ValueType::Entity: return QColor("#5c91e8");
    default: return QColor("#98a4af");
    }
}

bool ParsePinDefault(const QString& text, const aether::bp::PinType& type, aether::bp::Value& value) {
    using aether::bp::ValueType;
    const QString trimmed = text.trimmed();
    nlohmann::json json;
    bool ok = false;
    switch (type.type) {
    case ValueType::Bool:
        if (trimmed.compare("true", Qt::CaseInsensitive) == 0) json = true;
        else if (trimmed.compare("false", Qt::CaseInsensitive) == 0) json = false;
        else return false;
        break;
    case ValueType::Int: {
        const int number = trimmed.toInt(&ok);
        if (!ok) return false;
        json = number;
        break;
    }
    case ValueType::Float: {
        const float number = trimmed.toFloat(&ok);
        if (!ok) return false;
        json = number;
        break;
    }
    case ValueType::String:
        json = text.toStdString();
        break;
    case ValueType::Vec3:
    case ValueType::Quat: {
        QString components = trimmed;
        if (components.startsWith('(') && components.endsWith(')'))
            components = components.mid(1, components.size() - 2);
        const QStringList parts = components.split(',', Qt::SkipEmptyParts);
        const int count = type.type == ValueType::Vec3 ? 3 : 4;
        if (parts.size() != count) return false;
        json = nlohmann::json::array();
        for (const QString& part : parts) {
            const float number = part.trimmed().toFloat(&ok);
            if (!ok) return false;
            json.push_back(number);
        }
        break;
    }
    default:
        return false;
    }
    return aether::bp::ValueFromJson(json, type, value);
}

QString PinDefaultText(const aether::bp::Value& value, const aether::bp::PinType& type) {
    if (type.type == aether::bp::ValueType::String) {
        if (const auto* text = std::get_if<std::string>(&value)) return QString::fromStdString(*text);
    }
    return QString::fromStdString(aether::bp::ValueText(value));
}

class BlueprintScene final : public QGraphicsScene {
public:
    using LinkHandler = std::function<void(aether::bp::NodeId, const std::string&,
                                           aether::bp::NodeId, const std::string&)>;
    using MoveHandler = std::function<void(const std::vector<std::pair<aether::bp::NodeId,
                                                   std::pair<float, float>>>&)>;
    using CommentMoveHandler = std::function<void(const std::vector<std::tuple<int, float, float, float, float>>&)>;

    LinkHandler link_handler;
    MoveHandler move_handler;
    CommentMoveHandler comment_move_handler;

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        moved_before_.clear();
        for (QGraphicsItem* item : items()) {
            if (item->data(kNodeIdRole).isValid() && !item->data(kIsPinRole).toBool())
                moved_before_[item->data(kNodeIdRole).toUInt()] = item->pos();
            if (item->data(kCommentIndexRole).isValid())
                comment_moved_before_[item->data(kCommentIndexRole).toInt()] = item->pos();
        }
        QGraphicsItem* hit = itemAt(event->scenePos(), QTransform());
        if (hit && hit->data(kIsPinRole).toBool() && event->button() == Qt::LeftButton) {
            drag_pin_ = hit;
            temporary_wire_ = addPath(QPainterPath(), QPen(QColor("#80c8ff"), 2.0, Qt::DashLine));
            temporary_wire_->setZValue(-2.0);
            event->accept();
            return;
        }
        QGraphicsScene::mousePressEvent(event);
    }

    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
        if (drag_pin_ && temporary_wire_) {
            const QPointF start = drag_pin_->scenePos();
            const QPointF end = event->scenePos();
            QPainterPath path(start);
            const qreal bend = std::max<qreal>(45.0, std::abs(end.x() - start.x()) * 0.45);
            path.cubicTo(start + QPointF(drag_pin_->data(kPinOutputRole).toBool() ? bend : -bend, 0),
                         end - QPointF(drag_pin_->data(kPinOutputRole).toBool() ? bend : -bend, 0), end);
            temporary_wire_->setPath(path);
            event->accept();
            return;
        }
        QGraphicsScene::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        if (drag_pin_ && event->button() == Qt::LeftButton) {
            QGraphicsItem* end = itemAt(event->scenePos(), QTransform());
            while (end && !end->data(kIsPinRole).toBool()) end = end->parentItem();
            QGraphicsItem* start = drag_pin_;
            if (temporary_wire_) removeItem(temporary_wire_);
            delete temporary_wire_;
            temporary_wire_ = nullptr;
            drag_pin_ = nullptr;
            if (end && end != start && link_handler) {
                QGraphicsItem* out = start->data(kPinOutputRole).toBool() ? start : end;
                QGraphicsItem* in = out == start ? end : start;
                if (out->data(kPinOutputRole).toBool() && !in->data(kPinOutputRole).toBool())
                    link_handler(out->data(kNodeIdRole).toUInt(), out->data(kPinNameRole).toString().toStdString(),
                                 in->data(kNodeIdRole).toUInt(), in->data(kPinNameRole).toString().toStdString());
            }
            event->accept();
            return;
        }
        QGraphicsScene::mouseReleaseEvent(event);
        if (move_handler) {
            std::vector<std::pair<aether::bp::NodeId, std::pair<float, float>>> moved;
            for (QGraphicsItem* item : items()) {
                if (!item->data(kNodeIdRole).isValid() || item->data(kIsPinRole).toBool()) continue;
                const auto id = item->data(kNodeIdRole).toUInt();
                const QPointF now = item->pos();
                const auto before = moved_before_.find(id);
                if (before != moved_before_.end() && before->second != now)
                    moved.push_back({id, {static_cast<float>(now.x()), static_cast<float>(now.y())}});
            }
            if (!moved.empty()) move_handler(moved);
        }
        if (comment_move_handler) {
            std::vector<std::tuple<int, float, float, float, float>> moved;
            for (QGraphicsItem* item : items()) {
                if (!item->data(kCommentIndexRole).isValid()) continue;
                const int index = item->data(kCommentIndexRole).toInt();
                const QPointF now = item->pos();
                const auto before = comment_moved_before_.find(index);
                if (before != comment_moved_before_.end() && before->second != now)
                    moved.emplace_back(index, static_cast<float>(before->second.x()), static_cast<float>(before->second.y()),
                                       static_cast<float>(now.x()), static_cast<float>(now.y()));
            }
            if (!moved.empty()) comment_move_handler(moved);
        }
        moved_before_.clear();
        comment_moved_before_.clear();
    }

private:
    QGraphicsItem* drag_pin_ = nullptr;
    QGraphicsPathItem* temporary_wire_ = nullptr;
    std::map<aether::bp::NodeId, QPointF> moved_before_;
    std::map<int, QPointF> comment_moved_before_;
};

QColor CommentColor(aether::u32 stored) {
    return QColor(static_cast<int>(stored & 0xff), static_cast<int>((stored >> 8) & 0xff),
                  static_cast<int>((stored >> 16) & 0xff), static_cast<int>((stored >> 24) & 0xff));
}

aether::u32 StoredCommentColor(const QColor& color) {
    return static_cast<aether::u32>(color.red()) | (static_cast<aether::u32>(color.green()) << 8) |
           (static_cast<aether::u32>(color.blue()) << 16) | (static_cast<aether::u32>(color.alpha()) << 24);
}

} // namespace

BlueprintEditorWidget::BlueprintEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto* toolbar = new QHBoxLayout;
    auto* open = new QPushButton("Open…", this);
    open->setObjectName("blueprintOpenButton");
    auto* save = new QPushButton("Save", this);
    save->setObjectName("blueprintSaveButton");
    auto* compile = new QPushButton("Compile", this);
    compile->setObjectName("blueprintCompileButton");
    auto* undo = new QPushButton("Undo", this);
    auto* redo = new QPushButton("Redo", this);
    graph_combo_ = new QComboBox(this);
    graph_combo_->setObjectName("blueprintGraphSelector");
    file_label_ = new QLabel("No Blueprint open", this);
    file_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    toolbar->addWidget(open);
    toolbar->addWidget(save);
    toolbar->addWidget(compile);
    toolbar->addWidget(undo);
    toolbar->addWidget(redo);
    toolbar->addSpacing(8);
    toolbar->addWidget(new QLabel("GRAPH", this));
    toolbar->addWidget(graph_combo_);
    toolbar->addStretch(1);
    toolbar->addWidget(file_label_);
    root->addLayout(toolbar);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    scene_ = new BlueprintScene;
    view_ = new QGraphicsView(scene_, splitter);
    view_->setObjectName("blueprintGraphCanvas");
    view_->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    view_->setBackgroundBrush(QColor("#111820"));
    view_->setDragMode(QGraphicsView::RubberBandDrag);
    view_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    view_->setResizeAnchor(QGraphicsView::AnchorViewCenter);
    view_->setContextMenuPolicy(Qt::NoContextMenu);

    auto* side_tabs = new QTabWidget(splitter);
    auto* graph_side = new QWidget(side_tabs);
    auto* side_layout = new QVBoxLayout(graph_side);
    side_layout->setContentsMargins(8, 0, 0, 0);
    auto* palette_title = new QLabel("NODE PALETTE", graph_side);
    palette_title->setObjectName("sectionTitle");
    palette_filter_ = new QLineEdit(graph_side);
    palette_filter_->setObjectName("blueprintPaletteSearch");
    palette_filter_->setPlaceholderText("Search nodes…");
    palette_list_ = new QListWidget(graph_side);
    palette_list_->setObjectName("blueprintNodePalette");
    auto* add_node = new QPushButton("Add Node to Graph", graph_side);
    add_node->setObjectName("blueprintAddNodeButton");
    auto* add_comment = new QPushButton("Add Comment Box", graph_side);
    add_comment->setObjectName("blueprintAddCommentButton");
    auto* details_title = new QLabel("NODE DETAILS", graph_side);
    details_title->setObjectName("sectionTitle");
    node_details_title_ = new QLabel("Select a node to edit its comment and input defaults.", graph_side);
    node_details_title_->setWordWrap(true);
    node_details_title_->setObjectName("muted");
    node_comment_ = new QLineEdit(graph_side);
    node_comment_->setObjectName("blueprintNodeComment");
    node_comment_->setPlaceholderText("Node comment");
    breakpoint_toggle_ = new QPushButton("Set Breakpoint (F9)", graph_side);
    breakpoint_toggle_->setObjectName("blueprintToggleBreakpoint");
    breakpoint_toggle_->setEnabled(false);
    graph_comment_text_ = new QLineEdit(graph_side);
    graph_comment_text_->setObjectName("blueprintGraphCommentText");
    graph_comment_text_->setPlaceholderText("Comment box title");
    graph_comment_color_ = new QPushButton("Change Comment Color…", graph_side);
    graph_comment_color_->setObjectName("blueprintGraphCommentColor");
    graph_comment_remove_ = new QPushButton("Remove Comment Box", graph_side);
    graph_comment_remove_->setObjectName("blueprintGraphCommentRemove");
    graph_comment_text_->setEnabled(false);
    graph_comment_color_->setEnabled(false);
    graph_comment_remove_->setEnabled(false);
    auto* defaults_scroll = new QScrollArea(graph_side);
    defaults_scroll->setWidgetResizable(true);
    defaults_scroll->setFrameShape(QFrame::NoFrame);
    auto* defaults_content = new QWidget(defaults_scroll);
    pin_defaults_form_ = new QFormLayout(defaults_content);
    pin_defaults_form_->setContentsMargins(0, 0, 0, 0);
    pin_defaults_form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    defaults_scroll->setWidget(defaults_content);
    defaults_scroll->setMinimumHeight(90);
    auto* diagnostics_title = new QLabel("COMPILE RESULTS", graph_side);
    diagnostics_title->setObjectName("sectionTitle");
    diagnostics_ = new QPlainTextEdit(graph_side);
    diagnostics_->setObjectName("blueprintCompileResults");
    diagnostics_->setReadOnly(true);
    diagnostics_->setMaximumHeight(170);
    side_layout->addWidget(palette_title);
    side_layout->addWidget(palette_filter_);
    side_layout->addWidget(palette_list_, 1);
    side_layout->addWidget(add_node);
    side_layout->addWidget(add_comment);
    side_layout->addWidget(details_title);
    side_layout->addWidget(node_details_title_);
    side_layout->addWidget(node_comment_);
    side_layout->addWidget(breakpoint_toggle_);
    side_layout->addWidget(graph_comment_text_);
    side_layout->addWidget(graph_comment_color_);
    side_layout->addWidget(graph_comment_remove_);
    side_layout->addWidget(defaults_scroll, 1);
    side_layout->addWidget(diagnostics_title);
    side_layout->addWidget(diagnostics_);
    side_tabs->addTab(graph_side, "Graph");

    auto* members_page = new QWidget(side_tabs);
    auto* members_layout = new QVBoxLayout(members_page);
    members_layout->setContentsMargins(8, 8, 8, 8);
    auto* members_header = new QLabel("BLUEPRINT MEMBERS", members_page);
    members_header->setObjectName("sectionTitle");
    members_list_ = new QListWidget(members_page);
    members_list_->setObjectName("blueprintMembersList");
    members_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    auto* member_actions = new QHBoxLayout;
    auto* add_variable = new QPushButton("+ Variable", members_page);
    auto* add_function = new QPushButton("+ Function", members_page);
    auto* add_macro = new QPushButton("+ Macro", members_page);
    member_actions->addWidget(add_variable);
    member_actions->addWidget(add_function);
    member_actions->addWidget(add_macro);
    auto* rename_member = new QPushButton("Rename", members_page);
    auto* remove_member = new QPushButton("Remove", members_page);
    member_actions->addWidget(rename_member);
    member_actions->addWidget(remove_member);
    auto* member_details_scroll = new QScrollArea(members_page);
    member_details_scroll->setWidgetResizable(true);
    member_details_scroll->setFrameShape(QFrame::NoFrame);
    auto* member_details = new QWidget(member_details_scroll);
    member_details_form_ = new QFormLayout(member_details);
    member_details_form_->setContentsMargins(0, 0, 0, 0);
    member_details_form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    member_details_scroll->setWidget(member_details);
    members_layout->addWidget(members_header);
    members_layout->addWidget(members_list_, 1);
    members_layout->addLayout(member_actions);
    members_layout->addWidget(member_details_scroll, 1);
    side_tabs->addTab(members_page, "Members");
    splitter->addWidget(view_);
    splitter->addWidget(side_tabs);
    splitter->setStretchFactor(0, 1);
    splitter->setSizes({850, 290});
    root->addWidget(splitter, 1);

    status_label_ = new QLabel("Open a .abp asset to edit and compile a Blueprint.", this);
    status_label_->setObjectName("muted");
    root->addWidget(status_label_);

    auto* graph_scene = static_cast<BlueprintScene*>(scene_);
    graph_scene->link_handler = [this](aether::bp::NodeId from_node, const std::string& from_pin,
                                       aether::bp::NodeId to_node, const std::string& to_pin) {
        ConnectPins(from_node, from_pin, to_node, to_pin);
    };
    graph_scene->move_handler = [this](const auto& positions) { CommitNodePositions(positions); };
    graph_scene->comment_move_handler = [this](const auto& positions) { CommitCommentPositions(positions); };
    connect(open, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "Open Blueprint", QString(),
                                                           "Blueprints (*.abp);;All Files (*)");
        if (!path.isEmpty()) OpenFile(std::filesystem::path(path.toStdWString()));
    });
    connect(save, &QPushButton::clicked, this, [this] { Save(); });
    connect(compile, &QPushButton::clicked, this, [this] { Compile(); });
    connect(undo, &QPushButton::clicked, this, [this] { Undo(); });
    connect(redo, &QPushButton::clicked, this, [this] { Redo(); });
    connect(add_node, &QPushButton::clicked, this, [this] { AddSelectedNode(); });
    connect(add_comment, &QPushButton::clicked, this, [this] { AddGraphComment(); });
    connect(breakpoint_toggle_, &QPushButton::clicked, this, [this] { ToggleSelectedBreakpoint(); });
    auto* breakpoint_shortcut = new QShortcut(QKeySequence(Qt::Key_F9), this);
    breakpoint_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(breakpoint_shortcut, &QShortcut::activated, this, [this] { ToggleSelectedBreakpoint(); });
    connect(graph_comment_text_, &QLineEdit::editingFinished, this, [this] { EditSelectedCommentText(); });
    connect(graph_comment_color_, &QPushButton::clicked, this, [this] { EditSelectedCommentColor(); });
    connect(graph_comment_remove_, &QPushButton::clicked, this, [this] { RemoveSelectedComment(); });
    connect(palette_filter_, &QLineEdit::textChanged, this, [this] { RefreshPalette(); });
    connect(graph_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { selected_node_id_ = 0; selected_comment_index_ = -1; RefreshCanvas(); });
    connect(scene_, &QGraphicsScene::selectionChanged, this, [this] { RefreshNodeDetails(); });
    connect(members_list_, &QListWidget::currentItemChanged, this,
            [this] { RefreshMemberDetails(); });
    connect(add_variable, &QPushButton::clicked, this, [this] { AddVariable(); });
    connect(add_function, &QPushButton::clicked, this, [this] { AddFunction(false); });
    connect(add_macro, &QPushButton::clicked, this, [this] { AddFunction(true); });
    connect(rename_member, &QPushButton::clicked, this, [this] { RenameMember(); });
    connect(remove_member, &QPushButton::clicked, this, [this] { RemoveMember(); });
    connect(node_comment_, &QLineEdit::editingFinished, this, [this] {
        if (selected_node_id_ == 0) return;
        const std::string graph_name = graph_combo_->currentText().toStdString();
        const std::string comment = node_comment_->text().toStdString();
        const auto* graph = document_.Get().FindGraph(graph_name);
        const auto* node = graph ? graph->Find(selected_node_id_) : nullptr;
        if (!node || node->comment == comment) return;
        const auto id = selected_node_id_;
        document_.Edit("Node comment", [&](aether::bp::Blueprint& blueprint) {
            if (auto* edited_graph = blueprint.FindGraph(graph_name))
                if (auto* edited_node = edited_graph->Find(id)) edited_node->comment = comment;
        }, "node-comment:" + graph_name + ":" + std::to_string(id));
        SetStatus("Node comment updated · unsaved Blueprint changes");
    });
    palette_list_->setSortingEnabled(false);
}

BlueprintEditorWidget::~BlueprintEditorWidget() = default;

bool BlueprintEditorWidget::dirty() const { return document_.Dirty(); }
void BlueprintEditorWidget::SaveCurrent() { Save(); }
void BlueprintEditorWidget::UndoCurrent() { Undo(); }
void BlueprintEditorWidget::RedoCurrent() { Redo(); }

bool BlueprintEditorWidget::ConfirmDiscardChanges(QWidget* parent) const {
    if (!dirty()) return true;
    QMessageBox prompt(QMessageBox::Warning, "Unsaved Blueprint Changes",
                       "Save this Blueprint before continuing?", QMessageBox::Save | QMessageBox::Discard |
                       QMessageBox::Cancel, parent ? parent : const_cast<BlueprintEditorWidget*>(this));
    const auto choice = static_cast<QMessageBox::StandardButton>(prompt.exec());
    if (choice == QMessageBox::Save) {
        const_cast<BlueprintEditorWidget*>(this)->Save();
        return !document_.Dirty();
    }
    return choice == QMessageBox::Discard;
}

bool BlueprintEditorWidget::OpenFile(const std::filesystem::path& path, bool confirm_changes) {
    if (confirm_changes && !ConfirmDiscardChanges(this)) return false;
    std::string error;
    if (!document_.Load(path, &error)) {
        QMessageBox::critical(this, "Couldn't open Blueprint", QString::fromStdString(error));
        return false;
    }
    file_label_->setText(QString::fromStdWString(path.wstring()));
    file_label_->setToolTip(file_label_->text());
    diagnostics_->clear();
    selected_node_id_ = 0;
    selected_comment_index_ = -1;
    RefreshGraphs();
    RefreshPalette();
    RefreshMembers();
    SetStatus("Opened Blueprint · edit nodes, compile, then save");
    return true;
}

void BlueprintEditorWidget::ClearDocument() {
    document_ = aether::editor::BlueprintDocument{};
    selected_node_id_ = 0;
    selected_comment_index_ = -1;
    graph_combo_->clear();
    scene_->clear();
    palette_list_->clear();
    diagnostics_->clear();
    file_label_->setText("No Blueprint open");
    members_list_->clear();
    RefreshNodeDetails();
    RefreshMemberDetails();
    SetStatus("Open a .abp asset to edit and compile a Blueprint.");
}

void BlueprintEditorWidget::RefreshGraphs() {
    refreshing_ = true;
    const QString previous = graph_combo_->currentText();
    graph_combo_->clear();
    for (const auto& graph : document_.Get().graphs)
        graph_combo_->addItem(QString::fromStdString(graph.name));
    const int keep = graph_combo_->findText(previous);
    if (keep >= 0) graph_combo_->setCurrentIndex(keep);
    else if (graph_combo_->count() > 0) {
        const auto found = std::find_if(document_.Get().graphs.begin(), document_.Get().graphs.end(),
            [](const aether::bp::Graph& graph) { return graph.kind == aether::bp::GraphKind::EventGraph; });
        if (found != document_.Get().graphs.end()) graph_combo_->setCurrentText(QString::fromStdString(found->name));
    }
    refreshing_ = false;
    RefreshCanvas();
}

void BlueprintEditorWidget::RefreshCanvas() {
    if (refreshing_) return;
    const auto restore_node = selected_node_id_;
    const auto restore_comment = selected_comment_index_;
    scene_->clear();
    const std::string graph_name = graph_combo_->currentText().toStdString();
    const aether::bp::Graph* graph = document_.Get().FindGraph(graph_name);
    if (!graph) return;

    auto* graph_scene = static_cast<BlueprintScene*>(scene_);
    graph_scene->link_handler = [this](auto a, const auto& b, auto c, const auto& d) { ConnectPins(a, b, c, d); };
    graph_scene->move_handler = [this](const auto& positions) { CommitNodePositions(positions); };
    graph_scene->comment_move_handler = [this](const auto& positions) { CommitCommentPositions(positions); };
    std::map<std::pair<aether::bp::NodeId, std::string>, QGraphicsItem*> pins;
    std::set<std::pair<aether::bp::NodeId, std::string>> linked_inputs, linked_outputs;
    for (const auto& link : graph->links) {
        linked_inputs.insert({link.to.node, link.to.pin});
        linked_outputs.insert({link.from.node, link.from.pin});
    }

    for (int index = 0; index < static_cast<int>(graph->comments.size()); ++index) {
        const auto& comment = graph->comments[static_cast<std::size_t>(index)];
        auto* box = scene_->addRect(QRectF(0, 0, comment.width, comment.height),
                                    QPen(CommentColor(comment.color).darker(145), 1.5),
                                    CommentColor(comment.color));
        box->setPos(comment.x, comment.y);
        box->setZValue(-3.0);
        box->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable |
                      QGraphicsItem::ItemSendsGeometryChanges);
        box->setData(kCommentIndexRole, index);
        auto* title = scene_->addSimpleText(QString::fromStdString(comment.text), QFont("Segoe UI", 9, QFont::DemiBold));
        title->setBrush(QColor("#f2f5f8"));
        title->setPos(10, 6);
        title->setParentItem(box);
        title->setAcceptedMouseButtons(Qt::NoButton);
    }

    for (const aether::bp::Node& node : graph->nodes) {
        aether::bp::NodeError error;
        const auto signature = aether::bp::ResolveNode(document_.Get(), *graph, node, &error);
        const QString title = signature ? QString::fromStdString(signature->title)
                                        : QString::fromStdString(node.type + " · " + error.code);
        std::vector<aether::bp::PinDesc> inputs, outputs;
        if (signature) {
            for (const auto& pin : signature->pins)
                (pin.dir == aether::bp::PinDir::In ? inputs : outputs).push_back(pin);
        }
        const int rows = static_cast<int>(std::max<std::size_t>(1, std::max(inputs.size(), outputs.size())));
        const qreal width = std::max<qreal>(210.0, title.size() * 8.3 + 34.0);
        const qreal height = 39.0 + rows * 25.0;
        const QColor header = signature ? NodeColor(signature->category, signature->kind) : QColor("#7e3337");
        auto* box = scene_->addRect(QRectF(0, 0, width, height), QPen(QColor("#526170"), 1.0), QColor("#202a34"));
        box->setPos(node.x, node.y);
        box->setZValue(1.0);
        box->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
        box->setData(kNodeIdRole, node.id);
        const std::string breakpoint_key = document_.Path().generic_string() + "\n" + graph_name;
        const auto breakpoint_graph = breakpoints_.find(breakpoint_key);
        if (breakpoint_graph != breakpoints_.end() && breakpoint_graph->second.contains(node.id)) {
            auto* marker = scene_->addEllipse(QRectF(0, 0, 12, 12), QPen(QColor("#310d10"), 1.0), QColor("#ed3d4d"));
            marker->setParentItem(box);
            marker->setPos(width - 19, 9);
            marker->setAcceptedMouseButtons(Qt::NoButton);
            marker->setZValue(4.0);
        }
        auto* header_rect = scene_->addRect(QRectF(0, 0, width, 31), Qt::NoPen, header);
        header_rect->setParentItem(box);
        header_rect->setAcceptedMouseButtons(Qt::NoButton);
        auto* text = scene_->addSimpleText(title, QFont("Segoe UI", 9, QFont::DemiBold));
        text->setBrush(QColor("#f2f5f8"));
        text->setPos(10, 6);
        text->setParentItem(box);
        text->setAcceptedMouseButtons(Qt::NoButton);

        auto add_pins = [&](const std::vector<aether::bp::PinDesc>& descs, bool output) {
            for (int i = 0; i < static_cast<int>(descs.size()); ++i) {
                const auto& pin = descs[static_cast<size_t>(i)];
                const qreal row_y = 46.0 + i * 25.0;
                const qreal pin_x = output ? width - 5.0 : -5.0;
                auto* dot = scene_->addEllipse(QRectF(-5, -5, 10, 10), QPen(QColor("#111820"), 1.2), PinColor(pin.type));
                dot->setPos(pin_x, row_y);
                dot->setParentItem(box);
                dot->setZValue(3.0);
                dot->setData(kNodeIdRole, node.id);
                dot->setData(kPinNameRole, QString::fromStdString(pin.name));
                dot->setData(kPinOutputRole, output);
                dot->setData(kIsPinRole, true);
                dot->setToolTip(QString("%1 · %2").arg(QString::fromStdString(pin.name),
                                                        QString::fromStdString(aether::bp::TypeName(pin.type))));
                auto* label = scene_->addSimpleText(QString::fromStdString(pin.name), QFont("Segoe UI", 8));
                label->setBrush(QColor("#c2ccd5"));
                label->setPos(output ? width - label->boundingRect().width() - 13 : 12, row_y - 9);
                label->setParentItem(box);
                label->setAcceptedMouseButtons(Qt::NoButton);
                pins[{node.id, pin.name}] = dot;
            }
        };
        add_pins(inputs, false);
        add_pins(outputs, true);
        if (!node.comment.empty()) box->setToolTip(QString::fromStdString(node.comment));
    }

    for (const auto& link : graph->links) {
        const auto from = pins.find({link.from.node, link.from.pin});
        const auto to = pins.find({link.to.node, link.to.pin});
        if (from == pins.end() || to == pins.end()) continue;
        const QPointF a = from->second->scenePos();
        const QPointF b = to->second->scenePos();
        QPainterPath path(a);
        const qreal bend = std::max<qreal>(45.0, std::abs(b.x() - a.x()) * 0.45);
        path.cubicTo(a + QPointF(bend, 0), b - QPointF(bend, 0), b);
        auto* wire = scene_->addPath(path, QPen(QColor("#94a9bb"), 2.0));
        wire->setZValue(-1.0);
        wire->setAcceptedMouseButtons(Qt::NoButton);
    }
    if (restore_node != 0 || restore_comment >= 0) {
        for (QGraphicsItem* item : scene_->items()) {
            if (restore_node != 0 && item->data(kNodeIdRole).toUInt() == restore_node &&
                !item->data(kIsPinRole).toBool()) item->setSelected(true);
            if (restore_comment >= 0 && item->data(kCommentIndexRole).toInt() == restore_comment)
                item->setSelected(true);
        }
    }
    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-160, -120, 160, 120));
    RefreshNodeDetails();
    ApplyLiveTrace();
}

void BlueprintEditorWidget::SetLiveTraceEvents(
    const std::vector<std::tuple<std::filesystem::path, std::string, aether::bp::NodeId>>& events) {
    live_trace_node_ = 0;
    live_trace_fired_.clear();
    live_trace_asset_.clear();
    live_trace_graph_.clear();
    const auto current_path = document_.Path().lexically_normal();
    const std::string current_graph = graph_combo_->currentText().toStdString();
    for (const auto& [asset, graph, node] : events) {
        if (asset.lexically_normal() != current_path || graph != current_graph) continue;
        live_trace_asset_ = current_path;
        live_trace_graph_ = graph;
        live_trace_node_ = node;
        live_trace_fired_.insert(node);
    }
    ApplyLiveTrace();
}

void BlueprintEditorWidget::ClearLiveTrace() {
    live_trace_asset_.clear();
    live_trace_graph_.clear();
    live_trace_node_ = 0;
    live_trace_fired_.clear();
    ApplyLiveTrace();
}

void BlueprintEditorWidget::ApplyLiveTrace() {
    const bool matches = !live_trace_asset_.empty() &&
        live_trace_asset_.lexically_normal() == document_.Path().lexically_normal() &&
        live_trace_graph_ == graph_combo_->currentText().toStdString();
    for (QGraphicsItem* item : scene_->items()) {
        if (!item->data(kNodeIdRole).isValid() || item->data(kIsPinRole).toBool()) continue;
        const auto node = item->data(kNodeIdRole).toUInt();
        if (auto* rect = qgraphicsitem_cast<QGraphicsRectItem*>(item)) {
            if (matches && node == live_trace_node_)
                rect->setPen(QPen(QColor("#ffca62"), 3.0));
            else if (matches && live_trace_fired_.contains(node))
                rect->setPen(QPen(QColor("#4bd1a1"), 2.0));
            else
                rect->setPen(QPen(QColor("#526170"), 1.0));
        }
    }
}

void BlueprintEditorWidget::RefreshNodeDetails() {
    while (pin_defaults_form_->rowCount() > 0) pin_defaults_form_->removeRow(0);
    selected_node_id_ = 0;
    selected_comment_index_ = -1;
    const auto selected = scene_->selectedItems();
    const QGraphicsItem* selected_box = nullptr;
    for (const QGraphicsItem* item : selected) {
        const QGraphicsItem* candidate = item;
        while (candidate && candidate->parentItem()) candidate = candidate->parentItem();
        if (candidate && candidate->data(kCommentIndexRole).isValid()) {
            selected_box = candidate;
            break;
        }
        if (candidate && candidate->data(kNodeIdRole).isValid() && !candidate->data(kIsPinRole).toBool()) {
            selected_box = candidate;
            break;
        }
    }
    if (!selected_box) {
        node_details_title_->setText("Select a node or comment box to edit its properties.");
        node_comment_->clear();
        node_comment_->setEnabled(false);
        graph_comment_text_->clear();
        graph_comment_text_->setEnabled(false);
        graph_comment_color_->setEnabled(false);
        graph_comment_remove_->setEnabled(false);
        breakpoint_toggle_->setEnabled(false);
        return;
    }

    if (selected_box->data(kCommentIndexRole).isValid()) {
        selected_comment_index_ = selected_box->data(kCommentIndexRole).toInt();
        const auto* graph = document_.Get().FindGraph(graph_combo_->currentText().toStdString());
        if (!graph || selected_comment_index_ < 0 ||
            selected_comment_index_ >= static_cast<int>(graph->comments.size())) return;
        node_details_title_->setText("Comment box");
        node_comment_->clear();
        node_comment_->setEnabled(false);
        graph_comment_text_->setEnabled(true);
        graph_comment_text_->setText(QString::fromStdString(graph->comments[static_cast<std::size_t>(selected_comment_index_)].text));
        graph_comment_color_->setEnabled(true);
        graph_comment_remove_->setEnabled(true);
        breakpoint_toggle_->setEnabled(false);
        return;
    }

    selected_node_id_ = selected_box->data(kNodeIdRole).toUInt();
    graph_comment_text_->clear();
    graph_comment_text_->setEnabled(false);
    graph_comment_color_->setEnabled(false);
    graph_comment_remove_->setEnabled(false);
    breakpoint_toggle_->setEnabled(true);
    const std::string breakpoint_key = document_.Path().generic_string() + "\n" + graph_combo_->currentText().toStdString();
    const auto existing_breakpoints = breakpoints_.find(breakpoint_key);
    const bool breakpoint_set = existing_breakpoints != breakpoints_.end() && existing_breakpoints->second.contains(selected_node_id_);
    breakpoint_toggle_->setText(breakpoint_set ? "Clear Breakpoint (F9)" : "Set Breakpoint (F9)");
    const std::string graph_name = graph_combo_->currentText().toStdString();
    const auto* graph = document_.Get().FindGraph(graph_name);
    const auto* node = graph ? graph->Find(selected_node_id_) : nullptr;
    if (!node || !graph) return;
    const auto signature = aether::bp::ResolveNode(document_.Get(), *graph, *node);
    node_details_title_->setText(signature ? QString::fromStdString(signature->title)
                                           : QString::fromStdString(node->type));
    node_comment_->setEnabled(true);
    node_comment_->setText(QString::fromStdString(node->comment));
    if (!signature) return;

    for (const auto& pin : signature->pins) {
        if (pin.dir != aether::bp::PinDir::In || pin.type.IsExec()) continue;
        using aether::bp::ValueType;
        if (pin.type.type != ValueType::Bool && pin.type.type != ValueType::Int &&
            pin.type.type != ValueType::Float && pin.type.type != ValueType::String &&
            pin.type.type != ValueType::Vec3 && pin.type.type != ValueType::Quat) continue;
        const bool linked = std::any_of(graph->links.begin(), graph->links.end(), [&](const aether::bp::Link& link) {
            return link.to.node == node->id && link.to.pin == pin.name;
        });
        if (linked) continue;
        aether::bp::Value value = pin.default_value;
        if (node->defaults.contains(pin.name)) aether::bp::ValueFromJson(node->defaults[pin.name], pin.type, value);
        auto* editor = new QLineEdit(PinDefaultText(value, pin.type), this);
        editor->setObjectName(QString("blueprintPinDefault_%1_%2").arg(node->id).arg(QString::fromStdString(pin.name)));
        editor->setToolTip(QString("%1 default (%2)").arg(QString::fromStdString(pin.name),
                              QString::fromStdString(aether::bp::TypeName(pin.type))));
        pin_defaults_form_->addRow(QString::fromStdString(pin.name), editor);
        const auto node_id = node->id;
        const auto pin_name = pin.name;
        const auto pin_type = pin.type;
        connect(editor, &QLineEdit::editingFinished, this, [this, editor, graph_name, node_id, pin_name, pin_type] {
            aether::bp::Value edited;
            if (!ParsePinDefault(editor->text(), pin_type, edited)) {
                editor->setStyleSheet("border: 1px solid #d06060;");
                SetStatus("Invalid default value for " + QString::fromStdString(pin_name), true);
                return;
            }
            const auto* active_graph = document_.Get().FindGraph(graph_name);
            const auto* active_node = active_graph ? active_graph->Find(node_id) : nullptr;
            if (!active_node) return;
            const auto json = aether::bp::ValueToJson(edited);
            if (active_node->defaults.contains(pin_name) && active_node->defaults[pin_name] == json) return;
            document_.Edit("Set " + pin_name + " default", [&](aether::bp::Blueprint& blueprint) {
                if (auto* edited_graph = blueprint.FindGraph(graph_name))
                    if (auto* edited_node = edited_graph->Find(node_id)) edited_node->defaults[pin_name] = json;
            }, "pin-default:" + graph_name + ":" + std::to_string(node_id) + ":" + pin_name);
            editor->setStyleSheet(QString());
            SetStatus("Pin default updated · unsaved Blueprint changes");
        });
    }
}

void BlueprintEditorWidget::RefreshPalette() {
    const QString keep = palette_list_->currentItem() ? palette_list_->currentItem()->data(Qt::UserRole).toString() : QString();
    palette_list_->clear();
    const QString query = palette_filter_->text().trimmed();
    for (const auto& entry : aether::bp::ListNodeTypes(document_.Get())) {
        const QString title = QString::fromStdString(entry.title);
        const QString category = QString::fromStdString(entry.category);
        if (!query.isEmpty() && !title.contains(query, Qt::CaseInsensitive) &&
            !category.contains(query, Qt::CaseInsensitive) &&
            !QString::fromStdString(entry.id).contains(query, Qt::CaseInsensitive)) continue;
        auto* item = new QListWidgetItem(title + "\n" + category, palette_list_);
        item->setData(Qt::UserRole, QString::fromStdString(entry.id));
        item->setToolTip(QString::fromStdString(entry.id));
        if (item->data(Qt::UserRole).toString() == keep) palette_list_->setCurrentItem(item);
    }
}

void BlueprintEditorWidget::RefreshMembers() {
    const auto* current = members_list_->currentItem();
    const int keep_kind = current ? current->data(kMemberKindRole).toInt() : 0;
    const QString keep_name = current ? current->data(Qt::UserRole).toString() : QString();
    members_list_->clear();
    for (const auto& variable : document_.Get().variables) {
        auto* item = new QListWidgetItem(QString::fromStdString(variable.name) + "  ·  " +
                                         QString::fromStdString(aether::bp::TypeName(variable.type)), members_list_);
        item->setData(Qt::UserRole, QString::fromStdString(variable.name));
        item->setData(kMemberKindRole, kMemberVariable);
    }
    for (const auto& graph : document_.Get().graphs) {
        if (graph.kind == aether::bp::GraphKind::EventGraph) continue;
        const QString kind = graph.kind == aether::bp::GraphKind::Function ? "Function" : "Macro";
        auto* item = new QListWidgetItem(kind + "  ·  " + QString::fromStdString(graph.name), members_list_);
        item->setData(Qt::UserRole, QString::fromStdString(graph.name));
        item->setData(kMemberKindRole, kMemberGraph);
    }
    for (int i = 0; i < members_list_->count(); ++i) {
        auto* item = members_list_->item(i);
        if (item->data(kMemberKindRole).toInt() == keep_kind && item->data(Qt::UserRole).toString() == keep_name) {
            members_list_->setCurrentItem(item);
            break;
        }
    }
    if (!members_list_->currentItem()) RefreshMemberDetails();
}

void BlueprintEditorWidget::RefreshMemberDetails() {
    while (member_details_form_->rowCount() > 0) member_details_form_->removeRow(0);
    member_default_ = nullptr;
    member_tooltip_ = nullptr;
    member_type_ = nullptr;
    member_instance_editable_ = nullptr;
    member_expose_on_spawn_ = nullptr;
    const auto* item = members_list_->currentItem();
    if (!item) {
        member_details_form_->addRow(new QLabel("Select a variable, function, or macro.", this));
        return;
    }
    const QString name = item->data(Qt::UserRole).toString();
    const int kind = item->data(kMemberKindRole).toInt();
    if (kind == kMemberGraph) {
        const auto* graph = document_.Get().FindGraph(name.toStdString());
        if (!graph) return;
        const QString type = graph->kind == aether::bp::GraphKind::Function ? "Function" : "Macro";
        member_details_form_->addRow(new QLabel(type + " graph", this));
        const std::string graph_name = graph->name;
        auto* open_graph = new QPushButton("Open Graph", this);
        connect(open_graph, &QPushButton::clicked, this, [this, graph_name] {
            const int index = graph_combo_->findText(QString::fromStdString(graph_name));
            if (index >= 0) graph_combo_->setCurrentIndex(index);
            if (auto* tabs = findChild<QTabWidget*>()) tabs->setCurrentIndex(0);
        });
        member_details_form_->addRow(open_graph);

        if (graph->kind == aether::bp::GraphKind::Function) {
            auto* pure = new QCheckBox("Pure function", this);
            pure->setChecked(graph->pure);
            member_details_form_->addRow(pure);
            connect(pure, &QCheckBox::toggled, this, [this, graph_name](bool enabled) {
                document_.Edit("Set function purity", [&](aether::bp::Blueprint& blueprint) {
                    if (auto* edited = blueprint.FindGraph(graph_name)) edited->pure = enabled;
                });
                RefreshGraphs();
                RefreshPalette();
                RefreshMembers();
                SetStatus("Function purity updated · unsaved Blueprint changes");
            });
        }

        const bool allow_exec = graph->kind == aether::bp::GraphKind::Macro;
        auto edit_signature = [this, graph_name](const QString& label, bool inputs,
                                                 const std::function<void(std::vector<aether::bp::Variable>&)>& edit) {
            document_.Edit(label.toStdString(), [&](aether::bp::Blueprint& blueprint) {
                if (auto* edited = blueprint.FindGraph(graph_name)) edit(inputs ? edited->inputs : edited->outputs);
            });
            RefreshGraphs();
            RefreshPalette();
            RefreshMembers();
        };
        auto add_params = [&](const QString& section, bool inputs, const std::vector<aether::bp::Variable>& params) {
            auto* section_label = new QLabel(section, this);
            section_label->setObjectName("sectionTitle");
            member_details_form_->addRow(section_label);
            for (int index = 0; index < static_cast<int>(params.size()); ++index) {
                const auto param = params[static_cast<size_t>(index)];
                auto* row = new QWidget(this);
                auto* row_layout = new QHBoxLayout(row);
                row_layout->setContentsMargins(0, 0, 0, 0);
                auto* param_name = new QLineEdit(QString::fromStdString(param.name), row);
                param_name->setMaximumWidth(105);
                param_name->setObjectName(QString("blueprintSignatureName_%1_%2").arg(inputs ? "in" : "out").arg(index));
                auto* param_type = new QComboBox(row);
                const QStringList types = allow_exec
                    ? QStringList{"exec", "bool", "int", "float", "string", "Vec3", "Quat"}
                    : QStringList{"bool", "int", "float", "string", "Vec3", "Quat"};
                for (const QString& type_name : types) param_type->addItem(type_name, type_name);
                const QString current_type = QString::fromStdString(aether::bp::TypeName(param.type));
                if (param_type->findData(current_type) < 0) param_type->addItem(current_type, current_type);
                param_type->setCurrentIndex(param_type->findData(current_type));
                auto* remove = new QPushButton("×", row);
                remove->setMaximumWidth(28);
                row_layout->addWidget(param_name, 1);
                row_layout->addWidget(param_type, 1);
                row_layout->addWidget(remove);
                member_details_form_->addRow(row);

                connect(param_name, &QLineEdit::editingFinished, this,
                        [this, param_name, graph_name, index, inputs, param_name_before = param.name] {
                    const std::string edited_name = param_name->text().trimmed().toStdString();
                    const auto* current_graph = document_.Get().FindGraph(graph_name);
                    const auto& params_now = inputs ? current_graph->inputs : current_graph->outputs;
                    const bool duplicate = std::any_of(params_now.begin(), params_now.end(), [&](const auto& p) {
                        return p.name == edited_name && p.name != param_name_before;
                    });
                    if (!aether::editor::BlueprintDocument::IsValidName(edited_name) || duplicate) {
                        param_name->setText(QString::fromStdString(param_name_before));
                        SetStatus("Parameter names must be valid and unique within the signature.", true);
                        return;
                    }
                    if (edited_name == param_name_before) return;
                    document_.Edit("Rename signature parameter", [&](aether::bp::Blueprint& blueprint) {
                        auto* edited_graph = blueprint.FindGraph(graph_name);
                        auto& params_edit = inputs ? edited_graph->inputs : edited_graph->outputs;
                        if (static_cast<size_t>(index) < params_edit.size()) params_edit[static_cast<size_t>(index)].name = edited_name;
                    });
                    RefreshGraphs(); RefreshPalette(); RefreshMembers();
                    SetStatus("Signature parameter renamed · review compile diagnostics for affected links");
                });
                connect(param_type, qOverload<int>(&QComboBox::currentIndexChanged), this,
                        [this, param_type, graph_name, index, inputs, edit_signature](int) {
                    const auto parsed = aether::bp::ParseType(param_type->currentData().toString().toStdString());
                    if (!parsed) return;
                    edit_signature("Change signature parameter type", inputs,
                        [index, parsed = *parsed](std::vector<aether::bp::Variable>& values) {
                            if (static_cast<size_t>(index) >= values.size()) return;
                            auto& parameter = values[static_cast<size_t>(index)];
                            parameter.type = parsed;
                            parameter.default_value = parsed.IsExec() ? aether::bp::Value{} : aether::bp::DefaultValue(parsed);
                        });
                    SetStatus("Signature parameter type changed · review compile diagnostics for affected links");
                });
                connect(remove, &QPushButton::clicked, this, [edit_signature, inputs, index] {
                    edit_signature("Remove signature parameter", inputs, [index](std::vector<aether::bp::Variable>& values) {
                        if (static_cast<size_t>(index) < values.size()) values.erase(values.begin() + index);
                    });
                });
            }
            auto* add = new QPushButton("+ Add " + section, this);
            member_details_form_->addRow(add);
            connect(add, &QPushButton::clicked, this, [edit_signature, inputs] {
                edit_signature("Add signature parameter", inputs, [](std::vector<aether::bp::Variable>& values) {
                    int suffix = static_cast<int>(values.size());
                    aether::bp::Variable parameter;
                    do parameter.name = "Param" + std::to_string(suffix++);
                    while (std::any_of(values.begin(), values.end(), [&](const auto& value) { return value.name == parameter.name; }));
                    parameter.type = aether::bp::PinType::Of(aether::bp::ValueType::Float);
                    parameter.default_value = aether::bp::DefaultValue(parameter.type);
                    values.push_back(std::move(parameter));
                });
            });
        };
        add_params("Inputs", true, graph->inputs);
        add_params("Outputs", false, graph->outputs);
        return;
    }

    const auto found = std::find_if(document_.Get().variables.begin(), document_.Get().variables.end(),
        [&](const aether::bp::Variable& variable) { return variable.name == name.toStdString(); });
    if (found == document_.Get().variables.end()) return;
    const aether::bp::Variable variable = *found;
    member_details_form_->addRow(new QLabel("Variable properties", this));
    member_type_ = new QComboBox(this);
    const QStringList types = {"bool", "int", "float", "string", "Vec3", "Quat"};
    member_type_->addItems(types);
    const QString current_type = QString::fromStdString(aether::bp::TypeName(variable.type));
    member_type_->setCurrentText(current_type);
    member_details_form_->addRow("Type", member_type_);
    member_default_ = new QLineEdit(PinDefaultText(variable.default_value, variable.type), this);
    member_default_->setObjectName("blueprintVariableDefault");
    member_details_form_->addRow("Default", member_default_);
    member_instance_editable_ = new QCheckBox("Instance editable", this);
    member_instance_editable_->setChecked((variable.flags & aether::bp::Var_InstanceEditable) != 0);
    member_details_form_->addRow(member_instance_editable_);
    member_expose_on_spawn_ = new QCheckBox("Expose on spawn", this);
    member_expose_on_spawn_->setChecked((variable.flags & aether::bp::Var_ExposeOnSpawn) != 0);
    member_details_form_->addRow(member_expose_on_spawn_);
    member_tooltip_ = new QLineEdit(QString::fromStdString(variable.tooltip), this);
    member_tooltip_->setPlaceholderText("Describe this variable");
    member_details_form_->addRow("Tooltip", member_tooltip_);

    const std::string variable_name = variable.name;
    connect(member_type_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, variable_name](int) {
        const auto type = aether::bp::ParseType(member_type_->currentText().toStdString());
        if (!type || !document_.SetVariableType(variable_name, *type)) return;
        RefreshGraphs();
        RefreshMembers();
        RefreshPalette();
        SetStatus("Variable type updated · default reset and incompatible links removed");
    });
    connect(member_default_, &QLineEdit::editingFinished, this, [this, variable_name] {
        const auto* current_variable = document_.Get().FindVariable(variable_name);
        if (!current_variable) return;
        aether::bp::Value value;
        if (!ParsePinDefault(member_default_->text(), current_variable->type, value)) {
            member_default_->setStyleSheet("border: 1px solid #d06060;");
            SetStatus("Invalid default value for variable " + QString::fromStdString(variable_name), true);
            return;
        }
        const auto json = aether::bp::ValueToJson(value);
        if (aether::bp::ValueToJson(current_variable->default_value) == json) return;
        document_.Edit("Set default of " + variable_name, [&](aether::bp::Blueprint& blueprint) {
            for (auto& edited : blueprint.variables)
                if (edited.name == variable_name) edited.default_value = value;
        }, "variable-default:" + variable_name);
        member_default_->setStyleSheet(QString());
        SetStatus("Variable default updated · unsaved Blueprint changes");
    });
    auto update_flags_and_tooltip = [this, variable_name] {
        const u32 flags = (member_instance_editable_->isChecked() ? aether::bp::Var_InstanceEditable : 0u) |
                          (member_expose_on_spawn_->isChecked() ? aether::bp::Var_ExposeOnSpawn : 0u);
        const std::string tooltip = member_tooltip_->text().toStdString();
        const auto* current_variable = document_.Get().FindVariable(variable_name);
        if (!current_variable || (current_variable->flags == flags && current_variable->tooltip == tooltip)) return;
        document_.Edit("Edit variable " + variable_name, [&](aether::bp::Blueprint& blueprint) {
            for (auto& edited : blueprint.variables) {
                if (edited.name == variable_name) edited.flags = flags, edited.tooltip = tooltip;
            }
        }, "variable-metadata:" + variable_name);
        SetStatus("Variable properties updated · unsaved Blueprint changes");
    };
    connect(member_instance_editable_, &QCheckBox::toggled, this, [update_flags_and_tooltip] { update_flags_and_tooltip(); });
    connect(member_expose_on_spawn_, &QCheckBox::toggled, this, [update_flags_and_tooltip] { update_flags_and_tooltip(); });
    connect(member_tooltip_, &QLineEdit::editingFinished, this, [update_flags_and_tooltip] { update_flags_and_tooltip(); });
}

void BlueprintEditorWidget::AddVariable() {
    const QStringList types = {"bool", "int", "float", "string", "Vec3", "Quat"};
    bool ok = false;
    const QString selected = QInputDialog::getItem(this, "Add Blueprint Variable", "Type", types, 2, false, &ok);
    if (!ok) return;
    const auto type = aether::bp::ParseType(selected.toStdString());
    if (!type) return;
    const std::string name = document_.AddVariable(*type);
    RefreshMembers();
    RefreshPalette();
    for (int i = 0; i < members_list_->count(); ++i)
        if (members_list_->item(i)->data(Qt::UserRole).toString() == QString::fromStdString(name))
            members_list_->setCurrentRow(i);
    SetStatus("Added variable " + QString::fromStdString(name));
}

void BlueprintEditorWidget::AddFunction(bool macro) {
    const std::string name = macro ? document_.AddMacro() : document_.AddFunction();
    RefreshGraphs();
    RefreshPalette();
    RefreshMembers();
    const int index = graph_combo_->findText(QString::fromStdString(name));
    if (index >= 0) graph_combo_->setCurrentIndex(index);
    SetStatus(QString("Added %1 %2").arg(macro ? "macro" : "function", QString::fromStdString(name)));
}

void BlueprintEditorWidget::RenameMember() {
    const auto* item = members_list_->currentItem();
    if (!item) return;
    const QString from = item->data(Qt::UserRole).toString();
    bool ok = false;
    const QString to = QInputDialog::getText(this, "Rename Blueprint Member", "Name", QLineEdit::Normal, from, &ok).trimmed();
    if (!ok || to.isEmpty() || to == from) return;
    std::string error;
    const bool renamed = item->data(kMemberKindRole).toInt() == kMemberVariable
        ? document_.RenameVariable(from.toStdString(), to.toStdString(), &error)
        : document_.RenameGraph(from.toStdString(), to.toStdString(), &error);
    if (!renamed) {
        SetStatus(QString::fromStdString(error), true);
        return;
    }
    RefreshGraphs();
    RefreshMembers();
    RefreshPalette();
    SetStatus("Renamed Blueprint member");
}

void BlueprintEditorWidget::RemoveMember() {
    const auto* item = members_list_->currentItem();
    if (!item) return;
    const QString name = item->data(Qt::UserRole).toString();
    const int kind = item->data(kMemberKindRole).toInt();
    if (kind == kMemberVariable) document_.RemoveVariable(name.toStdString());
    else {
        std::string error;
        if (!document_.RemoveGraph(name.toStdString(), &error)) {
            SetStatus(QString::fromStdString(error), true);
            return;
        }
    }
    RefreshGraphs();
    RefreshMembers();
    RefreshPalette();
    SetStatus("Removed Blueprint member · undo is available");
}

void BlueprintEditorWidget::Save() {
    std::string error;
    if (document_.Path().empty()) {
        const QString path = QFileDialog::getSaveFileName(this, "Save Blueprint", QString(), "Blueprints (*.abp)");
        if (path.isEmpty()) return;
        if (!document_.SaveAs(std::filesystem::path(path.toStdWString()), &error)) {
            QMessageBox::critical(this, "Couldn't save Blueprint", QString::fromStdString(error));
            return;
        }
    } else if (!document_.Save(&error)) {
        QMessageBox::critical(this, "Couldn't save Blueprint", QString::fromStdString(error));
        return;
    }
    file_label_->setText(QString::fromStdWString(document_.Path().wstring()));
    SetStatus("Saved Blueprint · " + QString::fromStdWString(document_.Path().filename().wstring()));
}

void BlueprintEditorWidget::Compile() {
    const auto& result = document_.Compile();
    QStringList lines;
    for (const auto& diagnostic : result.diagnostics.diagnostics) {
        const QString severity = diagnostic.severity == aether::bp::Severity::Error ? "ERROR" : "WARNING";
        QString line = QString("%1  %2 · %3").arg(severity, QString::fromStdString(diagnostic.code),
                                                     QString::fromStdString(diagnostic.graph));
        if (diagnostic.node) line += QString(" · node %1").arg(diagnostic.node);
        line += "\n    " + QString::fromStdString(diagnostic.message);
        lines.push_back(line);
    }
    diagnostics_->setPlainText(lines.isEmpty() ? "No diagnostics. Blueprint compiled successfully." : lines.join("\n"));
    SetStatus(result.Ok() ? QString("Compiled · %1 warning(s)").arg(result.diagnostics.warnings)
                          : QString("Compile failed · %1 error(s)").arg(result.diagnostics.errors), !result.Ok());
}

void BlueprintEditorWidget::AddSelectedNode() {
    if (document_.Path().empty()) {
        SetStatus("Open a Blueprint before adding nodes.", true);
        return;
    }
    const auto* item = palette_list_->currentItem();
    const std::string graph_name = graph_combo_->currentText().toStdString();
    const auto* current_graph = document_.Get().FindGraph(graph_name);
    if (!item || !current_graph) return;
    const std::string type = item->data(Qt::UserRole).toString().toStdString();
    aether::bp::Node candidate;
    candidate.id = current_graph->NextId();
    candidate.type = type;
    aether::bp::NodeError node_error;
    if (!aether::bp::ResolveNode(document_.Get(), *current_graph, candidate, &node_error)) {
        SetStatus(QString::fromStdString(node_error.message), true);
        return;
    }
    document_.Edit("Add " + type, [&](aether::bp::Blueprint& blueprint) {
        auto* graph = blueprint.FindGraph(graph_name);
        candidate.id = graph->NextId();
        candidate.x = static_cast<float>(view_->mapToScene(view_->viewport()->rect().center()).x());
        candidate.y = static_cast<float>(view_->mapToScene(view_->viewport()->rect().center()).y());
        graph->nodes.push_back(candidate);
    });
    RefreshCanvas();
    SetStatus("Node added · drag pins to create a connection");
}

void BlueprintEditorWidget::ConnectPins(aether::bp::NodeId from_node, const std::string& from_pin,
                                        aether::bp::NodeId to_node, const std::string& to_pin) {
    const std::string graph_name = graph_combo_->currentText().toStdString();
    const auto* graph = document_.Get().FindGraph(graph_name);
    if (!graph) return;
    const auto* from = graph->Find(from_node);
    const auto* to = graph->Find(to_node);
    if (!from || !to || from_node == to_node) return;
    const auto out_signature = aether::bp::ResolveNode(document_.Get(), *graph, *from);
    const auto in_signature = aether::bp::ResolveNode(document_.Get(), *graph, *to);
    if (!out_signature || !in_signature) return;
    const auto* out = out_signature->Find(from_pin, aether::bp::PinDir::Out);
    const auto* in = in_signature->Find(to_pin, aether::bp::PinDir::In);
    if (!out || !in || aether::bp::CanConnect(out->type, in->type) == aether::bp::Compat::No) {
        SetStatus("Those pins can't be connected.", true);
        return;
    }
    document_.Edit("Connect Blueprint pins", [&](aether::bp::Blueprint& blueprint) {
        auto* edited = blueprint.FindGraph(graph_name);
        edited->links.erase(std::remove_if(edited->links.begin(), edited->links.end(), [&](const aether::bp::Link& link) {
            const bool same_input = !in->type.IsExec() && link.to.node == to_node && link.to.pin == to_pin;
            const bool same_exec_output = out->type.IsExec() && link.from.node == from_node && link.from.pin == from_pin;
            return same_input || same_exec_output;
        }), edited->links.end());
        edited->links.push_back({{from_node, from_pin}, {to_node, to_pin}});
    });
    RefreshCanvas();
    SetStatus("Pins connected");
}

void BlueprintEditorWidget::CommitNodePositions(
    const std::vector<std::pair<aether::bp::NodeId, std::pair<float, float>>>& positions) {
    if (positions.empty()) return;
    const std::string graph_name = graph_combo_->currentText().toStdString();
    document_.Edit("Move Blueprint nodes", [&](aether::bp::Blueprint& blueprint) {
        auto* graph = blueprint.FindGraph(graph_name);
        if (!graph) return;
        for (const auto& [id, position] : positions) {
            if (auto* node = graph->Find(id)) node->x = position.first, node->y = position.second;
        }
    }, "move-blueprint-nodes");
}

void BlueprintEditorWidget::AddGraphComment() {
    if (document_.Path().empty()) {
        SetStatus("Open a Blueprint before adding comment boxes.", true);
        return;
    }
    const std::string graph_name = graph_combo_->currentText().toStdString();
    const auto* graph = document_.Get().FindGraph(graph_name);
    if (!graph) return;

    QRectF bounds;
    bool has_nodes = false;
    for (QGraphicsItem* item : scene_->selectedItems()) {
        const QGraphicsItem* root = item;
        while (root && root->parentItem()) root = root->parentItem();
        if (!root || !root->data(kNodeIdRole).isValid() || root->data(kIsPinRole).toBool()) continue;
        const QRectF node_bounds = root->sceneBoundingRect();
        bounds = has_nodes ? bounds.united(node_bounds) : node_bounds;
        has_nodes = true;
    }
    if (!has_nodes) {
        const QPointF center = view_->mapToScene(view_->viewport()->rect().center());
        bounds = QRectF(center.x() - 110, center.y() - 55, 220, 110);
    }
    if (!document_.AddComment(graph_name, static_cast<float>(bounds.left()), static_cast<float>(bounds.top()),
                              static_cast<float>(bounds.right()), static_cast<float>(bounds.bottom()), "Comment")) return;
    const auto* updated_graph = document_.Get().FindGraph(graph_name);
    selected_node_id_ = 0;
    selected_comment_index_ = updated_graph ? static_cast<int>(updated_graph->comments.size()) - 1 : -1;
    RefreshCanvas();
    SetStatus("Comment box added · drag to organize nodes");
}

void BlueprintEditorWidget::CommitCommentPositions(
    const std::vector<std::tuple<int, float, float, float, float>>& positions) {
    if (positions.empty()) return;
    const std::string graph_name = graph_combo_->currentText().toStdString();
    document_.Edit("Move Blueprint comment box", [&](aether::bp::Blueprint& blueprint) {
        auto* graph = blueprint.FindGraph(graph_name);
        if (!graph) return;
        for (const auto& [index, old_x, old_y, new_x, new_y] : positions) {
            if (index < 0 || index >= static_cast<int>(graph->comments.size())) continue;
            auto& comment = graph->comments[static_cast<std::size_t>(index)];
            const float dx = new_x - old_x;
            const float dy = new_y - old_y;
            const float right = old_x + comment.width;
            const float bottom = old_y + comment.height;
            for (auto& node : graph->nodes) {
                if (node.x >= old_x && node.x <= right && node.y >= old_y && node.y <= bottom) {
                    node.x += dx;
                    node.y += dy;
                }
            }
            comment.x = new_x;
            comment.y = new_y;
        }
    }, "move-blueprint-comment");
    RefreshCanvas();
}

void BlueprintEditorWidget::EditSelectedCommentText() {
    if (selected_comment_index_ < 0) return;
    const std::string graph_name = graph_combo_->currentText().toStdString();
    const auto* graph = document_.Get().FindGraph(graph_name);
    if (!graph || selected_comment_index_ >= static_cast<int>(graph->comments.size())) return;
    const int index = selected_comment_index_;
    const std::string text = graph_comment_text_->text().toStdString();
    if (graph->comments[static_cast<std::size_t>(index)].text == text) return;
    document_.Edit("Edit comment box", [&](aether::bp::Blueprint& blueprint) {
        if (auto* edited = blueprint.FindGraph(graph_name);
            edited && index >= 0 && index < static_cast<int>(edited->comments.size()))
            edited->comments[static_cast<std::size_t>(index)].text = text;
    }, "blueprint-comment-text:" + graph_name + ":" + std::to_string(index));
    RefreshCanvas();
    SetStatus("Comment box text updated · unsaved Blueprint changes");
}

void BlueprintEditorWidget::EditSelectedCommentColor() {
    if (selected_comment_index_ < 0) return;
    const std::string graph_name = graph_combo_->currentText().toStdString();
    const auto* graph = document_.Get().FindGraph(graph_name);
    if (!graph || selected_comment_index_ >= static_cast<int>(graph->comments.size())) return;
    const int index = selected_comment_index_;
    const QColor initial = CommentColor(graph->comments[static_cast<std::size_t>(index)].color);
    const QColor color = QColorDialog::getColor(initial, this, "Comment Box Color", QColorDialog::ShowAlphaChannel);
    if (!color.isValid()) return;
    const auto stored = StoredCommentColor(color);
    document_.Edit("Set comment box color", [&](aether::bp::Blueprint& blueprint) {
        if (auto* edited = blueprint.FindGraph(graph_name);
            edited && index >= 0 && index < static_cast<int>(edited->comments.size()))
            edited->comments[static_cast<std::size_t>(index)].color = stored;
    }, "blueprint-comment-color:" + graph_name + ":" + std::to_string(index));
    RefreshCanvas();
    SetStatus("Comment box color updated · unsaved Blueprint changes");
}

void BlueprintEditorWidget::RemoveSelectedComment() {
    if (selected_comment_index_ < 0) return;
    const std::string graph_name = graph_combo_->currentText().toStdString();
    const int index = selected_comment_index_;
    document_.Edit("Remove comment box", [&](aether::bp::Blueprint& blueprint) {
        if (auto* graph = blueprint.FindGraph(graph_name);
            graph && index >= 0 && index < static_cast<int>(graph->comments.size()))
            graph->comments.erase(graph->comments.begin() + index);
    });
    selected_comment_index_ = -1;
    RefreshCanvas();
    SetStatus("Comment box removed · nodes were kept in place");
}

void BlueprintEditorWidget::ToggleSelectedBreakpoint() {
    if (selected_node_id_ == 0 || document_.Path().empty()) return;
    const std::string graph = graph_combo_->currentText().toStdString();
    const std::string key = document_.Path().generic_string() + "\n" + graph;
    auto& nodes = breakpoints_[key];
    const auto node = selected_node_id_;
    const bool enabled = !nodes.contains(node);
    if (enabled) nodes.insert(node);
    else nodes.erase(node);
    breakpoint_toggle_->setText(enabled ? "Clear Breakpoint (F9)" : "Set Breakpoint (F9)");
    RefreshCanvas();
    if (breakpoint_handler_) breakpoint_handler_(document_.Path(), graph, node, enabled);
    SetStatus(enabled ? QString("Breakpoint set · %1 / node %2").arg(QString::fromStdString(graph)).arg(node)
                      : QString("Breakpoint cleared · %1 / node %2").arg(QString::fromStdString(graph)).arg(node));
}

std::vector<BlueprintEditorWidget::Breakpoint> BlueprintEditorWidget::Breakpoints() const {
    std::vector<Breakpoint> result;
    for (const auto& [key, nodes] : breakpoints_) {
        const auto separator = key.find('\n');
        if (separator == std::string::npos) continue;
        const std::filesystem::path path(key.substr(0, separator));
        const std::string graph = key.substr(separator + 1);
        for (const auto node : nodes) result.push_back({path, graph, node});
    }
    return result;
}

void BlueprintEditorWidget::Undo() {
    if (document_.Undo()) { RefreshGraphs(); RefreshPalette(); RefreshMembers(); SetStatus("Undo complete"); }
}

void BlueprintEditorWidget::Redo() {
    if (document_.Redo()) { RefreshGraphs(); RefreshPalette(); RefreshMembers(); SetStatus("Redo complete"); }
}

void BlueprintEditorWidget::SetStatus(const QString& text, bool error) {
    status_label_->setText(text);
    status_label_->setStyleSheet(error ? "color: #ff8b83;" : QString());
}

} // namespace aether::editor::qt
