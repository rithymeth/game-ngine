#include "hierarchy_panel.h"

#include <QAction>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace aether::editor::qt {

namespace {

QTreeWidget* MakeHierarchy() {
    auto* tree = new QTreeWidget;
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(true);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setUniformRowHeights(true);
    tree->setAnimated(true);
    tree->setIndentation(16);
    tree->setAccessibleName("World hierarchy");
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* root = new QTreeWidgetItem(tree, {"WORLD"});
    root->setFlags(root->flags() & ~Qt::ItemIsSelectable);
    tree->expandAll();
    return tree;
}

} // namespace

QWidget* MakeHierarchyPanel(QTreeWidget*& tree, QLineEdit*& filter, QPushButton*& add, QPushButton*& duplicate) {
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    filter = new QLineEdit(body);
    filter->setObjectName("hierarchyFilter");
    filter->setAccessibleName("Filter the World hierarchy");
    filter->setPlaceholderText("Filter entities…");
    filter->setToolTip("Filter entity names. Matching ancestors remain visible for context. Press Ctrl+F to focus search.");
    filter->setClearButtonEnabled(true);
    auto* focus_hierarchy_search = new QAction("Focus Hierarchy Search", body);
    focus_hierarchy_search->setShortcut(QKeySequence::Find);
    focus_hierarchy_search->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    body->addAction(focus_hierarchy_search);
    QObject::connect(focus_hierarchy_search, &QAction::triggered, filter, [filter] {
        filter->setFocus(Qt::ShortcutFocusReason);
        filter->selectAll();
    });
    layout->addWidget(filter);
    auto* actions = new QHBoxLayout;
    add = new QPushButton("Add Entity", body);
    add->setAccessibleName("Add entity to the World");
    add->setToolTip("Create a new entity in the active world");
    duplicate = new QPushButton("Duplicate", body);
    duplicate->setAccessibleName("Duplicate selected entity");
    duplicate->setEnabled(false);
    actions->addWidget(add);
    actions->addWidget(duplicate);
    layout->addLayout(actions);
    tree = MakeHierarchy();
    layout->addWidget(tree, 1);
    return body;
}

} // namespace aether::editor::qt
