#include "script_editor_widget.h"

#include "aether/script/luau_host.h"
#include "aether/script/script_system.h"

#include <QFile>
#include <QFileDialog>
#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSyntaxHighlighter>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QVBoxLayout>

#include <array>
#include <vector>

namespace aether::editor::qt {
namespace {

class LuauHighlighter final : public QSyntaxHighlighter {
public:
    explicit LuauHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {
        keyword_.setForeground(QColor("#c792ea"));
        keyword_.setFontWeight(QFont::DemiBold);
        number_.setForeground(QColor("#f78c6c"));
        string_.setForeground(QColor("#c3e88d"));
        comment_.setForeground(QColor("#718096"));
        type_.setForeground(QColor("#82aaff"));

        const std::array<const char*, 21> keywords{{
            "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
            "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
            "true", "until", "while"
        }};
        for (const char* word : keywords)
            rules_.push_back({QRegularExpression(QStringLiteral("\\b%1\\b").arg(QLatin1String(word))), keyword_});
        rules_.push_back({QRegularExpression(QStringLiteral("\\b(?:continue|export|type|typeof)\\b")), type_});
        rules_.push_back({QRegularExpression(QStringLiteral("\\b[0-9]+(?:\\.[0-9]*)?(?:[eE][+-]?[0-9]+)?\\b")), number_});
        rules_.push_back({QRegularExpression(QStringLiteral("\\b0[xX][0-9A-Fa-f]+\\b")), number_});
    }

protected:
    void highlightBlock(const QString& text) override {
        for (const Rule& rule : rules_) {
            auto match = rule.pattern.globalMatch(text);
            while (match.hasNext()) {
                const auto result = match.next();
                setFormat(result.capturedStart(), result.capturedLength(), rule.format);
            }
        }
        const int lineComment = text.indexOf(QStringLiteral("--"));
        if (lineComment >= 0) setFormat(lineComment, text.size() - lineComment, comment_);
        auto strings = string_pattern_.globalMatch(text);
        while (strings.hasNext()) {
            const auto result = strings.next();
            setFormat(result.capturedStart(), result.capturedLength(), string_);
        }
    }

private:
    struct Rule { QRegularExpression pattern; QTextCharFormat format; };
    std::vector<Rule> rules_;
    QTextCharFormat keyword_;
    QTextCharFormat number_;
    QTextCharFormat string_;
    QTextCharFormat comment_;
    QTextCharFormat type_;
    QRegularExpression string_pattern_{QStringLiteral("\"(?:\\\\.|[^\"\\\\])*\"|'(?:\\\\.|[^'\\\\])*'")};
};

} // namespace

ScriptEditorWidget::ScriptEditorWidget(QWidget* parent)
    : QWidget(parent), host_(std::make_unique<script::LuauHost>()) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(6);
    auto* open = new QPushButton("Open…", this);
    open->setObjectName("scriptOpenButton");
    open->setToolTip("Open a Luau source file from the project");
    save_button_ = new QPushButton("Save", this);
    save_button_->setObjectName("scriptSaveButton");
    save_button_->setToolTip("Save the active Luau source");
    validate_button_ = new QPushButton("Validate", this);
    validate_button_->setObjectName("scriptValidateButton");
    validate_button_->setToolTip("Compile and inspect the active script class");
    file_label_ = new QLabel("No script open", this);
    file_label_->setObjectName("scriptFilePath");
    file_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    toolbar->addWidget(open);
    toolbar->addWidget(save_button_);
    toolbar->addWidget(validate_button_);
    toolbar->addSpacing(8);
    toolbar->addWidget(file_label_, 1);
    root->addLayout(toolbar);

    editor_ = new QPlainTextEdit(this);
    editor_->setObjectName("luauCodeEditor");
    editor_->setAccessibleName("Luau source editor");
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor_->setTabStopDistance(4 * editor_->fontMetrics().horizontalAdvance(QLatin1Char(' ')));
    QFont font(QStringLiteral("Cascadia Code"));
    font.setStyleHint(QFont::Monospace);
    font.setPointSize(11);
    editor_->setFont(font);
    editor_->setPlaceholderText("Open a .luau file from the Content Browser to start editing.");
    new LuauHighlighter(editor_->document());
    root->addWidget(editor_, 1);

    status_label_ = new QLabel("Luau source editor · open a script to edit and validate", this);
    status_label_->setObjectName("muted");
    status_label_->setWordWrap(true);
    root->addWidget(status_label_);

