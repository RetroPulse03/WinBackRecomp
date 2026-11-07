// Display settings.
//
// The window mode is handled here with SDL rather than through ultramodern's
// WindowMode, which RT64 implements as a borderless window only. The
// renderer's graphics config therefore always says Windowed, and SDL makes
// the window borderless (covering the monitor) or exclusive full screen
// (owning the monitor at its desktop resolution).

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <json/json.hpp>

#include "librecomp/game.hpp"
#include "ultramodern/renderer_context.hpp"

#include "sdl_include.hpp"
#include "display.hpp"
#include "rt64_context.hpp"

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
// Read by the game thread every time an object is tested for visibility.
std::atomic<bool> widescreen_on = false;
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
    j["antialiasing"] = s.antialiasing;
    j["widescreen"] = s.widescreen;
    j["crop_overscan"] = s.crop_overscan;
    j["integer_scale"] = s.integer_scale;
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
    // Widescreen is a fixed 16:9 (RT64's Manual ratio), not the window's shape, so crop and integer
    // scale work on the same picture in any window.
    cfg.ar_option = s.widescreen ? R::AspectRatio::Manual : R::AspectRatio::Original;
    cfg.msaa_option = s.antialiasing;
    R::set_graphics_config(cfg);
    winback::set_framing(s.crop_overscan, s.integer_scale);
    widescreen_on = s.widescreen;
}

} // namespace

// Read by the HUD hooks (src/game/hud.cpp) every frame.
extern "C" int winback_widescreen_on(void) {
    return widescreen_on ? 1 : 0;
}

// Hook in the game's view test (func_80028BB4): the objects it hides are those outside a horizontal half
// angle of half_fov * k, where k is fixed for a 4:3 view (8:3 for two-player split screen). In widescreen
// a view the renderer widens is 4/3 times as wide, so k grows by the ratio of the true half angles,
// keeping the game's own margin. Four-way split views aren't widened by the renderer and stay as they are.
extern "C" float winback_view_test_k(float k, double half_fov, int widened) {
    if (!widescreen_on || !widened) {
        return k;
    }
    // k is about 1.2 for a 4:3 view and 2.0 for an 8:3 one, plus 0.2 for some cameras.
    double aspect = k > 1.9f ? 8.0 / 3.0 : 4.0 / 3.0;
    double t = std::tan(half_fov);
    double scale = std::atan(t * aspect * (4.0 / 3.0)) / std::atan(t * aspect);
    return float(k * scale);
}

void load(bool developer, bool force_fullscreen) {
    Settings s;
    std::ifstream in(settings_path());
    if (in) {
        try {
            nlohmann::json j = nlohmann::json::parse(in);
            s.window_mode = j.value("window_mode", s.window_mode);
            s.resolution = j.value("resolution", s.resolution);
            s.antialiasing = j.value("antialiasing", s.antialiasing);
            s.widescreen = j.value("widescreen", s.widescreen);
            s.crop_overscan = j.value("crop_overscan", s.crop_overscan);
            s.integer_scale = j.value("integer_scale", s.integer_scale);
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
