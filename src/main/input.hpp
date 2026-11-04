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
}
