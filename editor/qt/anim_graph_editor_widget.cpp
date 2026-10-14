#include "anim_graph_editor_widget.h"

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <algorithm>
#include <array>

namespace aether::editor::qt {
namespace {
constexpr int kNodeIdRole=Qt::UserRole+1;
QString NodeLabel(const aether::anim::AnimNode& n){return QString("%1 · %2").arg(QString::fromUtf8(aether::anim::AnimNodeKindName(n.kind))).arg(n.id);}
QString NodeSubtitle(const aether::anim::AnimNode& n){if(!n.asset.empty())return QString::fromStdString(n.asset);if(!n.variable.empty())return QString::fromStdString(n.variable);if(!n.machine.empty())return QString::fromStdString(n.machine);return n.kind==aether::anim::AnimNodeKind::Slot?QString::fromStdString(n.slot):QString("Pose node");}

class PoseNodeItem final : public QGraphicsRectItem {
public:
    PoseNodeItem(const aether::anim::AnimNode& node,bool output=false):QGraphicsRectItem(0,0,190,68),id_(node.id),output_(output){
        setBrush(output?QColor("#49331d"):QColor("#252f3e"));setPen(QPen(output?QColor("#db9b42"):QColor("#526781"),1.5));
        setFlags(ItemIsMovable|ItemIsSelectable|ItemSendsGeometryChanges);setData(kNodeIdRole,qulonglong(node.id));setPos(node.x,node.y);
        auto* title=new QGraphicsTextItem(output?QString("OUTPUT POSE"):QString::fromUtf8(aether::anim::AnimNodeKindName(node.kind)),this);title->setDefaultTextColor(QColor("#e9eef5"));title->setFont(QFont("Segoe UI",10,QFont::DemiBold));title->setPos(10,7);
        auto* subtitle=new QGraphicsTextItem(output?QString("final pose"):NodeSubtitle(node),this);subtitle->setDefaultTextColor(QColor("#a4b2c4"));subtitle->setFont(QFont("Segoe UI",8));subtitle->setPos(10,36);
        setToolTip(output?"Connect the final pose here":QString("Node %1").arg(node.id));
    }
    u32 id()const{return id_;} bool output()const{return output_;}
private:u32 id_;bool output_;
};

class PoseGraphView final:public QGraphicsView{
public:explicit PoseGraphView(QWidget*p=nullptr):QGraphicsView(p){setRenderHint(QPainter::Antialiasing);setDragMode(RubberBandDrag);setViewportUpdateMode(FullViewportUpdate);setBackgroundBrush(QColor("#111721"));setTransformationAnchor(AnchorUnderMouse);setResizeAnchor(AnchorUnderMouse);}
    std::function<void()> moved;
protected:void mouseReleaseEvent(QMouseEvent*e)override{QGraphicsView::mouseReleaseEvent(e);if(moved)moved();}void wheelEvent(QWheelEvent*e)override{if(e->modifiers()&Qt::ControlModifier){scale(e->angleDelta().y()>0?1.12:0.89,e->angleDelta().y()>0?1.12:0.89);e->accept();return;}QGraphicsView::wheelEvent(e);}
};
}

AnimGraphEditorWidget::AnimGraphEditorWidget(QWidget* parent):QWidget(parent){
 auto* root=new QVBoxLayout(this);root->setContentsMargins(6,6,6,6);auto* bar=new QHBoxLayout;
 auto* create=new QPushButton("New");auto* open=new QPushButton("Open…");auto* save=new QPushButton("Save");auto* saveas=new QPushButton("Save As…");auto* undo=new QPushButton("Undo");auto* redo=new QPushButton("Redo");auto* add=new QPushButton("Add Pose Node");auto* del=new QPushButton("Delete Node");
 for(auto*b:{create,open,save,saveas,undo,redo,add,del})bar->addWidget(b);bar->addStretch();root->addLayout(bar);
 auto* split=new QSplitter;auto* left=new QWidget;auto* ll=new QVBoxLayout(left);ll->setContentsMargins(0,0,0,0);ll->addWidget(new QLabel("ANIMATION VARIABLES"));variables_=new QListWidget;ll->addWidget(variables_,1);auto* varbar=new QHBoxLayout;auto* addvar=new QPushButton("Add");auto* renamevar=new QPushButton("Rename");auto* delvar=new QPushButton("Remove");varbar->addWidget(addvar);varbar->addWidget(renamevar);varbar->addWidget(delvar);ll->addLayout(varbar);split->addWidget(left);
 auto* center=new QWidget;auto* cl=new QVBoxLayout(center);cl->setContentsMargins(0,0,0,0);scene_=new QGraphicsScene(this);view_=new PoseGraphView;view_->setScene(scene_);cl->addWidget(view_,1);split->addWidget(center);
 auto* right=new QWidget;auto* rl=new QVBoxLayout(right);rl->setContentsMargins(0,0,0,0);rl->addWidget(new QLabel("NODE DETAILS"));auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);details_=new QWidget;details_layout_=new QVBoxLayout(details_);details_layout_->setAlignment(Qt::AlignTop);scroll->setWidget(details_);rl->addWidget(scroll,2);rl->addWidget(new QLabel("DIAGNOSTICS"));diagnostics_=new QListWidget;rl->addWidget(diagnostics_,1);split->addWidget(right);split->setSizes({210,700,290});root->addWidget(split,1);status_=new QLabel("New animation graph · add a Clip node and connect a pose output");root->addWidget(status_);
 connect(create,&QPushButton::clicked,this,[this]{NewGraph();});connect(open,&QPushButton::clicked,this,[this]{auto p=QFileDialog::getOpenFileName(this,"Open Animation Graph",QString(),"Animation Graph (*.aanim)");if(!p.isEmpty())OpenFile(std::filesystem::path(p.toStdWString()));});connect(save,&QPushButton::clicked,this,[this]{SaveCurrent();});connect(saveas,&QPushButton::clicked,this,[this]{SaveAs();});connect(undo,&QPushButton::clicked,this,[this]{UndoCurrent();});connect(redo,&QPushButton::clicked,this,[this]{RedoCurrent();});
 auto* addmenu=new QMenu(add);for(auto k:{aether::anim::AnimNodeKind::Clip,aether::anim::AnimNodeKind::BlendSpace,aether::anim::AnimNodeKind::Blend,aether::anim::AnimNodeKind::BlendByBool,aether::anim::AnimNodeKind::BlendByInt,aether::anim::AnimNodeKind::Layered,aether::anim::AnimNodeKind::Additive,aether::anim::AnimNodeKind::Slot}){auto*a=addmenu->addAction(QString::fromUtf8(aether::anim::AnimNodeKindName(k)));connect(a,&QAction::triggered,this,[this,k]{AddNode(k);});}add->setMenu(addmenu);
 connect(del,&QPushButton::clicked,this,[this]{if(selected_node_){document_.DeleteNodes({selected_node_});selected_node_=0;Refresh();}});
 connect(scene_,&QGraphicsScene::selectionChanged,this,[this]{for(auto* i:scene_->selectedItems()){auto id=i->data(kNodeIdRole).toULongLong();if(id&&id<0xFFFFFF00u){SelectNode(u32(id));return;}}selected_node_=0;RefreshDetails();});
 auto* graph_view=static_cast<PoseGraphView*>(view_);graph_view->moved=[this]{CommitNodePosition();};
 connect(addvar,&QPushButton::clicked,this,[this]{auto types=QStringList{"Float","Bool","Int","Trigger"};bool ok=false;auto t=QInputDialog::getItem(this,"Add Animation Variable","Type",types,0,false,&ok);if(!ok)return;auto type=t=="Bool"?aether::anim::VarType::Bool:t=="Int"?aether::anim::VarType::Int:t=="Trigger"?aether::anim::VarType::Trigger:aether::anim::VarType::Float;document_.AddVariable(type);Refresh();});
 connect(renamevar,&QPushButton::clicked,this,[this]{auto* item=variables_->currentItem();if(!item)return;const auto from=item->data(Qt::UserRole).toString().toStdString();bool ok=false;auto to=QInputDialog::getText(this,"Rename Variable","Name",QLineEdit::Normal,item->text(),&ok);if(!ok)return;std::string error;if(!document_.RenameVariable(from,to.toStdString(),&error))SetStatus(QString::fromStdString(error),true);Refresh();});
 connect(delvar,&QPushButton::clicked,this,[this]{auto* item=variables_->currentItem();if(item){selected_variable_=item->data(Qt::UserRole).toString().toStdString();document_.RemoveVariable(selected_variable_);selected_variable_.clear();Refresh();}});
 connect(variables_,&QListWidget::currentRowChanged,this,[this](int row){if(rebuilding_)return;if(row<0){selected_variable_.clear();RefreshDetails();return;}selected_node_=0;selected_variable_=variables_->item(row)->data(Qt::UserRole).toString().toStdString();RefreshDetails();});
 Refresh();
}

