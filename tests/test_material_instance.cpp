#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/renderer/draw_list.h"
#include "aether/renderer/material_instance.h"
#include "test_framework.h"

#include <cstring>
#include <filesystem>
#include <map>

using namespace aether;
using namespace aether::mat;
using nlohmann::json;

// Phase 15 step 3: material instances, parameter buffers and the Blueprint
// parameter nodes.

namespace {

Material BaseMaterial() {
    Material m;
    m.parameters.push_back({"Albedo", PinType::Texture, {}, "albedo-guid", ""});
    m.parameters.push_back({"Tint", PinType::Float3, {1, 1, 1, 0}, "", ""});
    m.parameters.push_back({"Roughness", PinType::Float, {0.5f, 0, 0, 0}, "", ""});
    m.parameters.push_back({"Wind", PinType::Float2, {0.1f, 0.2f, 0, 0}, "", ""});
    const NodeId out = m.Add("Material.Output");
    const NodeId sample = m.Add("Texture.Sample"), mul = m.Add("Math.Multiply");
    m.Connect(m.Add("Param.Texture:Albedo"), "value", sample, "texture").Connect(sample, "rgb", mul, "a");
    m.Connect(m.Add("Param.Vector:Tint"), "value", mul, "b").Connect(mul, "result", out, "BaseColor");
    m.Connect(m.Add("Param.Scalar:Roughness"), "value", out, "Roughness");
    const NodeId pan = m.Add("Coords.Panner"), s2 = m.Add("Texture.Sample");
    m.Connect(m.Add("Param.Vector:Wind"), "value", pan, "speed");
    m.Connect(m.Add("Param.Texture:Albedo"), "value", s2, "texture").Connect(pan, "uv", s2, "uv").Connect(s2, "rgb", out, "Emissive");
    return m;
}

f32 FloatAt(const std::vector<u8>& bytes, u32 offset) {
    f32 v;
    std::memcpy(&v, bytes.data() + offset, sizeof v);
    return v;
}

} // namespace

AETHER_TEST(MaterialInstance_OverridesAndFiles) {
    MaterialInstance i;
    i.parent = "materials/M_Base.amat";
    i.SetValue("Tint", Vec4(1, 0, 0, 0));
    i.SetValue("Tint", Vec4(0, 1, 0, 0)); // replaces
    i.SetTexture("Albedo", "rust-guid");
    AETHER_CHECK(i.overrides.size() == 2 && i.Find("Tint")->value.y == 1.0f && i.Find("Albedo")->is_texture);
    AETHER_CHECK(i.Clear("Albedo") && !i.Clear("Albedo") && i.Find("Albedo") == nullptr);
    i.SetTexture("Albedo", "rust-guid");

    const json j = InstanceToJson(i);
    AETHER_CHECK(j["$type"] == "MaterialInstance" && j["parameters"]["Albedo"]["texture"] == "rust-guid");
    MaterialInstance back;
    std::string error;
    AETHER_CHECK(InstanceFromJson(j, back, &error) && back.parent == i.parent && back.overrides.size() == 2);
    AETHER_CHECK(back.Find("Tint")->value.y == 1.0f && back.Find("Albedo")->texture == "rust-guid");
    AETHER_CHECK(InstanceToJson(back) == j);
    json scalar = j;
    scalar["parameters"]["Roughness"] = 0.25; // a number is accepted
    AETHER_CHECK(InstanceFromJson(scalar, back, &error) && back.Find("Roughness")->value.x == 0.25f);

    for (const char* broken : {R"({"$type": "Material"})", R"({"$type": "MaterialInstance", "parameters": {}})",
                               R"({"$type": "MaterialInstance", "$version": 9, "parent": "a"})",
                               R"({"$type": "MaterialInstance", "parent": "a", "parameters": {"Tint": "red"}})",
                               R"({"$type": "MaterialInstance", "parent": "a", "parameters": {"Albedo": {"texture": 3}}})"}) {
        AETHER_CHECK(!InstanceFromJson(json::parse(broken), back, &error) && !error.empty());
    }

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_material_instance_test";
    std::filesystem::create_directories(dir);
    AETHER_CHECK(SaveInstance(i, dir / "MI_Rust.amati", &error));
    MaterialInstance loaded;
    AETHER_CHECK(LoadInstance(dir / "MI_Rust.amati", loaded, &error) && InstanceToJson(loaded) == j);
    AETHER_CHECK(!LoadInstance(dir / "missing.amati", loaded, &error));
    std::filesystem::remove_all(dir);
}

