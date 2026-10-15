#include "behavior_tree_editor_widget.h"

#include <QFileDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <fstream>

namespace aether::editor::qt {
namespace {
constexpr int kPathRole = Qt::UserRole + 1;
QString EncodePath(const aether::editor::BtPath& path) {
    QString out;
    for (usize i = 0; i < path.size(); ++i) { if (i) out += '/'; out += QString::number(path[i]); }
    return out;
}
aether::editor::BtPath DecodePath(const QString& text) {
    aether::editor::BtPath out;
    for (const QString& part : text.split('/', Qt::SkipEmptyParts)) { bool ok=false; const auto value=part.toULongLong(&ok); if(ok)out.push_back(static_cast<usize>(value)); }
    return out;
}
QString NodeText(const aether::ai::BtNode& node) {
    QString text=QString::fromStdString(node.Label());
    if(node.type==aether::ai::BtNodeType::Wait)text+=QString(" · %1 s").arg(node.seconds,0,'g',4);
    if(node.type==aether::ai::BtNodeType::MoveTo||node.type==aether::ai::BtNodeType::SetBlackboard||node.type==aether::ai::BtNodeType::ClearBlackboard)
        if(!node.key.empty())text+=QString(" · %1").arg(QString::fromStdString(node.key));
    return text;
}
}

BehaviorTreeEditorWidget::BehaviorTreeEditorWidget(QWidget* parent):QWidget(parent){
    auto* root=new QVBoxLayout(this);root->setContentsMargins(8,8,8,8);
    auto* toolbar=new QHBoxLayout;
    auto add_button=[&](const QString& label){auto* b=new QPushButton(label,this);toolbar->addWidget(b);return b;};
    auto* fresh=add_button("New");auto* open=add_button("Open…");auto* save=add_button("Save");auto* save_as=add_button("Save As…");
    auto* undo=add_button("Undo");auto* redo=add_button("Redo");toolbar->addStretch();
    root->addLayout(toolbar);
    auto* split=new QSplitter(this);tree_=new QTreeWidget(split);tree_->setHeaderLabel("Behavior Tree");tree_->setMinimumWidth(260);
    auto* right=new QWidget(split);auto* right_layout=new QVBoxLayout(right);right_layout->setContentsMargins(6,0,0,0);
    auto* node_buttons=new QHBoxLayout;auto* add=new QPushButton("Add child…",right);auto* duplicate=new QPushButton("Duplicate",right);
    auto* up=new QPushButton("Move up",right);auto* down=new QPushButton("Move down",right);auto* remove=new QPushButton("Remove",right);
    for(auto* b:{add,duplicate,up,down,remove})node_buttons->addWidget(b);right_layout->addLayout(node_buttons);
    detail_tabs_=new QTabWidget(right);
    auto* scroll=new QScrollArea(detail_tabs_);scroll->setWidgetResizable(true);properties_=new QWidget(scroll);properties_layout_=new QVBoxLayout(properties_);properties_layout_->setAlignment(Qt::AlignTop);scroll->setWidget(properties_);detail_tabs_->addTab(scroll,"Node");
    auto* bb_page=new QWidget(detail_tabs_);auto* bb_layout=new QVBoxLayout(bb_page);auto* bb_actions=new QHBoxLayout;
    auto* add_key=new QPushButton("Add key",bb_page);auto* rename_key=new QPushButton("Rename",bb_page);auto* remove_key=new QPushButton("Remove",bb_page);
    bb_actions->addWidget(add_key);bb_actions->addWidget(rename_key);bb_actions->addWidget(remove_key);bb_actions->addStretch();bb_layout->addLayout(bb_actions);
    auto* bb_split=new QSplitter(Qt::Vertical,bb_page);blackboard_keys_=new QListWidget(bb_split);blackboard_keys_->setMaximumHeight(180);
    auto* bb_scroll=new QScrollArea(bb_split);bb_scroll->setWidgetResizable(true);blackboard_details_=new QWidget(bb_scroll);blackboard_details_layout_=new QVBoxLayout(blackboard_details_);blackboard_details_layout_->setAlignment(Qt::AlignTop);bb_scroll->setWidget(blackboard_details_);
    bb_split->addWidget(blackboard_keys_);bb_split->addWidget(bb_scroll);bb_split->setSizes({160,300});bb_layout->addWidget(bb_split,1);detail_tabs_->addTab(bb_page,"Blackboard");right_layout->addWidget(detail_tabs_,1);
    auto* debug_page=new QWidget(detail_tabs_);auto* debug_layout=new QVBoxLayout(debug_page);debug_entities_=new QComboBox(debug_page);debug_layout->addWidget(debug_entities_);
    auto* debug_actions=new QHBoxLayout;auto* start_bt=new QPushButton("Start",debug_page);auto* stop_bt=new QPushButton("Stop",debug_page);auto* restart_bt=new QPushButton("Restart",debug_page);debug_actions->addWidget(start_bt);debug_actions->addWidget(stop_bt);debug_actions->addWidget(restart_bt);debug_layout->addLayout(debug_actions);
    debug_summary_=new QLabel("Start Play mode to inspect running Behavior Trees.",debug_page);debug_summary_->setWordWrap(true);debug_layout->addWidget(debug_summary_);
    auto* debug_split=new QSplitter(Qt::Vertical,debug_page);debug_nodes_=new QTreeWidget(debug_split);debug_nodes_->setHeaderLabels({"Node", "State", "Path"});debug_keys_=new QTreeWidget(debug_split);debug_keys_->setHeaderLabels({"Blackboard key", "Type", "Runtime value"});debug_split->addWidget(debug_nodes_);debug_split->addWidget(debug_keys_);debug_split->setSizes({340,180});debug_layout->addWidget(debug_split,1);detail_tabs_->addTab(debug_page,"Live Debugger");
    validation_=new QLabel(right);validation_->setWordWrap(true);right_layout->addWidget(validation_);
    split->addWidget(tree_);split->addWidget(right);split->setSizes({330,570});root->addWidget(split,1);
    status_=new QLabel("Behavior Tree · author a validated .abt asset",this);root->addWidget(status_);
    connect(fresh,&QPushButton::clicked,this,[this]{if(!ConfirmDiscardChanges(this))return;ClearDocument();SetStatus("New behavior tree");});
    connect(open,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getOpenFileName(this,"Open Behavior Tree",{},"Behavior Tree (*.abt)");if(!path.isEmpty())OpenFile(std::filesystem::path(path.toStdWString()));});
    connect(save,&QPushButton::clicked,this,[this]{SaveCurrent();});connect(save_as,&QPushButton::clicked,this,[this]{SaveAs();});
    connect(undo,&QPushButton::clicked,this,[this]{UndoCurrent();});connect(redo,&QPushButton::clicked,this,[this]{RedoCurrent();});
    connect(tree_,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem*,QTreeWidgetItem*){if(!rebuilding_)RefreshDetails();});
    connect(blackboard_keys_,&QListWidget::currentRowChanged,this,[this](int){RefreshKeyDetails();});
    connect(debug_entities_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){RefreshDebugger();});
    for(auto* button:{start_bt,stop_bt,restart_bt})connect(button,&QPushButton::clicked,this,[this,button,start_bt,stop_bt,restart_bt]{if(!debug_command_handler_||debug_entities_->currentIndex()<0)return;const QString id=debug_entities_->currentData().toString();const auto parts=id.split(':');if(parts.size()!=2)return;bool index_ok=false,generation_ok=false;const auto index=parts[0].toUInt(&index_ok);const auto generation=parts[1].toUInt(&generation_ok);if(!index_ok||!generation_ok)return;const QString command=button==start_bt?"Start":button==stop_bt?"Stop":"Restart";debug_command_handler_(Entity{index,generation},command);});
    connect(add_key,&QPushButton::clicked,this,[this]{AddBlackboardKey();});
    connect(rename_key,&QPushButton::clicked,this,[this]{auto* item=blackboard_keys_->currentItem();if(!item)return;const auto old=item->data(Qt::UserRole).toString();bool ok=false;const QString name=QInputDialog::getText(this,"Rename Blackboard Key","Key name",QLineEdit::Normal,old,&ok).trimmed();if(!ok||name.isEmpty())return;std::string error;if(!document_.RenameKey(old.toStdString(),name.toStdString(),&error))SetStatus(QString::fromStdString(error),true);else Refresh();});
    connect(remove_key,&QPushButton::clicked,this,[this]{auto* item=blackboard_keys_->currentItem();if(!item)return;const auto name=item->data(Qt::UserRole).toString();const auto users=document_.KeyUsers(name.toStdString());if(!users.empty()&&QMessageBox::question(this,"Remove Blackboard Key",QString("%1 node(s) reference this key. Remove it anyway?").arg(users.size()),QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;if(document_.RemoveKey(name.toStdString()))Refresh();});
    connect(add,&QPushButton::clicked,this,[this]{AddNode();});
    connect(duplicate,&QPushButton::clicked,this,[this]{auto* item=tree_->currentItem();if(!item)return;std::string error;auto path=document_.DuplicateNode(DecodePath(item->data(0,kPathRole).toString()),&error);if(!path)SetStatus(QString::fromStdString(error),true);else Refresh();});
    connect(up,&QPushButton::clicked,this,[this]{MoveSelected(-1);});connect(down,&QPushButton::clicked,this,[this]{MoveSelected(1);});
    connect(remove,&QPushButton::clicked,this,[this]{auto* item=tree_->currentItem();if(!item)return;std::string error;if(!document_.RemoveNode(DecodePath(item->data(0,kPathRole).toString()),&error))SetStatus(QString::fromStdString(error),true);else Refresh();});
    Refresh();RefreshBlackboard();RefreshDebugger();
}

void BehaviorTreeEditorWidget::Refresh(){
    rebuilding_=true;const QString selected=tree_->currentItem()?tree_->currentItem()->data(0,kPathRole).toString():QString{};tree_->clear();
    std::function<void(const aether::ai::BtNode&,const aether::editor::BtPath&,QTreeWidgetItem*)> add_node;
    add_node=[&](const auto& node,const auto& path,QTreeWidgetItem* parent){auto* item=parent?new QTreeWidgetItem(parent):new QTreeWidgetItem(tree_);item->setText(0,NodeText(node));item->setData(0,kPathRole,EncodePath(path));for(usize i=0;i<node.children.size();++i){auto child=path;child.push_back(i);add_node(node.children[i],child,item);}};
    add_node(document_.Get().root,{},nullptr);tree_->expandAll();
    if(!selected.isNull()){const auto items=tree_->findItems(QString(),Qt::MatchContains|Qt::MatchRecursive);for(auto* item:items)if(item->data(0,kPathRole).toString()==selected){tree_->setCurrentItem(item);break;}}
    if(!tree_->currentItem()&&tree_->topLevelItemCount())tree_->setCurrentItem(tree_->topLevelItem(0));
    rebuilding_=false;RefreshDetails();RefreshBlackboard();
    const auto& diagnostics=document_.Diagnostics();
    validation_->setText(diagnostics.empty()?"Validation · ready":"Validation · "+QString::number(diagnostics.size())+" issue(s)\n"+QString::fromStdString(diagnostics.front().code+": "+diagnostics.front().message));
    setWindowTitle(QString("Behavior Tree · %1%2").arg(QString::fromStdString(document_.Name()),document_.Dirty()?" *":""));
}

void BehaviorTreeEditorWidget::RefreshDetails(){
    while(auto* row=properties_layout_->takeAt(0)){if(row->widget())row->widget()->deleteLater();delete row;}
    auto* form=new QFormLayout;properties_layout_->addLayout(form);auto* item=tree_->currentItem();if(!item)return;
    const auto path=DecodePath(item->data(0,kPathRole).toString());const auto json=document_.NodeJson(path);if(!json.is_object())return;
    for(auto it=json.begin();it!=json.end();++it){if(it.key()=="type"||it.key()=="children"||it.key()=="decorators"||it.key()=="services")continue;const std::string key=it.key();auto* line=new QLineEdit(QString::fromStdString(it.value().dump()),properties_);form->addRow(QString::fromStdString(key),line);connect(line,&QLineEdit::editingFinished,this,[this,line,path,key]{nlohmann::json value;try{value=nlohmann::json::parse(line->text().toStdString());}catch(...){SetStatus("Enter a valid JSON value",true);return;}std::string error;if(!document_.SetNodeField(path,key,value,&error,key+EncodePath(path).toStdString()))SetStatus(QString::fromStdString(error),true);else Refresh();});}
    if (const auto* node = document_.Node(path); node && node->type == aether::ai::BtNodeType::RunLuau) {
        auto* hint = new QLabel("Play mode calls this method on the entity's Luau Script Component as (dt, first). Return \"running\", \"success\", or \"failure\".", properties_);
        hint->setWordWrap(true); hint->setObjectName("muted"); properties_layout_->addWidget(hint);
    }
    auto* attach_tools=new QHBoxLayout;auto* add_decorator=new QPushButton("Add decorator",properties_);auto* add_service=new QPushButton("Add service",properties_);attach_tools->addWidget(add_decorator);attach_tools->addWidget(add_service);properties_layout_->addLayout(attach_tools);
    connect(add_decorator,&QPushButton::clicked,this,[this]{AddNodeAttachment(false);});connect(add_service,&QPushButton::clicked,this,[this]{AddNodeAttachment(true);});
    auto add_attachment_editor=[this,path](const QString& title,const nlohmann::json& entries,bool service){auto* group=new QGroupBox(title,properties_);auto* box=new QVBoxLayout(group);for(usize index=0;index<entries.size();++index){const auto entry=entries[index];auto* heading=new QHBoxLayout;auto* label=new QLabel(QString("%1 · %2").arg(title).arg(QString::fromStdString(entry.value("type",std::string("Unknown")))),group);heading->addWidget(label);heading->addStretch();if(index>0){auto* up_button=new QPushButton("↑",group);up_button->setMaximumWidth(30);heading->addWidget(up_button);connect(up_button,&QPushButton::clicked,this,[this,path,index,service]{bool changed=service?document_.MoveService(path,index,index-1):document_.MoveDecorator(path,index,index-1);if(changed)Refresh();});}if(index+1<entries.size()){auto* down_button=new QPushButton("↓",group);down_button->setMaximumWidth(30);heading->addWidget(down_button);connect(down_button,&QPushButton::clicked,this,[this,path,index,service]{bool changed=service?document_.MoveService(path,index,index+1):document_.MoveDecorator(path,index,index+1);if(changed)Refresh();});}auto* remove_button=new QPushButton("Remove",group);heading->addWidget(remove_button);connect(remove_button,&QPushButton::clicked,this,[this,path,index,service]{bool changed=service?document_.RemoveService(path,index):document_.RemoveDecorator(path,index);if(changed)Refresh();});box->addLayout(heading);auto* fields=new QFormLayout;for(auto field=entry.begin();field!=entry.end();++field){if(field.key()=="type")continue;const std::string key=field.key();auto* line=new QLineEdit(QString::fromStdString(field.value().dump()),group);fields->addRow(QString::fromStdString(key),line);connect(line,&QLineEdit::editingFinished,this,[this,line,path,index,key,service]{nlohmann::json value;try{value=nlohmann::json::parse(line->text().toStdString());}catch(...){SetStatus("Enter a valid JSON value",true);return;}std::string error;const bool ok=service?document_.SetServiceField(path,index,key,value,&error):document_.SetDecoratorField(path,index,key,value,&error);if(!ok)SetStatus(QString::fromStdString(error),true);else Refresh();});}box->addLayout(fields);}properties_layout_->addWidget(group);};
    const auto decorators=json.value("decorators",nlohmann::json::array());const auto services=json.value("services",nlohmann::json::array());add_attachment_editor("Decorators",decorators,false);add_attachment_editor("Services",services,true);
    bool has_luau_service=false;for(const auto& service:services)has_luau_service|=service.value("type",std::string{})=="Luau";
    if(has_luau_service){auto* hint=new QLabel("Play mode calls each Luau service method on the entity's Script Component as (dt) at the configured interval.",properties_);hint->setWordWrap(true);hint->setObjectName("muted");properties_layout_->addWidget(hint);}
}

void BehaviorTreeEditorWidget::RefreshBlackboard(){
    const QString selected=blackboard_keys_->currentItem()?blackboard_keys_->currentItem()->data(Qt::UserRole).toString():QString{};
    blackboard_keys_->clear();for(const auto& key:document_.Get().blackboard.keys){auto* item=new QListWidgetItem(QString::fromStdString(key.name)+"  ·  "+QString::fromLatin1(aether::ai::BlackboardTypeName(key.type)),blackboard_keys_);item->setData(Qt::UserRole,QString::fromStdString(key.name));if(item->data(Qt::UserRole).toString()==selected)blackboard_keys_->setCurrentItem(item);}
    if(blackboard_keys_->currentRow()<0&&blackboard_keys_->count())blackboard_keys_->setCurrentRow(0);RefreshKeyDetails();
}

void BehaviorTreeEditorWidget::SetLiveDebugSnapshots(const std::vector<BehaviorTreeDebugSnapshot>& snapshots){
    QString selected=debug_entities_->currentData().toString();debug_snapshots_=snapshots;debug_entities_->clear();
    for(const auto& snapshot:debug_snapshots_){const QString id=QString::number(snapshot.entity.index)+":"+QString::number(snapshot.entity.generation);const QString text=QString("Entity %1 · %2 · %3").arg(snapshot.entity.index).arg(QString::fromStdString(snapshot.tree)).arg(snapshot.running?"running":"stopped");debug_entities_->addItem(text,id);if(id==selected)debug_entities_->setCurrentIndex(debug_entities_->count()-1);}
    RefreshDebugger();
}

void BehaviorTreeEditorWidget::SetDebugCommandHandler(std::function<void(Entity, const QString&)> handler){debug_command_handler_=std::move(handler);}

void BehaviorTreeEditorWidget::RefreshDebugger(){
    debug_nodes_->clear();debug_keys_->clear();const int index=debug_entities_->currentIndex();if(index<0||static_cast<usize>(index)>=debug_snapshots_.size()){debug_summary_->setText("Start Play mode to inspect running Behavior Trees. Add a BehaviorTreeComponent to an entity and choose an .abt asset.");return;}
    const auto& snapshot=debug_snapshots_[static_cast<usize>(index)];debug_summary_->setText(QString("%1 · %2\nActive node: %3").arg(QString::fromStdString(snapshot.tree),snapshot.running?"Running":"Stopped",snapshot.active_node.empty()?"—":QString::fromStdString(snapshot.active_node)));
    for(const auto& node:snapshot.nodes){auto* row=new QTreeWidgetItem(debug_nodes_);row->setText(0,QString::fromStdString(node.label));row->setText(1,QString::fromStdString(node.status));row->setText(2,QString::fromStdString(node.path));if(node.active){row->setForeground(0,QBrush(QColor("#79dfbf")));row->setForeground(1,QBrush(QColor("#79dfbf")));}}
    for(const auto& key:snapshot.blackboard){auto* row=new QTreeWidgetItem(debug_keys_);row->setText(0,QString::fromStdString(key.name));row->setText(1,QString::fromStdString(key.type));row->setText(2,QString::fromStdString(key.value));}
}

void BehaviorTreeEditorWidget::RefreshKeyDetails(){
    while(auto* row=blackboard_details_layout_->takeAt(0)){if(row->widget())row->widget()->deleteLater();delete row;}
    auto* item=blackboard_keys_->currentItem();if(!item){blackboard_details_layout_->addWidget(new QLabel("No keys yet. Add a typed key for this tree to read and write.",blackboard_details_));return;}
    const std::string name=item->data(Qt::UserRole).toString().toStdString();const auto* key=document_.Get().blackboard.Find(name);if(!key)return;
    auto* form=new QFormLayout;blackboard_details_layout_->addLayout(form);
    auto* type=new QComboBox(blackboard_details_);for(int i=0;i<=static_cast<int>(aether::ai::BlackboardType::Entity);++i)type->addItem(QString::fromLatin1(aether::ai::BlackboardTypeName(static_cast<aether::ai::BlackboardType>(i))));type->setCurrentIndex(static_cast<int>(key->type));form->addRow("Type",type);
    auto* initial=new QLineEdit(QString::fromStdString(aether::ai::ValueToJson(key->initial).dump()),blackboard_details_);initial->setPlaceholderText("null means unset; Vector uses [x, y, z]");form->addRow("Initial value",initial);
    auto* description=new QLineEdit(QString::fromStdString(key->description),blackboard_details_);form->addRow("Description",description);
    const auto users=document_.KeyUsers(name);auto* user_label=new QLabel(users.empty()?"Used by no nodes":QString("Referenced by %1 node(s)").arg(users.size()),blackboard_details_);user_label->setObjectName("muted");blackboard_details_layout_->addWidget(user_label);
    connect(type,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,name](int value){if(document_.SetKeyType(name,static_cast<aether::ai::BlackboardType>(value)))Refresh();});
    initial->setEnabled(key->type!=aether::ai::BlackboardType::Entity);connect(initial,&QLineEdit::editingFinished,this,[this,initial,name]{nlohmann::json value;try{value=nlohmann::json::parse(initial->text().toStdString());}catch(...){SetStatus("Initial value must be valid JSON",true);return;}std::string error;if(!document_.SetKeyInitial(name,value,&error))SetStatus(QString::fromStdString(error),true);else Refresh();});
    connect(description,&QLineEdit::editingFinished,this,[this,description,name]{if(document_.SetKeyDescription(name,description->text().toStdString()))Refresh();});
}

