#include "vfx_editor_widget.h"
#include "aether/gfx/shader_compiler.h"

#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineF>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QTextStream>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QLineEdit>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPoint>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace aether::editor::qt {
namespace {
constexpr int kEmitterRole = Qt::UserRole + 1;
constexpr int kStageRole = Qt::UserRole + 2;
constexpr int kModuleRole = Qt::UserRole + 3;
constexpr int kIsStageRole = Qt::UserRole + 4;

QString ParameterLabel(aether::vfx::ParameterType type) {
    switch (type) {
        case aether::vfx::ParameterType::Float: return "Float";
        case aether::vfx::ParameterType::Vector: return "Vector";
        case aether::vfx::ParameterType::Color: return "Color";
    }
    return "Float";
}

aether::vfx::ParameterType ParseParameterType(const QString& type) {
    if (type == "Vector") return aether::vfx::ParameterType::Vector;
    if (type == "Color") return aether::vfx::ParameterType::Color;
    return aether::vfx::ParameterType::Float;
}

aether::vfx::ParameterValue ParameterFromFields(aether::vfx::ParameterType type, const std::array<double, 4>& v) {
    if (type == aether::vfx::ParameterType::Vector) return aether::vfx::ParameterValue::Vector({float(v[0]), float(v[1]), float(v[2])});
    if (type == aether::vfx::ParameterType::Color) return aether::vfx::ParameterValue::Color({float(v[0]), float(v[1]), float(v[2]), float(v[3])});
    return aether::vfx::ParameterValue::Float(float(v[0]));
}

std::array<double, 4> ParameterFields(const aether::vfx::ParameterValue& value) {
    if (value.type == aether::vfx::ParameterType::Vector) return {value.v.x, value.v.y, value.v.z, 1.0};
    if (value.type == aether::vfx::ParameterType::Color) return {value.c.r, value.c.g, value.c.b, value.c.a};
    return {value.f, 0.0, 0.0, 1.0};
}

QString JsonText(const nlohmann::json& value) {
    return QString::fromStdString(value.dump());
}

QColor ParticleColor(const aether::vfx::LinearColor& c) {
    return QColor::fromRgbF(std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f),
                            std::clamp(c.b, 0.0f, 1.0f), std::clamp(c.a, 0.0f, 1.0f));
}
float LinearToSrgb(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}
float SrgbToLinear(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

class CurvePreview final : public QWidget {
public:
    using Changed = std::function<void(const nlohmann::json&)>;
    CurvePreview(aether::vfx::FloatCurve curve, Changed changed, QWidget* parent = nullptr)
        : QWidget(parent), curve_(std::move(curve)), changed_(std::move(changed)) { setMinimumHeight(120); setMouseTracking(true); }
    void SetInterpolation(aether::vfx::CurveInterp interpolation) { curve_.interp = interpolation; Commit(); update(); }
protected:
    QPointF Point(float t, float value) const {
        float lo = 0.0f, hi = 1.0f;
        if (!curve_.keys.empty()) { lo = curve_.keys.front().value; hi = lo; for (const auto& key : curve_.keys) { lo = std::min(lo, key.value); hi = std::max(hi, key.value); } }
        if (hi - lo < 0.001f) { lo -= 1.0f; hi += 1.0f; } else { const float pad = (hi - lo) * 0.15f; lo -= pad; hi += pad; }
        const QRectF plot = QRectF(rect()).adjusted(18, 8, -10, -20);
        return {plot.left() + t * plot.width(), plot.bottom() - (value - lo) / (hi - lo) * plot.height()};
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(), QColor("#141a20")); p.setRenderHint(QPainter::Antialiasing);
        const QRectF plot = QRectF(rect()).adjusted(18, 8, -10, -20); p.setPen(QColor("#2f3b46"));
        for (int i = 0; i <= 4; ++i) { const qreal x = plot.left() + plot.width() * i / 4.0; p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom())); }
        for (int i = 0; i <= 2; ++i) { const qreal y = plot.top() + plot.height() * i / 2.0; p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y)); }
        QPainterPath path; for (int i = 0; i <= 100; ++i) { const float t = i / 100.0f; const auto point = Point(t, curve_.Evaluate(t)); if (i == 0) path.moveTo(point); else path.lineTo(point); }
        p.setPen(QPen(QColor("#62d8b8"), 2)); p.drawPath(path);
        for (usize i = 0; i < curve_.keys.size(); ++i) { const QPointF point = Point(curve_.keys[i].time, curve_.keys[i].value); p.setPen(QPen(QColor("#0d151a"), 1)); p.setBrush(i == dragging_ ? QColor("#f0c36e") : QColor("#d9fff4")); p.drawEllipse(point, 5, 5); }
        p.setPen(QColor("#8795a2")); p.drawText(QRectF(0, height()-17, width(), 14), Qt::AlignCenter, "0  ·  normalized lifetime  ·  1");
    }
    void mousePressEvent(QMouseEvent* event) override {
        const QPointF pos = event->position(); usize hit = curve_.keys.size(); qreal best = 11.0;
        for (usize i = 0; i < curve_.keys.size(); ++i) { const auto point = Point(curve_.keys[i].time, curve_.keys[i].value); const qreal distance = QLineF(pos, point).length(); if (distance < best) best = distance, hit = i; }
        if (event->button() == Qt::RightButton) { if (hit < curve_.keys.size() && aether::editor::RemoveCurveKey(curve_, hit)) Commit(); update(); return; }
        if (event->button() != Qt::LeftButton) return;
        if (hit == curve_.keys.size()) { const auto [time, value] = ToValue(pos); dragging_ = aether::editor::AddCurveKey(curve_, time, value); Commit(); }
        else dragging_ = hit;
        event->accept(); update();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_ >= curve_.keys.size()) return;
        const auto [time, value] = ToValue(event->position()); dragging_ = aether::editor::MoveCurveKey(curve_, dragging_, time, value); Commit(); update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { dragging_ = curve_.keys.size(); }
