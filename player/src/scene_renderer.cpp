#include "aether/player/scene_renderer.h"

#include "aether/assets/gltf_loader.h"
#include "aether/assets/image.h"
#include "aether/core/log.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "aether/ui/draw.h"
#include "aether/ui/font.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

namespace aether::player {
namespace {

using namespace gfx::rhi;
using json = nlohmann::json;

struct SceneVertex {
    f32 position[3];
    f32 uv[2];
};
static_assert(sizeof(SceneVertex) == sizeof(f32) * 5);

struct DrawConstants {
    Mat4 mvp;
    f32 color[4] = {1, 1, 1, 1};
    u32 texture_index = 0;
    u32 use_texture = 0;
    u32 padding[2] = {};
};
static_assert(sizeof(DrawConstants) == 96);

constexpr char kSceneShader[] = R"(
struct PushConstants {
    float4x4 g_Mvp;
    float4 g_Color;
    uint g_TextureIndex;
    uint g_UseTexture;
    uint2 g_Padding;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);

struct VSInput {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
Texture2D g_Textures[32] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(g_PC.g_Mvp, float4(input.position, 1.0));
    output.uv = input.uv;
    return output;
}
float4 PSMain(PSInput input) : SV_TARGET {
    float4 base = g_PC.g_UseTexture != 0
        ? g_Textures[g_PC.g_TextureIndex].Sample(g_Sampler, input.uv)
        : float4(1, 1, 1, 1);
    return base * g_PC.g_Color;
}
)";

struct HudConstants {
    f32 rect[4]{};
    f32 viewport[4]{};
    f32 color[4]{};
    f32 uv[4]{};
    f32 sdf_range = 0.0f;
    f32 sdf_edge = 0.5f;
    f32 sdf_softness = 0.0f;
    u32 texture_index = 0;
    u32 use_texture = 0;
    u32 use_sdf = 0;
};
static_assert(sizeof(HudConstants) == 88);

constexpr char kHudShader[] = R"(
struct PushConstants {
    float4 g_Rect;
    float4 g_Viewport;
    float4 g_Color;
    float4 g_Uv;
    float g_SdfRange;
    float g_SdfEdge;
    float g_SdfSoftness;
    uint g_TextureIndex;
    uint g_UseTexture;
    uint g_UseSdf;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[32] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(uint vertex_id : SV_VertexID) {
    const float2 corners[6] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1)
    };
    float2 corner = corners[vertex_id];
    float2 pixel = g_PC.g_Rect.xy + corner * g_PC.g_Rect.zw;
    PSInput output;
    output.position = float4(pixel.x / g_PC.g_Viewport.x * 2.0 - 1.0,
                             1.0 - pixel.y / g_PC.g_Viewport.y * 2.0, 0.0, 1.0);
    output.uv = g_PC.g_Uv.xy + corner * g_PC.g_Uv.zw;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float alpha = g_PC.g_Color.a;
    if (g_PC.g_UseTexture != 0) {
        float sample_value = g_Textures[g_PC.g_TextureIndex].Sample(g_Sampler, input.uv).r;
        if (g_PC.g_UseSdf != 0) {
            float width = 1.0 / max(g_PC.g_SdfRange, 0.001) + g_PC.g_SdfSoftness;
            alpha *= saturate((sample_value - g_PC.g_SdfEdge) / width + 0.5);
        } else {
            alpha *= sample_value;
        }
    }
    return float4(g_PC.g_Color.rgb, alpha);
}
)";

