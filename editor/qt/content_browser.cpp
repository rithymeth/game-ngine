#include "content_browser.h"

#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSize>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace aether::editor::qt {

namespace {

// Paints the content grid's tiles: a folder glyph or a page with an extension
// badge, both drawn so the browser needs no icon assets.
class AssetBrowserDelegate final : public QStyledItemDelegate {
public:
    explicit AssetBrowserDelegate(const QFileSystemModel& model, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), model_(model) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (!index.isValid()) return;

        const QFileInfo info = model_.fileInfo(index);
        const QRect icon_rect(option.rect.center().x() - 24, option.rect.top() + 5, 48, 48);
        painter->save();
        QColor icon_back("#151a21");
        if (option.state & QStyle::State_Selected) icon_back = QColor("#244653");
        else if (option.state & QStyle::State_MouseOver) icon_back = QColor("#202c35");
        painter->fillRect(icon_rect, icon_back);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->translate(icon_rect.topLeft());

        if (info.isDir()) {
            QPainterPath rear;
            rear.moveTo(5, 11); rear.lineTo(20, 11); rear.lineTo(25, 16);
            rear.lineTo(44, 16); rear.lineTo(44, 38); rear.lineTo(5, 38); rear.closeSubpath();
            painter->setPen(QPen(QColor("#527f80"), 1.2));
            painter->setBrush(QColor("#294b55"));
            painter->drawPath(rear);

            QPainterPath front;
            front.moveTo(4, 19); front.lineTo(44, 19); front.lineTo(41, 41);
            front.quadTo(40.7, 43, 38.5, 43); front.lineTo(7, 43);
            front.quadTo(4.5, 43, 4, 40); front.closeSubpath();
            QLinearGradient folder_fill(0, 19, 0, 43);
            folder_fill.setColorAt(0.0, QColor("#5bd0af"));
            folder_fill.setColorAt(1.0, QColor("#277b77"));
            painter->setPen(QPen(QColor("#68d6b7"), 1.1));
            painter->setBrush(folder_fill);
            painter->drawPath(front);
            painter->setPen(QPen(QColor(255, 255, 255, 68), 1.0));
            painter->drawLine(QPointF(8, 23), QPointF(39, 23));
        } else {
            const QString extension = info.suffix().toLower();
            QString badge = "FILE";
            QColor accent("#8da0ae");
            if (extension == "gltf" || extension == "glb" || extension == "fbx" || extension == "obj") {
                badge = "3D"; accent = QColor("#55d7b5");
            } else if (extension == "ascene" || extension == "aesc") {
                badge = "SCN"; accent = QColor("#b9a0ff");
            } else if (extension == "asequence" || extension == "anim") {
                badge = "ANIM"; accent = QColor("#8bbcff");
            } else if (extension == "amontage") {
                badge = "MONT"; accent = QColor("#d7a2ff");
            } else if (extension == "png" || extension == "jpg" || extension == "jpeg" ||
                       extension == "tga" || extension == "hdr" || extension == "exr") {
                badge = "IMG"; accent = QColor("#f4bd72");
            } else if (extension == "lua" || extension == "luau" || extension == "cs" ||
                       extension == "cpp" || extension == "h" || extension == "hpp") {
                badge = "CODE"; accent = QColor("#86b9ff");
            } else if (extension == "wav" || extension == "ogg" || extension == "mp3" || extension == "flac") {
                badge = "SFX"; accent = QColor("#ec9bc7");
            }

            QPainterPath page;
            page.moveTo(10, 4); page.lineTo(29, 4); page.lineTo(39, 14);
            page.lineTo(39, 41); page.quadTo(39, 44, 36, 44); page.lineTo(11, 44);
            page.quadTo(8, 44, 8, 41); page.lineTo(8, 7); page.quadTo(8, 4, 10, 4);
            page.closeSubpath();
            painter->setPen(QPen(QColor("#455967"), 1.2));
            painter->setBrush(QColor("#202c36"));
            painter->drawPath(page);
            QPainterPath fold;
            fold.moveTo(29, 4); fold.lineTo(29, 13); fold.quadTo(29, 14, 31, 14); fold.lineTo(39, 14);
            painter->setPen(QPen(QColor("#455967"), 1.0));
            painter->setBrush(QColor("#33434f"));
            painter->drawPath(fold);
            painter->setPen(QPen(accent, 2.2, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(QPointF(14, 20), QPointF(14, 37));
            painter->setPen(QPen(QColor("#738490"), 1.2, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(QPointF(20, 23), QPointF(33, 23));
            painter->drawLine(QPointF(20, 28), QPointF(33, 28));
            painter->drawLine(QPointF(20, 33), QPointF(29, 33));

            const QRectF badge_rect(22, 35, 22, 11);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#172128"));
            painter->drawRoundedRect(badge_rect, 3, 3);
            painter->setPen(accent);
            QFont badge_font = painter->font();
            badge_font.setPointSizeF(5.8);
            badge_font.setBold(true);
            painter->setFont(badge_font);
            painter->drawText(badge_rect, Qt::AlignCenter, badge);
        }

        painter->restore();
    }

private:
    const QFileSystemModel& model_;
};

} // namespace

QWidget* MakeContentBrowser(QComboBox*& breadcrumb, QFileSystemModel*& asset_model, QListView*& asset_view,
                            QPushButton*& parent_button, QPushButton*& new_folder_button,
                            QPushButton*& place_model_button, QLineEdit*& asset_filter,
                            QComboBox*& asset_type_filter,
                            QLabel*& asset_context, QWidget*& welcome_panel,
                            QPushButton*& welcome_open_project, QPushButton*& welcome_new_project) {
    auto* root = new QWidget;
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(12, 8, 12, 12);
    auto* navigation = new QHBoxLayout;
    parent_button = new QPushButton("Up");
    parent_button->setAccessibleName("Open parent folder");
    parent_button->setToolTip("Parent folder");
    parent_button->setFixedWidth(48);
    new_folder_button = new QPushButton("+ Folder");
    new_folder_button->setAccessibleName("Create a folder in Content");
    place_model_button = new QPushButton("Place Model");
    place_model_button->setAccessibleName("Place selected model in the World");
    place_model_button->setObjectName("placeModelButton");
    place_model_button->setToolTip("Add the selected glTF or GLB asset to the active scene");
    place_model_button->setEnabled(false);
    asset_filter = new QLineEdit;
    asset_filter->setObjectName("contentSearch");
    asset_filter->setAccessibleName("Search names in the current Content folder");
    asset_filter->setPlaceholderText("Search this folder…");
    asset_filter->setToolTip("Search file and folder names in the current Content folder. Combine with the asset type filter. Press Ctrl+F to focus search.");
    asset_filter->setClearButtonEnabled(true);
    asset_filter->setMaximumWidth(220);
    auto* focus_content_search = new QAction("Focus Content Search", root);
    focus_content_search->setShortcut(QKeySequence::Find);
    focus_content_search->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    root->addAction(focus_content_search);
    QObject::connect(focus_content_search, &QAction::triggered, asset_filter, [asset_filter] {
        asset_filter->setFocus(Qt::ShortcutFocusReason);
        asset_filter->selectAll();
    });
    asset_type_filter = new QComboBox(root);
    asset_type_filter->setObjectName("contentTypeFilter");
    asset_type_filter->setAccessibleName("Filter content by asset type");
    asset_type_filter->setToolTip("Filter assets in the current folder by type. This combines with the name search.");
    asset_type_filter->addItem("All Assets", QStringList{});
    asset_type_filter->addItem("Models", QStringList{"gltf", "glb"});
    asset_type_filter->addItem("Scenes", QStringList{"ascene", "aesc"});
    asset_type_filter->addItem("Sequences", QStringList{"asequence"});
    asset_type_filter->addItem("Montages", QStringList{"amontage"});
    asset_type_filter->addItem("Projects", QStringList{"aproject"});
    breadcrumb = new QComboBox;
    breadcrumb->setObjectName("contentBreadcrumb");
    breadcrumb->setToolTip("Choose a parent folder in the project Content directory");
    breadcrumb->setMinimumContentsLength(22);
    breadcrumb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    breadcrumb->addItem("NO PROJECT  /  CONTENT");
    breadcrumb->setEnabled(false);
    navigation->addWidget(parent_button);
    navigation->addWidget(breadcrumb, 1);
    navigation->addWidget(asset_type_filter);
    navigation->addWidget(asset_filter);
    navigation->addWidget(new_folder_button);
    navigation->addWidget(place_model_button);
    layout->addLayout(navigation);
    asset_context = new QLabel("", root);
    asset_context->setObjectName("muted");
    asset_context->setWordWrap(true);
    layout->addWidget(asset_context);
    welcome_panel = new QWidget(root);
    welcome_panel->setObjectName("contentWelcomePanel");
    auto* welcome_layout = new QVBoxLayout(welcome_panel);
    welcome_layout->setContentsMargins(24, 18, 24, 18);
    welcome_layout->addStretch();
    auto* welcome_title = new QLabel("Start with a project", welcome_panel);
    welcome_title->setObjectName("contentWelcomeTitle");
    welcome_title->setAlignment(Qt::AlignCenter);
    welcome_layout->addWidget(welcome_title);
    auto* welcome_description = new QLabel(
        "Create a project or open one to browse assets, place models, and edit scenes.", welcome_panel);
    welcome_description->setObjectName("contentWelcomeDescription");
    welcome_description->setWordWrap(true);
    welcome_description->setAlignment(Qt::AlignCenter);
    welcome_layout->addWidget(welcome_description);
    auto* welcome_actions = new QHBoxLayout;
    welcome_actions->addStretch();
    welcome_open_project = new QPushButton("Open Project…", welcome_panel);
    welcome_open_project->setObjectName("emptyStateAction");
    welcome_open_project->setAccessibleName("Open an existing Aether project");
    welcome_open_project->setToolTip("Choose an existing .aproject file");
    welcome_new_project = new QPushButton("Create Project…", welcome_panel);
    welcome_new_project->setAccessibleName("Create a new Aether project");
    welcome_new_project->setToolTip("Create a project with the standard Content folders");
    welcome_actions->addWidget(welcome_open_project);
    welcome_actions->addWidget(welcome_new_project);
    welcome_actions->addStretch();
    welcome_layout->addLayout(welcome_actions);
    welcome_layout->addStretch();
    layout->addWidget(welcome_panel, 1);
    asset_model = new QFileSystemModel(root);
    asset_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    asset_model->setRootPath(QString());
    asset_view = new QListView(root);
    asset_view->setAccessibleName("Project content assets");
    asset_view->setModel(asset_model);
    asset_view->setItemDelegate(new AssetBrowserDelegate(*asset_model, asset_view));
    asset_view->setViewMode(QListView::IconMode);
    asset_view->setFlow(QListView::LeftToRight);
    asset_view->setWrapping(true);
    asset_view->setResizeMode(QListView::Adjust);
    asset_view->setMovement(QListView::Static);
    asset_view->setIconSize(QSize(48, 48));
    asset_view->setGridSize(QSize(148, 104));
    asset_view->setSpacing(8);
    asset_view->setUniformItemSizes(true);
    asset_view->setWordWrap(true);
    asset_view->setTextElideMode(Qt::ElideMiddle);
    asset_view->setSelectionMode(QAbstractItemView::SingleSelection);
    asset_view->setEnabled(false);
    asset_view->setRootIndex(QModelIndex());
    layout->addWidget(asset_view, 1);
    return root;
}

} // namespace aether::editor::qt
