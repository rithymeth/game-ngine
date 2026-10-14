#include "sound_cue_editor_widget.h"
#include "aether/audio/stream.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollBar>
#include <QFormLayout>
#include <QSlider>
#include <QPainter>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace aether::editor::qt {
namespace {
using aether::audio::CueNode;
using aether::audio::CueNodeType;

class WaveformWidget final : public QWidget {
public:
    explicit WaveformWidget(QWidget* parent=nullptr):QWidget(parent){setMinimumHeight(120);setMinimumWidth(280);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);}
    void SetWave(const aether::audio::SoundWave& wave){channels_=wave.channels;sample_rate_=wave.sample_rate;duration_=wave.Duration();bins_.clear();constexpr usize kBins=4096;const usize frames=wave.Frames();if(frames==0||channels_==0)return;const usize count=std::min(kBins,frames);bins_.resize(count*channels_);for(usize b=0;b<count;++b){const usize begin=b*frames/count,end=std::max(begin+1,(b+1)*frames/count);for(u32 channel=0;channel<channels_;++channel){float low=1.0f,high=-1.0f;for(usize f=begin;f<std::min(end,frames);++f){const float s=wave.samples[f*channels_+channel];low=std::min(low,s);high=std::max(high,s);}bins_[b*channels_+channel]={low,high};}}update();}
    void SetZoom(int value){zoom_=std::clamp(value,1,16);offset_=std::clamp(offset_,0.0f,1.0f-1.0f/zoom_);update();}
    void SetOffset(int value){offset_=float(value)/(1000.0f*zoom_);update();}
    void SetPositionChanged(std::function<void(float)> callback){position_changed_=std::move(callback);}
    float Position()const{return cursor_;}
protected:
    void paintEvent(QPaintEvent*)override{QPainter p(this);p.fillRect(rect(),QColor("#111820"));p.setRenderHint(QPainter::Antialiasing);const QRectF area=rect().adjusted(12,10,-12,-26);p.setPen(QPen(QColor("#34414d"),1));p.drawRect(area);for(int i=1;i<4;++i){const qreal y=area.top()+area.height()*i/4.0;p.drawLine(QPointF(area.left(),y),QPointF(area.right(),y));}if(bins_.empty()){p.setPen(QColor("#9eacbf"));p.drawText(area,Qt::AlignCenter,"Select a supported audio asset to inspect its waveform");return;}const qreal lane_h=area.height()/channels_;const usize total=bins_.size()/channels_;const usize visible=std::max<usize>(1,total/usize(zoom_));const usize start=std::min<usize>(usize(offset_*float(total)),total-visible);p.setPen(QPen(QColor("#67d9bd"),1));for(u32 ch=0;ch<channels_;++ch){const qreal center=area.top()+lane_h*(ch+0.5);for(int x=0;x<int(area.width());++x){const usize bin=start+std::min<usize>(visible-1,usize(qreal(x)/area.width()*visible));const auto& peak=bins_[bin*channels_+ch];p.drawLine(QPointF(area.left()+x,center-peak.second*lane_h*0.45),QPointF(area.left()+x,center-peak.first*lane_h*0.45));}}const qreal x=area.left()+(cursor_-offset_)*zoom_*area.width();p.setPen(QPen(QColor("#ef7c78"),1));p.drawLine(QPointF(x,area.top()),QPointF(x,area.bottom()));p.setPen(QColor("#9eacbf"));p.drawText(QRectF(area.left(),area.bottom()+2,area.width(),14),Qt::AlignLeft,QString("Position %1%   ·   Duration %2 s").arg(int(cursor_*100)).arg(duration_,0,'f',3));}
    void mousePressEvent(QMouseEvent*e)override{const auto area=rect().adjusted(12,10,-12,-26);cursor_=std::clamp(offset_+float(e->position().x()-area.left())/std::max(1,area.width()*zoom_),0.0f,1.0f);update();if(position_changed_)position_changed_(cursor_);}
private:
    std::vector<std::pair<float,float>> bins_;std::function<void(float)> position_changed_;u32 channels_=0,sample_rate_=0;float duration_=0,cursor_=0,offset_=0;int zoom_=1;
};

class CueNodeItem final : public QGraphicsRectItem {
public:
    using Moved = std::function<void(u32, QPointF)>;
    CueNodeItem(u32 id, QString title, QColor color, Moved moved, QGraphicsItem* parent = nullptr)
        : QGraphicsRectItem(0, 0, 190, 76, parent), id_(id), moved_(std::move(moved)) {
        setBrush(QColor("#202a34"));setPen(QPen(color,2));setFlags(ItemIsSelectable|ItemIsMovable|ItemSendsGeometryChanges);setData(0,id);
        auto* bar=new QGraphicsRectItem(0,0,190,23,this);bar->setBrush(color);bar->setPen(Qt::NoPen);auto* label=new QGraphicsTextItem(std::move(title),this);label->setDefaultTextColor(Qt::white);label->setPos(8,2);label->setTextWidth(174);
        auto* in=new QGraphicsEllipseItem(-4,34,8,8,this);in->setBrush(QColor("#67d9bd"));in->setPen(Qt::NoPen);auto* out=new QGraphicsEllipseItem(186,34,8,8,this);out->setBrush(QColor("#67d9bd"));out->setPen(Qt::NoPen);
    }
protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override { press_pos_=pos();QGraphicsRectItem::mousePressEvent(event); }
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override { QGraphicsRectItem::mouseReleaseEvent(event);if(moved_&&pos()!=press_pos_)moved_(id_,pos()); }
private:
    u32 id_;Moved moved_;QPointF press_pos_;
};

