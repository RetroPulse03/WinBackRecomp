// Keyboard and gamepad input mapped to N64 controllers.
//
// Player 1: keyboard plus the first gamepad. Players 2-4: further gamepads
// (split-screen modes will use these).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "sdl_include.hpp"
#include "input.hpp"

namespace {

enum N64Button : uint16_t {
    BTN_A = 0x8000, BTN_B = 0x4000, BTN_Z = 0x2000, BTN_START = 0x1000,
    BTN_DU = 0x0800, BTN_DD = 0x0400, BTN_DL = 0x0200, BTN_DR = 0x0100,
    BTN_L = 0x0020, BTN_R = 0x0010,
    BTN_CU = 0x0008, BTN_CD = 0x0004, BTN_CL = 0x0002, BTN_CR = 0x0001,
};

constexpr int max_players = 4;

struct PadState {
    uint16_t buttons = 0;
    float x = 0.0f;
    float y = 0.0f;
};

std::mutex state_mutex;
std::array<PadState, max_players> pads{};
std::array<SDL_GameController*, max_players> controllers{};

float axis(SDL_GameController* c, SDL_GameControllerAxis a) {
    float v = SDL_GameControllerGetAxis(c, a) / 32767.0f;
    constexpr float deadzone = 0.15f;
    if (std::fabs(v) < deadzone) {
        return 0.0f;
    }
    return std::clamp((v - std::copysign(deadzone, v)) / (1.0f - deadzone), -1.0f, 1.0f);
}

PadState read_controller(SDL_GameController* c) {
    PadState s{};
    auto btn = [&](SDL_GameControllerButton b, uint16_t mask) {
        if (SDL_GameControllerGetButton(c, b)) s.buttons |= mask;
    };
    btn(SDL_CONTROLLER_BUTTON_A, BTN_A);
    btn(SDL_CONTROLLER_BUTTON_X, BTN_B);
    btn(SDL_CONTROLLER_BUTTON_B, BTN_CD);
    btn(SDL_CONTROLLER_BUTTON_Y, BTN_CL);
    btn(SDL_CONTROLLER_BUTTON_START, BTN_START);
    btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BTN_L);
    btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BTN_R);
    btn(SDL_CONTROLLER_BUTTON_DPAD_UP, BTN_DU);
    btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN, BTN_DD);
    btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_DL);
    btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, BTN_DR);
    if (axis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.5f) s.buttons |= BTN_Z;
    if (axis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f) s.buttons |= BTN_R;
    float rx = axis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    float ry = axis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    if (rx > 0.5f) s.buttons |= BTN_CR;
    if (rx < -0.5f) s.buttons |= BTN_CL;
    if (ry > 0.5f) s.buttons |= BTN_CD;
    if (ry < -0.5f) s.buttons |= BTN_CU;
    s.x = axis(c, SDL_CONTROLLER_AXIS_LEFTX);
    s.y = -axis(c, SDL_CONTROLLER_AXIS_LEFTY);
    return s;
}

PadState read_keyboard() {
    PadState s{};
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    auto key = [&](SDL_Scancode sc, uint16_t mask) {
        if (k[sc]) s.buttons |= mask;
    };
    key(SDL_SCANCODE_F, BTN_A);
    key(SDL_SCANCODE_R, BTN_B);
    key(SDL_SCANCODE_SPACE, BTN_Z);
    key(SDL_SCANCODE_RETURN, BTN_START);
    key(SDL_SCANCODE_Q, BTN_L);
    key(SDL_SCANCODE_E, BTN_R);
    key(SDL_SCANCODE_Y, BTN_CU);
    key(SDL_SCANCODE_G, BTN_CD);
    key(SDL_SCANCODE_T, BTN_CL);
    key(SDL_SCANCODE_H, BTN_CR);
    key(SDL_SCANCODE_UP, BTN_DU);
    key(SDL_SCANCODE_DOWN, BTN_DD);
    key(SDL_SCANCODE_LEFT, BTN_DL);
    key(SDL_SCANCODE_RIGHT, BTN_DR);
    if (k[SDL_SCANCODE_A]) s.x -= 1.0f;
    if (k[SDL_SCANCODE_D]) s.x += 1.0f;
    if (k[SDL_SCANCODE_W]) s.y += 1.0f;
    if (k[SDL_SCANCODE_S]) s.y -= 1.0f;
    return s;
}