private:
    std::pair<float,float> ToValue(QPointF pos) const {
        const QRectF plot = QRectF(rect()).adjusted(18, 8, -10, -20); float lo = 0.0f, hi = 1.0f;
        if (!curve_.keys.empty()) { lo = curve_.keys.front().value; hi = lo; for (const auto& key : curve_.keys) { lo = std::min(lo, key.value); hi = std::max(hi, key.value); } }
        if (hi - lo < 0.001f) { lo -= 1.0f; hi += 1.0f; } else { const float pad = (hi - lo) * 0.15f; lo -= pad; hi += pad; }
        return {std::clamp(float((pos.x()-plot.left())/plot.width()),0.0f,1.0f), lo + (1.0f-std::clamp(float((pos.y()-plot.top())/plot.height()),0.0f,1.0f))*(hi-lo)};
    }
    void Commit() { if (changed_) changed_(aether::vfx::ToJson(curve_)); }
    aether::vfx::FloatCurve curve_; Changed changed_; usize dragging_ = std::numeric_limits<usize>::max();
};

class GradientPreview final : public QWidget {
public:
    using Changed = std::function<void(const nlohmann::json&)>;
    GradientPreview(aether::vfx::ColorGradient gradient, Changed changed, QWidget* parent = nullptr)
        : QWidget(parent), gradient_(std::move(gradient)), changed_(std::move(changed)) { setMinimumHeight(76); setMouseTracking(true); }
protected:
    qreal KeyX(float t) const { return 10.0 + std::clamp(t, 0.0f, 1.0f) * std::max(1, width()-20); }
    float TimeAt(qreal x) const { return std::clamp(float((x-10.0)/std::max(1,width()-20)),0.0f,1.0f); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(),QColor("#141a20")); p.setRenderHint(QPainter::Antialiasing);
        for(int x=10;x<width()-10;x+=8) for(int y=8;y<35;y+=8) p.fillRect(x,y,8,8,((x/8+y/8)&1)?QColor("#252e37"):QColor("#1b232b"));
        for(int x=10;x<width()-10;++x){const auto c=gradient_.Evaluate(TimeAt(x));p.setPen(QColor::fromRgbF(LinearToSrgb(c.r),LinearToSrgb(c.g),LinearToSrgb(c.b),1.0));p.drawLine(x,8,x,35);}
        for(usize i=0;i<gradient_.colors.size();++i){const auto& k=gradient_.colors[i];p.setPen(QPen(Qt::black,1));p.setBrush(QColor::fromRgbF(LinearToSrgb(k.r),LinearToSrgb(k.g),LinearToSrgb(k.b)));p.drawRect(QRectF(KeyX(k.time)-4,32,8,8));}
        p.setPen(QColor("#465460"));p.drawLine(10,58,width()-10,58);
        for(usize i=0;i<gradient_.alphas.size();++i){const auto& k=gradient_.alphas[i];p.setPen(QPen(Qt::black,1));p.setBrush(i==dragging_alpha_?QColor("#f0c36e"):QColor("#d9fff4"));p.drawEllipse(QPointF(KeyX(k.time),58-(std::clamp(k.a,0.0f,1.0f)-0.5f)*20),4,4);}
        p.setPen(QColor("#8795a2"));p.drawText(10,height()-2,"Color keys · click to add, double-click to edit, right-click to remove");
    }
    void mousePressEvent(QMouseEvent* event) override {
        const qreal x=event->position().x(); const float t=TimeAt(x); color_hit_=Nearest(gradient_.colors,x); alpha_hit_=NearestAlpha(x);
        if(event->button()==Qt::RightButton){if(event->position().y()<40&&color_hit_<gradient_.colors.size())aether::editor::RemoveColorKey(gradient_,color_hit_);else if(alpha_hit_<gradient_.alphas.size())aether::editor::RemoveAlphaKey(gradient_,alpha_hit_);Commit();update();return;}
        if(event->button()!=Qt::LeftButton)return;
        if(event->position().y()<40){if(color_hit_<gradient_.colors.size())dragging_color_=color_hit_;else{const auto c=gradient_.Evaluate(t);dragging_color_=aether::editor::AddColorKey(gradient_,t,c);Commit();}}
        else {if(alpha_hit_<gradient_.alphas.size())dragging_alpha_=alpha_hit_;else{dragging_alpha_=aether::editor::AddAlphaKey(gradient_,t,gradient_.Evaluate(t).a);Commit();}}
        event->accept();update();
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if(event->position().y()>=40){if(alpha_hit_<gradient_.alphas.size()){bool ok=false;const double a=QInputDialog::getDouble(this,"Edit Opacity Key","Opacity",gradient_.alphas[alpha_hit_].a,0.0,1.0,3,&ok);if(ok){gradient_.alphas[alpha_hit_].a=float(a);Commit();update();}}return;}
        if(color_hit_>=gradient_.colors.size())return;const auto& key=gradient_.colors[color_hit_];const QColor initial=QColor::fromRgbF(LinearToSrgb(key.r),LinearToSrgb(key.g),LinearToSrgb(key.b));const QColor chosen=QColorDialog::getColor(initial,this,"Gradient Color",QColorDialog::ShowAlphaChannel);if(chosen.isValid()){auto& target=gradient_.colors[color_hit_];target.r=SrgbToLinear(float(chosen.redF()));target.g=SrgbToLinear(float(chosen.greenF()));target.b=SrgbToLinear(float(chosen.blueF()));Commit();update();}
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        const float time=TimeAt(event->position().x());
        if(dragging_color_<gradient_.colors.size())dragging_color_=aether::editor::MoveColorKey(gradient_,dragging_color_,time),Commit(),update();
        else if(dragging_alpha_<gradient_.alphas.size()){const float alpha=std::clamp(float(1.0-(event->position().y()-48.0)/20.0),0.0f,1.0f);const float value=gradient_.alphas[dragging_alpha_].a;gradient_.alphas.erase(gradient_.alphas.begin()+static_cast<std::ptrdiff_t>(dragging_alpha_));dragging_alpha_=aether::editor::AddAlphaKey(gradient_,time,alpha);Commit();update();Q_UNUSED(value);}
        else {color_hit_=Nearest(gradient_.colors,event->position().x());alpha_hit_=NearestAlpha(event->position().x());}
    }
    void mouseReleaseEvent(QMouseEvent*) override {dragging_color_=gradient_.colors.size();dragging_alpha_=gradient_.alphas.size();}
