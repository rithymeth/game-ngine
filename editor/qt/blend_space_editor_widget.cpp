#include "blend_space_editor_widget.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace aether::editor::qt {
namespace {
using aether::anim::BlendSpace;
QDoubleSpinBox* Number(QWidget* parent, double value, double minimum = -100000.0, double maximum = 100000.0) {
    auto* box = new QDoubleSpinBox(parent);
    box->setRange(minimum, maximum); box->setDecimals(3); box->setSingleStep(0.1); box->setValue(value);
    return box;
}
}

class BlendSpaceCanvasWidget final : public QWidget {
public:
    explicit BlendSpaceCanvasWidget(BlendSpaceEditorWidget* owner) : QWidget(owner), owner_(owner) {
        setMinimumSize(360, 240); setMouseTracking(true); setFocusPolicy(Qt::StrongFocus);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect(), QColor("#111721"));
        const auto& s = owner_->document_.Get();
        canvas_ = {48, 22, float(std::max(width() - 72, 1)), float(std::max(height() - 62, 1))};
        p.setPen(QPen(QColor("#364354"), 1));
        const int divisions = std::max(owner_->snap_divisions_, 1);
        for (int i = 0; i <= divisions; ++i) {
            const float t = float(i) / divisions, x = canvas_.x0 + t * canvas_.width;
            p.drawLine(QPointF(x, canvas_.y0), QPointF(x, canvas_.y0 + canvas_.height));
            if (s.dimensions == 2) { const float y = canvas_.y0 + t * canvas_.height; p.drawLine(QPointF(canvas_.x0, y), QPointF(canvas_.x0 + canvas_.width, y)); }
        }
        p.setPen(QPen(QColor("#607b9f"), 1.5));
        for (const auto& tri : s.triangles) {
            if (tri[0] >= s.samples.size() || tri[1] >= s.samples.size() || tri[2] >= s.samples.size()) continue;
            QPointF pts[3];
            for (int n = 0; n < 3; ++n) { float x, y; canvas_.ParamToScreen(s, s.samples[tri[n]].x, s.samples[tri[n]].y, x, y); pts[n] = QPointF(x, y); }
            p.drawPolygon(pts, 3);
        }
        const auto weights = aether::anim::ComputeWeights(s, owner_->preview_x_, owner_->preview_y_);
        for (int i = 0; i < int(s.samples.size()); ++i) {
            float x, y; canvas_.ParamToScreen(s, s.samples[size_t(i)].x, s.samples[size_t(i)].y, x, y);
            float weight = 0; for (const auto& w : weights) if (int(w.sample) == i) weight = w.weight;
            p.setBrush(i == owner_->selected_ ? QColor("#f1ad48") : QColor("#d3dce8"));
            p.setPen(QPen(QColor("#101720"), 2)); p.drawEllipse(QPointF(x, y), 6 + 5 * weight, 6 + 5 * weight);
            p.setPen(QColor("#e0e8f1")); p.drawText(QPointF(x + 10, y - 7), QString::fromStdString(s.samples[size_t(i)].clip));
        }
        float px, py; canvas_.ParamToScreen(s, owner_->preview_x_, owner_->preview_y_, px, py);
        p.setBrush(QColor("#54dc91")); p.setPen(Qt::NoPen); p.drawEllipse(QPointF(px, py), 5, 5);
        p.setPen(QColor("#9baabd")); p.drawText(QRectF(12, height()-30, width()-24, 20), Qt::AlignLeft, QString::fromStdString(s.x.name));
        if (s.dimensions == 2) p.drawText(QRectF(4, 4, width()-8, 16), Qt::AlignRight, QString::fromStdString(s.y.name));
        if (s.samples.empty()) { p.setPen(QColor("#9baabd")); p.drawText(rect(), Qt::AlignCenter, "Add a sample to start shaping the blend space"); }
    }
    void mousePressEvent(QMouseEvent* e) override {
        setFocus(); float x, y; canvas_.ScreenToParam(owner_->document_.Get(), float(e->position().x()), float(e->position().y()), x, y);
        if (e->button() == Qt::RightButton) {
            bool ok = false; const QString clip = QInputDialog::getText(this, "Add Blend Sample", "Animation clip asset", QLineEdit::Normal, {}, &ok);
            if (ok && !clip.trimmed().isEmpty()) { owner_->selected_ = int(owner_->document_.AddSample(clip.trimmed().toStdString(), x, y)); owner_->Refresh(); }
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        dragging_ = canvas_.HitTest(owner_->document_.Get(), float(e->position().x()), float(e->position().y()));
        owner_->selected_ = dragging_;
        if (dragging_ < 0) { owner_->preview_x_ = std::clamp(x, owner_->document_.Get().x.min, owner_->document_.Get().x.max); owner_->preview_y_ = owner_->document_.Get().dimensions == 2 ? std::clamp(y, owner_->document_.Get().y.min, owner_->document_.Get().y.max) : 0; }
        owner_->Refresh();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        float x, y; canvas_.ScreenToParam(owner_->document_.Get(), float(e->position().x()), float(e->position().y()), x, y);
        const auto& s = owner_->document_.Get();
        if (dragging_ >= 0) owner_->document_.MoveSample(size_t(dragging_), x, y, u32(owner_->snap_divisions_), "blend_drag_" + std::to_string(dragging_));
        else { owner_->preview_x_ = std::clamp(x, s.x.min, s.x.max); owner_->preview_y_ = s.dimensions == 2 ? std::clamp(y, s.y.min, s.y.max) : 0; }
        owner_->Refresh();
    }
    void mouseReleaseEvent(QMouseEvent*) override { dragging_ = -1; }
    void keyPressEvent(QKeyEvent* e) override {
        if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && owner_->selected_ >= 0) {
            owner_->document_.RemoveSample(size_t(owner_->selected_)); owner_->selected_ = -1; owner_->Refresh(); return;
        }
        QWidget::keyPressEvent(e);
    }
