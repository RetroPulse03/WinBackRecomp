// Display settings.
//
// The window mode is handled here with SDL rather than through ultramodern's
// WindowMode, which RT64 implements as a borderless window only. The
// renderer's graphics config therefore always says Windowed, and SDL makes
// the window borderless (covering the monitor) or exclusive full screen
// (owning the monitor at its desktop resolution).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <json/json.hpp>

#include "librecomp/game.hpp"
#include "ultramodern/renderer_context.hpp"

#include "sdl_include.hpp"
#include "display.hpp"

namespace R = ultramodern::renderer;

namespace winback::display {

NLOHMANN_JSON_SERIALIZE_ENUM(WindowMode, {
    {WindowMode::Windowed, "Windowed"},
    {WindowMode::Borderless, "Borderless"},
    {WindowMode::Fullscreen, "Fullscreen"},
});

namespace {

std::mutex settings_mutex;
Settings settings;
// What F11 switches to from windowed.
WindowMode last_fullscreen = WindowMode::Borderless;
bool forced_fullscreen = false;
SDL_Window* window = nullptr;

std::filesystem::path settings_path() {
    return recomp::get_config_path() / "settings.json";
}

void save(const Settings& s) {
    nlohmann::json j;
    j["window_mode"] = s.window_mode;
    j["resolution"] = s.resolution;
    j["aspect_ratio"] = s.aspect_ratio;
    j["antialiasing"] = s.antialiasing;
    std::ofstream out(settings_path());
    if (out) {
        out << j.dump(4) << '\n';
    }
    else {
        std::fprintf(stderr, "[display] can't write %s\n", settings_path().string().c_str());
    }
}

void apply_window_mode(WindowMode mode) {
    if (window == nullptr) {
        return;
    }
    switch (mode) {
        case WindowMode::Windowed:
            SDL_SetWindowFullscreen(window, 0);
            break;
        case WindowMode::Borderless:
            SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
            break;
        case WindowMode::Fullscreen: {
            // Leave a borderless window first so SDL picks up the new mode.
            SDL_SetWindowFullscreen(window, 0);
            SDL_DisplayMode desktop;
            if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(window), &desktop) == 0) {
                SDL_SetWindowDisplayMode(window, &desktop);
            }
            SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
            break;
        }
    }
}

void apply_renderer(const Settings& s, bool developer) {
    R::GraphicsConfig cfg = R::get_graphics_config();
    cfg.developer_mode = developer;
    cfg.res_option = s.resolution;
    cfg.ar_option = s.aspect_ratio;
    cfg.msaa_option = s.antialiasing;
    R::set_graphics_config(cfg);
}

} // namespace

void load(bool developer, bool force_fullscreen) {
    Settings s;
    std::ifstream in(settings_path());
    if (in) {
        try {
            nlohmann::json j = nlohmann::json::parse(in);
            s.window_mode = j.value("window_mode", s.window_mode);
            s.resolution = j.value("resolution", s.resolution);
            s.aspect_ratio = j.value("aspect_ratio", s.aspect_ratio);
            s.antialiasing = j.value("antialiasing", s.antialiasing);
        }
        catch (const nlohmann::json::exception& e) {
            std::fprintf(stderr, "[display] ignoring settings.json: %s\n", e.what());
        }
    }
    forced_fullscreen = force_fullscreen && s.window_mode == WindowMode::Windowed;
    {
        std::lock_guard lock{settings_mutex};
        settings = s;
        if (s.window_mode != WindowMode::Windowed) {
            last_fullscreen = s.window_mode;
        }
    }

    R::GraphicsConfig cfg{};
    cfg.wm_option = R::WindowMode::Windowed;
    cfg.hr_option = R::HUDRatioMode::Clamp16x9;
    // Vulkan on every platform (RT64's Auto picks D3D12 on Windows). D3D12 stays
    // available through WINBACK_GFX_API=d3d12.
    cfg.api_option = R::GraphicsApi::Vulkan;
    // No frame interpolation: RT64 treats every task as a frame, and WinBack draws each frame with two tasks
    // (the scene, then the laser sight), so interpolated frames don't match the game's frames.
    cfg.rr_option = R::RefreshRate::Original;
    cfg.hpfb_option = R::HighPrecisionFramebuffer::Auto;
    cfg.rr_manual_value = 60;
    cfg.ds_option = 1;
    R::set_graphics_config(cfg);
    apply_renderer(s, developer);
}

void attach_window(SDL_Window* w) {
    window = w;
    apply_window_mode(forced_fullscreen ? WindowMode::Borderless : get().window_mode);
}

Settings get() {
    std::lock_guard lock{settings_mutex};
    return settings;
}

void set(const Settings& s) {
    Settings old;
    {
        std::lock_guard lock{settings_mutex};
        old = settings;
        settings = s;
        if (s.window_mode != WindowMode::Windowed) {
            last_fullscreen = s.window_mode;
        }
    }
    if (s.window_mode != old.window_mode || forced_fullscreen) {
        forced_fullscreen = false;
        apply_window_mode(s.window_mode);
    }
    apply_renderer(s, R::get_graphics_config().developer_mode);
    save(s);
}

void toggle_fullscreen() {
    Settings s = get();
    bool windowed = s.window_mode == WindowMode::Windowed && !forced_fullscreen;
    s.window_mode = windowed ? last_fullscreen : WindowMode::Windowed;
    set(s);
}

} // namespace winback::display