AETHER_TEST(MaterialInstance_ChainsResolveInOrder) {
    const Material base = BaseMaterial();
    MaterialInstance rust, dark_rust;
    rust.parent = "M_Base.amat";
    rust.SetValue("Tint", Vec4(0.8f, 0.4f, 0.2f, 0));
    rust.SetTexture("Albedo", "rust-guid");
    rust.SetValue("Roughness", Vec4(0.9f, 0, 0, 0));
    dark_rust.parent = "MI_Rust.amati";
    dark_rust.SetValue("Tint", Vec4(0.4f, 0.2f, 0.1f, 0));
    dark_rust.SetValue("Gloss", Vec4(1, 0, 0, 0));    // renamed away: MI001
    dark_rust.SetTexture("Roughness", "wrong-guid");  // a texture for a value: MI002

    std::vector<Diagnostic> diags;
    const ResolvedParameters r = ResolveParameters(base, {&rust, &dark_rust}, &diags);
    AETHER_CHECK(r.values.size() == 4 && r.values[0].name == "Albedo"); // declaration order
    AETHER_CHECK(r.Find("Tint")->value.x == 0.4f && r.Find("Tint")->source == 2);
    AETHER_CHECK(r.Find("Albedo")->texture == "rust-guid" && r.Find("Albedo")->source == 1);
    AETHER_CHECK(r.Find("Roughness")->value.x == 0.9f && r.Find("Roughness")->source == 1);
    AETHER_CHECK(r.Find("Wind")->value.y == 0.2f && r.Find("Wind")->source == 0);
    AETHER_CHECK(diags.size() == 2 && diags[0].code == "MI001" && diags[1].code == "MI002" && diags[1].pin == "Roughness");
    AETHER_CHECK(diags[0].severity == Severity::Warning);
    AETHER_CHECK(ResolveParameters(base, {}).Find("Tint")->value.x == 1.0f);

    // From "disk": an in-memory asset store.
    std::map<std::string, json> files;
    files["M_Base.amat"] = MaterialToJson(base);
    files["MI_Rust.amati"] = InstanceToJson(rust);
    files["MI_DarkRust.amati"] = InstanceToJson(dark_rust);
    const AssetReader read = [&](const std::string& path) -> std::optional<json> {
        auto it = files.find(path);
        return it == files.end() ? std::nullopt : std::optional<json>(it->second);
    };
    InstanceChain chain;
    std::string error;
    AETHER_CHECK(LoadInstanceChain("MI_DarkRust.amati", read, chain, &error));
    AETHER_CHECK(chain.material_path == "M_Base.amat" && chain.instances.size() == 2 && chain.instances[0].parent == "M_Base.amat");
    AETHER_CHECK(ResolveParameters(chain.material, chain.Pointers()).Find("Tint")->value.x == 0.4f);
    AETHER_CHECK(LoadInstanceChain("M_Base.amat", read, chain, &error) && chain.instances.empty()); // a material is its own chain

    MaterialInstance loop_a, loop_b;
    loop_a.parent = "B.amati";
    loop_b.parent = "A.amati";
    files["A.amati"] = InstanceToJson(loop_a);
    files["B.amati"] = InstanceToJson(loop_b);
    AETHER_CHECK(!LoadInstanceChain("A.amati", read, chain, &error) && error.rfind("MI003", 0) == 0);
    MaterialInstance orphan;
    orphan.parent = "gone.amat";
    files["Orphan.amati"] = InstanceToJson(orphan);
    AETHER_CHECK(!LoadInstanceChain("Orphan.amati", read, chain, &error) && error.rfind("MI004", 0) == 0);
    files["Blueprint.abp"] = json{{"$type", "Blueprint"}};
    orphan.parent = "Blueprint.abp";
    files["Orphan.amati"] = InstanceToJson(orphan);
    AETHER_CHECK(!LoadInstanceChain("Orphan.amati", read, chain, &error) && error.rfind("MI004", 0) == 0);
    for (int n = 0; n < 20; ++n) {
        MaterialInstance deep;
        deep.parent = n == 0 ? "M_Base.amat" : "D" + std::to_string(n - 1);
        files["D" + std::to_string(n)] = InstanceToJson(deep);
    }
    AETHER_CHECK(LoadInstanceChain("D15", read, chain, &error) && chain.instances.size() == 16);
    AETHER_CHECK(!LoadInstanceChain("D19", read, chain, &error) && error.rfind("MI005", 0) == 0);
}

