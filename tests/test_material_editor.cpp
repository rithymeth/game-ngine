#include "graph/material_editor.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <functional>

using namespace aether;
using namespace aether::editor;
using mat::NodeId;
using mat::PinType;
using nlohmann::json;

// Phase 15 step 5: the material editor (document, graph adapter, panels).

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1400, 900));
        ImGui::Begin("M_Test", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

// Output(1) <- Multiply(4) <- Sample(3).rgb, Tint(5); Roughness(6) -> Roughness.
mat::Material Lit() {
    mat::Material m;
    m.parameters.push_back({"Albedo", PinType::Texture, {}, "guid", ""});
    m.parameters.push_back({"Tint", PinType::Float3, {1, 1, 1, 0}, "", "Surface"});
    m.parameters.push_back({"Roughness", PinType::Float, {0.5f, 0, 0, 0}, "", "Surface"});
    const NodeId out = m.Add("Material.Output", {}, 600, 0);
    const NodeId tex = m.Add("Param.Texture:Albedo", {}, 0, 0);
    const NodeId sample = m.Add("Texture.Sample", {}, 150, 0);
    const NodeId mul = m.Add("Math.Multiply", {}, 350, 0);
    const NodeId tint = m.Add("Param.Vector:Tint", {}, 150, 150);
    const NodeId rough = m.Add("Param.Scalar:Roughness", {}, 350, 200);
    m.Connect(tex, "value", sample, "texture").Connect(sample, "rgb", mul, "a").Connect(tint, "value", mul, "b");
    m.Connect(mul, "result", out, "BaseColor").Connect(rough, "value", out, "Roughness");
    return m;
}

bool HasLink(const mat::Material& m, NodeId from, const std::string& fp, NodeId to, const std::string& tp) {
    return std::any_of(m.links.begin(), m.links.end(), [&](const mat::Link& l) {
        return l.from.node == from && l.from.pin == fp && l.to.node == to && l.to.pin == tp;
    });
}

usize CountType(const mat::Material& m, const std::string& type) {
    return static_cast<usize>(std::count_if(m.nodes.begin(), m.nodes.end(), [&](const mat::Node& n) { return n.type == type; }));
}

} // namespace

AETHER_TEST(MaterialEditor_DocumentUndoAndParameters) {
    MaterialDocument doc(Lit());
    AETHER_CHECK(doc.Name() == "Untitled" && !doc.Dirty() && doc.Generated().ok);
    const mat::GeneratedMaterial* cached = &doc.Generated();
    const u64 key = cached->permutation_key;
    AETHER_CHECK(&doc.Generated() == cached && doc.Generated().permutation_key == key); // cached

    // Edits, merged drags, undo and redo.
    doc.Edit("Move", [](mat::Material& m) { m.Find(4)->x = 10; }, "drag");
    doc.Edit("Move", [](mat::Material& m) { m.Find(4)->x = 20; }, "drag");
    AETHER_CHECK(doc.Dirty() && doc.UndoLabel() == "Move");
    AETHER_CHECK(doc.Undo() && doc.Get().Find(4)->x == 350.0f && !doc.CanUndo());
    AETHER_CHECK(doc.Redo() && doc.Get().Find(4)->x == 20.0f && !doc.CanRedo());

    // Parameters: add (free names), rename (nodes follow), retype, remove.
    AETHER_CHECK(doc.AddParameter(PinType::Float) == "Param" && doc.AddParameter(PinType::Float3) == "Param_1");
    AETHER_CHECK(doc.Get().FindParameter("Param_1")->default_value.x == 1.0f);
    std::string error;
    AETHER_CHECK(!doc.RenameParameter("Tint", "Roughness", &error) && !error.empty());
    AETHER_CHECK(!doc.RenameParameter("Tint", "", &error) && !doc.RenameParameter("Tint", " Pad", &error));
    AETHER_CHECK(!doc.RenameParameter("Nope", "X", &error));
    AETHER_CHECK(doc.RenameParameter("Tint", "Base Tint", &error) && doc.Get().Find(5)->type == "Param.Vector:Base Tint");
    AETHER_CHECK(doc.Generated().ok && doc.Generated().parameters.Find("Base Tint") != nullptr);
    AETHER_CHECK(doc.Undo() && doc.Get().Find(5)->type == "Param.Vector:Tint");
    AETHER_CHECK(doc.Redo());

    // A float3 parameter retyped to a texture: its node becomes a texture
    // node and its link into the Multiply no longer fits, so it's broken.
    AETHER_CHECK(doc.SetParameterType("Base Tint", PinType::Texture));
    AETHER_CHECK(doc.Get().Find(5)->type == "Param.Texture:Base Tint" && !HasLink(doc.Get(), 5, "value", 4, "b"));
    AETHER_CHECK(doc.Generated().ok);
    AETHER_CHECK(doc.Undo() && HasLink(doc.Get(), 5, "value", 4, "b"));
    // A scalar retyped to a float2 keeps its link into Roughness: it still fits, truncated (MT010).
    AETHER_CHECK(doc.SetParameterType("Roughness", PinType::Float2) && doc.Get().Find(6)->type == "Param.Vector:Roughness");
    AETHER_CHECK(HasLink(doc.Get(), 6, "value", 1, "Roughness") && doc.Analysis().Has("MT010"));
    AETHER_CHECK(!doc.SetParameterType("Nope", PinType::Float) && !doc.SetParameterType("Roughness", PinType::Any));

    AETHER_CHECK(doc.RemoveParameter("Roughness") && doc.Get().Find(6) == nullptr && doc.Get().FindParameter("Roughness") == nullptr);
    AETHER_CHECK(!doc.RemoveParameter("Roughness"));
    AETHER_CHECK(std::none_of(doc.Get().links.begin(), doc.Get().links.end(), [](const mat::Link& l) { return l.from.node == 6; }));
    AETHER_CHECK(MaterialDocument::IsValidName("Rim Light") && !MaterialDocument::IsValidName(std::string(65, 'a')));

    // Undo keeps the function library.
    auto lib = std::make_shared<mat::FunctionLibrary>();
    doc.SetFunctions(lib);
    doc.Edit("Nothing", [](mat::Material&) {});
    AETHER_CHECK(doc.Undo() && doc.Get().functions == lib);

    // Files.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_material_editor_test";
    std::filesystem::create_directories(dir);
    AETHER_CHECK(!doc.Save(&error));
    AETHER_CHECK(doc.SaveAs(dir / "M_Rock.amat", &error) && !doc.Dirty() && doc.Name() == "M_Rock");
    MaterialDocument reopened;
    AETHER_CHECK(reopened.Load(dir / "M_Rock.amat", &error) && reopened.Get().nodes.size() == doc.Get().nodes.size());
    AETHER_CHECK(!reopened.Load(dir / "missing.amat", &error));
    std::filesystem::remove_all(dir);
}