class CueOutputItem final : public QGraphicsRectItem {
public:
    using Chosen=std::function<void()>;
    CueOutputItem(bool active,Chosen chosen):QGraphicsRectItem(0,0,140,54),chosen_(std::move(chosen)){setBrush(QColor("#2d2227"));setPen(QPen(active?QColor("#ef7c78"):QColor("#854e50"),2));setFlags(ItemIsSelectable);auto* text=new QGraphicsTextItem(active?"CUE OUTPUT ✓":"CUE OUTPUT",this);text->setDefaultTextColor(Qt::white);text->setPos(12,15);}
protected:void mousePressEvent(QGraphicsSceneMouseEvent* event)override{QGraphicsRectItem::mousePressEvent(event);if(chosen_)chosen_();}
private:Chosen chosen_;
};

QString NodeTitle(const CueNode& node) {
    QString title = QString::fromLatin1(aether::audio::CueNodeTypeName(node.type));
    if (node.type == CueNodeType::Wave) title += QString(" · %1").arg(node.sound.empty() ? "(no sound)" : QString::fromStdString(node.sound));
    title += QString("  #%1").arg(node.id);
    if (!node.children.empty()) {
        title += "  ← ";
        for (usize i = 0; i < node.children.size(); ++i) { if (i) title += ", "; title += QString::number(node.children[i]); }
    }
    return title;
}

QDoubleSpinBox* Number(QWidget* parent, double value, double low, double high, int decimals = 3) {
    auto* box = new QDoubleSpinBox(parent); box->setRange(low, high); box->setDecimals(decimals); box->setSingleStep(decimals ? 0.05 : 1); box->setValue(value); return box;
}
}

SoundCueEditorWidget::SoundCueEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this); root->setContentsMargins(8, 8, 8, 8);
    auto* toolbar = new QHBoxLayout;
    auto button = [&](const QString& label) { auto* b = new QPushButton(label, this); toolbar->addWidget(b); return b; };
    auto* fresh = button("New"); auto* open = button("Open…"); auto* save = button("Save"); auto* save_as = button("Save As…");
    auto* undo = button("Undo"); auto* redo = button("Redo"); auto* mixer = button("Mixer"); toolbar->addStretch(); preview_button_=button("▶ Preview"); root->addLayout(toolbar);
    auto* split = new QSplitter(this); auto* left = new QWidget(split); auto* left_layout = new QVBoxLayout(left); left_layout->setContentsMargins(2, 2, 2, 2);
    left_layout->addWidget(new QLabel("CUE GRAPH", left));node_tabs_=new QTabWidget(left);graph_scene_=new QGraphicsScene(node_tabs_);graph_view_=new QGraphicsView(graph_scene_,node_tabs_);graph_view_->setRenderHint(QPainter::Antialiasing);graph_view_->setDragMode(QGraphicsView::RubberBandDrag);graph_view_->setBackgroundBrush(QColor("#121920"));graph_view_->setMinimumSize(400,300);node_tabs_->addTab(graph_view_,"Graph");nodes_=new QListWidget(node_tabs_);node_tabs_->addTab(nodes_,"Nodes");left_layout->addWidget(node_tabs_,1);
    auto* add_row = new QHBoxLayout; node_type_ = new QComboBox(left); node_type_->addItems({"Wave", "Random", "Sequence", "Modulator", "Concatenator", "Loop", "Mix", "Delay"}); auto* add = new QPushButton("Add Node", left); auto* remove = new QPushButton("Remove", left); add_row->addWidget(node_type_, 1); add_row->addWidget(add); add_row->addWidget(remove); left_layout->addLayout(add_row);
    auto* right = new QWidget(split); auto* right_layout = new QVBoxLayout(right); right_layout->setContentsMargins(2, 2, 2, 2); details_ = new QWidget(right); auto* form = new QFormLayout(details_); cue_name_ = new QLineEdit(details_); form->addRow("Cue name", cue_name_); right_layout->addWidget(details_, 1); diagnostics_ = new QLabel(right); diagnostics_->setWordWrap(true); right_layout->addWidget(diagnostics_); split->setSizes({330, 540}); root->addWidget(split, 1);
    preview_status_=new QLabel("Audio preview ready when the project has sound assets",this);root->addWidget(preview_status_);
    status_ = new QLabel("Sound Cue · author node behavior and connections", this); root->addWidget(status_);

    mixer_.AddDefaultBuses();cue_player_=std::make_unique<aether::audio::CuePlayer>(mixer_,sound_bank_);
    audio_output_=std::make_unique<aether::audio::AudioOutput>(mixer_,aether::audio::CreateDefaultBackend(mixer_.SampleRate()));std::string audio_error;
    if(!audio_output_->Start(480,&audio_error))preview_status_->setText(QString("Audio output unavailable · %1").arg(QString::fromStdString(audio_error)));
    preview_timer_=new QTimer(this);preview_timer_->setInterval(30);connect(preview_timer_,&QTimer::timeout,this,[this]{if(!cue_player_)return;cue_player_->Update();if(preview_handle_!=0&&!cue_player_->IsPlaying(preview_handle_)){preview_handle_=0;preview_timer_->stop();preview_button_->setText("▶ Preview");if(preview_asset_button_)preview_asset_button_->setText("▶ Preview Asset");}});

    connect(fresh, &QPushButton::clicked, this, [this] { if (!ConfirmDiscardChanges(this)) return; document_ = aether::editor::SoundCueDocument{}; selected_row_ = -1; graph_view_fitted_=false; Refresh(); SetStatus("New sound cue"); });
    connect(open, &QPushButton::clicked, this, [this] { const QString path = QFileDialog::getOpenFileName(this, "Open Sound Cue", {}, "Sound Cue (*.acue)"); if (!path.isEmpty()) OpenFile(std::filesystem::path(path.toStdWString())); });
    connect(save, &QPushButton::clicked, this, [this] { SaveCurrent(); }); connect(save_as, &QPushButton::clicked, this, [this] { SaveAs(); });
    connect(undo, &QPushButton::clicked, this, [this] { UndoCurrent(); }); connect(redo, &QPushButton::clicked, this, [this] { RedoCurrent(); });
    connect(preview_button_,&QPushButton::clicked,this,[this]{if(preview_handle_!=0)StopPreview();else PlayPreview();});
    connect(mixer,&QPushButton::clicked,this,[this]{ShowMixer();});
    connect(nodes_, &QListWidget::currentRowChanged, this, [this](int row) { if (!rebuilding_) { selected_row_ = row;for(auto* item:graph_scene_->items())if(item->data(0).toUInt()==(row>=0?nodes_->item(row)->data(Qt::UserRole).toUInt():0u))item->setSelected(true);RefreshDetails(); } });
    connect(graph_scene_,&QGraphicsScene::selectionChanged,this,[this]{if(rebuilding_)return;for(auto* item:graph_scene_->selectedItems()){const auto id=item->data(0).toUInt();for(int row=0;row<nodes_->count();++row)if(nodes_->item(row)->data(Qt::UserRole).toUInt()==id){selected_row_=row;nodes_->setCurrentRow(row);RefreshDetails();return;}}});
    connect(add, &QPushButton::clicked, this, [this] {
        const QString type_name = node_type_->currentText(); aether::audio::CueNodeType type;
        if (!aether::audio::ParseCueNodeType(type_name.toStdString(), type)) return;
        std::string sound;
        if (type == aether::audio::CueNodeType::Wave) { bool ok = false; const QString value = QInputDialog::getText(this, "Add Wave", "Sound asset name", QLineEdit::Normal, {}, &ok); if (!ok) return; sound = value.trimmed().toStdString(); }
        const u32 id = document_.AddNode(type, float(nodes_->count() % 4) * 220.0f, float(nodes_->count() / 4) * 150.0f, sound);
        selected_row_ = int(document_.Get().nodes.size()) - 1; Refresh(); for (int i = 0; i < nodes_->count(); ++i) if (nodes_->item(i)->data(Qt::UserRole).toUInt() == id) nodes_->setCurrentRow(i);
    });
    connect(remove, &QPushButton::clicked, this, [this] { if (selected_row_ < 0 || selected_row_ >= int(document_.Get().nodes.size())) return; const u32 id = document_.Get().nodes[size_t(selected_row_)].id; document_.DeleteNodes({id}); selected_row_ = -1; Refresh(); });
    connect(cue_name_, &QLineEdit::editingFinished, this, [this] { if (rebuilding_) return; const std::string value = cue_name_->text().trimmed().toStdString(); document_.Edit("Rename cue", [value](auto& cue) { cue.name = value; }, "cue-name"); Refresh(); });
    Refresh();
}