void BehaviorTreeEditorWidget::AddBlackboardKey(){
    const QStringList types={"Bool","Int","Float","String","Vector","Entity"};bool ok=false;const QString name=QInputDialog::getText(this,"Add Blackboard Key","Key name",QLineEdit::Normal,"NewKey",&ok).trimmed();if(!ok||name.isEmpty())return;const QString type=QInputDialog::getItem(this,"Key Type","Type",types,0,false,&ok);if(!ok)return;int type_index=types.indexOf(type);std::string error;if(!document_.AddKey(document_.UniqueKeyName(name.toStdString()),static_cast<aether::ai::BlackboardType>(type_index),&error))SetStatus(QString::fromStdString(error),true);else Refresh();
}

void BehaviorTreeEditorWidget::AddNodeAttachment(bool service){
    auto* item=tree_->currentItem();if(!item)return;const auto path=DecodePath(item->data(0,kPathRole).toString());const QStringList choices=service?QStringList{"Blueprint","Luau","DistanceTo"}:QStringList{"BlackboardCondition","Cooldown","Loop","TimeLimit","Inverter","ForceSuccess","ForceFailure"};bool ok=false;const QString choice=QInputDialog::getItem(this,service?"Add Behavior Service":"Add Decorator","Type",choices,0,false,&ok);if(!ok)return;const int index=choices.indexOf(choice);if(index<0)return;bool changed=service?document_.AddService(path,static_cast<aether::ai::BtServiceType>(index)):document_.AddDecorator(path,static_cast<aether::ai::BtDecoratorType>(index));if(changed)Refresh();
}