private:
    usize Nearest(const std::vector<aether::vfx::ColorGradient::ColorKey>& keys,qreal x)const{usize hit=keys.size();qreal best=10;for(usize i=0;i<keys.size();++i){const qreal d=std::abs(KeyX(keys[i].time)-x);if(d<best)best=d,hit=i;}return hit;}
    usize NearestAlpha(qreal x)const{usize hit=gradient_.alphas.size();qreal best=10;for(usize i=0;i<gradient_.alphas.size();++i){const qreal d=std::abs(KeyX(gradient_.alphas[i].time)-x);if(d<best)best=d,hit=i;}return hit;}
    void Commit(){if(changed_)changed_(aether::vfx::ToJson(gradient_));}
    aether::vfx::ColorGradient gradient_;Changed changed_;usize color_hit_=0,alpha_hit_=0,dragging_color_=std::numeric_limits<usize>::max(),dragging_alpha_=std::numeric_limits<usize>::max();
};
}

class ParticlePreviewCanvas final : public QWidget {
public:
    explicit ParticlePreviewCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(340, 280);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
    }
    void SetPreview(aether::editor::ParticlePreview* preview) { preview_ = preview; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(17, 22, 30));
        p.setRenderHint(QPainter::Antialiasing);
        if (!preview_) return;
        auto project = [this](const aether::Vec3& v, QPointF& out) {
            float x = 0, y = 0, depth = 0;
            if (!preview_->Project(v, float(width()), float(height()), x, y, depth) || depth > 200.0f) return false;
            out = QPointF(x, y); return true;
        };
        p.setPen(QPen(QColor(39, 49, 62), 1));
        for (int i = -8; i <= 8; ++i) {
            QPointF a, b;
            if (project({float(i), 0, -8}, a) && project({float(i), 0, 8}, b)) p.drawLine(a, b);
            if (project({-8, 0, float(i)}, a) && project({8, 0, float(i)}, b)) p.drawLine(a, b);
        }
        const auto* instance = preview_->Instance();
        if (instance) {
            aether::vfx::ParticleRenderData draw_data;
            aether::vfx::BuildRenderData(*instance, preview_->Camera(), draw_data);
            struct Dot { QPointF pt; float depth; QColor color; float radius; };
            std::vector<Dot> dots;
            for (const auto& batch : draw_data.sprites) for (const auto& sprite : batch.instances) {
                QPointF pt;
                if (!project(sprite.center, pt)) continue;
                QPointF edge;
                float radius = 4.0f;
                if (project(sprite.center + sprite.right, edge)) radius = std::clamp(std::hypot(edge.x()-pt.x(), edge.y()-pt.y()), 2.0, 30.0);
                float x=0,y=0,d=0; preview_->Project(sprite.center, float(width()), float(height()), x,y,d);
                dots.push_back({pt,d,ParticleColor(sprite.color),radius});
            }
            for (const auto& batch : draw_data.meshes) for (usize i=0;i<batch.transforms.size();++i) {
                const auto& m=batch.transforms[i];
                const aether::Vec3 pos{m.cols[3].x,m.cols[3].y,m.cols[3].z};
                QPointF pt; if (!project(pos,pt)) continue;
                float x=0,y=0,d=0; preview_->Project(pos,float(width()),float(height()),x,y,d);
                const auto color=i<batch.colors.size()?ParticleColor(batch.colors[i]):QColor(118,200,255);
                dots.push_back({pt,d,color,4.5f});
            }
            for (const auto& light : draw_data.lights) {
                QPointF pt; if (!project(light.position,pt)) continue;
                float x=0,y=0,d=0; preview_->Project(light.position,float(width()),float(height()),x,y,d);
                QColor c=ParticleColor(light.color); c.setAlphaF(std::clamp(light.intensity*0.45f,0.1f,0.7f));
                dots.push_back({pt,d,c,5.0f+std::clamp(light.radius,0.0f,12.0f)});
            }
            std::sort(dots.begin(),dots.end(),[](const Dot&a,const Dot&b){return a.depth>b.depth;});
            for (const auto& dot : dots) {
                p.setPen(Qt::NoPen); p.setBrush(dot.color); p.drawEllipse(dot.pt,dot.radius,dot.radius);
            }
        }
        p.setPen(QColor(170,185,205));
        p.drawText(12, 22, QString("LIVE PREVIEW  ·  %1 particles").arg(preview_->Count()));
        p.drawText(12, height()-12, QString("%1 s").arg(preview_->Time(),0,'f',2));
    }
    void mousePressEvent(QMouseEvent* e) override { last_=e->position().toPoint(); dragging_=e->button()==Qt::LeftButton; }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (dragging_ && preview_) { const QPoint d=e->position().toPoint()-last_; preview_->yaw-=d.x()*0.008f; preview_->pitch=std::clamp(preview_->pitch+d.y()*0.008f,-1.45f,1.45f); update(); }
        last_=e->position().toPoint();
    }
    void mouseReleaseEvent(QMouseEvent*) override { dragging_=false; }
    void wheelEvent(QWheelEvent* e) override { if (preview_) { preview_->distance=std::clamp(preview_->distance*(e->angleDelta().y()>0?0.9f:1.1f),1.0f,100.0f); update(); } }
private:
    aether::editor::ParticlePreview* preview_=nullptr;
    QPoint last_;
    bool dragging_=false;
};

