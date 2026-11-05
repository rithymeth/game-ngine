#pragma once

#include <QWidget>

class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace aether::editor::qt {

// The Qt editor's HIERARCHY dock (Phase 43.1 extraction): the filter box, the
// add/duplicate buttons and the world tree. The caller keeps the pointers and
// wires filtering, selection and refresh.
QWidget* MakeHierarchyPanel(QTreeWidget*& tree, QLineEdit*& filter, QPushButton*& add, QPushButton*& duplicate);

} // namespace aether::editor::qt
