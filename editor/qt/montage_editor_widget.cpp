#include "montage_editor_widget.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <fstream>

namespace aether::editor::qt {
namespace {
QDoubleSpinBox* Number(QWidget* parent, double value, double minimum, double maximum, const QString& suffix = {}) {
    auto* box = new QDoubleSpinBox(parent);
    box->setRange(minimum, maximum);
    box->setDecimals(3);
    box->setSingleStep(0.05);
    box->setValue(value);
    box->setSuffix(suffix);
    return box;
}
}

MontageEditorWidget::MontageEditorWidget(QWidget* parent) : QWidget(parent) { BuildUi(); Refresh(); }

void MontageEditorWidget::BuildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    auto* toolbar = new QHBoxLayout;
    auto button = [&](const QString& label) { auto* b = new QPushButton(label, this); toolbar->addWidget(b); return b; };
    auto* fresh = button("New"); auto* open = button("Open…"); auto* save = button("Save"); auto* save_as = button("Save As…");
    auto* undo = button("Undo"); auto* redo = button("Redo"); toolbar->addStretch();
    root->addLayout(toolbar);

    auto* split = new QSplitter(this);
    sections_ = new QListWidget(split);
    sections_->setMinimumWidth(190);
    auto* editor = new QWidget(split);
    auto* form = new QFormLayout(editor);
    name_ = new QLineEdit(editor); clip_ = new QLineEdit(editor); slot_ = new QLineEdit(editor);
    clip_->setPlaceholderText("Animation clip asset path"); slot_->setPlaceholderText("Default");
    blend_in_ = Number(editor, 0.2, 0, 60, " s"); blend_out_ = Number(editor, 0.2, 0, 60, " s");
    form->addRow("Montage", name_); form->addRow("Clip asset", clip_); form->addRow("Slot", slot_);
    form->addRow("Blend in", blend_in_); form->addRow("Blend out", blend_out_);

    auto* section_title = new QLabel("SECTION", editor);
    form->addRow(section_title);
    section_name_ = new QLineEdit(editor);
    section_start_ = Number(editor, 0, 0, 3600, " s");
    section_next_ = new QComboBox(editor);
    form->addRow("Name", section_name_); form->addRow("Start", section_start_); form->addRow("Next section", section_next_);
    auto* section_buttons = new QHBoxLayout;
    auto* add = new QPushButton("Add section", editor); auto* remove = new QPushButton("Remove section", editor);
    section_buttons->addWidget(add); section_buttons->addWidget(remove); form->addRow(section_buttons);

    auto* preview_row = new QHBoxLayout;
    preview_time_ = Number(editor, 0, 0, 3600, " s");
    preview_section_ = new QLabel("No section at playhead", editor);
    preview_row->addWidget(preview_time_); preview_row->addWidget(preview_section_, 1);
    form->addRow("Preview time", preview_row);
    diagnostics_ = new QLabel(editor); diagnostics_->setWordWrap(true); diagnostics_->setObjectName("muted");
    form->addRow("Validation", diagnostics_);
    split->addWidget(editor); split->setSizes({210, 650});
    root->addWidget(split, 1);
    status_ = new QLabel("Montage · compose clip sections and chain transitions", this);
    root->addWidget(status_);

