#pragma once

#include "game.h"
#include "aether/assets/image.h"
#include "aether/ui/font.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lanternwake {

enum class UiAction { None, Start, Resume, Restart, ToggleMute };
struct ViewOptions {
    float time = 0;
    float dt = kStep;
    float mouse_x = -1, mouse_y = -1;
    bool muted = false;
};

class Renderer {
public:
    Renderer();
    bool LoadAssets(const std::string& directory, std::string& error);
    void OnEvent(const Event& event);
    void Render(const Game& game, const ViewOptions& options);
    // Packed little-endian RGBA bytes: use SDL_PIXELFORMAT_ABGR8888.
    [[nodiscard]] const std::vector<std::uint32_t>& Pixels() const { return pixels_; }
    [[nodiscard]] UiAction HitTest(Phase phase, float x, float y) const;
    bool SaveScreenshot(const std::string& path, std::string& error) const;

private:
    struct Sprite {
        int width = 0, height = 0;
        std::vector<std::uint32_t> pixels;
    };
    struct Particle {
        float x, y, vx, vy, life, duration, radius;
        std::uint32_t color;
    };
    std::vector<std::uint32_t> pixels_, background_;
    Sprite keeper_, wraith_, shrine_;
    std::unique_ptr<aether::ui::SdfFont> body_, title_;
    std::vector<Particle> particles_;
    float flash_ = 0, toast_time_ = 0;
    std::string toast_;
    std::uint32_t random_ = 2199;

    float Random();
    void Blend(int x, int y, std::uint32_t color, float alpha = 1);
    void Rect(float x, float y, float w, float h, std::uint32_t color, float alpha = 1);
    void Rounded(float x, float y, float w, float h, float radius, std::uint32_t color, float alpha = 1);
    void Line(float x1, float y1, float x2, float y2, float thickness, std::uint32_t color, float alpha = 1);
    void Disc(float x, float y, float radius, std::uint32_t color, float alpha = 1);
    void Glow(float x, float y, float radius, std::uint32_t color, float strength);
    void Ring(float x, float y, float radius, float thickness, std::uint32_t color, float alpha = 1, float fraction = 1);
    void Diamond(float x, float y, float radius, std::uint32_t color, float alpha = 1);
    void DrawSprite(const Sprite& sprite, float x, float y, float alpha = 1, bool flip = false, float sx = 1, float sy = 1);
    void Text(const std::string& text, float x, float y, float size, std::uint32_t color, bool title = false,
              aether::ui::TextAlign align = aether::ui::TextAlign::Left, float width = 1000, float alpha = 1);
    void TrackingText(const std::string& text, float x, float y, float size, float spacing, std::uint32_t color);
    void Key(const std::string& label, float x, float y, float width = 28);
    void Button(const std::string& text, float x, float y, float w, float h, const ViewOptions& options, bool primary = true);
    void DrawWorld(const Game& game, const ViewOptions& options);
    void DrawHud(const Game& game, const ViewOptions& options);
    void DrawMenu(const Game& game, const ViewOptions& options);
};

} // namespace lanternwake
