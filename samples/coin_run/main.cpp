// Coin Run -- the Aether sample game (Windows).
//
// A software-rendered 2D platformer: the game logic is in coin_run.cpp
// (ECS-based, headless-testable); this file is only the window, keyboard
// polling, the fixed-timestep loop, and a pixel-buffer renderer blitted with
// GDI -- so it runs on any machine, GPU or not.
//
//   A/D or Left/Right  move        Space/W/Up  jump (hold for a higher jump)
//   R                  restart     Esc         quit
//
// Set AETHER_COINRUN_MAX_FRAMES=<N> to auto-close after N frames, for
// scripted verification; AETHER_COINRUN_SCREENSHOT=<file.bmp> also saves the
// last rendered frame on exit.

#include "coin_run.h"

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
using namespace aether::coin_run;

namespace {

constexpr i32 kTile = 20;
constexpr i32 kHud = 40;

struct Canvas {
    i32 w = 0;
    i32 h = 0;
    std::vector<u32> px; // 0x00RRGGBB

    Canvas(i32 width, i32 height) : w(width), h(height), px(static_cast<usize>(width) * height, 0) {}

    void Rect(i32 x, i32 y, i32 rw, i32 rh, u32 color) {
        for (i32 yy = std::max(0, y); yy < std::min(h, y + rh); ++yy) {
            for (i32 xx = std::max(0, x); xx < std::min(w, x + rw); ++xx) {
                px[static_cast<usize>(yy) * w + xx] = color;
            }
        }
    }
    void Disc(i32 cx, i32 cy, i32 r, u32 color) {
        for (i32 yy = cy - r; yy <= cy + r; ++yy) {
            for (i32 xx = cx - r; xx <= cx + r; ++xx) {
                if ((xx - cx) * (xx - cx) + (yy - cy) * (yy - cy) <= r * r && xx >= 0 && yy >= 0 && xx < w && yy < h) {
                    px[static_cast<usize>(yy) * w + xx] = color;
                }
            }
        }
    }
};

void Render(const Game& game, Canvas& c, f32 anim_time) {
    // Sky gradient.
    for (i32 y = 0; y < c.h; ++y) {
        u32 shade = static_cast<u32>(150 + 80 * y / c.h);
        c.Rect(0, y, c.w, 1, (90u << 16) | (shade << 8) | 255u);
    }

    i32 oy = kHud;
    for (i32 ty = 0; ty < game.Height(); ++ty) {
        for (i32 tx = 0; tx < game.Width(); ++tx) {
            i32 x = tx * kTile;
            i32 y = oy + ty * kTile;
            switch (game.TileAt(tx, ty)) {
            case Tile::Solid:
                c.Rect(x, y, kTile, kTile, 0x3b2f2f);
                c.Rect(x, y, kTile, 3, 0x5fa83c); // grassy top edge
                break;
            case Tile::Door:
                if (!game.DoorOpen()) {
                    c.Rect(x + 2, y, kTile - 4, kTile, 0x8a5a2b);
                    c.Rect(x + 12, y + 8, 3, 3, 0xffd84a); // knob
                } else {
                    c.Rect(x + 8, y, 4, kTile, 0x8a5a2b); // swung open: just the edge
                }
                break;
            case Tile::Spike:
                for (i32 s = 0; s < 2; ++s) {
                    for (i32 row = 0; row < kTile / 2; ++row) {
                        i32 half = row / 2 + 1;
                        c.Rect(x + s * 10 + 5 - half, y + kTile / 2 + row, half * 2, 1, 0xc8c8d4);
                    }
                }
                break;
            case Tile::Goal: {
                f32 bob = std::sin(anim_time * 4.0f) * 2.0f;
                c.Rect(x + 8, y + 2 + static_cast<i32>(bob), 3, kTile - 2, 0xeeeeee); // pole
                c.Rect(x + 11, y + 3 + static_cast<i32>(bob), 7, 6, 0xe84a4a);        // flag
                break;
            }
            default: break;
            }
        }
    }

    // Coins (spinning: width follows |cos|).
    game.GetWorld().ForEach<Position, Coin>([&](Position& p, Coin&) {
        i32 cx = static_cast<i32>(p.x * kTile);
        i32 cy = oy + static_cast<i32>(p.y * kTile);
        i32 hw = std::max(1, static_cast<i32>(std::fabs(std::cos(anim_time * 3.0f + p.x)) * 7.0f));
        for (i32 dy = -7; dy <= 7; ++dy) {
            i32 span = static_cast<i32>(hw * std::sqrt(std::max(0.0f, 1.0f - (dy * dy) / 49.0f)));
            c.Rect(cx - span, cy + dy, span * 2 + 1, 1, 0xffc31f);
        }
    });

    // Player.
    const Position& pp = game.PlayerPos();
    const Body& pb = game.PlayerBody();
    i32 px = static_cast<i32>((pp.x - pb.w * 0.5f) * kTile);
    i32 py = oy + static_cast<i32>((pp.y - pb.h) * kTile);
    i32 pw = static_cast<i32>(pb.w * kTile);
    i32 ph = static_cast<i32>(pb.h * kTile);
    c.Rect(px, py, pw, ph, 0x2a6df4);
    c.Rect(pb.vx < 0 ? px + 2 : px + pw - 6, py + 4, 4, 4, 0xffffff); // eye, on the side being walked towards

    // HUD: one pip per coin, red marks per death, a bar for elapsed time.
    c.Rect(0, 0, c.w, kHud, 0x1d1d28);
    for (i32 i = 0; i < game.CoinsTotal(); ++i) {
        u32 col = i < game.CoinsCollected() ? 0xffc31f : 0x4a4a58;
        c.Disc(20 + i * 24, 20, 8, col);
    }
    for (i32 i = 0; i < std::min(game.Deaths(), 20); ++i) {
        c.Rect(c.w - 20 - i * 14, 14, 10, 12, 0xd23a3a);
    }
    if (game.GetState() == State::Won) {
        c.Rect(0, oy + 120, c.w, 80, 0x00000000);
        for (i32 i = 0; i < 12; ++i) { // celebratory confetti row
            c.Disc(c.w / 2 - 110 + i * 20, oy + 160 + static_cast<i32>(std::sin(anim_time * 6 + i) * 20), 6,
                   (i % 3 == 0) ? 0xffc31f : (i % 3 == 1) ? 0xe84a4a : 0x4ae8a0);
        }
    }
}

void SaveBmp(const char* path, const Canvas& c) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = c.w;
    ih.biHeight = -c.h; // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(c.px.size() * 4);
    std::FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f) {
        std::fwrite(&fh, sizeof(fh), 1, f);
        std::fwrite(&ih, sizeof(ih), 1, f);
        std::fwrite(c.px.data(), 4, c.px.size(), f);
        std::fclose(f);
    }
}

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

} // namespace

