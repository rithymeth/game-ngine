#pragma once

#include "aether/math/mat4.h"
#include "aether/ui/widget_system.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aether {

class GuidIndex;

enum class WorldWidgetSpace : u8 {
    Screen, // projected: drawn flat on the screen over the entity, always upright and pixel-sharp
    World,  // a quad in the scene, lit by nothing, hidden by what's in front
};

// A UI layout shown at an entity's place in the world (Phase 18 step 5,
// §18.5): a health bar over an enemy, a name tag, a prompt on a door, a
// screen on a console. Bindings read the entity's own components, as with
// WidgetComponent, and the UI Blueprint library works on it.
struct WorldWidgetComponent {
    std::string layout;
    WorldWidgetSpace space = WorldWidgetSpace::Screen;
    Vec3 offset{0.0f, 2.0f, 0.0f};       // from the entity's position, in world units (not turned with it)
    f32 width = 200.0f, height = 40.0f;  // the widget's size in layout units
    f32 pivot_x = 0.5f, pivot_y = 1.0f;  // the point of the widget (0..1) at the anchor: bottom centre
    f32 max_distance = 0.0f;             // hidden farther from the camera (0: never)
    f32 reference_distance = 0.0f;       // Screen: full size up to here, smaller beyond (0: always full size)
    f32 min_scale = 0.25f;               // Screen: the smallest it shrinks to
    f32 world_width = 1.0f;              // World: the quad's width in world units (height by aspect)
    bool billboard = true;               // World: turn to face the camera (else the entity's facing)
    bool interactive = false;            // takes pointer input
    bool visible = true;
};

} // namespace aether

namespace aether::ui {

// The camera world widgets are placed for. The projection's clip space has
// y up and depth 0..1 (Mat4::PerspectiveRH, OrthographicRH); pixels have y
// down.
struct WorldCamera {
    Mat4 view, projection;
    Vec2 screen{1920.0f, 1080.0f}; // pixels
    f32 ui_scale = 1.0f;           // pixels per layout unit for Screen widgets (the HUD viewport's Scale())
};

// Runs a world's WorldWidgetComponents: loads and themes each layout in a
// viewport of its own, keeps bindings and animations going, places it for
// the camera each frame, draws it, and routes pointers to interactive ones.
class WorldUISystem {
public:
    WorldUISystem(World& world, const Font& font, UISystem::LayoutLoader loader, const Theme* theme = nullptr, const FontLibrary* fonts = nullptr,
                  const GuidIndex* guids = nullptr); // guids: place children by their world transform (else Transform alone)
    ~WorldUISystem();
    WorldUISystem(const WorldUISystem&) = delete;
    WorldUISystem& operator=(const WorldUISystem&) = delete;

    void Update(const WorldCamera& camera, f32 dt);

    // Where a widget went this frame.
    struct Placement {
        bool shown = false; // false: hidden, behind the camera, too far or off screen
        f32 distance = 0.0f; // camera to anchor
        f32 depth = 0.0f;    // the anchor's clip-space depth (0..1), for depth testing Screen widgets
        // Screen: in pixels.
        Vec2 anchor;
        f32 scale = 1.0f; // pixels per layout unit
        Rect rect;
        // World: layout units (x right, y down) to world; the quad's corners
        // (top-left, top-right, bottom-right, bottom-left).
        Mat4 transform;
        Vec3 corners[4];
    };
    const Placement* PlacementOf(Entity entity) const;
    usize ShownCount() const;

    // Screen widgets, in pixels, far ones first. Clipped to their boxes.
    DrawList PaintScreen() const;
    // World widgets, far ones first: each draw list in its layout units, for
    // the renderer to draw through `transform` (depth-tested) or into a
    // texture on the quad.
    struct WorldDraw {
        Entity entity;
        Mat4 transform;
        Vec2 size;
        f32 distance = 0.0f;
        DrawList list;
    };
    std::vector<WorldDraw> PaintWorld() const;

    // The nearest shown widget with something hittable under a pixel.
    struct Hit {
        Entity entity = kNullEntity;
        Widget* widget = nullptr;
        Vec2 local; // in the widget's layout units
        f32 distance = 0.0f;
    };
    Hit HitTest(Vec2 pixel) const;
    // Pointer input for interactive widgets, like UIInputRouter's: true when
    // a widget used it (so gameplay can skip it). A press stays with its
    // widget until released.
    bool PointerMove(Vec2 pixel);
    bool PointerDown(Vec2 pixel, int button = 0);
    bool PointerUp(Vec2 pixel, int button = 0);