VfxEditorWidget::VfxEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* root=new QVBoxLayout(this); root->setContentsMargins(6,6,6,6);
    auto* toolbar=new QHBoxLayout;
    auto* save=new QPushButton("Save"); auto* save_as=new QPushButton("Save As…");
    auto* undo=new QPushButton("Undo"); auto* redo=new QPushButton("Redo");
    auto* gpu_shader=new QPushButton("GPU Shader…");
    play_=new QPushButton("▶ Play"); auto* restart=new QPushButton("Restart");
    speed_=new QComboBox; speed_->addItems({"0.25×","0.5×","1×","2×","4×"}); speed_->setCurrentIndex(2);
    for(auto* b:{save,save_as,undo,redo,play_,restart,gpu_shader}) toolbar->addWidget(b);
    toolbar->addWidget(new QLabel("Speed")); toolbar->addWidget(speed_); toolbar->addStretch();
    root->addLayout(toolbar);
    auto* split=new QSplitter(Qt::Horizontal);
    auto* left=new QWidget; auto* ll=new QVBoxLayout(left); ll->setContentsMargins(0,0,0,0);
    ll->addWidget(new QLabel("EMITTERS")); emitters_=new QListWidget; ll->addWidget(emitters_,1);
    auto* emitter_buttons=new QHBoxLayout; auto* add_emitter=new QPushButton("+"); auto* dup=new QPushButton("Duplicate"); auto* remove=new QPushButton("−");
    emitter_buttons->addWidget(add_emitter); emitter_buttons->addWidget(dup); emitter_buttons->addWidget(remove); ll->addLayout(emitter_buttons);
    ll->addWidget(new QLabel("MODULE STACK")); stack_=new QTreeWidget; stack_->setHeaderHidden(true); ll->addWidget(stack_,2);
    auto* module_buttons=new QHBoxLayout; auto* add_module=new QPushButton("Add Module"); auto* up=new QPushButton("↑"); auto* down=new QPushButton("↓"); auto* remove_module=new QPushButton("Remove");
    module_buttons->addWidget(add_module); module_buttons->addWidget(up); module_buttons->addWidget(down); module_buttons->addWidget(remove_module); ll->addLayout(module_buttons);
    split->addWidget(left);
    auto* center=new QWidget; auto* cl=new QVBoxLayout(center); cl->setContentsMargins(0,0,0,0);
    canvas_=new ParticlePreviewCanvas; canvas_->SetPreview(&preview_); cl->addWidget(canvas_,1);
    timeline_=new QSlider(Qt::Horizontal); timeline_->setRange(0,600); cl->addWidget(timeline_);
    stats_=new QLabel; cl->addWidget(stats_);
    split->addWidget(center);
    auto* right=new QWidget; auto* rl=new QVBoxLayout(right); rl->setContentsMargins(0,0,0,0); rl->addWidget(new QLabel("PROPERTIES"));
    auto* scroll=new QScrollArea; scroll->setWidgetResizable(true); details_=new QWidget; details_layout_=new QVBoxLayout(details_); details_layout_->setAlignment(Qt::AlignTop); scroll->setWidget(details_); rl->addWidget(scroll,2);
    system_tabs_=new QTabWidget;
    auto make_system_tab=[this](const QString& title,QListWidget*& list,const QString& add_label,const QString& remove_label){auto* page=new QWidget;auto* layout=new QVBoxLayout(page);list=new QListWidget;layout->addWidget(list,1);auto* buttons=new QHBoxLayout;auto* add=new QPushButton(add_label);auto* remove=new QPushButton(remove_label);buttons->addWidget(add);buttons->addWidget(remove);layout->addLayout(buttons);system_tabs_->addTab(page,title);return std::pair<QPushButton*,QPushButton*>(add,remove);};
    auto [add_parameter,remove_parameter]=make_system_tab("Parameters",parameter_list_,"Add","Remove");
    auto [add_binding,remove_binding]=make_system_tab("Bindings",binding_list_,"Bind Parameter…","Remove");
    auto [add_sub_emitter,remove_sub_emitter]=make_system_tab("Sub-emitters",sub_emitter_list_,"Add","Remove");
    rl->addWidget(system_tabs_,2);
    gpu_status_=new QLabel; gpu_status_->setWordWrap(true); rl->addWidget(gpu_status_);
    rl->addWidget(new QLabel("VALIDATION")); diagnostics_=new QListWidget; rl->addWidget(diagnostics_,1);
    split->addWidget(right); split->setSizes({250,650,310}); root->addWidget(split,1);
    status_=new QLabel("New particle system · save as .avfx to keep your work"); root->addWidget(status_);
    timer_=new QTimer(this); timer_->setInterval(16);
    connect(timer_,&QTimer::timeout,this,[this]{ if(!playing_)return; float speed=std::pow(2.0f,float(speed_->currentIndex()-2)); preview_.Advance(1.0f/60.0f*speed); if(preview_.Time()>=60.0f)preview_.Seek(document_.Get(),0.0f); timeline_->blockSignals(true); timeline_->setValue(int(preview_.Time()*10)); timeline_->blockSignals(false); canvas_->update(); stats_->setText(QString("%1 particles · %2 s").arg(preview_.Count()).arg(preview_.Time(),0,'f',2)); });
    connect(save,&QPushButton::clicked,this,[this]{SaveCurrent();}); connect(save_as,&QPushButton::clicked,this,[this]{SaveAs();});
    connect(gpu_shader,&QPushButton::clicked,this,[this]{InspectGpuShader();});
    connect(undo,&QPushButton::clicked,this,[this]{UndoCurrent();}); connect(redo,&QPushButton::clicked,this,[this]{RedoCurrent();});
    connect(play_,&QPushButton::clicked,this,[this]{playing_=!playing_;play_->setText(playing_?"❚❚ Pause":"▶ Play");if(playing_)timer_->start();else timer_->stop();});
    connect(restart,&QPushButton::clicked,this,[this]{playing_=false;timer_->stop();play_->setText("▶ Play");preview_.Seek(document_.Get(),0);timeline_->setValue(0);canvas_->update();});
    connect(timeline_,&QSlider::valueChanged,this,[this](int v){preview_.Seek(document_.Get(),float(v)/10.0f);canvas_->update();stats_->setText(QString("%1 particles · %2 s").arg(preview_.Count()).arg(preview_.Time(),0,'f',2));});
    connect(add_emitter,&QPushButton::clicked,this,[this]{AddEmitter();});
    connect(dup,&QPushButton::clicked,this,[this]{if(auto i=document_.DuplicateEmitter(emitter_index_)){emitter_index_=*i;Refresh();}});
    connect(remove,&QPushButton::clicked,this,[this]{if(document_.RemoveEmitter(emitter_index_)){emitter_index_=std::min(emitter_index_,document_.Get().emitters.size()-1);Refresh();}});
    connect(emitters_,&QListWidget::currentRowChanged,this,[this](int row){SelectEmitter(row);});
    connect(stack_,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* i){SelectModule(i);});
    connect(stack_,&QTreeWidget::itemChanged,this,[this](QTreeWidgetItem* i,int){if(i->data(0,kIsStageRole).toBool()||!module_ref_)return;document_.SetModuleEnabled(*module_ref_,i->checkState(0)==Qt::Checked);SyncPreview();RefreshDiagnostics();});
    connect(add_module,&QPushButton::clicked,this,[this]{AddModule();});
    connect(add_parameter,&QPushButton::clicked,this,[this]{
        QDialog dialog(this);dialog.setWindowTitle("Add Particle Parameter");auto* form=new QFormLayout(&dialog);auto* name=new QLineEdit("NewParameter");auto* type=new QComboBox;type->addItems({"Float","Vector","Color"});std::array<QDoubleSpinBox*,4> fields{};const QStringList labels{"X / R","Y / G","Z / B","A"};for(int i=0;i<4;++i){fields[i]=new QDoubleSpinBox;fields[i]->setRange(-1000000,1000000);fields[i]->setDecimals(4);fields[i]->setValue(i==3?1.0:0.0);}
        form->addRow("Name",name);form->addRow("Type",type);for(int i=0;i<4;++i)form->addRow(labels[i],fields[i]);
        auto update_fields=[=](int){const int n=type->currentIndex()==0?1:type->currentIndex()==1?3:4;for(int i=0;i<4;++i)fields[i]->setVisible(i<n);};connect(type,qOverload<int>(&QComboBox::currentIndexChanged),&dialog,update_fields);update_fields(0);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);form->addRow(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);if(dialog.exec()!=QDialog::Accepted)return;
        std::array<double,4> values{};for(int i=0;i<4;++i)values[i]=fields[i]->value();std::string error;if(!document_.AddParameter(name->text().trimmed().toStdString(),ParameterFromFields(ParseParameterType(type->currentText()),values),&error))SetStatus(QString::fromStdString(error),true);else{RefreshSystemTools();RefreshDiagnostics();}
    });
    connect(remove_parameter,&QPushButton::clicked,this,[this]{auto* item=parameter_list_->currentItem();if(!item)return;document_.RemoveParameter(item->data(Qt::UserRole).toString().toStdString());RefreshSystemTools();RefreshDiagnostics();});
    connect(parameter_list_,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem* item){
        const std::string old_name=item->data(Qt::UserRole).toString().toStdString();const auto& params=document_.Get().parameters;auto it=std::find_if(params.begin(),params.end(),[&](const auto& p){return p.name==old_name;});if(it==params.end())return;auto value=it->value;
        QDialog dialog(this);dialog.setWindowTitle("Edit Particle Parameter");auto* form=new QFormLayout(&dialog);auto* name=new QLineEdit(QString::fromStdString(old_name));auto* type=new QComboBox;type->addItems({"Float","Vector","Color"});type->setCurrentText(ParameterLabel(value.type));std::array<QDoubleSpinBox*,4> fields{};const QStringList labels{"X / R","Y / G","Z / B","A"};auto initial=ParameterFields(value);for(int i=0;i<4;++i){fields[i]=new QDoubleSpinBox;fields[i]->setRange(-1000000,1000000);fields[i]->setDecimals(4);fields[i]->setValue(initial[i]);}form->addRow("Name",name);form->addRow("Type",type);for(int i=0;i<4;++i)form->addRow(labels[i],fields[i]);auto update=[=](int){const int n=type->currentIndex()==0?1:type->currentIndex()==1?3:4;for(int i=0;i<4;++i)fields[i]->setVisible(i<n);};connect(type,qOverload<int>(&QComboBox::currentIndexChanged),&dialog,update);update(0);auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);form->addRow(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);if(dialog.exec()!=QDialog::Accepted)return;
        std::array<double,4> values{};for(int i=0;i<4;++i)values[i]=fields[i]->value();const auto next=ParameterFromFields(ParseParameterType(type->currentText()),values);std::string error;if(!document_.RenameParameter(old_name,name->text().trimmed().toStdString(),&error)){SetStatus(QString::fromStdString(error),true);return;}document_.SetParameterDefault(name->text().trimmed().toStdString(),next,"parameter-value");RefreshSystemTools();RefreshDiagnostics();
    });
    connect(add_binding,&QPushButton::clicked,this,[this]{
        const auto* emitter=document_.EmitterAt(emitter_index_);if(!emitter||document_.Get().parameters.empty()){SetStatus("Add an emitter and parameter before creating a binding",true);return;}
        QStringList params,fields;for(const auto& p:document_.Get().parameters)params<<QString::fromStdString(p.name);
        const std::array<std::pair<aether::editor::VfxStage,const char*>,4> stages{{{aether::editor::VfxStage::Spawn,"spawn"},{aether::editor::VfxStage::Init,"init"},{aether::editor::VfxStage::Update,"update"},{aether::editor::VfxStage::Render,"render"}}};
        for(const auto& [stage,prefix]:stages)for(usize i=0;i<document_.ModuleCount(emitter_index_,stage);++i){auto json=document_.ModuleJson({emitter_index_,stage,i});for(auto it=json.begin();it!=json.end();++it)if(it.key()!="module"&&it.key()!="enabled")fields<<QString("%1[%2].%3").arg(prefix).arg(i).arg(QString::fromStdString(it.key()));}
        if(fields.empty()){SetStatus("Add a module before creating a binding",true);return;}
        QDialog dialog(this);dialog.setWindowTitle("Bind Parameter");auto* form=new QFormLayout(&dialog);auto* parameter=new QComboBox;parameter->addItems(params);auto* field=new QComboBox;field->addItems(fields);form->addRow("Parameter",parameter);form->addRow("Module field",field);auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);form->addRow(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);if(dialog.exec()!=QDialog::Accepted)return;
        if(document_.AddBinding(emitter_index_,{parameter->currentText().toStdString(),field->currentText().toStdString()})){RefreshSystemTools();RefreshDiagnostics();}
    });
    connect(remove_binding,&QPushButton::clicked,this,[this]{if(binding_list_->currentRow()>=0){document_.RemoveBinding(emitter_index_,usize(binding_list_->currentRow()));RefreshSystemTools();RefreshDiagnostics();}});
    auto edit_sub_emitter=[this](int row,bool create){
        const auto* emitter=document_.EmitterAt(emitter_index_);if(!emitter)return;aether::vfx::SubEmitter sub;if(!create){if(row<0||usize(row)>=emitter->sub_emitters.size())return;sub=emitter->sub_emitters[usize(row)];}else if(!document_.Get().emitters.empty())sub.emitter=document_.Get().emitters.front().settings.name;
        QDialog dialog(this);dialog.setWindowTitle(create?"Add Sub-emitter":"Edit Sub-emitter");auto* form=new QFormLayout(&dialog);auto* event=new QComboBox;event->addItems({"Birth","Death","Collision"});event->setCurrentIndex(int(sub.event));auto* target=new QComboBox;for(const auto& e:document_.Get().emitters)target->addItem(QString::fromStdString(e.settings.name));target->setCurrentText(QString::fromStdString(sub.emitter));auto spin=[&](double value,double lo,double hi,int decimals){auto* s=new QDoubleSpinBox;s->setRange(lo,hi);s->setDecimals(decimals);s->setValue(value);return s;};auto* min_count=spin(sub.count.min,0,100000,2);auto* max_count=spin(sub.count.max,0,100000,2);auto* probability=spin(sub.probability,0,1,3);auto* inherit_velocity=spin(sub.inherit_velocity,0,1000,3);auto* max_frame=spin(sub.max_per_frame,1,1000000,0);auto* inherit_color=new QCheckBox;inherit_color->setChecked(sub.inherit_color);auto* inherit_size=new QCheckBox;inherit_size->setChecked(sub.inherit_size);form->addRow("Trigger",event);form->addRow("Target emitter",target);form->addRow("Count minimum",min_count);form->addRow("Count maximum",max_count);form->addRow("Probability",probability);form->addRow("Inherit velocity",inherit_velocity);form->addRow("Inherit color",inherit_color);form->addRow("Inherit size",inherit_size);form->addRow("Events per frame cap",max_frame);auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);form->addRow(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);if(dialog.exec()!=QDialog::Accepted)return;
        sub.event=aether::vfx::ParticleEventKind(event->currentIndex());sub.emitter=target->currentText().toStdString();sub.count={float(min_count->value()),float(max_count->value())};sub.probability=float(probability->value());sub.inherit_velocity=float(inherit_velocity->value());sub.inherit_color=inherit_color->isChecked();sub.inherit_size=inherit_size->isChecked();sub.max_per_frame=uint32_t(max_frame->value());if(create)document_.AddSubEmitter(emitter_index_,sub);else document_.SetSubEmitter(emitter_index_,usize(row),sub);RefreshSystemTools();RefreshDiagnostics();
    };
    connect(add_sub_emitter,&QPushButton::clicked,this,[edit_sub_emitter]{edit_sub_emitter(-1,true);});connect(remove_sub_emitter,&QPushButton::clicked,this,[this]{if(sub_emitter_list_->currentRow()>=0){document_.RemoveSubEmitter(emitter_index_,usize(sub_emitter_list_->currentRow()));RefreshSystemTools();RefreshDiagnostics();}});connect(sub_emitter_list_,&QListWidget::itemDoubleClicked,this,[edit_sub_emitter](QListWidgetItem* item){edit_sub_emitter(item->data(Qt::UserRole).toInt(),false);});
    connect(remove_module,&QPushButton::clicked,this,[this]{if(module_ref_){document_.RemoveModule(*module_ref_);module_ref_.reset();Refresh();}});
    connect(up,&QPushButton::clicked,this,[this]{if(module_ref_&&module_ref_->index>0){module_ref_=document_.MoveModule(*module_ref_,module_ref_->index-1);Refresh();}});
    connect(down,&QPushButton::clicked,this,[this]{if(module_ref_&&module_ref_->index+1<document_.ModuleCount(emitter_index_,module_ref_->stage)){module_ref_=document_.MoveModule(*module_ref_,module_ref_->index+1);Refresh();}});
    Refresh();
}