private:
    BlendSpaceEditorWidget* owner_;
    aether::editor::BlendSpaceCanvas canvas_;
    int dragging_ = -1;
};

BlendSpaceEditorWidget::BlendSpaceEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this); root->setContentsMargins(6,6,6,6);
    auto* bar = new QHBoxLayout;
    auto addButton = [&](const QString& label) { auto* b = new QPushButton(label); bar->addWidget(b); return b; };
    auto* fresh = addButton("New"); auto* open = addButton("Open…"); auto* save = addButton("Save"); auto* saveAs = addButton("Save As…");
    auto* undo = addButton("Undo"); auto* redo = addButton("Redo"); bar->addWidget(new QLabel("Snap"));
    auto* snap = new QComboBox; for (const auto& text : {"Free", "2", "4", "8", "10"}) snap->addItem(text); snap->setCurrentIndex(2); bar->addWidget(snap); bar->addStretch(); root->addLayout(bar);
    auto* split = new QSplitter; samples_ = new QListWidget; samples_->setMinimumWidth(180); split->addWidget(samples_);
    canvas_ = new BlendSpaceCanvasWidget(this); split->addWidget(canvas_);
    auto* side = new QWidget; auto* sideLayout = new QVBoxLayout(side); sideLayout->setContentsMargins(4,0,0,0);
    details_ = new QWidget; details_layout_ = new QVBoxLayout(details_); details_layout_->setAlignment(Qt::AlignTop); sideLayout->addWidget(details_, 2);
    sideLayout->addWidget(new QLabel("LIVE BLEND WEIGHTS")); auto* weights = new QListWidget; weights->setObjectName("blendWeights"); sideLayout->addWidget(weights, 1);
    sideLayout->addWidget(new QLabel("VALIDATION")); auto* diagnostics = new QListWidget; diagnostics->setObjectName("blendDiagnostics"); sideLayout->addWidget(diagnostics, 1); split->addWidget(side); split->setSizes({190,650,280}); root->addWidget(split,1);
    status_ = new QLabel("1D Blend Space · right-click the canvas to add a clip sample"); root->addWidget(status_);
    connect(fresh,&QPushButton::clicked,this,[this]{if(ConfirmDiscardChanges(this)){document_=BlendSpaceDocument{};selected_=-1;preview_x_=preview_y_=0;Refresh();SetStatus("New blend space");}});
    connect(open,&QPushButton::clicked,this,[this]{const auto p=QFileDialog::getOpenFileName(this,"Open Blend Space",{},"Blend Space (*.ablend)");if(!p.isEmpty())OpenFile(std::filesystem::path(p.toStdWString()));});
    connect(save,&QPushButton::clicked,this,[this]{SaveCurrent();}); connect(saveAs,&QPushButton::clicked,this,[this]{SaveAs();});
    connect(undo,&QPushButton::clicked,this,[this]{UndoCurrent();}); connect(redo,&QPushButton::clicked,this,[this]{RedoCurrent();});
    connect(snap,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,snap](int){const QString t=snap->currentText();snap_divisions_=t=="Free"?0:t.toInt();canvas_->update();});
    connect(samples_,&QListWidget::currentRowChanged,this,[this](int row){if(rebuilding_)return;selected_=row;Refresh();});
    Refresh();
}