void BehaviorTreeEditorWidget::AddNode(){
    auto* item=tree_->currentItem();if(!item)return;const auto parent=DecodePath(item->data(0,kPathRole).toString());const auto* node=document_.Node(parent);
    if(!node||!aether::ai::IsComposite(node->type)){SetStatus("Select a composite node to add a child",true);return;}
    std::vector<aether::ai::BtNodeType> types=aether::editor::BtCompositeTypes();const auto tasks=aether::editor::BtTaskTypes();types.insert(types.end(),tasks.begin(),tasks.end());QStringList names;for(auto type:types)names<<QString::fromLatin1(aether::ai::BtNodeTypeName(type));bool ok=false;
    const QString choice=QInputDialog::getItem(this,"Add Behavior Node","Node type",names,0,false,&ok);if(!ok)return;const int index=names.indexOf(choice);if(index<0)return;std::string error;if(!document_.AddNode(parent,types[static_cast<usize>(index)],{},&error))SetStatus(QString::fromStdString(error),true);else Refresh();
}

void BehaviorTreeEditorWidget::MoveSelected(int offset){auto* item=tree_->currentItem();if(!item)return;auto path=DecodePath(item->data(0,kPathRole).toString());if(path.empty())return;const auto parent=aether::editor::BtPath(path.begin(),path.end()-1);const auto* p=document_.Node(parent);const long next=static_cast<long>(path.back())+offset;if(!p||next<0||next>=static_cast<long>(p->children.size()))return;std::string error;if(!document_.MoveNode(path,parent,static_cast<usize>(next),&error))SetStatus(QString::fromStdString(error),true);else Refresh();}