bool AnimGraphEditorWidget::OpenFile(const std::filesystem::path& p){if(!ConfirmDiscardChanges(this))return false;std::string error;if(!document_.Load(p,&error)){SetStatus(QString::fromStdString(error),true);return false;}selected_node_=0;Refresh();SetStatus(QString("Opened %1").arg(QString::fromStdWString(p.filename().wstring())));return true;}
bool AnimGraphEditorWidget::ConfirmDiscardChanges(QWidget*p)const{if(!document_.Dirty())return true;return QMessageBox::question(p?p:const_cast<AnimGraphEditorWidget*>(this),"Unsaved animation graph","Discard unsaved animation graph changes?",QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Cancel)==QMessageBox::Discard;}
void AnimGraphEditorWidget::ClearDocument(){document_=aether::editor::AnimGraphDocument{};selected_node_=0;Refresh();}
void AnimGraphEditorWidget::NewGraph(){if(!ConfirmDiscardChanges(this))return;document_=aether::editor::AnimGraphDocument{};selected_node_=0;Refresh();SetStatus("New animation graph");}
void AnimGraphEditorWidget::SaveCurrent(){if(document_.Path().empty()){SaveAs();return;}std::string e;if(!document_.Save(&e))SetStatus(QString::fromStdString(e),true);else{Refresh();SetStatus("Animation graph saved");}}
void AnimGraphEditorWidget::SaveAs(){auto p=QFileDialog::getSaveFileName(this,"Save Animation Graph",QString(),"Animation Graph (*.aanim)");if(p.isEmpty())return;std::string e;if(!document_.SaveAs(std::filesystem::path(p.toStdWString()),&e))SetStatus(QString::fromStdString(e),true);else{Refresh();SetStatus("Animation graph saved");}}
void AnimGraphEditorWidget::UndoCurrent(){if(document_.Undo())Refresh();}void AnimGraphEditorWidget::RedoCurrent(){if(document_.Redo())Refresh();}
void AnimGraphEditorWidget::SetStatus(const QString&m,bool error){status_->setText(m);status_->setStyleSheet(error?"color:#ff6b6b":"color:#9eacbf");}
void AnimGraphEditorWidget::AddNode(aether::anim::AnimNodeKind kind){auto center=view_->mapToScene(view_->viewport()->rect().center());auto id=document_.AddNode(kind,float(center.x()),float(center.y()));selected_node_=id;Refresh();}
void AnimGraphEditorWidget::SelectNode(u32 id){selected_node_=id;selected_variable_.clear();variables_->clearSelection();RefreshDetails();}
void AnimGraphEditorWidget::EditNode(const QString& label,const std::function<void(aether::anim::AnimNode&)>& edit,const QString& merge){if(!selected_node_)return;auto id=selected_node_;document_.Edit(label.toStdString(),[id,&edit](aether::anim::AnimGraph&g){for(auto&n:g.nodes)if(n.id==id)edit(n);},merge.toStdString());Refresh();}
void AnimGraphEditorWidget::CommitNodePosition(){std::map<u32,QPointF> moved;for(auto*i:scene_->items()){auto* node=dynamic_cast<PoseNodeItem*>(i);if(node&&!node->output()){const auto* n=document_.Get().Find(node->id());if(n&&(std::abs(n->x-node->pos().x())>0.5||std::abs(n->y-node->pos().y())>0.5))moved[node->id()]=node->pos();}}if(moved.empty())return;document_.Edit("Move pose nodes",[&](aether::anim::AnimGraph&g){for(auto&n:g.nodes)if(auto it=moved.find(n.id);it!=moved.end()){n.x=float(it->second.x());n.y=float(it->second.y());}},"move-pose-nodes");setWindowTitle(QString("Animation Graph · %1 *").arg(QString::fromStdString(document_.Name())));}