// WINBACK_AUTOPRESS=button@seconds[,button@seconds...]: scripted player 1
// presses for unattended tests, e.g. "start@10,a@13,a@15". Each press is held
// for 150 ms, or for the given seconds with button@seconds+hold (e.g. "r@32+10").
// Buttons: a b z start l r du dd dl dr cu cd cl cr.
struct AutoPress {
    uint16_t mask;
    double at;
    double hold;
};

std::vector<AutoPress> load_autopress() {
    std::vector<AutoPress> out;
    const char* env = std::getenv("WINBACK_AUTOPRESS");
    if (env == nullptr) {
        return out;
    }
    static const std::pair<const char*, uint16_t> names[] = {
        {"a", BTN_A}, {"b", BTN_B}, {"z", BTN_Z}, {"start", BTN_START}, {"l", BTN_L}, {"r", BTN_R},
        {"du", BTN_DU}, {"dd", BTN_DD}, {"dl", BTN_DL}, {"dr", BTN_DR},
        {"cu", BTN_CU}, {"cd", BTN_CD}, {"cl", BTN_CL}, {"cr", BTN_CR},
    };
    std::string spec = env;
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t at = item.find('@');
        if (at != std::string::npos) {
            std::string name = item.substr(0, at);
            for (const auto& [n, mask] : names) {
                if (name == n) {
                    size_t plus = item.find('+', at);
                    double hold = plus == std::string::npos ? 0.15 : std::atof(item.c_str() + plus + 1);
                    out.push_back({mask, std::atof(item.c_str() + at + 1), hold});
                }
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return out;
}

uint16_t autopress_buttons() {
    static const std::vector<AutoPress> presses = load_autopress();
    static const auto start = std::chrono::steady_clock::now();
    if (presses.empty()) {
        return 0;
    }
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    uint16_t buttons = 0;
    for (const AutoPress& p : presses) {
        if (t >= p.at && t < p.at + p.hold) {
            buttons |= p.mask;
        }
    }
    return buttons;
}

} // namespace

void winback::input::handle_event(const SDL_Event& ev) {
    if (ev.type == SDL_CONTROLLERDEVICEADDED) {
        SDL_GameController* c = SDL_GameControllerOpen(ev.cdevice.which);
        for (auto& slot : controllers) {
            if (slot == nullptr) {
                slot = c;
                break;
            }
        }
    }
    else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
        SDL_GameController* c = SDL_GameControllerFromInstanceID(ev.cdevice.which);
        for (auto& slot : controllers) {
            if (slot == c) {
                slot = nullptr;
            }
        }
        SDL_GameControllerClose(c);
    }
}

// Called from the game's controller thread; reads the state the window thread
// last saw (SDL keyboard/controller state is updated by event pumping).
void winback::input::poll() {
    std::array<PadState, max_players> next{};
    next[0] = read_keyboard();
    next[0].buttons |= autopress_buttons();
    for (int i = 0; i < max_players; i++) {
        if (controllers[i] == nullptr) {
            continue;
        }
        PadState c = read_controller(controllers[i]);
        next[i].buttons |= c.buttons;
        if (std::fabs(c.x) + std::fabs(c.y) > std::fabs(next[i].x) + std::fabs(next[i].y)) {
            next[i].x = c.x;
            next[i].y = c.y;
        }
    }
    std::lock_guard lock{state_mutex};
    pads = next;
}

bool winback::input::get(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num < 0 || controller_num >= max_players) {
        return false;
    }
    std::lock_guard lock{state_mutex};
    *buttons = pads[controller_num].buttons;
    *x = pads[controller_num].x;
    *y = pads[controller_num].y;
    return true;
}

void winback::input::set_rumble(int controller_num, bool rumble) {
    if (controller_num >= 0 && controller_num < max_players && controllers[controller_num]) {
        SDL_GameControllerRumble(controllers[controller_num], rumble ? 0xC000 : 0, rumble ? 0xC000 : 0, rumble ? 1000 : 0);
    }
}

ultramodern::input::connected_device_info_t winback::input::get_device_info(int controller_num) {
    // Port 1 is always connected (keyboard); other ports need a gamepad.
    bool connected = controller_num == 0 ||
        (controller_num < max_players && controllers[controller_num] != nullptr);
    return {
        .connected_device = connected ? ultramodern::input::Device::Controller : ultramodern::input::Device::None,
        .connected_pak = connected ? ultramodern::input::Pak::RumblePak : ultramodern::input::Pak::None,
    };
}