void SoundCueEditorWidget::SetStatus(const QString& message, bool error) { status_->setText(message); status_->setStyleSheet(error ? "color:#ff6b6b" : "color:#9eacbf"); }
void SoundCueEditorWidget::EditSelected(const std::function<void(aether::audio::CueNode&)>& edit, const std::string& label, const std::string& key) {
    if (selected_row_ < 0 || selected_row_ >= int(document_.Get().nodes.size())) return;
    const u32 id = document_.Get().nodes[size_t(selected_row_)].id;
    document_.Edit(label, [=](auto& cue) { if (auto* node = cue.Find(id)) edit(*node); }, key); Refresh();
}

void SoundCueEditorWidget::Refresh() {
    rebuilding_ = true; nodes_->clear();
    for (const auto& node : document_.Get().nodes) { auto* item = new QListWidgetItem(NodeTitle(node), nodes_); item->setData(Qt::UserRole, node.id); }
    selected_row_ = selected_row_ >= 0 && selected_row_ < nodes_->count() ? selected_row_ : -1; nodes_->setCurrentRow(selected_row_); cue_name_->setText(QString::fromStdString(document_.Get().name));
    RefreshGraph();
    const auto problems = document_.Diagnostics(); diagnostics_->setText(problems.empty() ? "Ready · no cue issues" : QString::fromStdString(problems.front().code + ": " + problems.front().message));
    rebuilding_ = false; RefreshDetails(); setWindowTitle(QString("Sound Cue · %1%2").arg(QString::fromStdString(document_.Name()), document_.Dirty() ? " *" : ""));
}