bool VfxEditorWidget::OpenFile(const std::filesystem::path& path) { if(!ConfirmDiscardChanges(this))return false;std::string error; if(!document_.Load(path,&error)){SetStatus(QString::fromStdString(error),true);return false;} emitter_index_=0;module_ref_.reset();Refresh();SetStatus(QString("Opened %1").arg(QString::fromStdWString(path.filename().wstring())));return true; }
bool VfxEditorWidget::ConfirmDiscardChanges(QWidget* parent) const { if(!document_.Dirty())return true; return QMessageBox::question(parent?parent:const_cast<VfxEditorWidget*>(this),"Unsaved particle system","Discard unsaved particle system changes?",QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Cancel)==QMessageBox::Discard; }
void VfxEditorWidget::ClearDocument(){document_=aether::editor::ParticleSystemDocument{};emitter_index_=0;module_ref_.reset();playing_=false;timer_->stop();Refresh();}
void VfxEditorWidget::SaveCurrent(){if(document_.Path().empty()){SaveAs();return;}std::string error;if(!document_.Save(&error))SetStatus(QString::fromStdString(error),true);else{Refresh();SetStatus("Particle system saved");}}
void VfxEditorWidget::SaveAs(){const QString path=QFileDialog::getSaveFileName(this,"Save Particle System",QString(),"Aether Particle System (*.avfx)");if(path.isEmpty())return;std::string error;if(!document_.SaveAs(std::filesystem::path(path.toStdWString()),&error))SetStatus(QString::fromStdString(error),true);else{Refresh();SetStatus("Particle system saved");}}
void VfxEditorWidget::UndoCurrent(){if(document_.Undo())Refresh();}
void VfxEditorWidget::RedoCurrent(){if(document_.Redo())Refresh();}
void VfxEditorWidget::SetStatus(const QString& text,bool error){status_->setText(text);status_->setStyleSheet(error?"color:#ff6b6b":"color:#9eacbf");}
void VfxEditorWidget::SyncPreview(){float time=preview_.Time();preview_.Seek(document_.Get(),time);canvas_->update();stats_->setText(QString("%1 particles · %2 s").arg(preview_.Count()).arg(preview_.Time(),0,'f',2));setWindowTitle(QString("VFX · %1%2").arg(QString::fromStdString(document_.Name()),document_.Dirty()?" *":""));}
void VfxEditorWidget::SelectEmitter(int row){if(rebuilding_||row<0||usize(row)>=document_.Get().emitters.size())return;emitter_index_=usize(row);module_ref_.reset();Refresh();}
void VfxEditorWidget::SelectModule(QTreeWidgetItem* item){if(!item||item->data(0,kIsStageRole).toBool()||!item->data(0,kModuleRole).isValid()){module_ref_.reset();RefreshDetails();return;}module_ref_=aether::editor::ModuleRef{usize(item->data(0,kEmitterRole).toInt()),aether::editor::VfxStage(item->data(0,kStageRole).toInt()),usize(item->data(0,kModuleRole).toInt())};RefreshDetails();}
void VfxEditorWidget::AddEmitter(){bool ok=false;QString name=QInputDialog::getText(this,"Add Emitter","Emitter name",QLineEdit::Normal,"Emitter",&ok);if(!ok)return;emitter_index_=document_.AddEmitter(name.toStdString());Refresh();}
void VfxEditorWidget::AddModule(){auto* item=stack_->currentItem();if(!item)return;const auto stage=item->data(0,kIsStageRole).toBool()?aether::editor::VfxStage(item->data(0,kStageRole).toInt()):module_ref_?module_ref_->stage:aether::editor::VfxStage::Spawn;auto names=aether::editor::StageModuleNames(stage);QStringList options;for(auto& n:names)options<<QString::fromStdString(n);bool ok=false;QString selected=QInputDialog::getItem(this,"Add Module",QString("%1 module").arg(QString::fromUtf8(aether::editor::StageLabel(stage))),options,0,false,&ok);if(!ok)return;std::string error;auto ref=document_.AddModule(emitter_index_,stage,selected.toStdString(),&error);if(!ref)SetStatus(QString::fromStdString(error),true);else{module_ref_=*ref;Refresh();}}
void VfxEditorWidget::EditJsonField(const std::string& key,const nlohmann::json& value){if(!module_ref_)return;std::string error;if(!document_.SetModuleField(*module_ref_,key,value,&error,key)){SetStatus(QString::fromStdString(error),true);RefreshDetails();return;}SyncPreview();RefreshDiagnostics();}
void VfxEditorWidget::Refresh(){rebuilding_=true;emitters_->clear();for(const auto& e:document_.Get().emitters)emitters_->addItem(QString::fromStdString(e.settings.name));if(!document_.Get().emitters.empty()){emitter_index_=std::min(emitter_index_,document_.Get().emitters.size()-1);emitters_->setCurrentRow(int(emitter_index_));}stack_->clear();module_ref_.reset();if(const auto* e=document_.EmitterAt(emitter_index_)){for(auto stage:{aether::editor::VfxStage::Spawn,aether::editor::VfxStage::Init,aether::editor::VfxStage::Update,aether::editor::VfxStage::Render}){auto* top=new QTreeWidgetItem(stack_,{QString::fromUtf8(aether::editor::StageLabel(stage))});top->setData(0,kStageRole,int(stage));top->setData(0,kEmitterRole,int(emitter_index_));top->setData(0,kIsStageRole,true);for(usize i=0;i<document_.ModuleCount(emitter_index_,stage);++i){aether::editor::ModuleRef ref{emitter_index_,stage,i};auto j=document_.ModuleJson(ref);auto* child=new QTreeWidgetItem(top,{QString::fromStdString(document_.ModuleName(ref))});child->setData(0,kEmitterRole,int(emitter_index_));child->setData(0,kStageRole,int(stage));child->setData(0,kModuleRole,int(i));child->setCheckState(0,j.value("enabled",true)?Qt::Checked:Qt::Unchecked);}}stack_->expandAll();}
    rebuilding_=false;RefreshDetails();RefreshSystemTools();SyncPreview();RefreshDiagnostics();setWindowTitle(QString("VFX · %1%2").arg(QString::fromStdString(document_.Name()),document_.Dirty()?" *":"")); }