    connect(fresh, &QPushButton::clicked, this, [this] {
        if (!ConfirmDiscardChanges(this)) return;
        montage_ = {}; path_.clear(); undo_.clear(); redo_.clear(); selected_ = -1; dirty_ = false; Refresh(); SetStatus("New montage");
    });
    connect(open, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, "Open Montage", {}, "Animation Montage (*.amontage)");
        if (!file.isEmpty()) OpenFile(std::filesystem::path(file.toStdWString()));
    });
    connect(save, &QPushButton::clicked, this, [this] { SaveCurrent(); });
    connect(save_as, &QPushButton::clicked, this, [this] { SaveAs(); });
    connect(undo, &QPushButton::clicked, this, [this] { UndoCurrent(); });
    connect(redo, &QPushButton::clicked, this, [this] { RedoCurrent(); });
    connect(sections_, &QListWidget::currentRowChanged, this, [this](int row) { if (!rebuilding_) { selected_ = row; Refresh(); } });
    connect(add, &QPushButton::clicked, this, [this] {
        Record();
        int serial = int(montage_.sections.size()) + 1;
        std::string section_name;
        do { section_name = "Section " + std::to_string(serial++); }
        while (montage_.FindSection(section_name) >= 0);
        const float start = montage_.sections.empty() ? 0.0f : montage_.sections.back().start + 1.0f;
        montage_.sections.push_back({section_name, start, {}}); montage_.Normalize();
        selected_ = int(montage_.FindSection(section_name)); dirty_ = true; Refresh();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (selected_ < 0 || selected_ >= int(montage_.sections.size())) return;
        const std::string removed = montage_.sections[size_t(selected_)].name; Record();
        montage_.sections.erase(montage_.sections.begin() + selected_);
        for (auto& section : montage_.sections) if (section.next == removed) section.next.clear();
        selected_ = -1; dirty_ = true; Refresh();
    });
    const auto edit_header = [this] {
        if (rebuilding_) return;
        Record(); montage_.name = name_->text().trimmed().toStdString(); montage_.clip = clip_->text().trimmed().toStdString();
        montage_.slot = slot_->text().trimmed().toStdString(); montage_.blend_in = float(blend_in_->value());
        montage_.blend_out = float(blend_out_->value()); dirty_ = true; Refresh();
    };
    connect(name_, &QLineEdit::editingFinished, this, edit_header); connect(clip_, &QLineEdit::editingFinished, this, edit_header);
    connect(slot_, &QLineEdit::editingFinished, this, edit_header); connect(blend_in_, &QDoubleSpinBox::editingFinished, this, edit_header);
    connect(blend_out_, &QDoubleSpinBox::editingFinished, this, edit_header);
    connect(section_name_, &QLineEdit::editingFinished, this, [this] { EditSection(); });
    connect(section_start_, &QDoubleSpinBox::editingFinished, this, [this] { EditSection(); });
    connect(section_next_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { EditSection(); });
    connect(preview_time_, &QDoubleSpinBox::valueChanged, this, [this](double time) {
        const auto& sections = montage_.sections;
        const int current = montage_.SectionAt(float(time));
        preview_section_->setText(current >= 0 && current < int(sections.size())
            ? QString("At %1 · next: %2").arg(QString::fromStdString(sections[size_t(current)].name),
                sections[size_t(current)].next.empty() ? QString("End") : QString::fromStdString(sections[size_t(current)].next))
            : QString("No section at playhead"));
    });
}

void MontageEditorWidget::Record() {
    undo_.push_back(montage_); if (undo_.size() > 128) undo_.erase(undo_.begin()); redo_.clear();
}

void MontageEditorWidget::EditSection() {
    if (rebuilding_ || selected_ < 0 || selected_ >= int(montage_.sections.size())) return;
    Record(); auto& section = montage_.sections[size_t(selected_)]; section.name = section_name_->text().trimmed().toStdString();
    section.start = float(section_start_->value()); section.next = section_next_->currentData().toString().toStdString();
    montage_.Normalize(); selected_ = int(montage_.FindSection(section.name)); dirty_ = true; Refresh();
}