void SoundCueEditorWidget::RefreshGraph(){
    graph_scene_->clear();std::unordered_map<u32,CueNodeItem*> items;QRectF bounds;
    auto moved=[this](u32 id,QPointF position){QTimer::singleShot(0,graph_view_,[this,id,position]{document_.Edit("Move cue node",[=](auto& cue){if(auto* node=cue.Find(id))node->x=float(position.x()),node->y=float(position.y());},"cue-node-position-"+std::to_string(id));for(int row=0;row<nodes_->count();++row)if(nodes_->item(row)->data(Qt::UserRole).toUInt()==id)nodes_->setCurrentRow(row);Refresh();});};
    auto color=[](aether::audio::CueNodeType type){switch(type){case aether::audio::CueNodeType::Wave:return QColor("#397eb2");case aether::audio::CueNodeType::Random:case aether::audio::CueNodeType::Sequence:return QColor("#8b61b5");case aether::audio::CueNodeType::Modulator:return QColor("#b28640");case aether::audio::CueNodeType::Concatenator:case aether::audio::CueNodeType::Loop:return QColor("#398c70");case aether::audio::CueNodeType::Mix:return QColor("#ac5755");case aether::audio::CueNodeType::Delay:return QColor("#707b8a");}return QColor("#64707c");};
    for(const auto& node:document_.Get().nodes){auto* item=new CueNodeItem(node.id,NodeTitle(node),color(node.type),moved);item->setPos(node.x,node.y);graph_scene_->addItem(item);items[node.id]=item;bounds|=item->mapRectToScene(item->rect());if(selected_row_>=0&&usize(selected_row_)<document_.Get().nodes.size()&&document_.Get().nodes[usize(selected_row_)].id==node.id)item->setSelected(true);}
    for(const auto& parent:document_.Get().nodes){const auto parent_it=items.find(parent.id);if(parent_it==items.end())continue;for(usize index=0;index<parent.children.size();++index){const auto child_it=items.find(parent.children[index]);if(child_it==items.end())continue;const QPointF from=child_it->second->mapToScene(QPointF(190,38));const QPointF to=parent_it->second->mapToScene(QPointF(0,38));QPainterPath path(from);const qreal bend=std::max(55.0,std::abs(to.x()-from.x())*0.45);path.cubicTo(from+QPointF(bend,0),to-QPointF(bend,0),to);auto* wire=graph_scene_->addPath(path,QPen(QColor("#67d9bd"),2));wire->setZValue(-2);}
    }
    qreal out_x=bounds.isNull()?260:bounds.right()+95; qreal out_y=0;if(const auto* root=document_.Get().Find(document_.Get().root))out_y=root->y+11;
    auto* output=new CueOutputItem(document_.Get().root!=0,[this]{const u32 root=document_.Get().root;for(int row=0;row<nodes_->count();++row)if(nodes_->item(row)->data(Qt::UserRole).toUInt()==root){nodes_->setCurrentRow(row);node_tabs_->setCurrentIndex(1);return;}});output->setPos(out_x,out_y);graph_scene_->addItem(output);
    if(const auto root=items.find(document_.Get().root);root!=items.end()){const QPointF from=root->second->mapToScene(QPointF(190,38));const QPointF to=output->mapToScene(QPointF(0,27));QPainterPath path(from);path.cubicTo(from+QPointF(50,0),to-QPointF(50,0),to);auto* wire=graph_scene_->addPath(path,QPen(QColor("#ef7c78"),2));wire->setZValue(-1);}
    bounds|=output->mapRectToScene(output->rect());graph_scene_->setSceneRect(bounds.adjusted(-100,-90,100,90));if(!bounds.isNull()&&node_tabs_->currentIndex()==0&&!graph_view_fitted_){graph_view_->fitInView(bounds.adjusted(-60,-60,60,60),Qt::KeepAspectRatio);graph_view_fitted_=true;}
}