void AnimGraphEditorWidget::Refresh(){rebuilding_=true;scene_->clear();const auto&g=document_.Get();for(const auto&n:g.nodes){auto* item=new PoseNodeItem(n);scene_->addItem(item);if(n.id==selected_node_)item->setSelected(true);}auto* output=new PoseNodeItem(aether::anim::AnimNode{0},true);output->setData(kNodeIdRole,qulonglong(0xFFFFFF00u));output->setPos(g.nodes.empty()?QPointF(300,100):QPointF(std::max_element(g.nodes.begin(),g.nodes.end(),[](const auto&a,const auto&b){return a.x<b.x;})->x+280,100));scene_->addItem(output);for(const auto&n:g.nodes){for(usize i=0;i<n.inputs.size();++i){if(!n.inputs[i])continue;const auto* source=g.Find(n.inputs[i]);if(!source)continue;QPointF a(source->x+190,source->y+34),b(n.x,n.y+35+float(i)*7);QPainterPath path(a);path.cubicTo(a+QPointF(65,0),b-QPointF(65,0),b);auto* link=scene_->addPath(path,QPen(QColor("#7792b4"),2));link->setZValue(-1);}}if(g.output){if(const auto*n=g.Find(g.output)){QPointF a(n->x+190,n->y+34),b(output->pos().x(),output->pos().y()+34);QPainterPath p(a);p.cubicTo(a+QPointF(60,0),b-QPointF(60,0),b);auto* link=scene_->addPath(p,QPen(QColor("#e5a64b"),3));link->setZValue(-1);}}scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-120,-100,160,120));variables_->clear();int variable_row=-1;for(const auto&v:g.variables){auto* item=new QListWidgetItem(QString("%1  ·  %2").arg(QString::fromStdString(v.name),QString::fromUtf8(aether::anim::VarTypeName(v.type))),variables_);item->setData(Qt::UserRole,QString::fromStdString(v.name));if(v.name==selected_variable_)variable_row=variables_->row(item);}if(variable_row>=0)variables_->setCurrentRow(variable_row);rebuilding_=false;RefreshDetails();RefreshDiagnostics();setWindowTitle(QString("Animation Graph · %1%2").arg(QString::fromStdString(document_.Name()),document_.Dirty()?" *":""));}

