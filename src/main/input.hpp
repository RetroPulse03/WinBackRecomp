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
}