AETHER_TEST(MaterialInstance_PacksBuffersWithoutRecompiling) {
    const Material base = BaseMaterial();
    const GeneratedMaterial g = GenerateHlsl(base);
    AETHER_CHECK(g.ok && g.parameters.size == 32 && g.textures.size() == 1);
    MaterialInstance rust;
    rust.SetValue("Tint", Vec4(0.8f, 0.4f, 0.2f, 0));
    rust.SetValue("Wind", Vec4(3, 4, 0, 0));
    rust.SetTexture("Albedo", "rust-guid");
    const ResolvedParameters values = ResolveParameters(base, {&rust});
    const std::vector<u8> bytes = PackParameters(g.parameters, values);
    AETHER_CHECK(bytes.size() == 32);
    const ParameterSlot& tint = *g.parameters.Find("Tint");
    const ParameterSlot& rough = *g.parameters.Find("Roughness");
    const ParameterSlot& wind = *g.parameters.Find("Wind");
    AETHER_CHECK(FloatAt(bytes, tint.offset) == 0.8f && FloatAt(bytes, tint.offset + 8) == 0.2f);
    AETHER_CHECK(FloatAt(bytes, rough.offset) == 0.5f && FloatAt(bytes, wind.offset + 4) == 4.0f);
    AETHER_CHECK(TextureBindings(g.textures, values) == (std::vector<std::string>{"rust-guid"}));

    // A dynamic block: changes repack lazily and bump the revision; same values don't.
    ParameterBlock block(g.parameters, g.textures, values);
    AETHER_CHECK(block.Buffer() == bytes && block.Revision() == 0);
    AETHER_CHECK(block.SetScalar("Roughness", 0.1f) && block.Revision() == 1);
    AETHER_CHECK(block.SetScalar("Roughness", 0.1f) && block.Revision() == 1);
    AETHER_CHECK(FloatAt(block.Buffer(), rough.offset) == 0.1f);
    AETHER_CHECK(block.SetVector("Tint", Vec4(0, 0, 1, 0)) && FloatAt(block.Buffer(), tint.offset + 8) == 1.0f);
    AETHER_CHECK(block.SetTexture("Albedo", "moss-guid") && block.Textures()[0] == "moss-guid");
    AETHER_CHECK(!block.SetScalar("Tint", 1.0f) && !block.SetVector("Roughness", Vec4(1, 1, 1, 1)));
    AETHER_CHECK(!block.SetTexture("Tint", "x") && !block.SetScalar("Nope", 1.0f) && !block.SetTexture("Nope", "x"));
    const u64 before = block.Revision();
    AETHER_CHECK(block.Reset("Roughness") && block.Value("Roughness")->x == 0.5f && block.Revision() == before + 1);
    block.ResetAll();
    AETHER_CHECK(block.Buffer() == bytes && block.Textures()[0] == "rust-guid");
    AETHER_CHECK(!block.Value("Albedo").has_value() && !block.Reset("Nope"));

    // The shader is shared: an instance only changes data.
    AETHER_CHECK(GenerateHlsl(base).permutation_key == g.permutation_key);
}