int main() {
    Game game;
    const i32 cw = game.Width() * kTile;
    const i32 ch = game.Height() * kTile + kHud;

    WindowDesc desc;
    desc.title = "Coin Run";
    desc.width = static_cast<u32>(cw);
    desc.height = static_cast<u32>(ch);
    Window window(desc);
    HWND hwnd = static_cast<HWND>(window.NativeHandle());

    i64 max_frames = -1;
    if (const char* env = std::getenv("AETHER_COINRUN_MAX_FRAMES")) {
        max_frames = std::atoll(env);
    }

    Canvas canvas(cw, ch);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cw;
    bmi.bmiHeader.biHeight = -ch; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    f32 accumulator = 0;
    f32 anim_time = 0;
    f32 title_timer = 0;
    bool prev_restart = false;
    i64 frame = 0;

    AETHER_LOG_INFO("CoinRun", "Collect all %d coins, open the door, reach the flag.", game.CoinsTotal());

    while (window.PumpMessages()) {
        if (window.IsMinimized()) {
            Sleep(16);
            continue;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        f32 dt = static_cast<f32>(now.QuadPart - last.QuadPart) / static_cast<f32>(freq.QuadPart);
        last = now;
        dt = std::min(dt, 0.1f); // don't spiral after a stall
        accumulator += dt;
        anim_time += dt;

        const bool focused = GetForegroundWindow() == hwnd;
        Input in;
        if (focused) {
            in.left = Down('A') || Down(VK_LEFT);
            in.right = Down('D') || Down(VK_RIGHT);
            in.jump = Down(VK_SPACE) || Down('W') || Down(VK_UP);
            if (Down(VK_ESCAPE)) {
                break;
            }
            bool restart = Down('R');
            if (restart && !prev_restart) {
                game.Restart();
            }
            prev_restart = restart;
        }

        while (accumulator >= Game::kStep) {
            game.Step(in);
            accumulator -= Game::kStep;
        }

        Render(game, canvas, anim_time);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC dc = GetDC(hwnd);
        StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, cw, ch, canvas.px.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd, dc);

        title_timer -= dt;
        if (title_timer <= 0) {
            title_timer = 0.2f;
            char title[160];
            if (game.GetState() == State::Won) {
                std::snprintf(title, sizeof(title), "Coin Run - YOU WIN in %.1fs (%d deaths) - R to play again",
                              game.Time(), game.Deaths());
            } else {
                std::snprintf(title, sizeof(title), "Coin Run - coins %d/%d%s - %.1fs - deaths %d", game.CoinsCollected(),
                              game.CoinsTotal(), game.DoorOpen() ? " - DOOR OPEN" : "", game.Time(), game.Deaths());
            }
            SetWindowTextA(hwnd, title);
        }

        if (max_frames >= 0 && ++frame >= max_frames) {
            break;
        }
        Sleep(1);
    }
    if (const char* shot = std::getenv("AETHER_COINRUN_SCREENSHOT")) {
        SaveBmp(shot, canvas);
    }
    return 0;
}