    connect(open, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "Open Luau Script", QString(),
                                                           "Luau Scripts (*.luau);;All Files (*)");
        if (!path.isEmpty()) OpenFile(std::filesystem::path(path.toStdWString()));
    });
    connect(save_button_, &QPushButton::clicked, this, [this] { Save(); });
    connect(validate_button_, &QPushButton::clicked, this, [this] { Validate(); });
    connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
        UpdateTitle();
        status_label_->setText("Modified · save or validate to apply changes");
        status_label_->setStyleSheet("color: #d5ad66;");
    });
    connect(editor_, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        const QTextCursor cursor = editor_->textCursor();
        status_label_->setToolTip(QString("Line %1, column %2").arg(cursor.blockNumber() + 1).arg(cursor.positionInBlock() + 1));
    });
    save_button_->setEnabled(false);
    validate_button_->setEnabled(false);
}

ScriptEditorWidget::~ScriptEditorWidget() = default;

bool ScriptEditorWidget::dirty() const {
    return editor_->document()->isModified();
}

bool ScriptEditorWidget::ConfirmDiscardChanges(QWidget* parent) const {
    if (!dirty()) return true;
    QMessageBox prompt(QMessageBox::Warning, "Unsaved Luau Changes",
                       "Save the script before continuing?", QMessageBox::Save | QMessageBox::Discard |
                       QMessageBox::Cancel, parent ? parent : const_cast<ScriptEditorWidget*>(this));
    const auto choice = static_cast<QMessageBox::StandardButton>(prompt.exec());
    if (choice == QMessageBox::Save)
        return const_cast<ScriptEditorWidget*>(this)->Save();
    return choice == QMessageBox::Discard;
}

bool ScriptEditorWidget::OpenFile(const std::filesystem::path& path, bool confirm_changes) {
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, "Couldn't open Luau script", file.errorString());
        return false;
    }
    const QByteArray source = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        QMessageBox::critical(this, "Couldn't read Luau script", file.errorString());
        return false;
    }
    if (confirm_changes && !ConfirmDiscardChanges(this)) return false;
    file_path_ = std::filesystem::absolute(path).lexically_normal();
    editor_->setPlainText(QString::fromUtf8(source));
    editor_->document()->setModified(false);
    save_button_->setEnabled(true);
    validate_button_->setEnabled(true);
    status_label_->setText("Ready · changes are kept in this source file");
    status_label_->setStyleSheet(QString());
    UpdateTitle();
    return true;
}

void ScriptEditorWidget::ClearDocument() {
    file_path_.clear();
    editor_->clear();
    editor_->document()->setModified(false);
    save_button_->setEnabled(false);
    validate_button_->setEnabled(false);
    status_label_->setText("Luau source editor · open a script to edit and validate");
    status_label_->setStyleSheet(QString());
    UpdateTitle();
}

bool ScriptEditorWidget::Save() {
    if (file_path_.empty()) {
        status_label_->setText("Open or create a .luau file before saving");
        return false;
    }
    QSaveFile file(QString::fromStdWString(file_path_.wstring()));
    const QByteArray source = editor_->toPlainText().toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(source) != source.size() || !file.commit()) {
        QMessageBox::critical(this, "Couldn't save Luau script", file.errorString());
        return false;
    }
    editor_->document()->setModified(false);
    status_label_->setText("Saved · " + QString::fromStdWString(file_path_.filename().wstring()));
    status_label_->setStyleSheet("color: #77cf9b;");
    UpdateTitle();
    return true;
}

void ScriptEditorWidget::Validate() {
    if (file_path_.empty()) return;
    const ScriptClassInfo info = script::ScriptSystem::DescribeSource(
        *host_, editor_->toPlainText().toStdString(), file_path_.filename().string());
    if (!info.ok) {
        status_label_->setText("Compile error · " + QString::fromStdString(info.error));
        status_label_->setStyleSheet("color: #ff8b83;");
        return;
    }
    status_label_->setText(QString("Valid · %1 exposed variable(s), %2 callback(s)%3")
        .arg(static_cast<qulonglong>(info.variables.size()))
        .arg(static_cast<qulonglong>(info.callbacks.size()))
        .arg(dirty() ? " · unsaved edits" : ""));
    status_label_->setStyleSheet("color: #77cf9b;");
}

void ScriptEditorWidget::UpdateTitle() {
    if (file_path_.empty()) file_label_->setText("No script open");
    else file_label_->setText(QString::fromStdWString(file_path_.wstring()) + (dirty() ? "  ●" : ""));
    file_label_->setToolTip(file_label_->text());
}

} // namespace aether::editor::qt