AETHER_TEST(MaterialInstance_EntityOverridesFromBlueprints) {
    RegisterRenderComponents();
    World world;
    GuidIndex guids;
    auto mesh = [&](const char* material) {
        const Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, Transform{Vec3(0, 0, -5), Quaternion::Identity()});
        guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
        ModelRenderer m;
        SetModelPath(m, "models/crate.gltf");
        world.AddComponent(e, m);
        if (material != nullptr) world.AddComponent(e, MaterialParameters{material, {}, 0});
        return e;
    };
    const Entity plain = mesh(nullptr), rusty = mesh("MI_Rust.amati"), rusty2 = mesh("MI_Rust.amati");

    // A Blueprint on `rusty`: BeginPlay sets Roughness and Tint, reads Roughness back.
    bp::Blueprint blueprint;
    bp::Graph events;
    events.name = "EventGraph";
    blueprint.graphs.push_back(events);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    const bp::NodeId begin = b.Add("Event.BeginPlay");
    const bp::NodeId set_rough = b.Add("Call.Native:MaterialParameters.SetScalarParameter");
    const bp::NodeId set_tint = b.Add("Call.Native:MaterialParameters.SetVectorParameter");
    const bp::NodeId get_rough = b.Add("Call.Native:MaterialParameters.GetScalarParameter");
    const bp::NodeId print = b.Add("Debug.Print");
    b.Default(set_rough, "name", "Roughness").Default(set_rough, "value", 0.2);
    b.Default(set_tint, "name", "Tint").Default(set_tint, "value", json::array({0.1, 0.9, 0.3})).Default(set_tint, "alpha", 1.0);
    b.Default(get_rough, "name", "Roughness");
    b.Connect(begin, "then", set_rough, "exec").Connect(set_rough, "then", set_tint, "exec").Connect(set_tint, "then", print, "exec");
    b.Connect(get_rough, "return", print, "text");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::printf("    %s: %s\n", d.code.c_str(), d.message.c_str());
    AETHER_CHECK(compiled.Ok());
    bp::BlueprintVM vm(world);
    std::vector<std::string> printed;
    vm.SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });
    AETHER_CHECK(vm.Attach(rusty, compiled.blueprint));
    vm.BeginPlay();
    AETHER_CHECK(printed == (std::vector<std::string>{"0.2"}) && vm.Errors().empty());
    const MaterialParameters& params = *world.GetComponent<MaterialParameters>(rusty);
    AETHER_CHECK(params.values.size() == 2 && params.revision == 2 && params.GetVectorParameter("Tint").y == 0.9f);

    // The overrides reach a parameter block; reapplying doesn't bump its revision.
    const Material base = BaseMaterial();
    const GeneratedMaterial g = GenerateHlsl(base);
    ParameterBlock block(g.parameters, g.textures, ResolveParameters(base, {}));
    params.ApplyTo(block);
    const u64 revision = block.Revision();
    AETHER_CHECK(block.Value("Roughness")->x == 0.2f && block.Value("Tint")->y == 0.9f && revision > 0);
    params.ApplyTo(block);
    AETHER_CHECK(block.Revision() == revision);
    MaterialParameters cleared = params;
    AETHER_CHECK(cleared.ClearParameter("Roughness") && !cleared.ClearParameter("Roughness"));
    cleared.SetTextureParameter("Tint", "not-a-texture"); // doesn't fit: the base value stays
    cleared.ApplyTo(block);
    AETHER_CHECK(block.Value("Roughness")->x == 0.5f && block.Value("Tint")->y == 1.0f);

    // Extraction carries the material and the overrides; batches split by material.
    const RenderScene scene = ExtractRenderScene(world, guids);
    AETHER_CHECK(scene.objects.size() == 3 && scene.material_parameters.size() == 1);
    AETHER_CHECK(scene.objects[0].entity == plain && scene.objects[0].material_key == 0 && scene.objects[0].parameters == -1);
    AETHER_CHECK(scene.objects[1].material_key != 0 && scene.objects[1].material_key == scene.objects[2].material_key);
    AETHER_CHECK(scene.objects[1].parameters == 0 && scene.objects[2].parameters == -1 && scene.objects[2].entity == rusty2);
    AETHER_CHECK(scene.material_parameters[0].GetScalarParameter("Roughness") == 0.2f);
    Camera camera;
    const View view = MakeView(camera, Mat4::Identity(), 1.0f);
    const DrawList list = BuildDrawList(scene, view, Cull(scene, view));
    AETHER_CHECK(list.batches.size() == 2 && list.InstanceCount() == 3);
    AETHER_CHECK(list.batches[0].mesh_key == list.batches[1].mesh_key && list.batches[0].material_key != list.batches[1].material_key);
}
