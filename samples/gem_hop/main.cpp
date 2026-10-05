// Gem Hop -- the Aether 3D sample game (Windows).
//
// A third-person platformer. Gameplay is in gem_hop.cpp (ECS-based,
// headless-testable); this file is the window, keyboard polling, the
// fixed-timestep loop, an orbit camera, and a small software 3D rasteriser
// (near-plane clipping, z-buffer, flat lighting, blob shadow) blitted with
// GDI -- so it runs on any machine, GPU or not.
//
//   W/A/S/D     move (relative to the camera)    Space  jump (hold = higher)
//   Mouse       look around (cursor is captured while focused)
//   Left/Right/Up/Down  also turn the camera
//   R           restart                          Esc    quit
//
// Set AETHER_GEMHOP_MAX_FRAMES=<N> to auto-close after N frames;
// AETHER_GEMHOP_SCREENSHOT=<file.bmp> saves the last frame on exit.

#include "gem_hop.h"

#include "aether/core/log.h"
#include "aether/platform/window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace aether;
using namespace aether::gem_hop;

namespace {

constexpr i32 kW = 960;
constexpr i32 kH = 540;

u32 Rgb(f32 r, f32 g, f32 b) {
    auto c = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    return (c(r) << 16) | (c(g) << 8) | c(b);
}

struct Color {
    f32 r, g, b;
};

struct Camera {
    Vec3 pos;
    Vec3 right, up, fwd;
    f32 focal = 0;

    void LookAt(const Vec3& eye, const Vec3& target) {
        pos = eye;
        fwd = (target - eye).Normalized();
        right = fwd.Cross({0, 1, 0}).Normalized();
        up = right.Cross(fwd);
        focal = (kH * 0.5f) / std::tan(0.5f * 1.0f); // ~57 degree vertical FOV
    }
    Vec3 ToView(const Vec3& w) const {
        Vec3 d = w - pos;
        return {d.Dot(right), d.Dot(up), d.Dot(fwd)};
    }
};

class Renderer {
public:
    Renderer() : color_(static_cast<usize>(kW) * kH), depth_(static_cast<usize>(kW) * kH) {}

    const std::vector<u32>& Pixels() const { return color_; }

    void Clear() {
        for (i32 y = 0; y < kH; ++y) {
            f32 t = static_cast<f32>(y) / kH;
            u32 sky = Rgb(0.35f + 0.35f * t, 0.55f + 0.30f * t, 0.95f - 0.05f * t);
            std::fill_n(color_.begin() + static_cast<usize>(y) * kW, kW, sky);
        }
        std::fill(depth_.begin(), depth_.end(), 0.0f); // storing 1/z: 0 = infinitely far
    }

    void BeginFrame(const Camera& cam) { cam_ = cam; }

    // Flat-shaded triangle in world space; `n` is its (unit) normal.
    void Tri(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& n, Color col) {
        static const Vec3 light = Vec3{0.45f, 0.8f, 0.35f}.Normalized();
        f32 shade = 0.35f + 0.65f * std::max(0.0f, n.Dot(light));
        u32 rgb = Rgb(col.r * shade, col.g * shade, col.b * shade);

        // Clip against the near plane in view space.
        constexpr f32 kNear = 0.1f;
        Vec3 in[3] = {cam_.ToView(a), cam_.ToView(b), cam_.ToView(c)};
        Vec3 poly[4];
        int count = 0;
        for (int i = 0; i < 3; ++i) {
            const Vec3& p = in[i];
            const Vec3& q = in[(i + 1) % 3];
            bool pin = p.z >= kNear;
            bool qin = q.z >= kNear;
            if (pin) {
                poly[count++] = p;
            }
            if (pin != qin) {
                f32 t = (kNear - p.z) / (q.z - p.z);
                poly[count++] = p + (q - p) * t;
            }
        }
        for (int i = 1; i + 1 < count; ++i) {
            Raster(poly[0], poly[i], poly[i + 1], rgb);
        }
    }

