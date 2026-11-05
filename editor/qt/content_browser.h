#pragma once

#include <QWidget>

class QComboBox;
class QFileSystemModel;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;

namespace aether::editor::qt {

// The Qt editor's CONTENT BROWSER dock (Phase 43.1 extraction): folder
// navigation, type/name filters, the icon grid and the no-project welcome
// panel. The caller keeps the pointers and wires navigation and selection.
QWidget* MakeContentBrowser(QComboBox*& breadcrumb, QFileSystemModel*& asset_model, QListView*& asset_view,
                            QPushButton*& parent_button, QPushButton*& new_folder_button,
                            QPushButton*& place_model_button, QLineEdit*& asset_filter,
                            QComboBox*& asset_type_filter, QLabel*& asset_context, QWidget*& welcome_panel,
                            QPushButton*& welcome_open_project, QPushButton*& welcome_new_project);

} // namespace aether::editor::qt
