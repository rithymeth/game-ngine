#include "tilemap_editor_widget.h"

#include "aether/assets/asset_database.h"
#include "aether/sprite2d/tilemap.h"

#include <QComboBox>
#include <QFileInfo>
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QKeySequence>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace aether::editor::qt {
namespace {

QColor TileColor(i32 tile) {
    const u32 h = static_cast<u32>(tile) * 2654435761u;
    const qreal hue = static_cast<qreal>((h >> 8) & 0xFFFF) / 65535.0;
    return QColor::fromHsvF(hue, 0.55, 0.88);
}

class TileCanvas final : public QWidget {
public:
    explicit TileCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(360, 260);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
    }

    TilemapEditDocument* document = nullptr;
    std::function<void()> changed;
    qreal zoom = 28.0;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#171b20"));
        if (!document) return;
        const auto& map = document->Map();
        const auto& tileset = document->GetTileset();
        const qreal ox = (width() - static_cast<qreal>(map.width) * zoom) * 0.5 + pan_.x();
        const qreal oy = (height() + static_cast<qreal>(map.height) * zoom) * 0.5 + pan_.y();
        const i32 x0 = std::max<i32>(0, static_cast<i32>(std::floor(-ox / zoom)));
        const i32 x1 = std::min<i32>(static_cast<i32>(map.width), static_cast<i32>(std::ceil((width() - ox) / zoom)));
        const i32 y0 = std::max<i32>(0, static_cast<i32>(std::floor((oy - height()) / zoom)));
        const i32 y1 = std::min<i32>(static_cast<i32>(map.height), static_cast<i32>(std::ceil(oy / zoom)));
        painter.setRenderHint(QPainter::Antialiasing, false);
        for (i32 layer = 0; layer < static_cast<i32>(map.layers.size()); ++layer) {
            for (i32 y = y0; y < y1; ++y) {
                for (i32 x = x0; x < x1; ++x) {
                    const i32 tile = sprite2d::ResolveTile(map, tileset, static_cast<usize>(layer), x, y);
                    if (tile < 0) continue;
                    const QRectF cell(ox + x * zoom, oy - (y + 1) * zoom, zoom, zoom);
                    QColor color = TileColor(tile);
                    color.setAlpha(layer == static_cast<i32>(document->active_layer) ? 255 : 100);
                    painter.fillRect(cell, color);
                    if (tileset.IsSolid(tile) && map.layers[static_cast<usize>(layer)].collides)
                        painter.setPen(QPen(QColor("#ff7373"), 1.0));
                    else
                        painter.setPen(QPen(QColor(255, 255, 255, 35), 1.0));
                    painter.drawRect(cell);
                }
            }
        }
        if (hover_cell_.x() >= 0 && hover_cell_.y() >= 0 && map.InBounds(hover_cell_.x(), hover_cell_.y())) {
            painter.setPen(QPen(QColor("#ffffff"), 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(ox + hover_cell_.x() * zoom, oy - (hover_cell_.y() + 1) * zoom, zoom, zoom));
        }
        if (rectangle_active_) {
            const i32 ax = std::min(rect_start_.x(), hover_cell_.x());
            const i32 ay = std::min(rect_start_.y(), hover_cell_.y());
            const i32 bx = std::max(rect_start_.x(), hover_cell_.x());
            const i32 by = std::max(rect_start_.y(), hover_cell_.y());
            painter.setPen(QPen(QColor("#9fd4ff"), 2.0, Qt::DashLine));
            painter.setBrush(QColor(120, 190, 255, 45));
            painter.drawRect(QRectF(ox + ax * zoom, oy - (by + 1) * zoom, (bx - ax + 1) * zoom, (by - ay + 1) * zoom));
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (!document) return;
        if (event->button() == Qt::MiddleButton) {
            panning_ = true;
            pan_start_ = event->position().toPoint();
            setCursor(Qt::ClosedHandCursor);
            return;
        }
        if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton) return;
        UpdateCell(event->position().toPoint());
        if (event->button() == Qt::LeftButton && document->tool == TileTool::Rectangle) {
            rectangle_active_ = true;
            rect_start_ = hover_cell_;
            update();
            return;
        }
        if (event->button() == Qt::LeftButton && (document->tool == TileTool::Fill || document->tool == TileTool::Pick)) {
            document->Apply(hover_cell_.x(), hover_cell_.y());
            if (changed) changed();
            update();
            return;
        }
        if (event->button() == Qt::RightButton) saved_tool_ = document->tool, document->tool = TileTool::Erase;
        document->BeginStroke();
        painting_ = true;
        document->Apply(hover_cell_.x(), hover_cell_.y());
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (panning_) {
            const QPoint current = event->position().toPoint();
            pan_ += current - pan_start_;
            pan_start_ = current;
            update();
            return;
        }
        UpdateCell(event->position().toPoint());
        if (painting_ && document) document->Apply(hover_cell_.x(), hover_cell_.y());
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::MiddleButton && panning_) {
            panning_ = false;
            unsetCursor();
            return;
        }
        if (event->button() == Qt::LeftButton && rectangle_active_ && document) {
            UpdateCell(event->position().toPoint());
            rectangle_active_ = false;
            const auto& map = document->Map();
            const i32 max_x = map.width == 0 ? 0 : static_cast<i32>(map.width) - 1;
            const i32 max_y = map.height == 0 ? 0 : static_cast<i32>(map.height) - 1;
            document->Rectangle(std::clamp(rect_start_.x(), 0, max_x), std::clamp(rect_start_.y(), 0, max_y),
                                std::clamp(hover_cell_.x(), 0, max_x), std::clamp(hover_cell_.y(), 0, max_y));
            if (changed) changed();
            update();
            return;
        }
        if (painting_ && document) {
            painting_ = false;
            document->EndStroke();
            document->tool = saved_tool_;
            if (changed) changed();
            update();
        }
    }

    void wheelEvent(QWheelEvent* event) override {
        const qreal factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        zoom = std::clamp(zoom * factor, 8.0, 96.0);
        update();
        event->accept();
    }

private:
    void UpdateCell(QPoint point) {
        if (!document) return;
        const auto& map = document->Map();
        const qreal ox = (width() - static_cast<qreal>(map.width) * zoom) * 0.5 + pan_.x();
        const qreal oy = (height() + static_cast<qreal>(map.height) * zoom) * 0.5 + pan_.y();
        hover_cell_ = QPoint(static_cast<int>(std::floor((point.x() - ox) / zoom)),
                             static_cast<int>(std::floor((oy - point.y()) / zoom)));
    }

    QPoint pan_, pan_start_, hover_cell_{-1, -1}, rect_start_{-1, -1};
    TileTool saved_tool_ = TileTool::Paint;
    bool panning_ = false, painting_ = false, rectangle_active_ = false;
};

} // namespace