void SoundCueEditorWidget::RefreshDetails() {
    preview_asset_button_=nullptr;
    auto* form = qobject_cast<QFormLayout*>(details_->layout());
    while (form->rowCount() > 1) form->removeRow(1);
    if (selected_row_ < 0 || selected_row_ >= int(document_.Get().nodes.size())) { form->addRow(new QLabel("Select a node to edit its settings, inputs, and output.")); return; }
    const CueNode node = document_.Get().nodes[size_t(selected_row_)];
    if (node.type == aether::audio::CueNodeType::Wave) {
        auto* sound = new QComboBox(details_);sound->setEditable(true);for(const auto& name:document_.Sounds())sound->addItem(QString::fromStdString(name));sound->setCurrentText(QString::fromStdString(node.sound));form->addRow("Sound asset", sound);connect(sound,&QComboBox::activated,this,[this,sound]{const auto value=sound->currentText().trimmed().toStdString();EditSelected([value](auto& n){n.sound=value;},"Set wave sound","wave.sound");});connect(sound->lineEdit(),&QLineEdit::editingFinished,this,[this,sound]{const auto value=sound->currentText().trimmed().toStdString();EditSelected([value](auto& n){n.sound=value;},"Set wave sound","wave.sound");});
        auto* waveform=new WaveformWidget(details_);auto* waveform_info=new QLabel("Choose a project WAV, Ogg, or FLAC asset",details_);waveform_info->setWordWrap(true);form->addRow("Waveform",waveform);form->addRow(waveform_info);
        auto* zoom=new QSlider(Qt::Horizontal,details_);zoom->setRange(1,16);zoom->setValue(1);form->addRow("Zoom",zoom);auto* scroll=new QScrollBar(Qt::Horizontal,details_);scroll->setRange(0,0);form->addRow("Scroll",scroll);connect(zoom,&QSlider::valueChanged,waveform,[waveform,scroll](int value){waveform->SetZoom(value);scroll->setRange(0,(value-1)*1000);scroll->setEnabled(value>1);});connect(scroll,&QScrollBar::valueChanged,waveform,[waveform](int value){waveform->SetOffset(value);});
        auto* preview_asset=new QPushButton("▶ Preview Asset",details_);preview_asset_button_=preview_asset;form->addRow(preview_asset);connect(preview_asset,&QPushButton::clicked,this,[this,sound,preview_asset]{if(preview_handle_!=0){StopPreview();return;}const std::string asset=sound->currentText().trimmed().toStdString();if(asset.empty()){SetStatus("Choose an audio asset to preview",true);return;}preview_asset_cue_=aether::audio::SoundCue{};preview_asset_cue_.name="Wave Preview";preview_asset_cue_.root=1;aether::audio::CueNode wave;wave.id=1;wave.type=aether::audio::CueNodeType::Wave;wave.sound=asset;preview_asset_cue_.nodes.push_back(std::move(wave));preview_handle_=cue_player_->Play(preview_asset_cue_);if(preview_handle_==0){SetStatus("Could not preview this audio asset",true);return;}preview_button_->setText("■ Stop");preview_asset->setText("■ Stop Preview");preview_timer_->start();SetStatus(QString("Previewing %1").arg(QString::fromStdString(asset)));});
        const auto sound_names=document_.Sounds();if(std::find(sound_names.begin(),sound_names.end(),node.sound)!=sound_names.end()&&!content_root_.empty()){
            const auto file_path=content_root_/std::filesystem::path(node.sound);std::error_code ec;const auto file_size=std::filesystem::file_size(file_path,ec);
            if(ec||file_size==0)waveform_info->setText("Audio asset file is unavailable.");
            else if(file_size>128ull*1024ull*1024ull)waveform_info->setText("Waveform preview is limited to audio files under 128 MB.");
            else{std::ifstream file(file_path,std::ios::binary);std::vector<aether::u8> bytes(std::istreambuf_iterator<char>(file),{});aether::audio::SoundWave decoded;std::string decode_error;if(!file||!aether::audio::DecodeSound(bytes,decoded,&decode_error))waveform_info->setText(QString("Could not decode waveform: %1").arg(QString::fromStdString(decode_error)));else{waveform->SetWave(decoded);waveform_info->setText(QString("%1 · %2 Hz · %3 channel(s) · %4 frames · %5 s · Click waveform to mark a position").arg(QFileInfo(QString::fromStdString(node.sound)).fileName()).arg(decoded.sample_rate).arg(decoded.channels).arg(decoded.Frames()).arg(decoded.Duration(),0,'f',3));waveform->SetPositionChanged([this](float position){SetStatus(QString("Waveform position · %1% of asset").arg(int(position*100)));});}}
        } else waveform_info->setText("Choose an audio asset from the active project's Content folder.");
        auto* looping = new QCheckBox; looping->setChecked(node.looping); form->addRow("Loop", looping); connect(looping,&QCheckBox::toggled,this,[this](bool value){EditSelected([value](auto& n){n.looping=value;},"Set wave loop","wave.loop");});
    }
    auto field = [this, form](const QString& label, double value, double lo, double hi, const char* key, auto setter) { auto* spin=Number(details_,value,lo,hi);form->addRow(label,spin);connect(spin,&QDoubleSpinBox::editingFinished,this,[this,spin,key,setter]{const float v=float(spin->value());EditSelected([v,setter](auto& n){setter(n,v);},std::string("Set ")+key,key);}); };
    switch(node.type) {
    case aether::audio::CueNodeType::Random: { auto* check=new QCheckBox;check->setChecked(node.no_repeat);form->addRow("Avoid repeating",check);connect(check,&QCheckBox::toggled,this,[this](bool v){EditSelected([v](auto& n){n.no_repeat=v;},"Set repeat behavior","random.no_repeat");}); break; }
    case aether::audio::CueNodeType::Modulator: field("Volume min (dB)",node.volume_min_db,-60,12,"mod.volume_min",[](auto& n,float v){n.volume_min_db=v;});field("Volume max (dB)",node.volume_max_db,-60,12,"mod.volume_max",[](auto& n,float v){n.volume_max_db=v;});field("Pitch min",node.pitch_min,0.1,4,"mod.pitch_min",[](auto& n,float v){n.pitch_min=v;});field("Pitch max",node.pitch_max,0.1,4,"mod.pitch_max",[](auto& n,float v){n.pitch_max=v;});break;
    case aether::audio::CueNodeType::Loop: field("Repeats (0 = forever)",node.count,0,100000,"loop.count",[](auto& n,float v){n.count=u32(v);});break;
    case aether::audio::CueNodeType::Delay: field("Delay min (s)",node.delay_min,0,3600,"delay.min",[](auto& n,float v){n.delay_min=v;});field("Delay max (s)",node.delay_max,0,3600,"delay.max",[](auto& n,float v){n.delay_max=v;});break;
    default: break;
    }
    auto* root_button=new QPushButton(document_.Get().root==node.id?"Cue Output ✓":"Use as Cue Output",details_);form->addRow(root_button);connect(root_button,&QPushButton::clicked,this,[this,id=node.id]{document_.SetRoot(id);Refresh();});
    auto* inputs=new QLabel;QString input_text="Inputs: ";if(node.children.empty())input_text+="none";for(usize i=0;i<node.children.size();++i){if(i)input_text+=", ";input_text+=QString("%1 → node #%2").arg(i+1).arg(node.children[i]);}inputs->setText(input_text);form->addRow(inputs);
    if(node.type!=aether::audio::CueNodeType::Wave){auto* row=new QHBoxLayout;auto* source=new QComboBox;for(const auto& candidate:document_.Get().nodes)if(candidate.id!=node.id)source->addItem(NodeTitle(candidate),candidate.id);auto* pin=Number(details_,node.children.size(),0,100000,0);auto* connect_button=new QPushButton("Connect input",details_);row->addWidget(source,1);row->addWidget(pin);row->addWidget(connect_button);form->addRow("Add / replace input",row);connect(connect_button,&QPushButton::clicked,this,[this,source,pin,id=node.id]{std::string error;if(source->count()==0)return;if(!document_.Connect(source->currentData().toUInt(),id,usize(pin->value()),&error))SetStatus(QString::fromStdString(error),true);Refresh();});
        if(!node.children.empty()){auto* disconnect_row=new QHBoxLayout;auto* index=Number(details_,node.children.size()-1,0,node.children.size()-1,0);auto* disconnect=new QPushButton("Remove input",details_);disconnect_row->addWidget(index);disconnect_row->addWidget(disconnect);form->addRow("Disconnect input",disconnect_row);connect(disconnect,&QPushButton::clicked,this,[this,index,id=node.id]{document_.Disconnect(id,usize(index->value()));Refresh();});}}
}