void VfxEditorWidget::RefreshSystemTools(){
    parameter_list_->clear();for(const auto& parameter:document_.Get().parameters){auto* item=new QListWidgetItem(QString("%1  ·  %2").arg(QString::fromStdString(parameter.name),ParameterLabel(parameter.value.type)),parameter_list_);item->setData(Qt::UserRole,QString::fromStdString(parameter.name));}
    binding_list_->clear();sub_emitter_list_->clear();const auto* emitter=document_.EmitterAt(emitter_index_);if(!emitter){binding_list_->addItem("Select or add an emitter to edit bindings");sub_emitter_list_->addItem("Select or add an emitter to edit sub-emitters");return;}
    for(usize i=0;i<emitter->bindings.size();++i){const auto& binding=emitter->bindings[i];auto* item=new QListWidgetItem(QString("%1  →  %2").arg(QString::fromStdString(binding.parameter),QString::fromStdString(binding.field)),binding_list_);item->setData(Qt::UserRole,int(i));}
    sub_emitter_list_->clear();for(usize i=0;i<emitter->sub_emitters.size();++i){const auto& sub=emitter->sub_emitters[i];const QString event= sub.event==aether::vfx::ParticleEventKind::Birth?"Birth":sub.event==aether::vfx::ParticleEventKind::Death?"Death":"Collision";auto* item=new QListWidgetItem(QString("%1  →  %2  ·  %3–%4").arg(event,QString::fromStdString(sub.emitter)).arg(sub.count.min).arg(sub.count.max),sub_emitter_list_);item->setData(Qt::UserRole,int(i));}
    if(binding_list_->count()==0)binding_list_->addItem("No parameter bindings");if(sub_emitter_list_->count()==0)sub_emitter_list_->addItem("No sub-emitters");
}
void VfxEditorWidget::RefreshDetails(){while(auto* item=details_layout_->takeAt(0)){if(item->widget())item->widget()->deleteLater();delete item;}auto* form=new QFormLayout;details_layout_->addLayout(form);if(!module_ref_){const auto settings=document_.EmitterSettings(emitter_index_);for(const char* key:{"name","enabled","max_particles","space","duration","looping","start_delay","warmup","target"}){if(!settings.contains(key))continue;const auto val=settings[key];auto* line=new QLineEdit(JsonText(val));form->addRow(QString::fromLatin1(key),line);connect(line,&QLineEdit::editingFinished,this,[this,key=std::string(key),line]{nlohmann::json v;try{v=nlohmann::json::parse(line->text().toStdString());std::string err;if(!document_.SetEmitterSetting(emitter_index_,key,v,&err,key)){SetStatus(QString::fromStdString(err),true);return;}if(key=="name")Refresh();else{SyncPreview();RefreshDiagnostics();}}catch(...){SetStatus("Enter a valid JSON value",true);}});}return;}
    auto json=document_.ModuleJson(*module_ref_);for(auto it=json.begin();it!=json.end();++it){if(it.key()=="module"||it.key()=="enabled")continue;const std::string key=it.key();const auto value=it.value();
        if(key=="curve"||(value.is_object()&&value.contains("keys"))){aether::vfx::FloatCurve curve;std::string error;if(aether::vfx::FromJson(value,curve,&error)){auto* interpolation=new QComboBox(details_);interpolation->addItems({"Linear","Smooth","Constant"});interpolation->setCurrentIndex(static_cast<int>(curve.interp));form->addRow(QString::fromStdString(key)+" interpolation",interpolation);auto* graph=new CurvePreview(std::move(curve),[this,key](const nlohmann::json& updated){EditJsonField(key,updated);},details_);graph->setToolTip("Left-click to add or drag keys. Right-click a key to remove it. The last key is preserved.");form->addRow(graph);connect(interpolation,qOverload<int>(&QComboBox::currentIndexChanged),graph,[graph](int index){graph->SetInterpolation(static_cast<aether::vfx::CurveInterp>(index));});continue;}}
        if(key=="gradient"||key=="color"||(value.is_object()&&(value.contains("colors")||value.contains("alphas")))){aether::vfx::ColorGradient gradient;std::string error;if(aether::vfx::FromJson(value,gradient,&error)){auto* graph=new GradientPreview(std::move(gradient),[this,key](const nlohmann::json& updated){EditJsonField(key,updated);},details_);graph->setToolTip("Click the bar to add a color key, double-click a key to edit it, and right-click to remove. Add and edit opacity keys below the bar.");form->addRow(QString::fromStdString(key)+" gradient",graph);continue;}}
        auto* line=new QLineEdit(JsonText(value));line->setPlaceholderText("JSON value");form->addRow(QString::fromStdString(key),line);connect(line,&QLineEdit::editingFinished,this,[this,key,line]{try{EditJsonField(key,nlohmann::json::parse(line->text().toStdString()));}catch(...){SetStatus("Enter a valid JSON value",true);}});}
}
void VfxEditorWidget::RefreshDiagnostics(){
    diagnostics_->clear();
    for(const auto& d:document_.Diagnostics()){auto* item=new QListWidgetItem(QString::fromStdString(d.message),diagnostics_);item->setForeground(d.error?QColor("#ff6b6b"):QColor("#e6b450"));}
    if(diagnostics_->count()==0)diagnostics_->addItem("No particle system validation issues");
    const auto* emitter=document_.EmitterAt(emitter_index_);
    if(!emitter){gpu_status_->setText("GPU preview readiness · add an emitter to inspect its shader");return;}
    const auto support=aether::vfx::CheckGpuSupport(*emitter);
    QString text=support.supported
        ? "GPU shader eligible · preview simulation is CPU-backed until RHI compute dispatch is implemented."
        : "CPU fallback required · " + QString::fromStdString(support.reasons.front());
    gpu_status_->setText(text);
}