std::string Base64(std::span<const u8> bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (usize i = 0; i < bytes.size(); i += 3) {
        const u32 a = bytes[i];
        const u32 b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const u32 c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const u32 value = (a << 16) | (b << 8) | c;
        out.push_back(alphabet[(value >> 18) & 63]);
        out.push_back(alphabet[(value >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? alphabet[value & 63] : '=');
    }
    return out;
}

std::string ResolveContentPath(const std::string& owner_path, const std::string& uri) {
    const std::filesystem::path base = std::filesystem::path(owner_path).parent_path();
    return (base / std::filesystem::path(uri)).lexically_normal().generic_string();
}

Mat4 EntityTransform(const Transform& transform) {
    return Mat4::Translation(transform.position) * transform.rotation.ToMat4();
}

} // namespace

struct SceneRenderer::Impl {
    struct Primitive {
        BufferHandle vertices;
        BufferHandle indices;
        u32 index_count = 0;
        f32 color[4] = {1, 1, 1, 1};
        SampledTextureHandle texture;
        bool has_texture = false;
    };
    struct Model {
        assets::GltfScene scene;
        std::vector<std::vector<Primitive>> meshes;
        bool valid = false;
    };

    IDevice& device;
    ISwapChain& swap_chain;
    const GamePackage& package;
    PipelineHandle pipeline;
    PipelineHandle hud_pipeline;
    SampledTextureHandle white_texture;
    std::unique_ptr<ui::SdfFont> hud_font;
    std::unordered_map<std::string, Model> models;
    std::unordered_map<std::string, SampledTextureHandle> textures;

    Impl(IDevice& d, ISwapChain& s, const GamePackage& p) : device(d), swap_chain(s), package(p) {
        PipelineDesc desc;
        desc.hlsl_source = kSceneShader;
        desc.push_constant_size_bytes = sizeof(DrawConstants);
        desc.use_vertex_buffer = true;
        desc.enable_bindless_textures = true;
        desc.depth_test = true;
        pipeline = device.CreatePipeline(desc, swap_chain);

        constexpr std::array<u8, 4> white{255, 255, 255, 255};
        white_texture = device.CreateTexture(1, 1, white.data());
        if (white_texture.IsValid()) textures.emplace("", white_texture);
        // This first proof HUD is specific to the AETHER-01 content contract.
        if (package.Manifest().project != "AETHER-01") return;

        PipelineDesc hud_desc;
        hud_desc.hlsl_source = kHudShader;
        hud_desc.push_constant_size_bytes = sizeof(HudConstants);
        hud_desc.enable_bindless_textures = true;
        hud_desc.enable_blending = true;
        hud_pipeline = device.CreatePipeline(hud_desc, swap_chain);

        std::vector<u8> font_bytes;
        if (package.ReadContent("fonts/Roboto-Medium.ttf", font_bytes)) {
            std::string error;
            hud_font = ui::SdfFont::FromMemory(std::move(font_bytes), {}, &error);
            if (hud_font) {
                hud_font->Prewarm("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:/-%");
                const auto& atlas = hud_font->AtlasPixels();
                std::vector<u8> rgba(atlas.size() * 4);
                for (usize i = 0; i < atlas.size(); ++i) {
                    rgba[i * 4] = atlas[i];
                    rgba[i * 4 + 1] = atlas[i];
                    rgba[i * 4 + 2] = atlas[i];
                    rgba[i * 4 + 3] = 255;
                }
                const SampledTextureHandle font_texture = device.CreateTexture(
                    static_cast<u32>(hud_font->AtlasWidth()), static_cast<u32>(hud_font->AtlasHeight()), rgba.data());
                if (font_texture.IsValid()) {
                    hud_font->SetTexture(font_texture.index);
                } else {
                    AETHER_LOG_WARN("PlayerRenderer", "Couldn't upload the AETHER-01 HUD font atlas");
                    hud_font.reset();
                }
            } else {
                AETHER_LOG_WARN("PlayerRenderer", "Couldn't load the AETHER-01 HUD font: %s", error.c_str());
            }
        } else {
            AETHER_LOG_WARN("PlayerRenderer", "Cooked package has no fonts/Roboto-Medium.ttf; HUD text is unavailable");
        }
    }

    void DrawHud(Game& game, ICommandList& commands) {
        if (!hud_font) return;
        const World& world = game.GetWorld();
        const f32 width = static_cast<f32>(swap_chain.Width());
        const f32 height = static_cast<f32>(swap_chain.Height());
        if (width <= 0 || height <= 0) return;

        const auto read_attribute = [&](Entity entity, const char* name, f32 fallback) {
            if (!world.HasComponent<gas::AttributeSet>(entity)) return fallback;
            const gas::AttributeSet* attributes = world.GetComponent<gas::AttributeSet>(entity);
            return attributes ? attributes->Get(name, fallback) : fallback;
        };
        f32 health = 3.0f, max_health = 3.0f, ammo = 30.0f, max_ammo = 30.0f;
        f32 damage_flash = 0.0f, hit_confirm = 0.0f, reloading = 0.0f;
        i32 mission_stage = 0;
        i32 weapon_index = 1;
        bool research_clear = false;
        f32 archive_time = 0;
        i32 menu_state = 0, menu_row = 0;
        f32 settings_status = 0;
        f32 save_status = 0.0f;
        const std::vector<Entity> players = FindEntitiesWithTag(world, "Player");
        if (!players.empty()) {
            const Entity player = players.front();
            health = read_attribute(player, "Health", health);
            max_health = read_attribute(player, "MaxHealth", max_health);
            ammo = read_attribute(player, "Ammo", ammo);
            max_ammo = read_attribute(player, "MaxAmmo", max_ammo);
            damage_flash = read_attribute(player, "DamageFlash", damage_flash);
            hit_confirm = read_attribute(player, "HitConfirm", hit_confirm);
            reloading = read_attribute(player, "Reloading", reloading);
            mission_stage = static_cast<i32>(read_attribute(player, "MissionStage", 0));
            save_status = read_attribute(player, "SaveStatus", 0);
            weapon_index = static_cast<i32>(read_attribute(player, "WeaponIndex", 1));
            research_clear = read_attribute(player, "ResearchClear", 0) > 0;
            archive_time = read_attribute(player, "ArchiveTime", 0);
            menu_state = static_cast<i32>(read_attribute(player, "MenuState", 0));
            menu_row = static_cast<i32>(read_attribute(player, "MenuRow", 0));
            settings_status = read_attribute(player, "SettingsStatus", 0);
        }
        health = std::clamp(health, 0.0f, max_health);
        ammo = std::clamp(ammo, 0.0f, max_ammo);

        bool scout_found = false;
        f32 boss_phase = 1, boss_attack = 0;
        f32 scout_health = 0.0f, scout_max_health = 3.0f;
        std::string threat_name = "SCOUT // THREAT";
        for (const Entity scout : FindEntitiesWithTag(world, "Scout")) {
            scout_found = true;
            scout_health = read_attribute(scout, "Health", 3.0f);
            scout_max_health = read_attribute(scout, "MaxHealth", 3.0f);
            if (!world.HasComponent<gas::AttributeSet>(scout)) {
                const Transform* transform = world.GetComponent<Transform>(scout);
                if (transform && transform->position.y < -50.0f) scout_health = 0.0f;
            }
            break;
        }
        scout_health = std::clamp(scout_health, 0.0f, scout_max_health);
        if (mission_stage == 2) {
            f32 nearest = 1.0e30f;
            threat_name = "RESEARCH DEFENSES";
            scout_health = 0;
            if (!players.empty()) {
                const auto* hero = world.GetComponent<Transform>(players.front());
                for (const auto* role : {"Sentinel", "Hunter"}) {
                    for (const Entity enemy : FindEntitiesWithTag(world, role)) {
                        const auto* transform = world.GetComponent<Transform>(enemy);
                        if (!hero || !transform || read_attribute(enemy, "Health", 0) <= 0) continue;
                        const Vec3 delta = transform->position - hero->position;
                        const f32 distance = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
                        if (distance >= nearest) continue;
                        nearest = distance;
                        scout_found = true;
                        const bool sentinel = std::string_view(role) == "Sentinel";
                        scout_max_health = sentinel ? 6.0f : 4.0f;
                        scout_health = read_attribute(enemy, "Health", scout_max_health);
                        boss_attack = read_attribute(enemy, "AttackState", 0);
                        threat_name = sentinel ? "SENTINEL / SHIELD " + std::to_string(static_cast<i32>(read_attribute(enemy, "Shield", 0)))
                                               : "HUNTER // AMBUSH";
                    }
                }
            }
        }
        if (mission_stage >= 3) {
            const auto bosses = FindEntitiesWithTag(world, "Warden");
            if (!bosses.empty()) {
                scout_found = true;
                scout_health = read_attribute(bosses.front(), "Health", 12);
                scout_max_health = 12;
                boss_phase = read_attribute(bosses.front(), "Phase", 1);
                boss_attack = read_attribute(bosses.front(), "AttackState", 0);
                threat_name = "WARDEN / PHASE " + std::to_string(static_cast<i32>(boss_phase));
            }
        }

        ui::DrawList list;
        const ui::Color panel{0.015f, 0.035f, 0.055f, 0.86f};
        const ui::Color pale{0.82f, 0.93f, 0.97f, 1.0f};
        const ui::Color muted{0.36f, 0.57f, 0.63f, 1.0f};
        const ui::Color cyan{0.18f, 0.82f, 0.95f, 1.0f};
        const ui::Color red{0.96f, 0.20f, 0.17f, 1.0f};
        const ui::Color dim{0.08f, 0.16f, 0.19f, 1.0f};
        auto text = [&](std::string_view value, f32 x, f32 y, f32 size, ui::Color color, f32 box_width = 340.0f) {
            list.AddText(*hud_font, value, size, {x, y, box_width, size + 6.0f}, color);
        };
        auto solid = [&](f32 x, f32 y, f32 w, f32 h, ui::Color color) {
            list.AddQuad({x, y, w, h}, color);
        };

        if (damage_flash > 0.0f) {
            list.AddQuad({0, 0, width, height}, {0.72f, 0.035f, 0.025f, 0.17f * std::clamp(damage_flash, 0.0f, 1.0f)});
        }

        solid(24, 22, 250, 104, panel);
        solid(24, 22, 3, 104, cyan);
        text("AEGIS-9", 42, 31, 21, pale, 180);
        text("HEALTH", 42, 67, 13, muted, 100);
        text(std::to_string(static_cast<i32>(std::ceil(health))) + "/" +
                 std::to_string(static_cast<i32>(max_health)),
             194, 65, 14, health <= 1 ? red : pale, 70);
        solid(42, 96, 211, 7, dim);
        if (max_health > 0) solid(42, 96, 211 * health / max_health, 7, health <= 1 ? red : cyan);

        const f32 objective_x = std::max(278.0f, width * 0.5f - 165.0f);
        solid(objective_x, 22, 330, 58, panel);
        text("MISSION 01  /  HELIOS-7", objective_x + 18, 28, 12, muted, 290);
        constexpr const char* objectives[] = {"E RETRIEVE AEGIS RIFLE", "NEUTRALIZE SCOUT",
                                              "CLEAR RESEARCH DEFENSES", "DEFEAT WARDEN",
                                              "E READ ARCHIVE TERMINAL", "FIRST CONTACT COMPLETE"};
        text(health <= 0 ? "R RELOAD CHECKPOINT" : mission_stage == 2 && research_clear ? "ENTER WARDEN ARENA" : objectives[std::clamp(mission_stage, 0, 5)],
             objective_x + 18, 49, 17, health <= 0 ? red : pale, 295);

        const f32 scout_x = width - 274.0f;
        solid(scout_x, 22, 250, 74, panel);
        text(threat_name, scout_x + 16, 31, 13, muted, 220);
        if (scout_found && scout_health > 0) {
            text(std::to_string(static_cast<i32>(std::ceil(scout_health))) + "/" +
                     std::to_string(static_cast<i32>(scout_max_health)),
                 scout_x + 202, 50, 13, pale, 42);
            solid(scout_x + 16, 73, 218, 7, dim);
            if (scout_max_health > 0) solid(scout_x + 16, 73, 218 * scout_health / scout_max_health, 7, red);
        } else {
            text("NEUTRALIZED", scout_x + 16, 57, 13, cyan, 220);
        }

        const f32 cx = width * 0.5f, cy = height * 0.5f;
        const ui::Color reticle = hit_confirm > 0.0f ? cyan : pale;
        solid(cx - 13, cy - 1, 8, 2, reticle);
        solid(cx + 5, cy - 1, 8, 2, reticle);
        solid(cx - 1, cy - 13, 2, 8, reticle);
        solid(cx - 1, cy + 5, 2, 8, reticle);

        const f32 weapon_x = width - 274.0f;
        solid(weapon_x, height - 92.0f, 250, 68, panel);
        solid(weapon_x, height - 92.0f, 3, 68, cyan);
        constexpr const char* weapons[] = {"AEGIS RIFLE", "ARC PISTOL", "VOLT SHOTGUN"};
        text(mission_stage == 0 ? "RIFLE NOT EQUIPPED" : weapons[std::clamp(weapon_index, 1, 3) - 1], weapon_x + 17, height - 83.0f, 14, pale, 210);
        text(mission_stage == 0 ? "E TO RETRIEVE" : reloading > 0.5f ? "RELOADING" : "AMMO  " + std::to_string(static_cast<i32>(ammo)) +
                 " / " + std::to_string(static_cast<i32>(max_ammo)),
             weapon_x + 17, height - 55.0f, 13, ammo <= 5 ? red : muted, 220);

        solid(24, height - 88.0f, 440, 64, panel);
        text("ESC / START: MENU", 26, height - 111.0f, 12, muted, 300);
        text(mission_stage == 0 ? "HELIOS-7 CRYO BAY  /  87 YEARS LATER" :
             mission_stage >= 4 ? "FACILITY LOSS WAS DELIBERATE" :
             mission_stage == 3 ? "AETHER: PRESERVE PLANETARY LIFE" : "SECURITY NETWORK ONLINE",
             40, height - 79.0f, 13, pale, 410);
        text(save_status < 0 ? "CHECKPOINT SAVE FAILED" : mission_stage == 5 ? "END OF GREYBOX PROOF / N NEW GAME" :
             boss_attack > 0 ? "MOVE OUT OF THE MARKED ATTACK AREA" : save_status > 0 ? "CHECKPOINT SAVED" : "AWAKENING",
             40, height - 53.0f, 12, save_status < 0 || boss_attack > 0 ? red : muted, 410);

        if (mission_stage == 5) {
            const f32 reveal_x = std::max(24.0f, width * 0.5f - 260.0f);
            solid(reveal_x, height * 0.56f, 520, 92, panel);
            text("HELIOS-7 / ARCHIVE REVEAL", reveal_x + 20, height * 0.56f + 13, 14, cyan, 480);
            const char* reveal = archive_time < 2 ? "DECRYPTING THE FACILITY RECORD" :
                                 archive_time < 4 ? "AETHER REDIRECTED THE FAILURE" :
                                 archive_time < 6 ? "THIS FACILITY WAS SACRIFICED" :
                                 archive_time < 8 ? "TO PRESERVE PLANETARY LIFE" : "FIRST CONTACT COMPLETE";
            text(reveal, reveal_x + 20, height * 0.56f + 44, 20, pale, 480);
        }

        if (menu_state != 0) {
            const f32 mx = std::max(24.0f, width * 0.5f - 240.0f);
            const f32 my = std::max(24.0f, height * 0.5f - 190.0f);
            solid(mx, my, 480, 380, {0.015f, 0.035f, 0.055f, 0.97f});
            solid(mx, my, 3, 380, cyan);
            text(menu_state == 2 ? "AUDIO SETTINGS" : menu_state == 3 ? "CONTROLS" : "PAUSED / FIRST CONTACT",
                 mx + 24, my + 20, 23, pale, 430);
            if (menu_state == 1) {
                const char* rows[] = {"RESUME", "AUDIO SETTINGS", "CONTROLS", "LOAD CHECKPOINT", "NEW GAME", "QUIT"};
                for (i32 i = 0; i < 6; ++i) {
                    if (i == menu_row) solid(mx + 20, my + 66 + i * 37.0f, 440, 33, dim);
                    text(rows[i], mx + 34, my + 70 + i * 37.0f, 17, i == menu_row ? cyan : pale, 410);
                }
            } else if (menu_state == 2) {
                const auto& settings = game.Settings().Get();
                const f32 volumes[] = {settings.master, settings.music, settings.sfx, settings.voice};
                const char* rows[] = {"MASTER", "MUSIC", "SFX", "VOICE", "BACK"};
                for (i32 i = 0; i < 5; ++i) {
                    if (i == menu_row) solid(mx + 20, my + 66 + i * 42.0f, 440, 36, dim);
                    text(rows[i], mx + 34, my + 72 + i * 42.0f, 17, i == menu_row ? cyan : pale, 290);
                    if (i < 4) text(std::to_string(static_cast<i32>(std::round(volumes[i] * 100))) + "%",
                                    mx + 366, my + 72 + i * 42.0f, 17, cyan, 70);
                }
            } else {
                const char* rows[] = {"WASD / LEFT STICK: MOVE", "MOUSE / RIGHT STICK: LOOK", "LEFT CLICK / RT: FIRE",
                    "1 2 3 / D-PAD: WEAPONS", "R / X: RELOAD    SPACE / A: JUMP", "E / B: INTERACT    F5: CHECKPOINT"};
                for (i32 i = 0; i < 6; ++i) text(rows[i], mx + 24, my + 73 + i * 35.0f, 15, pale, 432);
            }
            text(settings_status == -1 ? "SETTINGS SAVE FAILED" : settings_status == -2 ? "NO CHECKPOINT YET" :
                 settings_status == 1 ? "SETTINGS SAVED" : "SIMULATION PAUSED",
                 mx + 24, my + 310, 13, settings_status < 0 ? red : muted, 430);
            text(menu_state == 2 ? "LEFT/RIGHT ADJUST   ESC BACK" : "ARROWS SELECT   ENTER CONFIRM   ESC BACK",
                 mx + 24, my + 344, 12, muted, 435);
        }
        commands.BindPipeline(hud_pipeline);
        commands.BindBindlessTextures();
        for (const ui::DrawQuad& quad : list.quads) {
            if (quad.clip >= list.clips.size() || quad.rect.Empty()) continue;
            const ui::Rect clipped = quad.rect.Intersect(list.clips[quad.clip]);
            if (clipped.Empty()) continue;
            const f32 u0 = (clipped.x - quad.rect.x) / quad.rect.w;
            const f32 v0 = (clipped.y - quad.rect.y) / quad.rect.h;
            HudConstants constants;
            constants.rect[0] = clipped.x;
            constants.rect[1] = clipped.y;
            constants.rect[2] = clipped.w;
            constants.rect[3] = clipped.h;
            constants.viewport[0] = width;
            constants.viewport[1] = height;
            constants.color[0] = quad.color.r;
            constants.color[1] = quad.color.g;
            constants.color[2] = quad.color.b;
            constants.color[3] = quad.color.a;
            constants.uv[0] = quad.uv.x + quad.uv.w * u0;
            constants.uv[1] = quad.uv.y + quad.uv.h * v0;
            constants.uv[2] = quad.uv.w * clipped.w / quad.rect.w;
            constants.uv[3] = quad.uv.h * clipped.h / quad.rect.h;
            constants.sdf_range = quad.sdf_range;
            constants.sdf_edge = quad.sdf_edge;
            constants.sdf_softness = quad.sdf_softness;
            constants.texture_index = quad.texture;
            constants.use_texture = quad.texture != 0 ? 1u : 0u;
            constants.use_sdf = quad.sdf_range > 0.0f ? 1u : 0u;
            commands.SetPushConstants(&constants, sizeof(constants));
            commands.Draw(6);
        }
    }

    bool LoadTexture(const std::string& path, SampledTextureHandle& out) {
        if (path.empty()) {
            out = white_texture;
            return false;
        }
        if (const auto found = textures.find(path); found != textures.end()) {
            out = found->second;
            return out.IsValid() && out.index != white_texture.index;
        }
        if (textures.size() >= kMaxBindlessTextures) {
            AETHER_LOG_WARN("PlayerRenderer", "Bindless texture limit reached; using the white fallback for '%s'", path.c_str());
            out = white_texture;
            return false;
        }
        std::vector<u8> bytes;
        assets::ImageData image;
        if (!package.ReadContent(path, bytes) || !assets::DecodeImageFromMemory(bytes, image)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't load model texture '%s'; using the white fallback", path.c_str());
            out = white_texture;
            return false;
        }
        out = device.CreateTexture(image.width, image.height, image.pixels.data());
        if (out.IsValid()) textures.emplace(path, out);
        return out.IsValid() && out.index != white_texture.index;
    }

    bool LoadModel(const std::string& path, Model& out) {
        std::vector<u8> source;
        if (!package.ReadContent(path, source)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't read cooked model '%s'", path.c_str());
            return false;
        }
        json document = json::parse(source.begin(), source.end(), nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            AETHER_LOG_WARN("PlayerRenderer", "Cooked model '%s' isn't valid glTF JSON", path.c_str());
            return false;
        }

        // A cooked package is a VFS, not a filesystem. Embed the already
        // cooked buffer bytes into the in-memory glTF document so the shared
        // parser can safely load it without opening arbitrary paths.
        if (document.contains("buffers") && document["buffers"].is_array()) {
            for (json& buffer : document["buffers"]) {
                if (!buffer.contains("uri") || !buffer["uri"].is_string()) continue;
                const std::string uri = buffer["uri"].get<std::string>();
                if (uri.rfind("data:", 0) == 0) continue;
                const std::string buffer_path = ResolveContentPath(path, uri);
                std::vector<u8> bytes;
                if (!package.ReadContent(buffer_path, bytes)) {
                    AETHER_LOG_WARN("PlayerRenderer", "Model '%s' references missing buffer '%s'", path.c_str(), buffer_path.c_str());
                    return false;
                }
                buffer["uri"] = "data:application/octet-stream;base64," + Base64(bytes);
            }
        }
        const std::string cooked_json = document.dump();
        if (!assets::LoadGltfFromMemory(std::span<const u8>(reinterpret_cast<const u8*>(cooked_json.data()), cooked_json.size()),
                                        out.scene)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't parse cooked model '%s'", path.c_str());
            return false;
        }

        std::vector<SampledTextureHandle> material_textures;
        std::vector<bool> material_has_textures;
        if (document.contains("materials") && document["materials"].is_array()) {
            material_textures.reserve(document["materials"].size());
            for (const json& material : document["materials"]) {
                SampledTextureHandle texture = white_texture;
                bool has_texture = false;
                const json* pbr = material.contains("pbrMetallicRoughness") ? &material["pbrMetallicRoughness"] : nullptr;
                if (pbr && pbr->contains("baseColorTexture")) {
                    const usize index = (*pbr)["baseColorTexture"].value("index", usize(-1));
                    if (index < document.value("textures", json::array()).size()) {
                        const json& texture_info = document["textures"][index];
                        const usize image_index = texture_info.value("source", usize(-1));
                        if (image_index < document.value("images", json::array()).size()) {
                            const json& image = document["images"][image_index];
                            if (image.contains("uri") && image["uri"].is_string()) {
                                const std::string uri = image["uri"].get<std::string>();
                                if (uri.rfind("data:", 0) != 0) {
                                    has_texture = LoadTexture(ResolveContentPath(path, uri), texture);
                                }
                            }
                        }
                    }
                }
                material_textures.push_back(texture);
                material_has_textures.push_back(has_texture);
            }
        }

        out.meshes.resize(out.scene.meshes.size());
        for (usize mesh_index = 0; mesh_index < out.scene.meshes.size(); ++mesh_index) {
            const auto& source_mesh = out.scene.meshes[mesh_index];
            auto& destination_mesh = out.meshes[mesh_index];
            destination_mesh.reserve(source_mesh.primitives.size());
            for (const assets::GltfPrimitive& primitive : source_mesh.primitives) {
                if (primitive.vertices.empty() || primitive.indices.empty()) continue;
                std::vector<SceneVertex> vertices;
                vertices.reserve(primitive.vertices.size());
                for (const assets::GltfVertex& vertex : primitive.vertices) {
                    vertices.push_back({{vertex.position[0], vertex.position[1], vertex.position[2]},
                                        {vertex.uv[0], vertex.uv[1]}});
                }
                Primitive draw;
                draw.vertices = device.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(SceneVertex));
                draw.indices = device.CreateIndexBuffer(primitive.indices.data(), primitive.indices.size() * sizeof(u32),
                                                        IndexFormat::UInt32);
                draw.index_count = static_cast<u32>(primitive.indices.size());
                if (primitive.material_index >= 0 && static_cast<usize>(primitive.material_index) < out.scene.materials.size()) {
                    const auto& material = out.scene.materials[static_cast<usize>(primitive.material_index)];
                    std::copy(std::begin(material.base_color), std::end(material.base_color), draw.color);
                    const usize material_index = static_cast<usize>(primitive.material_index);
                    if (material_index < material_textures.size()) {
                        draw.texture = material_textures[material_index];
                        draw.has_texture = material_has_textures[material_index];
                    }
                } else {
                    draw.texture = white_texture;
                }
                destination_mesh.push_back(draw);
            }
        }
        out.valid = true;
        AETHER_LOG_INFO("PlayerRenderer", "Loaded '%s' (%zu mesh(es), %zu node instance(s))", path.c_str(),
                        out.scene.meshes.size(), out.scene.node_instances.size());
        return true;
    }

    Model* FindModel(const std::string& path) {
        auto [it, inserted] = models.try_emplace(path);
        if (inserted) LoadModel(path, it->second);
        return it->second.valid ? &it->second : nullptr;
    }
};