bool SoundCueEditorWidget::OpenFile(const std::filesystem::path& path) { if(!ConfirmDiscardChanges(this))return false;StopPreview();std::string error;if(!document_.Load(path,&error)){SetStatus(QString::fromStdString(error),true);return false;}selected_row_=-1;graph_view_fitted_=false;if(content_root_.empty())SetContentRoot(path.parent_path());Refresh();SetStatus("Opened sound cue");return true; }
void SoundCueEditorWidget::ClearDocument(){StopPreview();document_=aether::editor::SoundCueDocument{};selected_row_=-1;graph_view_fitted_=false;Refresh();}
bool SoundCueEditorWidget::ConfirmDiscardChanges(QWidget* parent)const{return !document_.Dirty()||QMessageBox::question(parent?parent:const_cast<SoundCueEditorWidget*>(this),"Unsaved Sound Cue","Discard unsaved sound cue changes?",QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Cancel)==QMessageBox::Discard;}
void SoundCueEditorWidget::SaveCurrent(){if(document_.Path().empty()){SaveAs();return;}std::string error;if(!document_.Save(&error))SetStatus(QString::fromStdString(error),true);else{Refresh();SetStatus("Sound cue saved");}}
void SoundCueEditorWidget::SaveAs(){const QString path=QFileDialog::getSaveFileName(this,"Save Sound Cue",{},"Sound Cue (*.acue)");if(path.isEmpty())return;std::string error;if(!document_.SaveAs(std::filesystem::path(path.toStdWString()),&error))SetStatus(QString::fromStdString(error),true);else{Refresh();SetStatus("Sound cue saved");}}
void SoundCueEditorWidget::UndoCurrent(){if(document_.Undo()){selected_row_=-1;Refresh();}}
void SoundCueEditorWidget::RedoCurrent(){if(document_.Redo()){selected_row_=-1;Refresh();}}
void SoundCueEditorWidget::SetContentRoot(const std::filesystem::path& root){content_root_=root;LoadSoundBank();}
void SoundCueEditorWidget::LoadSoundBank(){
    StopPreview();sound_bank_=aether::audio::SoundBank{};std::vector<std::string> names;if(content_root_.empty()){document_.SetSounds({});return;}
    std::error_code ec;for(std::filesystem::recursive_directory_iterator it(content_root_,std::filesystem::directory_options::skip_permission_denied,ec),end;it!=end;it.increment(ec)){
        if(ec){ec.clear();continue;}if(!it->is_regular_file(ec))continue;auto ext=it->path().extension().string();std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});if(ext!=".wav"&&ext!=".ogg"&&ext!=".flac")continue;
        std::ifstream file(it->path(),std::ios::binary);if(!file)continue;auto bytes=std::make_shared<std::vector<aether::u8>>(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());const auto relative=std::filesystem::relative(it->path(),content_root_,ec);if(ec){ec.clear();continue;}const std::string name=relative.generic_string();std::string error;if(sound_bank_.AddStreamed(name,bytes,&error))names.push_back(name);
    }
    std::sort(names.begin(),names.end());const auto count=names.size();document_.SetSounds(std::move(names));preview_status_->setText(QString("Audio preview · %1 sound assets loaded").arg(count));
}
void SoundCueEditorWidget::PlayPreview(){if(!cue_player_||!audio_output_||!audio_output_->Running()){SetStatus("Audio output is unavailable",true);return;}if(document_.Get().root==0||document_.Get().Find(document_.Get().root)==nullptr){SetStatus("Choose a cue output node before previewing",true);return;}preview_handle_=cue_player_->Play(document_.Get());if(preview_handle_==0){SetStatus("The cue has no playable sound nodes",true);return;}preview_button_->setText("■ Stop");preview_timer_->start();SetStatus("Previewing sound cue");}
void SoundCueEditorWidget::StopPreview(){if(cue_player_&&preview_handle_!=0)cue_player_->Stop(preview_handle_,0.02f);preview_handle_=0;if(preview_timer_)preview_timer_->stop();if(preview_button_)preview_button_->setText("▶ Preview");if(preview_asset_button_)preview_asset_button_->setText("▶ Preview Asset");}