AETHER_TEST(MaterialEditor_ClipboardKeepsInternalLinks) {
    MaterialDocument doc(Lit());
    const std::string text = doc.CopyNodes({3, 4, 1}); // the output isn't copied
    const json j = json::parse(text);
    AETHER_CHECK(j["aether.material_nodes"] == 1 && j["nodes"].size() == 2 && j["links"].size() == 1);
    const std::vector<NodeId> pasted = doc.PasteNodes(text, 1000, 500);
    AETHER_CHECK(pasted.size() == 2 && CountType(doc.Get(), "Material.Output") == 1);
    AETHER_CHECK(doc.Get().Find(pasted[0])->x == 1000.0f && doc.Get().Find(pasted[1])->x == 1200.0f);
    AETHER_CHECK(HasLink(doc.Get(), pasted[0], "rgb", pasted[1], "a"));
    const usize nodes_before = doc.Get().nodes.size();
    AETHER_CHECK(doc.PasteNodes("not json", 0, 0).empty() && doc.PasteNodes(R"({"aether.nodes": 1})", 0, 0).empty());
    AETHER_CHECK(doc.Get().nodes.size() == nodes_before);
    AETHER_CHECK(doc.Undo() && doc.Get().nodes.size() == nodes_before - 2 && !doc.CanUndo()); // the one real paste, nothing else
    const std::vector<NodeId> copies = doc.DuplicateNodes({4});
    AETHER_CHECK(copies.size() == 1 && doc.Get().Find(copies[0])->x == 380.0f && doc.Get().Find(copies[0])->type == "Math.Multiply");
    AETHER_CHECK(doc.CopyNodes({1}).empty() && doc.DuplicateNodes({1}).empty());
}

