#pragma once

#include <cstdint>

#include "ultramodern/input.hpp"

union SDL_Event;

namespace winback::input {
    void handle_event(const SDL_Event& ev);
    void poll();
    bool get(int controller_num, uint16_t* buttons, float* x, float* y);
    void set_rumble(int controller_num, bool rumble);
    ultramodern::input::connected_device_info_t get_device_info(int controller_num);

    // What mods can see and change (mods/include/winback_mod.h).
    struct RawPad {
        uint32_t pad_buttons;
        uint32_t other_buttons;
        float left_x, left_y, right_x, right_y;
    };
    RawPad read_raw(int controller_num);
    void set_mod_pad(int controller_num, uint16_t buttons, float x, float y);

    // Controller layouts, chosen in the recomp menu's Controls screen and saved
    // to settings.json. The keyboard's keys are fixed.

    // What a controller button presses.
    // LookLeft and LookRight turn the view that way: they press whichever of
    // C-Left/C-Right does that under the game's camera option.
    enum class Bind : uint8_t { None, A, B, Z, R, L, Start, CUp, CDown, CLeft, CRight, DUp, DDown, DLeft, DRight, CameraReset, LookLeft, LookRight, Count };
    // The controller buttons that can be bound, in the menu's order.
    enum PadInput { PadA, PadB, PadX, PadY, PadLB, PadRB, PadLT, PadRT, PadStart, PadL3, PadR3, PadDUp, PadDDown, PadDLeft, PadDRight, PadCount };
    // Camera: left and right turn the view that way by how far the stick is
    // pushed (as LookLeft/LookRight for anything that reads buttons). CButtons: all four
    // directions press the C buttons.
    enum class RightStick { Camera, CButtons, None, Count };
    // AB: in the game's menus A confirms and B goes back, whatever they do in
    // play. AsPlay: menus get the same buttons as play.
    enum class MenuButtons { AB, AsPlay, Count };

    struct Layout {
        Bind buttons[PadCount];
        RightStick right_stick;
        MenuButtons menus;
    };

    enum class Preset { PS2, N64, Custom, Count };

    // The layout a preset uses; Custom gives the PS2 layout, which a new
    // custom layout starts from.
    Layout preset_layout(Preset preset);

    struct ControlSettings {
        Preset preset = Preset::PS2;
        Layout custom = preset_layout(Preset::PS2);
    };

    // Reads the controls from settings.json. Call before the game starts.
    void load_controls();
    ControlSettings get_controls();
    // Window thread: applies and saves new controls.
    void set_controls(const ControlSettings& settings);
    // The layout in use.
    Layout active_layout(const ControlSettings& settings);
}