    void Quad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, Color col) {
        Vec3 n = (b - a).Cross(d - a).Normalized();
        Quad(a, b, c, d, n, col);
    }
    void Quad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const Vec3& n, Color col) {
        Tri(a, b, c, n, col);
        Tri(a, c, d, n, col);
    }

    // Axis-aligned box; `top_col` for the +y face, `side_col` for the rest.
    void Box(const Vec3& lo, const Vec3& hi, Color top_col, Color side_col) {
        Vec3 p[8];
        for (int i = 0; i < 8; ++i) {
            p[i] = {(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
        }
        Quad(p[2], p[6], p[7], p[3], Vec3{0, 1, 0}, top_col);  // +y
        Quad(p[0], p[1], p[5], p[4], Vec3{0, -1, 0}, side_col); // -y
        Quad(p[1], p[3], p[7], p[5], Vec3{1, 0, 0}, side_col); // +x
        Quad(p[0], p[4], p[6], p[2], Vec3{-1, 0, 0}, side_col); // -x
        Quad(p[4], p[5], p[7], p[6], Vec3{0, 0, 1}, side_col); // +z
        Quad(p[0], p[2], p[3], p[1], Vec3{0, 0, -1}, side_col); // -z
    }

    // Spinning octahedron.
    void Gem(const Vec3& c, f32 r, f32 spin, Color col) {
        f32 s = std::sin(spin), co = std::cos(spin);
        auto rot = [&](f32 x, f32 y, f32 z) { return Vec3{c.x + x * co - z * s, c.y + y, c.z + x * s + z * co}; };
        Vec3 t = rot(0, r * 1.3f, 0), bt = rot(0, -r * 1.3f, 0);
        Vec3 e[4] = {rot(r, 0, 0), rot(0, 0, r), rot(-r, 0, 0), rot(0, 0, -r)};
        for (int i = 0; i < 4; ++i) {
            const Vec3& a = e[i];
            const Vec3& b = e[(i + 1) % 4];
            Tri(t, a, b, (a - t).Cross(b - t).Normalized(), col);
            Tri(bt, b, a, (b - bt).Cross(a - bt).Normalized(), col);
        }
    }

private:
    void Raster(const Vec3& a, const Vec3& b, const Vec3& c, u32 rgb) {
        auto proj = [&](const Vec3& v) {
            return Vec3{kW * 0.5f + cam_.focal * v.x / v.z, kH * 0.5f - cam_.focal * v.y / v.z, v.z};
        };
        Vec3 pa = proj(a), pb = proj(b), pc = proj(c);
        f32 area = (pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x);
        if (std::fabs(area) < 1e-6f) {
            return;
        }
        i32 x0 = std::max(0, static_cast<i32>(std::floor(std::min({pa.x, pb.x, pc.x}))));
        i32 x1 = std::min(kW - 1, static_cast<i32>(std::ceil(std::max({pa.x, pb.x, pc.x}))));
        i32 y0 = std::max(0, static_cast<i32>(std::floor(std::min({pa.y, pb.y, pc.y}))));
        i32 y1 = std::min(kH - 1, static_cast<i32>(std::ceil(std::max({pa.y, pb.y, pc.y}))));
        f32 ia = 1.0f / pa.z, ib = 1.0f / pb.z, ic = 1.0f / pc.z;
        f32 inv_area = 1.0f / area;
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                f32 px = x + 0.5f, py = y + 0.5f;
                f32 w0 = ((pb.x - px) * (pc.y - py) - (pb.y - py) * (pc.x - px)) * inv_area;
                f32 w1 = ((pc.x - px) * (pa.y - py) - (pc.y - py) * (pa.x - px)) * inv_area;
                f32 w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) {
                    continue;
                }
                f32 inv_z = w0 * ia + w1 * ib + w2 * ic;
                usize idx = static_cast<usize>(y) * kW + x;
                if (inv_z > depth_[idx]) {
                    depth_[idx] = inv_z;
                    color_[idx] = rgb;
                }
            }
        }
    }

    Camera cam_;
    std::vector<u32> color_;
    std::vector<f32> depth_;
};

void FillRect(std::vector<u32>& px, i32 x, i32 y, i32 w, i32 h, u32 c) {
    for (i32 yy = std::max(0, y); yy < std::min(kH, y + h); ++yy) {
        for (i32 xx = std::max(0, x); xx < std::min(kW, x + w); ++xx) {
            px[static_cast<usize>(yy) * kW + xx] = c;
        }
    }
}