TilemapEditorWidget::TilemapEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    auto* toolbar = new QHBoxLayout;
    tool_ = new QComboBox(this);
    for (const char* name : {"Paint", "Erase", "Rectangle", "Fill", "Pick"}) tool_->addItem(name);
    layer_ = new QComboBox(this);
    undo_ = new QPushButton("Undo", this);
    redo_ = new QPushButton("Redo", this);
    auto* save = new QPushButton("Save", this);
    toolbar->addWidget(new QLabel("Tool", this));
    toolbar->addWidget(tool_);
    toolbar->addWidget(new QLabel("Layer", this));
    toolbar->addWidget(layer_);
    toolbar->addWidget(undo_);
    toolbar->addWidget(redo_);
    toolbar->addStretch(1);
    toolbar->addWidget(save);
    root->addLayout(toolbar);
    auto* split = new QSplitter(this);
    palette_ = new QListWidget(split);
    palette_->setMinimumWidth(130);
    canvas_ = new TileCanvas(split);
    split->addWidget(palette_);
    split->addWidget(canvas_);
    split->setStretchFactor(1, 1);
    root->addWidget(split, 1);
    status_ = new QLabel("Open a .atilemap from the Content Browser to begin painting", this);
    root->addWidget(status_);

    QObject::connect(tool_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (document_ && !refreshing_) document_->tool = static_cast<TileTool>(index);
    });
    QObject::connect(layer_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (!document_ || refreshing_ || index < 0) return;
        document_->active_layer = static_cast<usize>(index);
        Refresh();
    });
    QObject::connect(palette_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (document_ && !refreshing_ && row >= 0) document_->brush = palette_->item(row)->data(Qt::UserRole).toInt();
    });
    QObject::connect(undo_, &QPushButton::clicked, this, [this] { UndoCurrent(); });
    QObject::connect(redo_, &QPushButton::clicked, this, [this] { RedoCurrent(); });
    QObject::connect(save, &QPushButton::clicked, this, [this] { SaveCurrent(); });
    auto* save_shortcut = new QShortcut(QKeySequence::Save, this);
    QObject::connect(save_shortcut, &QShortcut::activated, this, [this] { SaveCurrent(); });
    auto* undo_shortcut = new QShortcut(QKeySequence::Undo, this);
    QObject::connect(undo_shortcut, &QShortcut::activated, this, [this] { UndoCurrent(); });
    auto* redo_shortcut = new QShortcut(QKeySequence::Redo, this);
    QObject::connect(redo_shortcut, &QShortcut::activated, this, [this] { RedoCurrent(); });
    static_cast<TileCanvas*>(canvas_)->changed = [this] { Refresh(); };
    static_cast<TileCanvas*>(canvas_)->document = nullptr;
    Refresh();
}

