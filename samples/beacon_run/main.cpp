// Beacon Run -- the Aether top-down arena sample (Windows).

#include "beacon_run.h"

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
using namespace aether::beacon_run;

namespace {

constexpr i32 kScale = 32;
constexpr i32 kHud = 56;
constexpr i32 kWidth = static_cast<i32>(Game::kWidth) * kScale;
constexpr i32 kHeight = static_cast<i32>(Game::kHeight) * kScale + kHud;

struct Canvas {
    std::vector<u32> pixels = std::vector<u32>(static_cast<usize>(kWidth) * kHeight);

    void Rect(i32 x, i32 y, i32 w, i32 h, u32 color) {
        for (i32 py = std::max(0, y); py < std::min(kHeight, y + h); ++py) {
            for (i32 px = std::max(0, x); px < std::min(kWidth, x + w); ++px) {
                pixels[static_cast<usize>(py) * kWidth + px] = color;
            }
        }
    }

    void Disc(i32 cx, i32 cy, i32 radius, u32 color) {
        for (i32 y = -radius; y <= radius; ++y) {
            for (i32 x = -radius; x <= radius; ++x) {
                if (x * x + y * y <= radius * radius && cx + x >= 0 && cx + x < kWidth && cy + y >= 0 &&
                    cy + y < kHeight) {
                    pixels[static_cast<usize>(cy + y) * kWidth + cx + x] = color;
                }
            }
        }
    }
};

void Render(const Game& game, Canvas& canvas, f32 elapsed) {
    canvas.Rect(0, 0, kWidth, kHeight, 0x101725);
    canvas.Rect(0, kHud, kWidth, kHeight - kHud, 0x172337);
    for (i32 x = 0; x <= static_cast<i32>(Game::kWidth); ++x) {
        canvas.Rect(x * kScale, kHud, 1, kHeight - kHud, 0x1d2b40);
    }
    for (i32 y = 0; y <= static_cast<i32>(Game::kHeight); ++y) {
        canvas.Rect(0, kHud + y * kScale, kWidth, 1, 0x1d2b40);
    }

    game.GetWorld().ForEach<Position, Beacon>([&](Position& p, Beacon&) {
        const i32 x = static_cast<i32>(p.x * kScale);
        const i32 y = kHud + static_cast<i32>(p.y * kScale);
        const i32 radius = 7 + static_cast<i32>((std::sin(elapsed * 5.0f + p.x) + 1.0f) * 2.0f);
        canvas.Disc(x, y, radius + 4, 0x335cbdc8);
        canvas.Disc(x, y, radius, 0x63f0dc);
        canvas.Disc(x, y, std::max(2, radius / 3), 0xe5fff9);
    });

    game.GetWorld().ForEach<Position, Drone>([&](Position& p, Drone&) {
        const i32 x = static_cast<i32>(p.x * kScale);
        const i32 y = kHud + static_cast<i32>(p.y * kScale);
        canvas.Disc(x, y, 17, 0x5c263d);
        canvas.Disc(x, y, 12, 0xf04f67);
        canvas.Disc(x, y, 4, 0xffd6d5);
    });

    const Position& player = game.PlayerPos();
    const i32 px = static_cast<i32>(player.x * kScale);
    const i32 py = kHud + static_cast<i32>(player.y * kScale);
    const bool blinking = game.IsInvulnerable() && static_cast<i32>(elapsed * 12.0f) % 2 == 0;
    if (!blinking) {
        canvas.Disc(px, py, 15, 0x285b9c);
        canvas.Disc(px, py, 11, 0x62a8ff);
        canvas.Disc(px - 3, py - 3, 3, 0xe4f2ff);
    }

    canvas.Rect(0, 0, kWidth, kHud, 0x101725);
    for (i32 i = 0; i < game.BeaconsTotal(); ++i) {
        canvas.Disc(24 + i * 24, 27, 7, i < game.BeaconsCollected() ? 0x63f0dc : 0x3c4657);
    }
    for (i32 i = 0; i < 3; ++i) {
        canvas.Disc(kWidth - 30 - i * 28, 27, 9, i < game.HitsRemaining() ? 0xf04f67 : 0x3c4657);
    }

    if (game.GetState() != State::Playing) {
        canvas.Rect(kWidth / 2 - 132, kHeight / 2 - 25, 264, 50, 0x101725);
        const u32 color = game.GetState() == State::Won ? 0x63f0dc : 0xf04f67;
        canvas.Rect(kWidth / 2 - 100, kHeight / 2 - 5, 200, 10, color);
    }
}

void SaveBmp(const char* path, const Canvas& canvas) {
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = kWidth;
    info.biHeight = -kHeight;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(canvas.pixels.size() * sizeof(u32));
    std::FILE* output = nullptr;
    if (fopen_s(&output, path, "wb") == 0 && output) {
        std::fwrite(&file, sizeof(file), 1, output);
        std::fwrite(&info, sizeof(info), 1, output);
        std::fwrite(canvas.pixels.data(), sizeof(u32), canvas.pixels.size(), output);
        std::fclose(output);
    }
}

bool Down(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

} // namespace

int main() {
    Game game;
    WindowDesc desc;
    desc.title = "Beacon Run";
    desc.width = kWidth;
    desc.height = kHeight;
    Window window(desc);
    HWND hwnd = static_cast<HWND>(window.NativeHandle());

    i64 max_frames = -1;
    if (const char* env = std::getenv("AETHER_BEACONRUN_MAX_FRAMES")) {
        max_frames = std::atoll(env);
    }

    Canvas canvas;
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = kWidth;
    bitmap.bmiHeader.biHeight = -kHeight;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;

    LARGE_INTEGER frequency{}, last{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&last);
    f32 accumulator = 0;
    f32 elapsed = 0;
    f32 title_timer = 0;
    bool previous_restart = false;
    i64 frame = 0;

    AETHER_LOG_INFO("BeaconRun", "Collect all %d beacons and avoid the patrol drones.", game.BeaconsTotal());
    while (window.PumpMessages()) {
        if (window.IsMinimized()) {
            Sleep(16);
            continue;
        }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        f32 dt = static_cast<f32>(now.QuadPart - last.QuadPart) / static_cast<f32>(frequency.QuadPart);
        last = now;
        dt = std::min(dt, 0.1f);
        accumulator += dt;
        elapsed += dt;

        Input input;
        if (GetForegroundWindow() == hwnd) {
            input.move_x = (Down('D') || Down(VK_RIGHT) ? 1.0f : 0.0f) - (Down('A') || Down(VK_LEFT) ? 1.0f : 0.0f);
            input.move_y = (Down('S') || Down(VK_DOWN) ? 1.0f : 0.0f) - (Down('W') || Down(VK_UP) ? 1.0f : 0.0f);
            if (Down(VK_ESCAPE)) {
                break;
            }
            const bool restart = Down('R');
            if (restart && !previous_restart) {
                game.Restart();
            }
            previous_restart = restart;
        }

        while (accumulator >= Game::kStep) {
            game.Step(input);
            accumulator -= Game::kStep;
        }

        Render(game, canvas, elapsed);
        RECT client{};
        GetClientRect(hwnd, &client);
        HDC dc = GetDC(hwnd);
        StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, kWidth, kHeight, canvas.pixels.data(), &bitmap,
                      DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd, dc);

        title_timer -= dt;
        if (title_timer <= 0) {
            title_timer = 0.2f;
            char title[120];
            if (game.GetState() == State::Won) {
                std::snprintf(title, sizeof(title), "Beacon Run - arena clear in %.1fs - R to replay", game.Time());
            } else if (game.GetState() == State::Lost) {
                std::snprintf(title, sizeof(title), "Beacon Run - caught by drones - R to replay");
            } else {
                std::snprintf(title, sizeof(title), "Beacon Run - beacons %d/%d - hits %d - %.1fs",
                              game.BeaconsCollected(), game.BeaconsTotal(), game.HitsTaken(), game.Time());
            }
            SetWindowTextA(hwnd, title);
        }

        if (max_frames >= 0 && ++frame >= max_frames) {
            break;
        }
        Sleep(1);
    }
    if (const char* screenshot = std::getenv("AETHER_BEACONRUN_SCREENSHOT")) {
        SaveBmp(screenshot, canvas);
    }
    return 0;
}