bool BehaviorTreeEditorWidget::OpenFile(const std::filesystem::path& path){if(!ConfirmDiscardChanges(this))return false;std::string error;if(!document_.Load(path,&error)){SetStatus(QString::fromStdString(error),true);return false;}Refresh();SetStatus("Opened "+QString::fromStdWString(path.filename().wstring()));return true;}
bool BehaviorTreeEditorWidget::ConfirmDiscardChanges(QWidget* parent)const{return !document_.Dirty()||QMessageBox::question(parent?parent:const_cast<BehaviorTreeEditorWidget*>(this),"Unsaved behavior tree","Discard unsaved behavior tree changes?",QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Cancel)==QMessageBox::Discard;}
void BehaviorTreeEditorWidget::ClearDocument(){document_=aether::editor::BehaviorTreeDocument{};Refresh();}
void BehaviorTreeEditorWidget::SaveCurrent(){std::string error;if(document_.Path().empty())SaveAs();else if(!document_.Save(&error))SetStatus(QString::fromStdString(error),true);else{Refresh();SetStatus("Behavior tree saved");}}
void BehaviorTreeEditorWidget::SaveAs(){const auto file=QFileDialog::getSaveFileName(this,"Save Behavior Tree",{},"Behavior Tree (*.abt)");if(file.isEmpty())return;std::string error;if(!document_.SaveAs(std::filesystem::path(file.toStdWString()),&error))SetStatus(QString::fromStdString(error),true);else{Refresh();SetStatus("Behavior tree saved");}}
void BehaviorTreeEditorWidget::UndoCurrent(){if(document_.Undo())Refresh();}
void BehaviorTreeEditorWidget::RedoCurrent(){if(document_.Redo())Refresh();}
void BehaviorTreeEditorWidget::SetStatus(const QString& message,bool error){status_->setText(message);status_->setStyleSheet(error?"color:#ff6b6b":"color:#9eacbf");}

} // namespace aether::editor::qt