void MontageEditorWidget::Refresh() {
    rebuilding_ = true; montage_.Normalize();
    sections_->clear();
    for (const auto& section : montage_.sections)
        sections_->addItem(QString("%1   %2 s  →  %3").arg(QString::fromStdString(section.name)).arg(section.start, 0, 'f', 2)
            .arg(section.next.empty() ? QString("End") : QString::fromStdString(section.next)));
    selected_ = selected_ >= 0 && selected_ < int(montage_.sections.size()) ? selected_ : -1;
    sections_->setCurrentRow(selected_);
    name_->setText(QString::fromStdString(montage_.name)); clip_->setText(QString::fromStdString(montage_.clip));
    slot_->setText(QString::fromStdString(montage_.slot)); blend_in_->setValue(montage_.blend_in); blend_out_->setValue(montage_.blend_out);
    const bool has_section = selected_ >= 0;
    section_name_->setEnabled(has_section); section_start_->setEnabled(has_section); section_next_->setEnabled(has_section);
    if (has_section) {
        const auto& section = montage_.sections[size_t(selected_)]; section_name_->setText(QString::fromStdString(section.name));
        section_start_->setValue(section.start); section_next_->clear(); section_next_->addItem("End montage", QString{});
        for (const auto& candidate : montage_.sections)
            section_next_->addItem(candidate.name == section.name ? QString("%1 · loop").arg(QString::fromStdString(candidate.name))
                                                                  : QString::fromStdString(candidate.name),
                                   QString::fromStdString(candidate.name));
        section_next_->setCurrentIndex(std::max(0, section_next_->findData(QString::fromStdString(section.next))));
    } else { section_name_->clear(); section_start_->setValue(0); section_next_->clear(); }
    const auto problems = aether::anim::ValidateMontage(montage_);
    diagnostics_->setText(problems.empty() ? "Ready · no montage issues" : QString::fromStdString(problems.front()));
    preview_time_->setMaximum(std::max(3600.0, double(montage_.sections.empty() ? 0.0f : montage_.sections.back().start)));
    const int current = montage_.SectionAt(float(preview_time_->value()));
    preview_section_->setText(current >= 0 && current < int(montage_.sections.size())
        ? QString("At %1 · next: %2").arg(QString::fromStdString(montage_.sections[size_t(current)].name),
            montage_.sections[size_t(current)].next.empty() ? QString("End") : QString::fromStdString(montage_.sections[size_t(current)].next))
        : QString("No section at playhead"));
    rebuilding_ = false;
}

bool MontageEditorWidget::OpenFile(const std::filesystem::path& path) {
    if (!ConfirmDiscardChanges(this)) return false;
    std::ifstream file(path); if (!file) { SetStatus("Can't read " + QString::fromStdWString(path.wstring()), true); return false; }
    nlohmann::json json; try { file >> json; } catch (const std::exception& e) { SetStatus(QString::fromUtf8(e.what()), true); return false; }
    aether::anim::Montage loaded; std::string error;
    if (!aether::anim::MontageFromJson(json, loaded, &error)) { SetStatus(QString::fromStdString(error), true); return false; }
    montage_ = std::move(loaded); path_ = path; undo_.clear(); redo_.clear(); selected_ = -1; dirty_ = false; Refresh();
    SetStatus("Opened montage · " + QString::fromStdString(montage_.name)); return true;
}

void MontageEditorWidget::ClearDocument() {
    montage_ = {}; path_.clear(); undo_.clear(); redo_.clear(); selected_ = -1; dirty_ = false; Refresh();
}

bool MontageEditorWidget::ConfirmDiscardChanges(QWidget* parent) const {
    if (!dirty_) return true;
    return QMessageBox::question(parent ? parent : const_cast<MontageEditorWidget*>(this), "Unsaved Montage",
        "Discard unsaved montage changes?", QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Discard;
}

bool MontageEditorWidget::SaveTo(const std::filesystem::path& path) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) { SetStatus("Can't write montage file", true); return false; }
    file << aether::anim::MontageToJson(montage_).dump(2) << "\n";
    if (!file) { SetStatus("Failed while writing montage file", true); return false; }
    path_ = path; dirty_ = false; SetStatus("Saved " + QString::fromStdWString(path_.wstring())); return true;
}
void MontageEditorWidget::SaveCurrent() { if (path_.empty()) SaveAs(); else SaveTo(path_); }
void MontageEditorWidget::SaveAs() {
    const QString path = QFileDialog::getSaveFileName(this, "Save Montage", {}, "Animation Montage (*.amontage)");
    if (!path.isEmpty()) SaveTo(std::filesystem::path(path.toStdWString()));
}
void MontageEditorWidget::UndoCurrent() {
    if (undo_.empty()) return; redo_.push_back(montage_); montage_ = std::move(undo_.back()); undo_.pop_back(); dirty_ = true; selected_ = -1; Refresh();
}
void MontageEditorWidget::RedoCurrent() {
    if (redo_.empty()) return; undo_.push_back(montage_); montage_ = std::move(redo_.back()); redo_.pop_back(); dirty_ = true; selected_ = -1; Refresh();
}
void MontageEditorWidget::SetStatus(const QString& message, bool error) { status_->setText(message); status_->setStyleSheet(error ? "color: #ff897d" : ""); }

} // namespace aether::editor::qt