AETHER_TEST(MaterialEditor_GraphLinksAreTypeChecked) {
    mat::Material m = Lit();
    const GraphViewModel model = BuildMaterialView(m, mat::Analyze(m));
    AETHER_CHECK(model.nodes.size() == 6 && model.links.size() == 5);
    const GraphNodeView* mul = model.Find(4);
    AETHER_CHECK(mul->title == "Multiply" && mul->inputs.size() == 2 && mul->inputs[0].connected && mul->outputs[0].connected);
    AETHER_CHECK(mul->inputs[0].color == MaterialPinColor(PinType::Float3)); // inferred from its result
    AETHER_CHECK(model.Find(2)->outputs[0].color == MaterialPinColor(PinType::Texture) && model.Find(1)->outputs.empty());
    const auto link = std::find_if(model.links.begin(), model.links.end(), [](const GraphLinkView& l) { return l.to_pin == "Roughness"; });
    AETHER_CHECK(link->color == MaterialPinColor(PinType::Float));

    std::string error;
    // A texture into math, a float2 into BaseColor, two outputs, a self link: refused.
    AETHER_CHECK(!ConnectMaterialPins(m, {2, "value", true}, {4, "b", false}, &error) && !error.empty());
    const NodeId uv = AddMaterialNode(m, "Input.TexCoord", 0, 300);
    AETHER_CHECK(!ConnectMaterialPins(m, {uv, "uv", true}, {1, "BaseColor", false}, &error));
    AETHER_CHECK(!ConnectMaterialPins(m, {uv, "uv", true}, {4, "result", true}, &error));
    AETHER_CHECK(!ConnectMaterialPins(m, {4, "result", true}, {4, "a", false}, &error));
    AETHER_CHECK(!ConnectMaterialPins(m, {4, "result", true}, {1, "Sparkle", false}, &error));
    // A loop: Multiply's result back into the Sample's uv (the sample feeds the multiply).
    AETHER_CHECK(!ConnectMaterialPins(m, {4, "result", true}, {3, "uv", false}, &error) && error.find("loop") != std::string::npos);
    // Fits: a float2 into the sample's uv (either order), replacing nothing; a float broadcasts into BaseColor, replacing.
    AETHER_CHECK(ConnectMaterialPins(m, {3, "uv", false}, {uv, "uv", true}, &error) && HasLink(m, uv, "uv", 3, "uv"));
    AETHER_CHECK(ConnectMaterialPins(m, {6, "value", true}, {1, "BaseColor", false}, &error));
    AETHER_CHECK(HasLink(m, 6, "value", 1, "BaseColor") && !HasLink(m, 4, "result", 1, "BaseColor"));
    AETHER_CHECK(mat::Analyze(m).Ok());

    // Deleting never takes the Material Output; the widget's edits apply together.
    GraphViewResult edits;
    edits.moved = {{4, {5.0f, 6.0f}}};
    edits.deleted = {1, 5};
    const std::vector<std::string> errors = ApplyMaterialEdits(m, edits);
    AETHER_CHECK(errors.size() == 1 && m.Find(1) != nullptr && m.Find(5) == nullptr && m.Find(4)->y == 6.0f);
    AETHER_CHECK(DisconnectMaterial(m, {{uv, "uv"}, {3, "uv"}}) && !DisconnectMaterial(m, {{uv, "uv"}, {3, "uv"}}));

    // A node placed from a dragged wire links its first fitting pin.
    const NodeId sat = AddMaterialNode(m, "Math.Saturate", 0, 0);
    AETHER_CHECK(ConnectToNewMaterialNode(m, {3, "r", true}, sat) && HasLink(m, 3, "r", sat, "x"));
    const NodeId time = AddMaterialNode(m, "Input.Time", 0, 0);
    AETHER_CHECK(!ConnectToNewMaterialNode(m, {3, "r", true}, time)); // no inputs

    // Errors show on their nodes.
    mat::Material broken = Lit();
    broken.Add("Texture.Sample");
    const GraphViewModel bad = BuildMaterialView(broken, mat::Analyze(broken));
    AETHER_CHECK(bad.Find(7)->error.find("MT007") != std::string::npos && bad.Find(4)->error.empty());
}

AETHER_TEST(MaterialEditor_PaletteSearchFitsTheDraggedPin) {
    mat::Material m = Lit();
    std::vector<mat::PaletteEntry> all = SearchMaterialPalette(m, "", nullptr, 500);
    AETHER_CHECK(all.size() > 30);
    AETHER_CHECK(std::none_of(all.begin(), all.end(), [](const mat::PaletteEntry& e) { return e.id == "Material.Output"; })); // one already
    AETHER_CHECK(SearchMaterialPalette(m, "mult").front().id == "Math.Multiply");
    AETHER_CHECK(SearchMaterialPalette(m, "tex samp").front().id == "Texture.Sample");
    AETHER_CHECK(SearchMaterialPalette(m, "zzzz").empty());
    // From the texture parameter's output: only nodes that take a texture.
    const GraphPinRef texture{2, "value", true};
    const std::vector<mat::PaletteEntry> takes_texture = SearchMaterialPalette(m, "", &texture, 500);
    AETHER_CHECK(!takes_texture.empty());
    for (const mat::PaletteEntry& e : takes_texture) {
        AETHER_CHECK(e.id == "Texture.Sample" || e.id == "Texture.Triplanar" || e.id == "Custom.HLSL" || e.id == "Utility.Reroute");
    }
    // Into BaseColor (dragged back from an input): nodes with an output that fits a float3.
    const GraphPinRef base{1, "BaseColor", false};
    const std::vector<mat::PaletteEntry> feeds = SearchMaterialPalette(m, "", &base, 500);
    auto has = [](const std::vector<mat::PaletteEntry>& v, const std::string& id) {
        return std::any_of(v.begin(), v.end(), [&](const mat::PaletteEntry& e) { return e.id == id; });
    };
    AETHER_CHECK(has(feeds, "Math.Add") && has(feeds, "Input.WorldPosition") && has(feeds, "Param.Scalar:Roughness"));
    AETHER_CHECK(!has(feeds, "Input.TexCoord") && !has(feeds, "Param.Texture:Albedo"));
}