void DrawScene(const Game& game, Renderer& r, f32 yaw, f32 pitch, f32 anim) {
    const Vec3 pp = game.PlayerPos();
    Vec3 target{pp.x, pp.y + 0.8f, pp.z};
    f32 dist = 9.0f;
    Vec3 eye = target + Vec3{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)} * dist;
    Camera cam;
    cam.LookAt(eye, target);

    r.Clear();
    r.BeginFrame(cam);

    // Platforms; the goal pad (last) glows once it's live.
    const auto& plats = game.Platforms();
    for (usize i = 0; i < plats.size(); ++i) {
        const Platform& p = plats[i];
        Color top{0.36f, 0.72f, 0.30f}, side{0.45f, 0.32f, 0.22f};
        if (i + 1 == plats.size()) {
            top = game.GoalActive() ? Color{1.0f, 0.85f, 0.2f} : Color{0.55f, 0.55f, 0.6f};
            side = {0.4f, 0.4f, 0.45f};
        }
        r.Box({p.MinX(), p.Bottom(), p.MinZ()}, {p.MaxX(), p.top, p.MaxZ()}, top, side);
    }

    // Blob shadow on the platform below the player.
    f32 best = -1e9f;
    for (const Platform& p : plats) {
        if (pp.x > p.MinX() && pp.x < p.MaxX() && pp.z > p.MinZ() && pp.z < p.MaxZ() && p.top <= pp.y + 0.05f) {
            best = std::max(best, p.top);
        }
    }
    if (best > -1e8f) {
        f32 h = 0.35f, y = best + 0.02f;
        r.Quad({pp.x - h, y, pp.z - h}, {pp.x - h, y, pp.z + h}, {pp.x + h, y, pp.z + h}, {pp.x + h, y, pp.z - h},
               {0.1f, 0.2f, 0.1f});
    }

    // Gems.
    game.GetWorld().ForEach<Transform, Gem>([&](Transform& t, Gem&) {
        r.Gem({t.pos.x, t.pos.y + std::sin(anim * 2.0f + t.pos.x) * 0.1f, t.pos.z}, 0.3f, anim * 2.5f, {0.3f, 0.95f, 1.0f});
    });

    // Player: a body box with a lighter "head" so facing/size reads.
    const Body& b = game.PlayerBody();
    r.Box({pp.x - b.half_w, pp.y, pp.z - b.half_w}, {pp.x + b.half_w, pp.y + b.height * 0.75f, pp.z + b.half_w},
          {0.95f, 0.3f, 0.3f}, {0.85f, 0.2f, 0.2f});
    r.Box({pp.x - 0.22f, pp.y + b.height * 0.75f, pp.z - 0.22f}, {pp.x + 0.22f, pp.y + b.height + 0.1f, pp.z + 0.22f},
          {1.0f, 0.85f, 0.7f}, {0.95f, 0.75f, 0.6f});
}

void DrawHud(const Game& game, std::vector<u32>& px) {
    FillRect(px, 0, 0, kW, 34, Rgb(0.1f, 0.1f, 0.15f));
    for (i32 i = 0; i < game.GemsTotal(); ++i) {
        u32 col = i < game.GemsCollected() ? Rgb(0.3f, 0.95f, 1.0f) : Rgb(0.3f, 0.3f, 0.36f);
        i32 cx = 20 + i * 26;
        for (i32 d = -8; d <= 8; ++d) { // diamond
            i32 span = 9 - std::abs(d);
            FillRect(px, cx - span, 17 + d, span * 2, 1, col);
        }
    }
    for (i32 i = 0; i < std::min(game.Deaths(), 20); ++i) {
        FillRect(px, kW - 20 - i * 14, 10, 10, 14, Rgb(0.85f, 0.25f, 0.25f));
    }
    if (game.GetState() == State::Won) {
        FillRect(px, 0, kH / 2 - 30, kW, 60, Rgb(1.0f, 0.85f, 0.2f));
        FillRect(px, 0, kH / 2 - 24, kW, 48, Rgb(0.1f, 0.1f, 0.15f));
        for (i32 i = 0; i < 20; ++i) { // row of gems as the "you win" banner
            FillRect(px, kW / 2 - 200 + i * 20, kH / 2 - 6, 12, 12, i % 2 ? Rgb(1, 0.85f, 0.2f) : Rgb(0.3f, 0.95f, 1));
        }
    }
}

void SaveBmp(const char* path, const std::vector<u32>& px) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = kW;
    ih.biHeight = -kH;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(px.size() * 4);
    std::FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f) {
        std::fwrite(&fh, sizeof(fh), 1, f);
        std::fwrite(&ih, sizeof(ih), 1, f);
        std::fwrite(px.data(), 4, px.size(), f);
        std::fclose(f);
    }
}

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

} // namespace