void AnimGraphEditorWidget::RefreshDetails(){while(auto*i=details_layout_->takeAt(0)){if(i->widget())i->widget()->deleteLater();delete i;}auto* form=new QFormLayout;details_layout_->addLayout(form);if(!selected_node_){if(!selected_variable_.empty()){auto it=std::find_if(document_.Get().variables.begin(),document_.Get().variables.end(),[this](const auto&v){return v.name==selected_variable_;});if(it!=document_.Get().variables.end()){const auto variable=*it;auto* type=new QComboBox;for(auto t:{aether::anim::VarType::Bool,aether::anim::VarType::Int,aether::anim::VarType::Float,aether::anim::VarType::Trigger})type->addItem(QString::fromUtf8(aether::anim::VarTypeName(t)),int(t));type->setCurrentIndex(type->findData(int(variable.type)));form->addRow("Type",type);connect(type,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,type]{document_.SetVariableType(selected_variable_,aether::anim::VarType(type->currentData().toInt()));Refresh();});auto* def=new QDoubleSpinBox;def->setRange(-1000000,1000000);def->setDecimals(3);def->setValue(variable.default_value);form->addRow("Default",def);connect(def,&QDoubleSpinBox::editingFinished,this,[this,def]{auto name=selected_variable_;const double value=def->value();document_.Edit("Set variable default",[&](aether::anim::AnimGraph&g){for(auto&v:g.variables)if(v.name==name)v.default_value=float(value);},"variable-default:"+name);Refresh();});return;}}auto* hint=new QLabel("Select a variable or pose node to edit it.");hint->setWordWrap(true);form->addRow(hint);return;}const auto*nptr=document_.Get().Find(selected_node_);if(!nptr)return;const auto n=*nptr;auto* heading=new QLabel(NodeLabel(n));heading->setStyleSheet("font-weight:600;color:#dce7f4");form->addRow(heading);
 auto* inputs=new QFormLayout;for(usize index=0;index<n.inputs.size();++index){auto* combo=new QComboBox;combo->addItem("(unconnected)",0);for(const auto& source:document_.Get().nodes)if(source.id!=n.id)combo->addItem(NodeLabel(source),qulonglong(source.id));int current=combo->findData(qulonglong(n.inputs[index]));combo->setCurrentIndex(current<0?0:current);inputs->addRow(QString("Input %1").arg(index+1),combo);connect(combo,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,combo,index,nid=n.id](int){const u32 from=u32(combo->currentData().toULongLong());if(!from){document_.Edit("Disconnect pose",[=](aether::anim::AnimGraph&g){for(auto&x:g.nodes)if(x.id==nid&&index<x.inputs.size())x.inputs[index]=0;});Refresh();return;}std::string error;if(!document_.ConnectPose(from,nid,index,&error))SetStatus(QString::fromStdString(error),true);Refresh();});}if(n.kind==aether::anim::AnimNodeKind::BlendByInt){auto* more=new QPushButton("Add input");connect(more,&QPushButton::clicked,this,[this]{EditNode("Add input",[](auto&x){x.inputs.push_back(0);});});inputs->addRow(more);}if(n.inputs.size())details_layout_->addLayout(inputs);
 auto* variable=new QComboBox;variable->addItem("(none)","");for(const auto&v:document_.Get().variables)variable->addItem(QString::fromStdString(v.name),QString::fromStdString(v.name));variable->setCurrentIndex(std::max(0,variable->findData(QString::fromStdString(n.variable))));auto setvar=[this,variable](const QString&label){EditNode(label,[v=variable->currentData().toString().toStdString()](auto&x){x.variable=v;});};
 if(n.kind==aether::anim::AnimNodeKind::Blend||n.kind==aether::anim::AnimNodeKind::BlendByBool||n.kind==aether::anim::AnimNodeKind::BlendByInt||n.kind==aether::anim::AnimNodeKind::Additive){form->addRow("Variable",variable);connect(variable,qOverload<int>(&QComboBox::currentIndexChanged),this,[setvar](int){setvar("Set variable");});}
 if(n.kind==aether::anim::AnimNodeKind::Clip||n.kind==aether::anim::AnimNodeKind::BlendSpace){auto* asset=new QLineEdit(QString::fromStdString(n.asset));form->addRow(n.kind==aether::anim::AnimNodeKind::Clip?"Clip asset":"Blend space",asset);connect(asset,&QLineEdit::editingFinished,this,[this,asset]{auto v=asset->text().toStdString();EditNode("Set asset",[v](auto&x){x.asset=v;});});}
 if(n.kind==aether::anim::AnimNodeKind::Clip){auto* loop=new QComboBox;loop->addItems({"Loop","Once"});loop->setCurrentIndex(n.loop?0:1);form->addRow("Playback",loop);connect(loop,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,loop](int){const bool v=loop->currentIndex()==0;EditNode("Set loop",[v](auto&x){x.loop=v;});});auto* rate=new QDoubleSpinBox;rate->setRange(0,10);rate->setSingleStep(.1);rate->setValue(n.rate);form->addRow("Rate",rate);connect(rate,&QDoubleSpinBox::editingFinished,this,[this,rate]{const double v=rate->value();EditNode("Set rate",[v](auto&x){x.rate=float(v);},"rate");});}
 if(n.kind==aether::anim::AnimNodeKind::BlendByBool||n.kind==aether::anim::AnimNodeKind::BlendByInt){auto* time=new QDoubleSpinBox;time->setRange(0,5);time->setSingleStep(.05);time->setValue(n.blend_time);form->addRow("Blend time",time);connect(time,&QDoubleSpinBox::editingFinished,this,[this,time]{const double v=time->value();EditNode("Set blend time",[v](auto&x){x.blend_time=float(v);},"blend-time");});}
 if(n.kind==aether::anim::AnimNodeKind::Layered||n.kind==aether::anim::AnimNodeKind::Slot){auto* text=new QLineEdit(n.kind==aether::anim::AnimNodeKind::Layered?QString::fromStdString(n.bone):QString::fromStdString(n.slot));form->addRow(n.kind==aether::anim::AnimNodeKind::Layered?"Bone":"Slot",text);connect(text,&QLineEdit::editingFinished,this,[this,text,layer=n.kind==aether::anim::AnimNodeKind::Layered]{auto s=text->text().toStdString();EditNode("Set node label",[s,layer](auto&x){if(layer)x.bone=s;else x.slot=s;});});}
 auto* output=new QPushButton(document_.Get().output==n.id?"Current Output Pose":"Use as Output Pose");form->addRow(output);connect(output,&QPushButton::clicked,this,[this,id=n.id]{document_.SetOutput(id);Refresh();});
}
void AnimGraphEditorWidget::RefreshDiagnostics(){diagnostics_->clear();for(const auto&d:document_.Diagnostics()){auto*i=new QListWidgetItem(QString::fromStdString(d.code+": "+d.message),diagnostics_);i->setForeground(d.error?QColor("#ff7070"):QColor("#e6b450"));}if(diagnostics_->count()==0)diagnostics_->addItem("No animation graph issues");}

} // namespace aether::editor::qt