void SoundCueEditorWidget::ShowMixer(){
    auto* dialog=new QDialog(this);dialog->setWindowTitle("Audio Mixer");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->resize(880,590);
    auto* layout=new QVBoxLayout(dialog);auto* tree=new QTreeWidget(dialog);tree->setColumnCount(5);tree->setHeaderLabels({"Bus","Parent","Volume (dB)","Mute","Peak L / R"});tree->setRootIsDecorated(false);tree->setAlternatingRowColors(true);layout->addWidget(tree,1);
    auto* effects_panel=new QWidget(dialog);auto* effects_layout=new QVBoxLayout(effects_panel);effects_layout->setContentsMargins(0,0,0,0);effects_layout->addWidget(new QLabel("BUS EFFECTS",effects_panel));auto* fx_controls=new QHBoxLayout;auto* fx_type=new QComboBox(effects_panel);fx_type->addItems({"Low-pass Filter","High-pass Filter","Peak Filter","Compressor","Reverb"});auto* add_fx=new QPushButton("Add Effect",effects_panel);auto* remove_fx=new QPushButton("Remove",effects_panel);auto* fx_up=new QPushButton("↑",effects_panel);auto* fx_down=new QPushButton("↓",effects_panel);fx_up->setToolTip("Move effect earlier in the chain");fx_down->setToolTip("Move effect later in the chain");fx_controls->addWidget(fx_type,1);fx_controls->addWidget(add_fx);fx_controls->addWidget(remove_fx);fx_controls->addWidget(fx_up);fx_controls->addWidget(fx_down);effects_layout->addLayout(fx_controls);auto* fx_split=new QSplitter(Qt::Horizontal,effects_panel);auto* fx_list=new QListWidget(fx_split);auto* fx_details=new QWidget(fx_split);auto* fx_form=new QFormLayout(fx_details);fx_split->addWidget(fx_list);fx_split->addWidget(fx_details);fx_split->setSizes({220,600});effects_layout->addWidget(fx_split);layout->addWidget(effects_panel,1);
    auto* actions=new QHBoxLayout;auto* add=new QPushButton("Add Child Bus…",dialog);auto* close=new QPushButton("Close",dialog);actions->addWidget(add);actions->addStretch();actions->addWidget(close);layout->addLayout(actions);connect(close,&QPushButton::clicked,dialog,&QDialog::close);
    auto rebuild=[this,tree](){tree->clear();for(aether::audio::BusId id=0;id<mixer_.BusCount();++id){auto* row=new QTreeWidgetItem(tree);row->setText(0,QString::fromStdString(mixer_.BusName(id)));row->setText(1,id==aether::audio::kMasterBus?"—":QString::fromStdString(mixer_.BusName(mixer_.BusParent(id))));
        auto* volume=new QSlider(Qt::Horizontal,tree);volume->setRange(-600,120);volume->setValue(int(std::lround(mixer_.BusVolume(id)*10.0f)));volume->setToolTip(QString("%1 dB").arg(mixer_.BusVolume(id),0,'f',1));tree->setItemWidget(row,2,volume);connect(volume,&QSlider::valueChanged,this,[this,volume,id](int value){const float db=float(value)/10.0f;mixer_.SetBusVolume(id,db);volume->setToolTip(QString("%1 dB").arg(db,0,'f',1));});
        auto* mute=new QCheckBox("Mute",tree);mute->setChecked(mixer_.BusMuted(id));tree->setItemWidget(row,3,mute);connect(mute,&QCheckBox::toggled,this,[this,id](bool value){mixer_.SetBusMuted(id,value);});
        auto* meter=new QProgressBar(tree);meter->setRange(0,1000);meter->setTextVisible(true);meter->setFormat("L 0% · R 0%");tree->setItemWidget(row,4,meter);
    }};
    rebuild();if(tree->topLevelItemCount())tree->setCurrentItem(tree->topLevelItem(0));
    auto rebuild_fx_list=[this,tree,fx_list]{fx_list->clear();const auto* row=tree->currentItem();if(!row)return;const auto bus=static_cast<aether::audio::BusId>(tree->indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(row)));for(usize i=0;i<mixer_.EffectCount(bus);++i){auto* effect=mixer_.EffectAt(bus,i);if(!effect)continue;auto* item=new QListWidgetItem(QString("%1  %2").arg(i+1).arg(effect->Name()),fx_list);item->setData(Qt::UserRole,int(i));}};
    auto refresh_fx_details=[this,tree,fx_list,fx_form]{while(fx_form->rowCount())fx_form->removeRow(0);const auto* row=tree->currentItem();const auto* selected=fx_list->currentItem();if(!row||!selected){fx_form->addRow(new QLabel("Select an effect to edit its parameters."));return;}const auto bus=static_cast<aether::audio::BusId>(tree->indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(row)));const auto index=static_cast<usize>(selected->data(Qt::UserRole).toInt());auto* effect=mixer_.EffectAt(bus,index);if(!effect)return;
        auto* bypass=new QCheckBox("Bypass effect",fx_form->parentWidget());bypass->setChecked(effect->bypass);fx_form->addRow(bypass);connect(bypass,&QCheckBox::toggled,this,[this,bus,index](bool value){mixer_.Post([bus,index,value](auto& mixer){if(auto* e=mixer.EffectAt(bus,index))e->bypass=value;});});
        auto add_number=[this,fx_form,bus,index](const QString& label,double value,double low,double high,int decimals,std::function<void(aether::audio::AudioEffect&,float)> setter){auto* spin=Number(fx_form->parentWidget(),value,low,high,decimals);fx_form->addRow(label,spin);connect(spin,&QDoubleSpinBox::editingFinished,this,[this,bus,index,spin,setter]{const float v=float(spin->value());mixer_.Post([bus,index,v,setter](auto& mixer){if(auto* e=mixer.EffectAt(bus,index))setter(*e,v);});});};
        if(auto* filter=dynamic_cast<aether::audio::FilterEffect*>(effect)){const auto& f=filter->Filter();add_number("Frequency (Hz)",f.frequency,20,20000,0,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::FilterEffect*>(&e)){const auto& f=x->Filter();x->Set(v,f.q,f.gain_db);}});add_number("Q",f.q,0.1,12,2,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::FilterEffect*>(&e)){const auto& f=x->Filter();x->Set(f.frequency,v,f.gain_db);}});add_number("Gain (dB)",f.gain_db,-24,24,1,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::FilterEffect*>(&e)){const auto& f=x->Filter();x->Set(f.frequency,f.q,v);}});}
        else if(auto* comp=dynamic_cast<aether::audio::CompressorEffect*>(effect)){add_number("Threshold (dB)",comp->threshold_db,-60,0,1,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::CompressorEffect*>(&e))x->threshold_db=v;});add_number("Ratio",comp->ratio,1,20,1,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::CompressorEffect*>(&e))x->ratio=v;});add_number("Attack (ms)",comp->attack_ms,0.1,200,1,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::CompressorEffect*>(&e))x->attack_ms=v;});add_number("Release (ms)",comp->release_ms,1,2000,0,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::CompressorEffect*>(&e))x->release_ms=v;});add_number("Makeup (dB)",comp->makeup_db,-24,24,1,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::CompressorEffect*>(&e))x->makeup_db=v;});}
        else if(auto* reverb=dynamic_cast<aether::audio::ReverbEffect*>(effect)){add_number("Room size",reverb->room_size,0,1,2,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::ReverbEffect*>(&e))x->room_size=v;});add_number("Damping",reverb->damping,0,1,2,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::ReverbEffect*>(&e))x->damping=v;});add_number("Wet",reverb->wet,0,1,2,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::ReverbEffect*>(&e))x->wet=v;});add_number("Dry",reverb->dry,0,1,2,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::ReverbEffect*>(&e))x->dry=v;});add_number("Stereo width",reverb->width,0,1,2,[](auto& e,float v){if(auto* x=dynamic_cast<aether::audio::ReverbEffect*>(&e))x->width=v;});}
    };
    connect(tree,&QTreeWidget::currentItemChanged,dialog,[rebuild_fx_list,refresh_fx_details]{rebuild_fx_list();refresh_fx_details();});connect(fx_list,&QListWidget::currentRowChanged,dialog,[refresh_fx_details](int){refresh_fx_details();});
    auto change_fx_order=[this,tree,fx_list,rebuild_fx_list,refresh_fx_details](int direction){const auto* row=tree->currentItem();const int selected=fx_list->currentRow();if(!row||selected<0)return;const auto bus=static_cast<aether::audio::BusId>(tree->indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(row)));const int target=selected+direction;if(target<0||target>=fx_list->count())return;if(!mixer_.MoveEffect(bus,usize(selected),usize(target)))return;rebuild_fx_list();fx_list->setCurrentRow(target);refresh_fx_details();SetStatus(QString("Reordered mixer effect on %1").arg(QString::fromStdString(mixer_.BusName(bus))));};
    connect(fx_up,&QPushButton::clicked,dialog,[change_fx_order]{change_fx_order(-1);});connect(fx_down,&QPushButton::clicked,dialog,[change_fx_order]{change_fx_order(1);});connect(remove_fx,&QPushButton::clicked,dialog,[this,tree,fx_list,rebuild_fx_list,refresh_fx_details]{const auto* row=tree->currentItem();const int selected=fx_list->currentRow();if(!row||selected<0)return;const auto bus=static_cast<aether::audio::BusId>(tree->indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(row)));if(!mixer_.RemoveEffect(bus,usize(selected)))return;rebuild_fx_list();if(fx_list->count())fx_list->setCurrentRow(std::min(selected,fx_list->count()-1));refresh_fx_details();SetStatus(QString("Removed mixer effect from %1").arg(QString::fromStdString(mixer_.BusName(bus))));});
    connect(add_fx,&QPushButton::clicked,dialog,[this,tree,fx_type,rebuild_fx_list,fx_list]{const auto* row=tree->currentItem();if(!row)return;const auto bus=static_cast<aether::audio::BusId>(tree->indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(row)));const QString type=fx_type->currentText();std::unique_ptr<aether::audio::AudioEffect> effect;if(type=="Low-pass Filter")effect=std::make_unique<aether::audio::FilterEffect>(aether::audio::Biquad::Type::LowPass,1000.0f);else if(type=="High-pass Filter")effect=std::make_unique<aether::audio::FilterEffect>(aether::audio::Biquad::Type::HighPass,1000.0f);else if(type=="Peak Filter")effect=std::make_unique<aether::audio::FilterEffect>(aether::audio::Biquad::Type::Peak,1000.0f,0.7071f,0.0f);else if(type=="Compressor")effect=std::make_unique<aether::audio::CompressorEffect>();else effect=std::make_unique<aether::audio::ReverbEffect>();if(!mixer_.AddEffect(bus,std::move(effect))){SetStatus("Could not add mixer effect",true);return;}rebuild_fx_list();fx_list->setCurrentRow(fx_list->count()-1);SetStatus(QString("Added %1 to %2").arg(type,QString::fromStdString(mixer_.BusName(bus))));});
    auto* timer=new QTimer(dialog);timer->setInterval(50);connect(timer,&QTimer::timeout,dialog,[this,tree]{for(int row=0;row<tree->topLevelItemCount();++row){const auto id=aether::audio::BusId(row);if(id>=mixer_.BusCount())continue;const auto level=mixer_.Meter(id);const float left=std::clamp(level.peak[0],0.0f,1.0f),right=std::clamp(level.peak[1],0.0f,1.0f);auto* meter=qobject_cast<QProgressBar*>(tree->itemWidget(tree->topLevelItem(row),4));if(meter){meter->setValue(int(std::max(left,right)*1000.0f));meter->setFormat(QString("L %1% · R %2%").arg(int(left*100)).arg(int(right*100)));}}});timer->start();
    connect(add,&QPushButton::clicked,dialog,[this,dialog,rebuild]{bool ok=false;const QString name=QInputDialog::getText(dialog,"Add Mixer Bus","Bus name",QLineEdit::Normal,{},&ok).trimmed();if(!ok||name.isEmpty())return;if(mixer_.FindBus(name.toStdString())!=aether::audio::Mixer::kInvalidBus){QMessageBox::warning(dialog,"Bus Already Exists","Choose a unique bus name.");return;}QStringList parents;for(aether::audio::BusId id=0;id<mixer_.BusCount();++id)parents<<QString::fromStdString(mixer_.BusName(id));const QString parent=QInputDialog::getItem(dialog,"Parent Bus","Route this bus into",parents,0,false,&ok);if(!ok)return;
        const bool restart=audio_output_&&audio_output_->Running();StopPreview();if(restart)audio_output_->Stop();const auto parent_id=mixer_.FindBus(parent.toStdString());const auto new_id=mixer_.AddBus(name.toStdString(),parent_id);std::string error;if(new_id==aether::audio::Mixer::kInvalidBus){QMessageBox::warning(dialog,"Could Not Add Bus","The mixer could not create this bus.");}else{rebuild();SetStatus(QString("Added mixer bus · %1 → %2").arg(name,parent));}if(restart&&!audio_output_->Start(480,&error))QMessageBox::warning(dialog,"Audio Output","The bus was added, but audio output could not restart: "+QString::fromStdString(error));});
    tree->header()->setSectionResizeMode(0,QHeaderView::ResizeToContents);tree->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);tree->header()->setSectionResizeMode(2,QHeaderView::Stretch);tree->header()->setSectionResizeMode(3,QHeaderView::ResizeToContents);tree->header()->setSectionResizeMode(4,QHeaderView::Stretch);dialog->show();
}

} // namespace aether::editor::qt