    Widget* RootOf(Entity entity) const;
    Widget* FindWidget(Entity entity, const std::string& widget) const;
    UIAnimator* AnimatorOf(Entity entity) const;
    DataBinder* BinderOf(Entity entity) const;
    UIInputRouter* RouterOf(Entity entity) const;
    void AddGlobalSource(const std::string& name, void* object, const reflect::TypeInfo& type);

    // Control events (for Blueprints, with UISystem::BlueprintEventName) and finished animations.
    const std::vector<WidgetEvent>& Events() const { return events_; }
    const std::vector<std::string>& Problems() const { return problems_; }

    static WorldUISystem* Active();
    void MakeActive();

private:
    struct Instance {
        Entity entity;
        std::string layout;
        WorldWidgetComponent settings;
        Viewport viewport;
        std::unique_ptr<UIInputRouter> router;
        Widget* root = nullptr; // owned by the viewport
        std::unique_ptr<UIAnimator> animator;
        std::unique_ptr<DataBinder> binder;
        std::vector<Binding> bindings;
        std::vector<std::string> sources;
        Placement placement;
        u64 seen = 0;
    };
    bool Create(Instance& in);
    void Place(Instance& in, const WorldCamera& camera);
    bool ToLocal(const Instance& in, Vec2 pixel, Vec2& local, f32& distance) const;
    Instance* Find(Entity e);
    const Instance* Find(Entity e) const;
    Instance* InteractiveAt(Vec2 pixel, Vec2& local);
    void Report(const std::string& key, const std::string& message);

    World& world_;
    const Font& font_;
    UISystem::LayoutLoader loader_;
    const Theme* theme_;
    const FontLibrary* fonts_;
    const GuidIndex* guids_;
    std::map<u32, std::unique_ptr<Instance>> instances_;
    detail::GlobalSources globals_;
    std::vector<WidgetEvent> events_, pending_;
    std::vector<std::string> problems_;
    std::map<std::string, bool> reported_;
    u64 generation_ = 0;
    // For pointer rays.
    Mat4 inverse_view_projection_;
    Vec3 camera_position_;
    Vec2 screen_;
    Entity hovered_ = kNullEntity, captured_ = kNullEntity;
};

void RegisterWorldWidgetComponents();

} // namespace aether::ui

AETHER_ENUM(aether::WorldWidgetSpace, 1, AETHER_ENUM_VALUE(Screen), AETHER_ENUM_VALUE(World))

AETHER_REFLECT(aether::WorldWidgetComponent, 1,
    AETHER_FIELD(layout, Field_EditAnywhere, {.tooltip = "The UI layout (.aui) to show"}),
    AETHER_FIELD(space, Field_EditAnywhere, {.tooltip = "Screen: drawn flat over the entity; World: a quad in the scene"}),
    AETHER_FIELD(offset, Field_EditAnywhere, {.tooltip = "From the entity's position", .units = "m"}),
    AETHER_FIELD(width, Field_EditAnywhere, {.range_min = 1, .range_max = 8192}),
    AETHER_FIELD(height, Field_EditAnywhere, {.range_min = 1, .range_max = 8192}),
    AETHER_FIELD(pivot_x, Field_EditAnywhere, {.tooltip = "The widget's point at the anchor (0 left, 1 right)", .range_min = 0, .range_max = 1}),
    AETHER_FIELD(pivot_y, Field_EditAnywhere, {.tooltip = "The widget's point at the anchor (0 top, 1 bottom)", .range_min = 0, .range_max = 1}),
    AETHER_FIELD(max_distance, Field_EditAnywhere, {.tooltip = "Hidden farther away (0: never)", .range_min = 0, .range_max = 100000, .units = "m"}),
    AETHER_FIELD(reference_distance, Field_EditAnywhere, {.tooltip = "Screen: full size up to here, smaller beyond (0: constant)", .range_min = 0, .range_max = 100000, .units = "m"}),
    AETHER_FIELD(min_scale, Field_EditAnywhere, {.range_min = 0.01, .range_max = 1}),
    AETHER_FIELD(world_width, Field_EditAnywhere, {.tooltip = "World: the quad's width", .range_min = 0.001, .range_max = 10000, .units = "m"}),
    AETHER_FIELD(billboard, Field_EditAnywhere, {.tooltip = "World: face the camera"}),
    AETHER_FIELD(interactive, Field_EditAnywhere, {.tooltip = "Takes pointer input"}),
    AETHER_FIELD(visible, Field_EditAnywhere)
)