SceneRenderer::SceneRenderer(IDevice& device, ISwapChain& swap_chain, const GamePackage& package)
    : impl_(std::make_unique<Impl>(device, swap_chain, package)) {}
SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::Draw(Game& game, ICommandList& commands) {
    Impl& renderer = *impl_;
    World& world = game.GetWorld();
    GuidIndex& guids = game.Guids();

    // Resolve model assets before the render pass: texture uploads submit
    // their own short GPU command list and must not interrupt a frame pass.
    world.ForEach<ModelRenderer>([&](const ModelRenderer& model) {
        if (model.asset_path[0] != '\0') renderer.FindModel(model.asset_path);
    });

    commands.BeginRenderPass(renderer.swap_chain, {0.015f, 0.025f, 0.045f, 1.0f});
    const Entity camera_entity = FindActiveCamera(world, guids);
    const Camera* camera = world.IsAlive(camera_entity) ? world.GetComponent<Camera>(camera_entity) : nullptr;
    if (camera) {
        const f32 aspect = static_cast<f32>(renderer.swap_chain.Width()) /
                           static_cast<f32>(std::max(renderer.swap_chain.Height(), 1u));
        const Mat4 view_projection = CameraProjection(*camera, aspect) * CameraView(world, guids, camera_entity);
        commands.BindPipeline(renderer.pipeline);
        commands.BindBindlessTextures();
        world.ForEachArchetype([&](const Archetype& archetype) {
            if (!archetype.Mask().test(GetComponentId<Transform>()) || !archetype.Mask().test(GetComponentId<ModelRenderer>())) return;
            for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
                const Entity* entities = archetype.EntityArray(chunk);
                const auto* transforms = static_cast<const Transform*>(archetype.ComponentArray(chunk, GetComponentId<Transform>()));
                const auto* models = static_cast<const ModelRenderer*>(archetype.ComponentArray(chunk, GetComponentId<ModelRenderer>()));
                const usize count = archetype.ChunkEntityCount(chunk);
                for (usize row = 0; row < count; ++row) {
                    const Impl::Model* model = renderer.FindModel(models[row].asset_path);
                    if (!model) continue;
                    const Mat4 entity_transform = ComputeWorldTransform(world, guids, entities[row]);
                    (void)transforms; // ComputeWorldTransform also includes any parent chain.
                    for (const assets::GltfNodeInstance& node : model->scene.node_instances) {
                        if (node.mesh_index >= model->meshes.size()) continue;
                        const Mat4 mvp = view_projection * entity_transform * node.world_transform;
                        for (const Impl::Primitive& primitive : model->meshes[node.mesh_index]) {
                            DrawConstants constants;
                            constants.mvp = mvp;
                            std::copy(std::begin(primitive.color), std::end(primitive.color), constants.color);
                            constants.texture_index = primitive.texture.IsValid() ? primitive.texture.index : renderer.white_texture.index;
                            constants.use_texture = primitive.has_texture ? 1u : 0u;
                            commands.BindVertexBuffer(primitive.vertices, sizeof(SceneVertex));
                            commands.BindIndexBuffer(primitive.indices, IndexFormat::UInt32);
                            commands.SetPushConstants(&constants, sizeof(constants));
                            commands.DrawIndexed(primitive.index_count);
                        }
                    }
                }
            }
        });
    }
    renderer.DrawHud(game, commands);
    commands.EndRenderPass();
}

} // namespace aether::player