void VfxEditorWidget::InspectGpuShader(){
    const auto* emitter=document_.EmitterAt(emitter_index_);
    if(!emitter){SetStatus("Add an emitter before inspecting its GPU shader",true);return;}
    const auto generated=aether::vfx::GenerateParticleShader(*emitter);
    QDialog dialog(this);dialog.setWindowTitle("GPU Particle Shader");dialog.resize(860,620);
    auto* layout=new QVBoxLayout(&dialog);
    auto* summary=new QLabel;
    summary->setWordWrap(true);
    QString message=generated.ok
        ? QString("Shader generated · %1 module constants · %2 · async compute queues are available, but the shared RHI does not yet expose compute pipelines or storage-buffer bindings, so preview remains CPU-backed.").arg(generated.module_constants).arg(generated.uses_depth?"uses scene depth":"no scene depth")
        : QString("GPU shader unavailable for this emitter.");
    if(!generated.errors.empty())for(const auto& reason:generated.errors)message+=QString("\n• %1").arg(QString::fromStdString(reason));
    summary->setText(message);layout->addWidget(summary);
    auto* compile=new QPushButton("Validate Compute Shader Backends");compile->setEnabled(generated.ok);layout->addWidget(compile);
    connect(compile,&QPushButton::clicked,&dialog,[this,&dialog,&generated,summary]{
        QStringList results;
        bool any_failure=false;
#if defined(_WIN32)
        try{
            for(const char* entry:{"ResetMain","EmitMain","UpdateMain"})
                (void)aether::gfx::CompileHLSL(generated.hlsl,entry,"cs_5_1","vfx_editor_particle_compute_d3d12");
            results<<"D3D12: all three compute entry points compiled";
        }catch(const std::exception& e){
            results<<QString("D3D12: compile failed (%1)").arg(QString::fromUtf8(e.what()));
            any_failure=true;
        }
#else
        results<<"D3D12: compiler unavailable on this platform";
#endif
        try{
            for(const char* entry:{"ResetMain","EmitMain","UpdateMain"})
                (void)aether::gfx::CompileHLSLToSPIRV(generated.hlsl,entry,"cs_6_0","vfx_editor_particle_compute_vulkan");
            results<<"Vulkan: all three compute entry points compiled to SPIR-V";
        }catch(const std::exception& e){
            results<<QString("Vulkan: compile failed (%1)").arg(QString::fromUtf8(e.what()));
            any_failure=true;
        }
        summary->setText(QString("Backend shader validation\n%1\n\nThe Qt particle preview remains CPU-backed until the shared RHI supports compute pipelines and storage-buffer bindings.").arg(results.join("\n")));
        SetStatus(any_failure?"GPU particle shader validation failed":"GPU particle shader validation finished",any_failure);
    });
    auto* source=new QPlainTextEdit;source->setReadOnly(true);source->setLineWrapMode(QPlainTextEdit::NoWrap);
    source->setPlainText(QString::fromStdString(generated.hlsl));layout->addWidget(source,1);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Close);layout->addWidget(buttons);
    auto* save=buttons->button(QDialogButtonBox::Save);save->setEnabled(generated.ok);
    connect(save,&QPushButton::clicked,&dialog,[this,&dialog,source]{const QString path=QFileDialog::getSaveFileName(this,"Export GPU Particle Shader",QString(),"HLSL shader (*.hlsl)");if(path.isEmpty())return;QFile file(path);if(!file.open(QIODevice::WriteOnly|QIODevice::Text)){SetStatus(QString("Could not write shader: %1").arg(file.errorString()),true);return;}QTextStream stream(&file);stream<<source->toPlainText();SetStatus("GPU particle shader exported");});
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    dialog.exec();
}

} // namespace aether::editor::qt