AETHER_TEST(MaterialEditor_PanelsDrawAndEdit) {
    HeadlessImGui imgui;
    MaterialDocument doc(Lit());
    MaterialEditor editor(doc);
    for (int i = 0; i < 3; ++i) imgui.Frame([&] { editor.Draw(); });
    AETHER_CHECK(editor.Selected().kind == MaterialEditor::ItemKind::None);

    // The palette, opened from a dragged texture pin, places a linked sample.
    const GraphPinRef texture_pin{2, "value", true};
    editor.OpenPalette(100, 400, &texture_pin);
    imgui.Frame([&] { editor.Draw(); });
    AETHER_CHECK(editor.PaletteOpen());
    editor.SetPaletteQuery("triplanar");
    AETHER_CHECK(editor.PaletteResults().front().id == "Texture.Triplanar");
    const NodeId tri = editor.PlaceFromPalette("Texture.Triplanar");
    AETHER_CHECK(tri != 0 && !editor.PaletteOpen() && HasLink(doc.Get(), 2, "value", tri, "texture"));
    AETHER_CHECK(editor.Selected().kind == MaterialEditor::ItemKind::Node && editor.Selected().node == tri);
    AETHER_CHECK(doc.Get().Find(tri)->x == 100.0f && doc.UndoLabel() == "Add node");

    // Every kind of Details panel draws: a node, each configurable node type, a parameter, the settings.
    for (const char* type : {"Const.Float3", "Math.ComponentMask", "Input.TexCoord", "Math.Noise", "Utility.Reroute", "Custom.HLSL", "Math.Lerp"}) {
        NodeId id = 0;
        doc.Edit("Add", [&](mat::Material& m) { id = AddMaterialNode(m, type, 0, 600); });
        editor.FocusNode(id);
        imgui.Frame([&] { editor.Draw(); });
        AETHER_CHECK(editor.Selected().node == id && editor.View().selection == std::set<u32>{id});
    }
    editor.Select({MaterialEditor::ItemKind::Parameter, "Tint", 0});
    imgui.Frame([&] { editor.Draw(); });
    const NodeId added = editor.AddParameterNode("Tint", 50, 50);
    AETHER_CHECK(added != 0 && doc.Get().Find(added)->type == "Param.Vector:Tint" && editor.AddParameterNode("Nope", 0, 0) == 0);
    AETHER_CHECK(editor.RenameParameter("Tint", "Color") && doc.Get().Find(added)->type == "Param.Vector:Color");
    AETHER_CHECK(!editor.RenameParameter("Color", "Roughness") && !editor.Status().empty());
    editor.Select({MaterialEditor::ItemKind::Parameter, "Color", 0});
    imgui.Frame([&] { editor.Draw(); });
    editor.Select({});
    for (MaterialEditor::BottomTab tab : {MaterialEditor::BottomTab::Diagnostics, MaterialEditor::BottomTab::Hlsl, MaterialEditor::BottomTab::Stats}) {
        editor.bottom_tab = tab;
        imgui.Frame([&] { editor.Draw(); });
    }

    // Undo past a node forgets its selection; a missing parameter too.
    editor.FocusNode(added);
    doc.Undo(); // the rename
    doc.Undo(); // the parameter node
    imgui.Frame([&] { editor.Draw(); });
    AETHER_CHECK(doc.Get().Find(added) == nullptr);
    editor.FocusNode(9999); // no such node: nothing happens
    AETHER_CHECK(!editor.SaveNow() && editor.Status().rfind("Save failed", 0) == 0);

    // A material function opens too.
    mat::Material fn;
    fn.is_function = true;
    fn.Add("Function.Input", {{"name", "X"}, {"type", "float"}});
    MaterialDocument fdoc(fn);
    MaterialEditor fedit(fdoc);
    fedit.FocusNode(1);
    for (int i = 0; i < 2; ++i) imgui.Frame([&] { fedit.Draw(); });
    AETHER_CHECK(fdoc.Analysis().Has("MT011")); // no output yet
}