void BlendSpaceEditorWidget::Refresh() {
    rebuilding_=true; const auto& s=document_.Get(); samples_->clear();
    for(size_t i=0;i<s.samples.size();++i)samples_->addItem(QString("%1  (%2, %3)").arg(QString::fromStdString(s.samples[i].clip)).arg(s.samples[i].x,0,'f',2).arg(s.samples[i].y,0,'f',2));
    if(selected_>=int(s.samples.size()))selected_=-1; samples_->setCurrentRow(selected_);
    while(auto* item=details_layout_->takeAt(0)){if(item->widget())item->widget()->deleteLater();delete item;}
    auto* form=new QFormLayout; details_layout_->addWidget(new QLabel("BLEND SPACE"));
    auto* dims=new QComboBox; dims->addItems({"1D · one parameter","2D · two parameters"});dims->setCurrentIndex(s.dimensions==2?1:0);form->addRow("Dimensions",dims);
    auto axis=[&](bool yAxis){const auto a=yAxis?s.y:s.x;auto* name=new QLineEdit(QString::fromStdString(a.name));auto* range=new QWidget;auto* row=new QHBoxLayout(range);row->setContentsMargins(0,0,0,0);auto* lo=Number(range,a.min);auto* hi=Number(range,a.max);row->addWidget(lo);row->addWidget(hi);form->addRow(yAxis?"Y axis":"X axis",name);form->addRow(yAxis?"Y range":"X range",range);
      auto commit=[this,yAxis,name,lo,hi]{const auto n=name->text().toStdString();const float mn=float(lo->value()),mx=float(hi->value());document_.Edit("Edit blend axis",[&](BlendSpace& b){auto& ax=yAxis?b.y:b.x;ax.name=n;ax.min=mn;ax.max=mx;},yAxis?"blend_y_axis":"blend_x_axis");Refresh();};
      connect(name,&QLineEdit::editingFinished,this,commit);connect(lo,&QDoubleSpinBox::editingFinished,this,commit);connect(hi,&QDoubleSpinBox::editingFinished,this,commit);};
    axis(false);if(s.dimensions==2)axis(true);
    connect(dims,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){document_.Edit("Set blend dimensions",[i](BlendSpace& b){b.dimensions=i?2u:1u;});Refresh();});
    details_layout_->addLayout(form);
    if(selected_>=0){const size_t i=size_t(selected_);const auto sample=s.samples[i];auto* sampleForm=new QFormLayout;auto* clip=new QLineEdit(QString::fromStdString(sample.clip));auto* x=Number(details_,sample.x,s.x.min,s.x.max);auto* y=Number(details_,sample.y,s.y.min,s.y.max);auto* rate=Number(details_,sample.rate,0.01,10);sampleForm->addRow("Clip asset",clip);sampleForm->addRow(QString::fromStdString(s.x.name),x);if(s.dimensions==2)sampleForm->addRow(QString::fromStdString(s.y.name),y);sampleForm->addRow("Playback rate",rate);details_layout_->addWidget(new QLabel("SELECTED SAMPLE"));details_layout_->addLayout(sampleForm);
      auto commitSample=[this,i,clip,x,y,rate]{const auto c=clip->text().toStdString();const float xv=float(x->value()),yv=float(y->value()),rv=float(rate->value());document_.Edit("Edit blend sample",[&](BlendSpace& b){if(i<b.samples.size()){b.samples[i].clip=c;b.samples[i].x=xv;b.samples[i].y=yv;b.samples[i].rate=rv;}},"blend_sample_"+std::to_string(i));Refresh();};connect(clip,&QLineEdit::editingFinished,this,commitSample);connect(x,&QDoubleSpinBox::editingFinished,this,commitSample);connect(y,&QDoubleSpinBox::editingFinished,this,commitSample);connect(rate,&QDoubleSpinBox::editingFinished,this,commitSample);
      auto* remove=new QPushButton("Remove sample");details_layout_->addWidget(remove);connect(remove,&QPushButton::clicked,this,[this,i]{document_.RemoveSample(i);selected_=-1;Refresh();});}
    auto* weightList=findChild<QListWidget*>("blendWeights");weightList->clear();for(const auto&w:aether::anim::ComputeWeights(s,preview_x_,preview_y_))if(w.sample<s.samples.size())weightList->addItem(QString("%1  ·  %2%").arg(QString::fromStdString(s.samples[w.sample].clip)).arg(int(w.weight*100)));
    auto* diag=findChild<QListWidget*>("blendDiagnostics");diag->clear();for(const auto&d:document_.Diagnostics())diag->addItem(QString("%1  %2").arg(QString::fromStdString(d.code),QString::fromStdString(d.message)));
    canvas_->update();setWindowTitle(QString("Blend Space · %1%2").arg(QString::fromStdString(document_.Name()),document_.Dirty()?" *":""));rebuilding_=false;
}
bool BlendSpaceEditorWidget::OpenFile(const std::filesystem::path& path){if(!ConfirmDiscardChanges(this))return false;std::ifstream f(path);if(!f){SetStatus("Could not open blend space",true);return false;}try{nlohmann::json j;f>>j;aether::anim::BlendSpace space;std::string error;if(!aether::anim::BlendSpaceFromJson(j,space,&error)){SetStatus(QString::fromStdString(error),true);return false;}document_=BlendSpaceDocument(std::move(space),path);selected_=-1;Refresh();SetStatus(QString("Opened %1").arg(QString::fromStdWString(path.filename().wstring())));return true;}catch(const std::exception&e){SetStatus(QString::fromUtf8(e.what()),true);return false;}}
bool BlendSpaceEditorWidget::ConfirmDiscardChanges(QWidget*p)const{if(!document_.Dirty())return true;return QMessageBox::question(p?p:const_cast<BlendSpaceEditorWidget*>(this),"Unsaved blend space","Discard unsaved blend space changes?",QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Cancel)==QMessageBox::Discard;}
void BlendSpaceEditorWidget::ClearDocument(){document_=BlendSpaceDocument{};selected_=-1;Refresh();}
void BlendSpaceEditorWidget::SaveCurrent(){if(document_.Path().empty()){SaveAs();return;}std::string e;if(!document_.Save(&e))SetStatus(QString::fromStdString(e),true);else{Refresh();SetStatus("Blend space saved");}}
void BlendSpaceEditorWidget::SaveAs(){auto p=QFileDialog::getSaveFileName(this,"Save Blend Space",{},"Blend Space (*.ablend)");if(p.isEmpty())return;if(!p.endsWith(".ablend",Qt::CaseInsensitive))p+=".ablend";std::string e;if(!document_.SaveAs(std::filesystem::path(p.toStdWString()),&e))SetStatus(QString::fromStdString(e),true);else{Refresh();SetStatus("Blend space saved");}}
void BlendSpaceEditorWidget::UndoCurrent(){if(document_.Undo())Refresh();}void BlendSpaceEditorWidget::RedoCurrent(){if(document_.Redo())Refresh();}
void BlendSpaceEditorWidget::SetStatus(const QString&t,bool error){status_->setText(t);status_->setStyleSheet(error?"color:#ff6b6b":"color:#9eacbf");}
} // namespace aether::editor::qt
