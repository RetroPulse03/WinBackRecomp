#pragma once

#include "ultramodern/config.hpp"

struct SDL_Window;

// Display settings: window mode and the renderer options shown in the recomp
// menu's Display screen. They are saved to settings.json next to the exe.

namespace winback::display {
    enum class WindowMode { Windowed, Borderless, Fullscreen };

    struct Settings {
        WindowMode window_mode = WindowMode::Windowed;
        ultramodern::renderer::Resolution resolution = ultramodern::renderer::Resolution::Auto;
        ultramodern::renderer::Antialiasing antialiasing = ultramodern::renderer::Antialiasing::MSAA2X;
        bool crop_overscan = false;
        bool integer_scale = false;
    };

    // Reads settings.json and sets the renderer's starting configuration.
    // force_fullscreen (--fullscreen, Big Picture) starts borderless without
    // saving it. Call before the renderer starts.
    void load(bool developer, bool force_fullscreen);

    // Window thread: applies the window mode once the window exists.
    void attach_window(SDL_Window* window);

    Settings get();

    // Window thread: applies and saves new settings.
    void set(const Settings& settings);

    // Window thread: F11 / Alt+Enter switches between windowed and the last
    // full-screen mode.
    void toggle_fullscreen();
}