bool TilemapEditorWidget::OpenFile(const std::filesystem::path& tilemap_path, const std::filesystem::path& content_root) {
    if (!ConfirmDiscardChanges(this)) return false;
    sprite2d::TilemapData map;
    std::string error;
    if (!sprite2d::LoadTilemap(tilemap_path, map, &error)) {
        SetStatus(QString::fromStdString(error), true);
        return false;
    }
    assets::AssetDatabase database(content_root);
    database.Scan();
    const assets::AssetRecord* record = database.Find(map.tileset);
    if (!record || record->importer != "Tileset") {
        SetStatus("The tilemap references a tileset that is missing from this project's Content folder", true);
        return false;
    }
    sprite2d::Tileset tileset;
    const auto tileset_path = content_root / std::filesystem::path(record->path);
    if (!sprite2d::LoadTileset(tileset_path, tileset, &error)) {
        SetStatus(QString::fromStdString(error), true);
        return false;
    }
    document_ = std::make_unique<TilemapEditDocument>(std::move(map), std::move(tileset));
    tilemap_path_ = tilemap_path;
    tileset_path_ = tileset_path;
    content_root_ = content_root;
    static_cast<TileCanvas*>(canvas_)->document = document_.get();
    Refresh();
    SetStatus(QString("Opened %1").arg(QFileInfo(QString::fromStdWString(tilemap_path.wstring())).fileName()));
    return true;
}

void TilemapEditorWidget::ClearDocument() {
    document_.reset();
    tilemap_path_.clear();
    tileset_path_.clear();
    content_root_.clear();
    static_cast<TileCanvas*>(canvas_)->document = nullptr;
    Refresh();
}

bool TilemapEditorWidget::ConfirmDiscardChanges(QWidget* parent) const {
    if (!dirty()) return true;
    const auto answer = QMessageBox::question(parent, "Unsaved Tilemap", "Save tilemap and tileset changes before closing?",
                                               QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                               QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Discard) return true;
    const_cast<TilemapEditorWidget*>(this)->SaveCurrent();
    return !dirty();
}

void TilemapEditorWidget::SaveCurrent() {
    if (!document_) return;
    std::string error;
    if (!document_->Save(tilemap_path_, tileset_path_, &error)) {
        SetStatus(QString::fromStdString(error), true);
        return;
    }
    assets::AssetDatabase database(content_root_);
    database.Scan();
    SetStatus("Tilemap and tileset saved");
    Refresh();
}

void TilemapEditorWidget::UndoCurrent() {
    if (document_ && document_->Undo()) Refresh();
}

void TilemapEditorWidget::RedoCurrent() {
    if (document_ && document_->Redo()) Refresh();
}

void TilemapEditorWidget::Refresh() {
    refreshing_ = true;
    tool_->setEnabled(document_ != nullptr);
    layer_->setEnabled(document_ != nullptr);
    palette_->setEnabled(document_ != nullptr);
    undo_->setEnabled(document_ && document_->CanUndo());
    redo_->setEnabled(document_ && document_->CanRedo());
    palette_->clear();
    layer_->clear();
    if (document_) {
        const auto& map = document_->Map();
        for (const auto& layer : map.layers) layer_->addItem(QString::fromStdString(layer.name));
        layer_->setCurrentIndex(static_cast<int>(document_->active_layer));
        const u32 count = document_->GetTileset().TileCount();
        for (u32 i = 0; i < count; ++i) {
            auto* item = new QListWidgetItem(QString("Tile %1%2").arg(i).arg(document_->GetTileset().IsSolid(static_cast<i32>(i)) ? "  · solid" : ""));
            item->setBackground(TileColor(static_cast<i32>(i)));
            item->setData(Qt::UserRole, static_cast<int>(i));
            palette_->addItem(item);
        }
        for (usize i = 0; i < document_->GetTileset().autotiles.size(); ++i) {
            const int value = sprite2d::AutotileCell(i);
            auto* item = new QListWidgetItem(QString("Auto: %1").arg(QString::fromStdString(document_->GetTileset().autotiles[i].name)));
            item->setBackground(QColor("#557a8e"));
            item->setData(Qt::UserRole, value);
            palette_->addItem(item);
        }
        int selected_brush = -1;
        for (int i = 0; i < palette_->count(); ++i) {
            if (palette_->item(i)->data(Qt::UserRole).toInt() == document_->brush) {
                selected_brush = i;
                break;
            }
        }
        if (selected_brush < 0 && palette_->count() > 0) selected_brush = 0;
        if (selected_brush >= 0) palette_->setCurrentRow(selected_brush);
        tool_->setCurrentIndex(static_cast<int>(document_->tool));
        status_->setText(QString("%1 × %2   %3").arg(map.width).arg(map.height).arg(document_->dirty() ? "Unsaved changes" : "Saved"));
    }
    canvas_->setEnabled(document_ != nullptr);
    canvas_->update();
    refreshing_ = false;
}

void TilemapEditorWidget::SetStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setStyleSheet(error ? "color:#ff8585" : "color:#9cb7cc");
}

} // namespace aether::editor::qt
