#include "ai_debugger_widget.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

namespace aether::editor::qt {

AiDebuggerWidget::AiDebuggerWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    auto* intro = new QLabel("Live AI state during Play: what each agent's script reports (AI.SetDebugState), "
                             "its perception target and known actors, and its Nav.MoveTowards path. Select a row "
                             "to select the agent; the viewport draws its path, next corner, goal and target line.", this);
    intro->setWordWrap(true);
    intro->setObjectName("panelDescription");
    root->addWidget(intro);

    auto* options = new QHBoxLayout;
    show_all_ = new QCheckBox("Draw all agents", this);
    show_all_->setToolTip("Draw every agent's path and target in the viewport, not only the selected one");
    options->addWidget(show_all_);
    options->addStretch(1);
    status_ = new QLabel("Not playing", this);
    options->addWidget(status_);
    root->addLayout(options);

    table_ = new QTableWidget(0, 5, this);
    table_->setHorizontalHeaderLabels({"Agent", "State", "Target", "Known", "Path"});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(table_, 1);

    QObject::connect(show_all_, &QCheckBox::toggled, this, [this](bool) { if (overlay_changed_) overlay_changed_(); });
    QObject::connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        if (updating_) return;
        const int row = table_->currentRow();
        if (row < 0 || row >= static_cast<int>(entities_.size())) return;
        selected_ = entities_[static_cast<usize>(row)];
        if (activated_) activated_(selected_);
    });
}

void AiDebuggerWidget::SetRows(const std::vector<AiDebugRow>& rows) {
    updating_ = true;
    table_->setRowCount(static_cast<int>(rows.size()));
    entities_.clear();
    for (usize i = 0; i < rows.size(); ++i) {
        const AiDebugRow& r = rows[i];
        entities_.push_back(r.entity);
        const QString cells[] = {QString::fromStdString(r.name), QString::fromStdString(r.state),
                                 QString::fromStdString(r.target), r.target.empty() ? QString() : QString::number(r.known),
                                 QString::fromStdString(r.path)};
        for (int c = 0; c < 5; ++c) {
            QTableWidgetItem* item = table_->item(static_cast<int>(i), c);
            if (!item) table_->setItem(static_cast<int>(i), c, item = new QTableWidgetItem);
            if (item->text() != cells[c]) item->setText(cells[c]);
        }
    }
    updating_ = false;
    status_->setText(QString("%1 agent%2").arg(rows.size()).arg(rows.size() == 1 ? "" : "s"));
    SyncSelection();
}

void AiDebuggerWidget::Clear() {
    updating_ = true;
    table_->setRowCount(0);
    entities_.clear();
    updating_ = false;
    status_->setText("Not playing");
}

void AiDebuggerWidget::SetSelectedEntity(Entity entity) {
    selected_ = entity;
    SyncSelection();
}

void AiDebuggerWidget::SyncSelection() {
    updating_ = true;
    int row = -1;
    for (usize i = 0; i < entities_.size(); ++i)
        if (entities_[i] == selected_) row = static_cast<int>(i);
    if (row < 0) table_->clearSelection();
    else if (table_->currentRow() != row) table_->selectRow(row);
    updating_ = false;
}

bool AiDebuggerWidget::ShowAllAgents() const { return show_all_->isChecked(); }

} // namespace aether::editor::qt