int main() {
    Game game;

    WindowDesc desc;
    desc.title = "Gem Hop";
    desc.width = kW;
    desc.height = kH;
    Window window(desc);
    HWND hwnd = static_cast<HWND>(window.NativeHandle());

    i64 max_frames = -1;
    if (const char* env = std::getenv("AETHER_GEMHOP_MAX_FRAMES")) {
        max_frames = std::atoll(env);
    }

    Renderer renderer;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kW;
    bmi.bmiHeader.biHeight = -kH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    f32 accumulator = 0, anim = 0, title_timer = 0;
    f32 yaw = 3.14159f; // start looking down +z, the direction of the route
    f32 pitch = 0.5f;
    bool prev_restart = false;
    bool mouse_ready = false;
    bool cursor_hidden = false;
    i64 frame = 0;

    AETHER_LOG_INFO("GemHop", "Collect all %d gems, then stand on the goal pad.", game.GemsTotal());

    while (window.PumpMessages()) {
        if (window.IsMinimized()) {
            Sleep(16);
            continue;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        f32 dt = std::min(0.1f, static_cast<f32>(now.QuadPart - last.QuadPart) / static_cast<f32>(freq.QuadPart));
        last = now;
        accumulator += dt;
        anim += dt;

        Input in;
        if (GetForegroundWindow() == hwnd) {
            if (Down(VK_ESCAPE)) {
                break;
            }
            if (Down(VK_LEFT)) yaw += 2.0f * dt;
            if (Down(VK_RIGHT)) yaw -= 2.0f * dt;
            if (Down(VK_UP)) pitch -= 1.0f * dt; // look up = camera lower
            if (Down(VK_DOWN)) pitch += 1.0f * dt;

            // Mouse look: measure how far the cursor moved from the window
            // centre, then put it back (so it never hits a screen edge).
            RECT rc;
            GetClientRect(hwnd, &rc);
            POINT centre{rc.right / 2, rc.bottom / 2};
            ClientToScreen(hwnd, &centre);
            POINT cur;
            GetCursorPos(&cur);
            if (mouse_ready) {
                yaw -= (cur.x - centre.x) * 0.003f;
                pitch += (cur.y - centre.y) * 0.003f;
            }
            SetCursorPos(centre.x, centre.y);
            mouse_ready = true;
            if (!cursor_hidden) {
                ShowCursor(FALSE);
                cursor_hidden = true;
            }
            pitch = std::clamp(pitch, 0.05f, 1.3f);

            f32 fwd = (Down('W') ? 1.0f : 0.0f) - (Down('S') ? 1.0f : 0.0f);
            f32 side = (Down('D') ? 1.0f : 0.0f) - (Down('A') ? 1.0f : 0.0f);
            // The camera looks along -(sin yaw, cos yaw); its right is (-fz, fx).
            f32 fx = -std::sin(yaw), fz = -std::cos(yaw);
            in.move_x = fx * fwd - fz * side;
            in.move_z = fz * fwd + fx * side;
            in.jump = Down(VK_SPACE);

            bool restart = Down('R');
            if (restart && !prev_restart) {
                game.Restart();
            }
            prev_restart = restart;
        }
        if (GetForegroundWindow() != hwnd) {
            mouse_ready = false; // re-centre on focus without a jump
            if (cursor_hidden) {
                ShowCursor(TRUE);
                cursor_hidden = false;
            }
        }
        while (accumulator >= Game::kStep) {
            game.Step(in);
            accumulator -= Game::kStep;
        }

        DrawScene(game, renderer, yaw, pitch, anim);
        std::vector<u32> frame_px = renderer.Pixels();
        DrawHud(game, frame_px);

        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC dc = GetDC(hwnd);
        StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, kW, kH, frame_px.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd, dc);

        title_timer -= dt;
        if (title_timer <= 0) {
            title_timer = 0.2f;
            char title[160];
            if (game.GetState() == State::Won) {
                std::snprintf(title, sizeof(title), "Gem Hop - YOU WIN in %.1fs (%d deaths) - R to play again",
                              game.Time(), game.Deaths());
            } else {
                std::snprintf(title, sizeof(title), "Gem Hop - gems %d/%d%s - %.1fs - deaths %d", game.GemsCollected(),
                              game.GemsTotal(), game.GoalActive() ? " - GOAL LIVE" : "", game.Time(), game.Deaths());
            }
            SetWindowTextA(hwnd, title);
        }

        if (max_frames >= 0 && ++frame >= max_frames) {
            if (const char* shot = std::getenv("AETHER_GEMHOP_SCREENSHOT")) {
                std::vector<u32> px = renderer.Pixels();
                DrawHud(game, px);
                SaveBmp(shot, px);
            }
            break;
        }
        Sleep(1);
    }
    return 0;
}
