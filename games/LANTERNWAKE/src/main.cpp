#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "audio.h"
#include "game.h"
#include "renderer.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

EM_JS(void, LanternwakeBrowserReady, (), {
    if (Module['onGameReady']) Module['onGameReady']();
});
EM_JS(void, LanternwakeBrowserFailure, (const char* message), {
    if (Module['onGameFailure']) Module['onGameFailure'](UTF8ToString(message));
});
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef LANTERNWAKE_ASSET_DIR
#define LANTERNWAKE_ASSET_DIR "assets"
#endif

namespace lanternwake {
namespace {

struct Options {
    bool headless = false, autoplay = false, mute = false, audio = true;
    int frames = 0, width = kWidth, height = kHeight;
    std::string screenshot, assets, scene = "title";
};

void Usage() {
    std::puts(
        "Lanternwake — an Aether game\n"
        "Usage: lanternwake [options]\n"
        "  --assets DIR          Use this asset directory\n"
        "  --headless            Render without a window or audio device\n"
        "  --frames N            Exit after N rendered frames (headless default: 1)\n"
        "  --screenshot FILE     Save the final composed frame as PNG\n"
        "  --scene title|play    Initial screen (default: title)\n"
        "  --autoplay            Demonstrate the real game with an automatic keeper\n"
        "  --window WIDTHxHEIGHT Initial native window size\n"
        "  --mute                Start with sound muted\n"
        "  --no-audio            Do not open an audio device\n"
        "  --help                Show this help\n\n"
        "WASD/arrows move · Space dash · F/left mouse pulse · hold E offer\n"
        "Enter begin · Escape pause/resume · R retry · M mute");
}

bool Integer(std::string_view text, int& value) {
    if (text.empty()) return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

bool ParseOptions(int argc, char** argv, Options& options, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        auto value = [&]() -> const char* {
            if (i + 1 == argc) {
                error = "Missing value after " + std::string(argument);
                return nullptr;
            }
            return argv[++i];
        };
        if (argument == "--headless") options.headless = true;
        else if (argument == "--autoplay") options.autoplay = true;
        else if (argument == "--mute") options.mute = true;
        else if (argument == "--no-audio") options.audio = false;
        else if (argument == "--assets") {
            const char* text = value(); if (!text) return false;
            options.assets = text;
        } else if (argument == "--screenshot") {
            const char* text = value(); if (!text) return false;
            options.screenshot = text;
        } else if (argument == "--scene") {
            const char* text = value(); if (!text) return false;
            options.scene = text;
            if (options.scene != "title" && options.scene != "play") {
                error = "--scene must be title or play";
                return false;
            }
        } else if (argument == "--frames") {
            const char* text = value(); if (!text) return false;
            if (!Integer(text, options.frames) || options.frames < 1) {
                error = "--frames must be a positive integer";
                return false;
            }
        } else if (argument == "--window") {
            const char* text = value(); if (!text) return false;
            const std::string_view size(text);
            const auto x = size.find('x');
            if (x == size.npos || !Integer(size.substr(0, x), options.width) ||
                !Integer(size.substr(x + 1), options.height) ||
                options.width < 320 || options.height < 200 ||
                options.width > 7680 || options.height > 4320) {
                error = "--window expects WIDTHxHEIGHT between 320x200 and 7680x4320";
                return false;
            }
        } else {
            error = "Unknown option: " + std::string(argument);
            return false;
        }
    }
    if (options.headless && options.frames == 0) options.frames = 1;
    if (options.autoplay) options.scene = "play";
    return true;
}

const char* PhaseName(Phase phase) {
    switch (phase) {
    case Phase::Title: return "title";
    case Phase::Playing: return "playing";
    case Phase::Paused: return "paused";
    case Phase::Won: return "won";
    case Phase::Lost: return "lost";
    }
    return "unknown";
}

class App {
public:
    explicit App(Options options) : options_(std::move(options)) {}
    ~App() {
        audio_.Stop();
        if (texture_) SDL_DestroyTexture(texture_);
        if (display_) SDL_DestroyRenderer(display_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    bool Initialize(std::string& error) {
        SDL_SetMainReady();
        const auto flags = options_.headless ? SDL_INIT_TIMER : SDL_INIT_VIDEO | SDL_INIT_TIMER;
        if (SDL_Init(flags) != 0) {
            error = "SDL could not start: " + std::string(SDL_GetError());
            return false;
        }
        if (!LoadAssets(error)) return false;
        if (!options_.headless) {
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
            window_ = SDL_CreateWindow("Lanternwake | Aether", SDL_WINDOWPOS_CENTERED,
                SDL_WINDOWPOS_CENTERED, options_.width, options_.height,
                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
            if (!window_) {
                error = "Could not create the game window: " + std::string(SDL_GetError());
                return false;
            }
            display_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!display_) display_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
            if (!display_) {
                error = "Could not create the game display: " + std::string(SDL_GetError());
                return false;
            }
            texture_ = SDL_CreateTexture(display_, SDL_PIXELFORMAT_ABGR8888,
                SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight);
            if (!texture_) {
                error = "Could not create the frame texture: " + std::string(SDL_GetError());
                return false;
            }
            SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
            if (options_.audio) {
                std::string audio_error;
                if (!audio_.Start(audio_error))
                    std::fprintf(stderr, "Lanternwake: sound unavailable: %s\n", audio_error.c_str());
            }
        }
        audio_.SetMuted(options_.mute);
        if (options_.scene == "play") {
            Input start;
            start.start = true;
            Step(start);
        }
        last_counter_ = SDL_GetPerformanceCounter();
        return true;
    }

    bool Frame() {
        const auto now = SDL_GetPerformanceCounter();
        const double seconds = static_cast<double>(now - last_counter_) / SDL_GetPerformanceFrequency();
        last_counter_ = now;
        const float dt = options_.headless ? kStep : static_cast<float>(std::clamp(seconds, 0.0, 0.1));
        PollEvents();
        if (quit_) return false;

        if (focused_) {
            if (game_.State() == Phase::Playing) {
                accumulator_ += dt;
                while (accumulator_ + 0.000001f >= kStep && game_.State() == Phase::Playing) {
                    Step(options_.autoplay ? Autoplay() : ReadInput());
                    ClearEdges();
                    accumulator_ -= kStep;
                }
                if (game_.State() != Phase::Playing) accumulator_ = 0;
            } else {
                accumulator_ = 0;
                // Menu commands are still consumed once, even while simulation
                // time is stopped. Starting/resuming does not inherit stale dt.
                if (edges_.start || edges_.pause || edges_.restart) {
                    Step(ReadInput());
                }
                ClearEdges();
            }
        } else {
            accumulator_ = 0;
            ClearEdges();
        }

        ui_time_ += dt;
        renderer_.Render(game_, {ui_time_, focused_ && game_.State() != Phase::Paused ? dt : 0.0f,
                                mouse_x_, mouse_y_, audio_.Muted()});
        if (display_) {
            if (SDL_UpdateTexture(texture_, nullptr, renderer_.Pixels().data(), kWidth * sizeof(std::uint32_t)) != 0) {
                std::fprintf(stderr, "Lanternwake: frame upload failed: %s\n", SDL_GetError());
                failed_ = true;
                return false;
            }
            SDL_SetRenderDrawColor(display_, 5, 12, 19, 255);
            SDL_RenderClear(display_);
            const auto area = DisplayArea();
            SDL_RenderCopy(display_, texture_, nullptr, &area);
            SDL_RenderPresent(display_);
        }
        ++frames_;
        if (options_.frames > 0 && frames_ >= options_.frames) return false;
        if (!options_.headless) SDL_Delay(1);
        return true;
    }

    int Finish() {
        if (!options_.screenshot.empty()) {
            std::string error;
            if (!renderer_.SaveScreenshot(options_.screenshot, error)) {
                std::fprintf(stderr, "Lanternwake: screenshot failed: %s\n", error.c_str());
                failed_ = true;
            } else {
                std::printf("Saved %s\n", options_.screenshot.c_str());
            }
        }
        std::printf("Lanternwake: frames=%d phase=%s embers=%d shrines=%d hits=%d score=%d\n",
            frames_, PhaseName(game_.State()), game_.Collected(), game_.LitShrines(), game_.Hits(), game_.Score());
        return failed_ ? 1 : 0;
    }

private:
    Options options_;
    Game game_;
    Renderer renderer_;
    Audio audio_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* display_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    std::array<bool, SDL_NUM_SCANCODES> held_{};
    Input edges_{};
    bool focused_ = true, quit_ = false, failed_ = false;
    float accumulator_ = 0, ui_time_ = 0, mouse_x_ = -1, mouse_y_ = -1;
    int frames_ = 0;
    std::uint64_t last_counter_ = 0;

    bool LoadAssets(std::string& error) {
        namespace fs = std::filesystem;
        std::vector<fs::path> candidates;
        if (!options_.assets.empty()) {
            candidates.emplace_back(options_.assets);
        } else if (const char* environment = std::getenv("LANTERNWAKE_ASSETS"); environment && *environment) {
            candidates.emplace_back(environment);
        } else {
#if defined(__EMSCRIPTEN__)
            candidates.emplace_back("/assets");
#else
            if (char* base = SDL_GetBasePath()) {
                candidates.emplace_back(fs::path(base) / "assets");
                SDL_free(base);
            }
#endif
            candidates.emplace_back("assets");
            candidates.emplace_back("games/LANTERNWAKE/assets");
            candidates.emplace_back(LANTERNWAKE_ASSET_DIR);
        }
        for (const auto& directory : candidates) {
            std::error_code ec;
            if (!fs::is_directory(directory, ec)) continue;
            if (!renderer_.LoadAssets(directory.string(), error)) {
                error = "Could not load assets in '" + directory.string() + "': " + error;
                return false;
            }
            std::printf("Lanternwake assets: %s\n", directory.string().c_str());
            return true;
        }
        error = "Game assets were not found. Place the assets folder beside lanternwake, "
                "or pass --assets PATH. Searched:";
        for (const auto& directory : candidates) error += "\n  " + directory.string();
        return false;
    }

    void Step(const Input& input) {
        game_.Step(input);
        for (const Event& event : game_.Events()) renderer_.OnEvent(event);
        audio_.HandleEvents(game_.Events());
    }

    void ClearEdges() { edges_ = {}; }

    Input ReadInput() const {
        Input input = edges_;
        input.move_x = static_cast<float>(held_[SDL_SCANCODE_D] || held_[SDL_SCANCODE_RIGHT]) -
                       static_cast<float>(held_[SDL_SCANCODE_A] || held_[SDL_SCANCODE_LEFT]);
        input.move_y = static_cast<float>(held_[SDL_SCANCODE_S] || held_[SDL_SCANCODE_DOWN]) -
                       static_cast<float>(held_[SDL_SCANCODE_W] || held_[SDL_SCANCODE_UP]);
        input.interact = held_[SDL_SCANCODE_E];
        return input;
    }

    Input Autoplay() const {
        Input input = edges_;
        const Position player = game_.PlayerPosition();
        Position goal = Game::GatePosition();
        float closest = 1.0e20f;
        bool found = false;
        auto consider = [&](Position position) {
            const float x = position.x - player.x, y = position.y - player.y;
            const float distance = x * x + y * y;
            if (distance < closest) { goal = position; closest = distance; found = true; }
        };
        if (game_.Player().carried >= 4) {
            game_.World().ForEach<Position, Shrine>([&](const Position& position, const Shrine& shrine) {
                if (!shrine.lit) consider(position);
            });
        } else {
            game_.World().ForEach<Position, Mote>([&](const Position& position, const Mote& mote) {
                if (!mote.collected) consider(position);
            });
        }
        const float dx = goal.x - player.x, dy = goal.y - player.y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance > 8.0f) {
            input.move_x = dx / distance;
            input.move_y = dy / distance;
        }
        input.interact = distance < 45.0f || (!found && game_.NearGate());
        bool danger = false;
        game_.World().ForEach<Position, Wraith>([&](const Position& position, const Wraith& wraith) {
            const float x = position.x - player.x, y = position.y - player.y;
            danger = danger || (x * x + y * y < 125.0f * 125.0f && wraith.stunned <= 0);
        });
        input.pulse = input.pulse || (danger && game_.Player().pulse_cooldown <= 0);
        input.dash = input.dash || (danger && game_.Player().dash_cooldown <= 0 && distance > 100);
        return input;
    }

    SDL_Rect DisplayArea() const {
        int width = kWidth, height = kHeight;
        if (display_) SDL_GetRendererOutputSize(display_, &width, &height);
        const float scale = std::min(static_cast<float>(width) / kWidth, static_cast<float>(height) / kHeight);
        const int fitted_width = std::max(1, static_cast<int>(kWidth * scale));
        const int fitted_height = std::max(1, static_cast<int>(kHeight * scale));
        return {(width - fitted_width) / 2, (height - fitted_height) / 2, fitted_width, fitted_height};
    }

    void MousePosition(int x, int y) {
        int width = kWidth, height = kHeight, pixel_width = kWidth, pixel_height = kHeight;
        SDL_GetWindowSize(window_, &width, &height);
        SDL_GetRendererOutputSize(display_, &pixel_width, &pixel_height);
        const auto area = DisplayArea();
        const float px = static_cast<float>(x) * pixel_width / std::max(1, width);
        const float py = static_cast<float>(y) * pixel_height / std::max(1, height);
        mouse_x_ = (px - area.x) * kWidth / area.w;
        mouse_y_ = (py - area.y) * kHeight / area.h;
        if (mouse_x_ < 0 || mouse_x_ >= kWidth || mouse_y_ < 0 || mouse_y_ >= kHeight)
            mouse_x_ = mouse_y_ = -1;
    }

    void Action(UiAction action) {
        switch (action) {
        case UiAction::Start: edges_.start = true; break;
        case UiAction::Resume: edges_.pause = true; break;
        case UiAction::Restart: edges_.restart = true; break;
        case UiAction::ToggleMute: audio_.SetMuted(!audio_.Muted()); break;
        case UiAction::None: break;
        }
    }

    void PollEvents() {
        if (options_.headless) return;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) quit_ = true;
            else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST || event.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    focused_ = false;
                    held_.fill(false);
                    ClearEdges();
                    accumulator_ = 0;
                    if (game_.State() == Phase::Playing) {
                        Input pause;
                        pause.pause = true;
                        Step(pause);
                    }
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || event.window.event == SDL_WINDOWEVENT_RESTORED) {
                    focused_ = true;
                    accumulator_ = 0;
                } else if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
                    mouse_x_ = mouse_y_ = -1;
                }
            } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                const auto key = event.key.keysym.scancode;
                if (key >= 0 && key < SDL_NUM_SCANCODES) held_[key] = event.type == SDL_KEYDOWN;
                if (event.type != SDL_KEYDOWN || event.key.repeat || !focused_) continue;
                audio_.Wake();
                switch (key) {
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: edges_.start = true; break;
                case SDL_SCANCODE_ESCAPE: edges_.pause = true; break;
                case SDL_SCANCODE_R: edges_.restart = true; break;
                case SDL_SCANCODE_SPACE: edges_.dash = true; break;
                case SDL_SCANCODE_F: edges_.pulse = true; break;
                case SDL_SCANCODE_M: audio_.SetMuted(!audio_.Muted()); break;
                default: break;
                }
            } else if (event.type == SDL_MOUSEMOTION) {
                MousePosition(event.motion.x, event.motion.y);
            } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT && focused_) {
                MousePosition(event.button.x, event.button.y);
                audio_.Wake();
                const auto action = renderer_.HitTest(game_.State(), mouse_x_, mouse_y_);
                if (action != UiAction::None) Action(action);
                else if (game_.State() == Phase::Playing && mouse_x_ >= 0) edges_.pulse = true;
            }
        }
    }
};

} // namespace
} // namespace lanternwake

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h") {
            lanternwake::Usage();
            return 0;
        }
    }
    lanternwake::Options options;
    std::string error;
    if (!lanternwake::ParseOptions(argc, argv, options, error)) {
        std::fprintf(stderr, "Lanternwake: %s\nUse --help for options.\n", error.c_str());
        return 2;
    }
    auto app = std::make_unique<lanternwake::App>(std::move(options));
    if (!app->Initialize(error)) {
        std::fprintf(stderr, "Lanternwake: %s\n", error.c_str());
#if defined(__EMSCRIPTEN__)
        LanternwakeBrowserFailure(error.c_str());
#endif
        return 1;
    }
#if defined(__EMSCRIPTEN__)
    LanternwakeBrowserReady();
    auto* browser_app = app.release();
    emscripten_set_main_loop_arg([](void* context) {
        auto* current = static_cast<lanternwake::App*>(context);
        if (!current->Frame()) {
            current->Finish();
            emscripten_cancel_main_loop();
            delete current;
        }
    }, browser_app, 0, true);
    return 0;
#else
    while (app->Frame()) {}
    return app->Finish();
#endif
}
