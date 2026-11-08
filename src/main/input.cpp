// Keyboard and gamepad input mapped to N64 controllers.
//
// Player 1: keyboard plus the first gamepad. Players 2-4: further gamepads
// (split-screen modes will use these).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include <json/json.hpp>

#include "sdl_include.hpp"
#include "input.hpp"
#include "menu.hpp"
#include "settings_file.hpp"

namespace winback::input {

NLOHMANN_JSON_SERIALIZE_ENUM(Preset, {
    {Preset::PS2, "PS2"},
    {Preset::N64, "N64"},
    {Preset::Custom, "Custom"},
});

NLOHMANN_JSON_SERIALIZE_ENUM(Bind, {
    {Bind::None, "None"},
    {Bind::A, "A"},
    {Bind::B, "B"},
    {Bind::Z, "Z"},
    {Bind::R, "R"},
    {Bind::L, "L"},
    {Bind::Start, "Start"},
    {Bind::CUp, "CUp"},
    {Bind::CDown, "CDown"},
    {Bind::CLeft, "CLeft"},
    {Bind::CRight, "CRight"},
    {Bind::DUp, "DUp"},
    {Bind::DDown, "DDown"},
    {Bind::DLeft, "DLeft"},
    {Bind::DRight, "DRight"},
    {Bind::CameraReset, "CameraReset"},
    {Bind::LookLeft, "LookLeft"},
    {Bind::LookRight, "LookRight"},
});

NLOHMANN_JSON_SERIALIZE_ENUM(RightStick, {
    {RightStick::Camera, "Camera"},
    {RightStick::CButtons, "CButtons"},
    {RightStick::None, "None"},
});

NLOHMANN_JSON_SERIALIZE_ENUM(MenuButtons, {
    {MenuButtons::AB, "AB"},
    {MenuButtons::AsPlay, "AsPlay"},
});

} // namespace winback::input

namespace {

using namespace winback::input;

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
    // Right stick X, and the C-Left/C-Right it presses for code that only
    // reads buttons. The player camera reads the stick itself.
    float cam_x = 0.0f;
    float cam_y = 0.0f;
    uint16_t cam_buttons = 0;
    // For mods: the right stick as held, whatever the layout does with it.
    float right_x = 0.0f;
    float right_y = 0.0f;
    // For mods: the controller's own buttons (SDL button bits and these two for
    // the triggers), and the N64 buttons pressed by the keyboard or a script.
    uint32_t pad_buttons = 0;
    uint16_t other_buttons = 0;
};

constexpr uint32_t PAD_LT = 1u << 30, PAD_RT = 1u << 31;

// A mod's layout: what the game reads during normal play once a mod has set it.
struct ModPad {
    bool active = false;
    uint16_t buttons = 0;
    float x = 0.0f;
    float y = 0.0f;
};

std::mutex state_mutex;
std::array<PadState, max_players> pads{};
std::array<SDL_GameController*, max_players> controllers{};
std::array<ModPad, max_players> mod_pads{};

float axis(SDL_GameController* c, SDL_GameControllerAxis a) {
    float v = SDL_GameControllerGetAxis(c, a) / 32767.0f;
    constexpr float deadzone = 0.15f;
    if (std::fabs(v) < deadzone) {
        return 0.0f;
    }
    return std::clamp((v - std::copysign(deadzone, v)) / (1.0f - deadzone), -1.0f, 1.0f);
}

// Right stick click snaps the camera behind the player. A press is held here
// until the game's camera update takes it (hook in config/winback.us.toml).
std::array<bool, max_players> rear_snap_held{};
std::array<std::atomic<bool>, max_players> rear_snap_pending{};

// Whether the game is in normal play rather than a menu, set by the game thread
// each frame from the game's own state (winback_level_frame). Controllers use
// the gameplay layout during all of normal play, whatever the camera is doing.
std::atomic<bool> gameplay_input{false};

bool in_gameplay() {
    return gameplay_input.load(std::memory_order_relaxed);
}

// Controller buttons pressed by WINBACK_AUTOPRESS.
uint32_t scripted_pad = 0;
bool scripted_pad_used = false;

// The N64 buttons each Bind presses (none for None and CameraReset; LookLeft
// and LookRight depend on the game's camera option, see look_buttons).
constexpr uint16_t bind_masks[] = {
    0, BTN_A, BTN_B, BTN_Z, BTN_R, BTN_L, BTN_START, BTN_CU, BTN_CD, BTN_CL, BTN_CR,
    BTN_DU, BTN_DD, BTN_DL, BTN_DR, 0, 0, 0,
};

// Whether C-Right turns the view left for each controller: the game's own
// camera option (bit 1 of D_8015D6AC clear; it's set by default, so C-Left
// looks left), reported each frame by the camera update (winback_camera_stick).
std::array<std::atomic<bool>, max_players> c_right_looks_left{};

struct LookButtons {
    uint16_t left, right;
};

LookButtons look_buttons(int player) {
    return c_right_looks_left[player].load(std::memory_order_relaxed) ? LookButtons{BTN_CR, BTN_CL}
                                                                      : LookButtons{BTN_CL, BTN_CR};
}
static_assert(std::size(bind_masks) == size_t(Bind::Count));

// Each PadInput's bit in PadState::pad_buttons.
constexpr uint32_t pad_bits[PadCount] = {
    1u << SDL_CONTROLLER_BUTTON_A, 1u << SDL_CONTROLLER_BUTTON_B,
    1u << SDL_CONTROLLER_BUTTON_X, 1u << SDL_CONTROLLER_BUTTON_Y,
    1u << SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 1u << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    PAD_LT, PAD_RT,
    1u << SDL_CONTROLLER_BUTTON_START,
    1u << SDL_CONTROLLER_BUTTON_LEFTSTICK, 1u << SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    1u << SDL_CONTROLLER_BUTTON_DPAD_UP, 1u << SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    1u << SDL_CONTROLLER_BUTTON_DPAD_LEFT, 1u << SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
};

// The chosen controls (the window thread writes them, the controller thread
// reads them).
std::mutex controls_mutex;
ControlSettings controls;

// One controller through the given layout.
PadState read_controller(SDL_GameController* c, int player, const Layout& layout) {
    PadState s{};
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        if (SDL_GameControllerGetButton(c, SDL_GameControllerButton(b))) s.pad_buttons |= 1u << b;
    }
    if (axis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.5f) s.pad_buttons |= PAD_LT;
    if (axis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f) s.pad_buttons |= PAD_RT;
    s.pad_buttons |= scripted_pad;
    bool play = in_gameplay();
    // In the game's menus A and B can be fixed to confirm and back. Other
    // buttons that press A or B in play then do nothing there.
    bool menu_ab = !play && layout.menus == MenuButtons::AB;
    bool snap = false;
    for (int i = 0; i < PadCount; i++) {
        if (!(s.pad_buttons & pad_bits[i])) {
            continue;
        }
        Bind b = layout.buttons[i];
        if (menu_ab) {
            if (i == PadA) b = Bind::A;
            else if (i == PadB) b = Bind::B;
            else if (b == Bind::A || b == Bind::B) continue;
        }
        snap |= b == Bind::CameraReset;
        s.buttons |= bind_masks[size_t(b)];
        if (b == Bind::LookLeft) s.buttons |= look_buttons(player).left;
        if (b == Bind::LookRight) s.buttons |= look_buttons(player).right;
    }
    // Camera behind the player: once per press, during play.
    if (snap && !rear_snap_held[player] && play) {
        rear_snap_pending[player] = true;
    }
    rear_snap_held[player] = snap;
    s.right_x = axis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    s.right_y = -axis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    switch (layout.right_stick) {
        case RightStick::Camera:
            // Stick right turns the view right.
            s.cam_x = s.right_x;
            s.cam_y = s.right_y;
            if (s.cam_x > 0.5f) s.cam_buttons |= look_buttons(player).right;
            if (s.cam_x < -0.5f) s.cam_buttons |= look_buttons(player).left;
            break;
        case RightStick::CButtons:
            if (s.right_x > 0.5f) s.buttons |= BTN_CR;
            if (s.right_x < -0.5f) s.buttons |= BTN_CL;
            if (s.right_y > 0.5f) s.buttons |= BTN_CU;
            if (s.right_y < -0.5f) s.buttons |= BTN_CD;
            break;
        default:
            break;
    }
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
// Buttons: a b z start l r du dd dl dr cu cd cl cr, the left stick su sd sl sr
// and the right stick ru rd rl rr (held fully over). The controller's own
// buttons, which go through the current layout: pad_a pad_b pad_x pad_y pad_lb
// pad_rb pad_lt pad_rt pad_dl pad_dr pad_dd pad_start.
// WINBACK_AUTOPRESS2 does the same for player 2 (N64 buttons and sticks only),
// and makes port 2 count as connected.
enum AutoStick : uint16_t { AUTO_SU = 1, AUTO_SD, AUTO_SL, AUTO_SR, AUTO_RU, AUTO_RD, AUTO_RL, AUTO_RR };

struct AutoPress {
    uint16_t mask;
    uint16_t stick;
    double at;
    double hold;
    uint32_t pad = 0;
};

std::vector<AutoPress> load_autopress(int player) {
    std::vector<AutoPress> out;
    const char* env = std::getenv(player == 0 ? "WINBACK_AUTOPRESS" : "WINBACK_AUTOPRESS2");
    if (env == nullptr) {
        return out;
    }
    static const std::pair<const char*, uint16_t> names[] = {
        {"a", BTN_A}, {"b", BTN_B}, {"z", BTN_Z}, {"start", BTN_START}, {"l", BTN_L}, {"r", BTN_R},
        {"du", BTN_DU}, {"dd", BTN_DD}, {"dl", BTN_DL}, {"dr", BTN_DR},
        {"cu", BTN_CU}, {"cd", BTN_CD}, {"cl", BTN_CL}, {"cr", BTN_CR},
    };
    static const std::pair<const char*, AutoStick> stick_names[] = {
        {"su", AUTO_SU}, {"sd", AUTO_SD}, {"sl", AUTO_SL}, {"sr", AUTO_SR},
        {"ru", AUTO_RU}, {"rd", AUTO_RD}, {"rl", AUTO_RL}, {"rr", AUTO_RR},
    };
    static const std::pair<const char*, uint32_t> pad_names[] = {
        {"pad_a", 1u << SDL_CONTROLLER_BUTTON_A}, {"pad_b", 1u << SDL_CONTROLLER_BUTTON_B},
        {"pad_x", 1u << SDL_CONTROLLER_BUTTON_X}, {"pad_y", 1u << SDL_CONTROLLER_BUTTON_Y},
        {"pad_lb", 1u << SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, {"pad_rb", 1u << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
        {"pad_dl", 1u << SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {"pad_dr", 1u << SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
        {"pad_dd", 1u << SDL_CONTROLLER_BUTTON_DPAD_DOWN}, {"pad_start", 1u << SDL_CONTROLLER_BUTTON_START},
        {"pad_lt", PAD_LT}, {"pad_rt", PAD_RT},
    };
    std::string spec = env;
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t at = item.find('@');
        if (at != std::string::npos) {
            std::string name = item.substr(0, at);
            size_t plus = item.find('+', at);
            double hold = plus == std::string::npos ? 0.15 : std::atof(item.c_str() + plus + 1);
            double when = std::atof(item.c_str() + at + 1);
            for (const auto& [n, mask] : names) {
                if (name == n) {
                    out.push_back({mask, 0, when, hold});
                }
            }
            for (const auto& [n, stick] : stick_names) {
                if (name == n) {
                    out.push_back({0, stick, when, hold});
                }
            }
            for (const auto& [n, pad] : pad_names) {
                if (name == n && player == 0) {
                    out.push_back({0, 0, when, hold, pad});
                    scripted_pad_used = true;
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

bool scripted_player2() {
    static const bool on = std::getenv("WINBACK_AUTOPRESS2") != nullptr;
    return on;
}

void apply_autopress(PadState& s, int player) {
    static const std::vector<AutoPress> all_presses[2] = {load_autopress(0), load_autopress(1)};
    static const auto start = std::chrono::steady_clock::now();
    const std::vector<AutoPress>& presses = all_presses[player];
    if (player == 0) {
        scripted_pad = 0;
    }
    if (presses.empty()) {
        return;
    }
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    for (const AutoPress& p : presses) {
        if (t >= p.at && t < p.at + p.hold) {
            s.buttons |= p.mask;
            if (player == 0) {
                scripted_pad |= p.pad;
            }
            switch (p.stick) {
                case AUTO_SU: s.y = 1.0f; break;
                case AUTO_SD: s.y = -1.0f; break;
                case AUTO_SL: s.x = -1.0f; break;
                case AUTO_SR: s.x = 1.0f; break;
                case AUTO_RU: s.cam_y = s.right_y = 1.0f; break;
                case AUTO_RD: s.cam_y = s.right_y = -1.0f; break;
                case AUTO_RL: s.cam_x = s.right_x = -1.0f; break;
                case AUTO_RR: s.cam_x = s.right_x = 1.0f; break;
                default: break;
            }
        }
    }
}

} // namespace

// Called by the game thread each frame.
extern "C" void winback_set_gameplay_input(int active) {
    gameplay_input.store(active != 0, std::memory_order_relaxed);
}

// Called by the game thread's camera update, which says whether C-Left turns
// the view left under the game's camera option: the right stick's X position
// for analog camera turning (positive turns the view right), or 0 while
// C-Left/C-Right is pressed some other way (triggers, keyboard, scripted
// presses), so that those turn at full speed.
extern "C" float winback_camera_stick(int controller_num, int c_left_left) {
    if (controller_num < 0 || controller_num >= max_players) {
        return 0.0f;
    }
    c_right_looks_left[controller_num].store(c_left_left == 0, std::memory_order_relaxed);
    std::lock_guard lock{state_mutex};
    const PadState& p = pads[controller_num];
    return (p.buttons & (BTN_CL | BTN_CR)) ? 0.0f : p.cam_x;
}

// Called by the game thread's camera update; true once per right stick click.
extern "C" int winback_take_rear_snap(int controller_num) {
    if (controller_num < 0 || controller_num >= max_players) {
        return 0;
    }
    bool pressed = rear_snap_pending[controller_num].exchange(false);
    std::lock_guard lock{state_mutex};
    return pressed && !mod_pads[controller_num].active;
}

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
    Layout layout;
    {
        std::lock_guard lock{controls_mutex};
        layout = active_layout(controls);
    }
    std::array<PadState, max_players> next{};
    next[0] = read_keyboard();
    apply_autopress(next[0], 0);
    next[0].other_buttons = next[0].buttons;
    if (max_players > 1 && scripted_player2()) {
        apply_autopress(next[1], 1);
        next[1].other_buttons = next[1].buttons;
    }
    for (int i = 0; i < max_players; i++) {
        if (controllers[i] == nullptr && (i != 0 || !scripted_pad_used)) {
            continue;
        }
        PadState c = read_controller(controllers[i], i, layout);
        next[i].buttons |= c.buttons;
        next[i].cam_buttons |= c.cam_buttons;
        next[i].pad_buttons |= c.pad_buttons;
        if (std::fabs(c.cam_x) > std::fabs(next[i].cam_x)) {
            next[i].cam_x = c.cam_x;
        }
        if (std::fabs(c.cam_y) > std::fabs(next[i].cam_y)) {
            next[i].cam_y = c.cam_y;
        }
        if (std::fabs(c.right_x) > std::fabs(next[i].right_x)) {
            next[i].right_x = c.right_x;
        }
        if (std::fabs(c.right_y) > std::fabs(next[i].right_y)) {
            next[i].right_y = c.right_y;
        }
        if (std::fabs(c.x) + std::fabs(c.y) > std::fabs(next[i].x) + std::fabs(next[i].y)) {
            next[i].x = c.x;
            next[i].y = c.y;
        }
    }
    // The game gets no input while the overlay menu is open, nor afterwards
    // until everything is released (the press that closed the menu).
    static bool menu_guard = false;
    if (winback::menu::is_open()) {
        menu_guard = true;
    }
    if (menu_guard) {
        bool idle = true;
        for (const PadState& p : next) {
            idle &= p.buttons == 0 && p.cam_buttons == 0 && p.pad_buttons == 0 && p.x == 0.0f && p.y == 0.0f
                && p.cam_x == 0.0f && p.cam_y == 0.0f && p.right_x == 0.0f && p.right_y == 0.0f;
        }
        menu_guard = winback::menu::is_open() || !idle;
        next = {};
    }
    std::lock_guard lock{state_mutex};
    pads = next;
}

bool winback::input::get(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num < 0 || controller_num >= max_players) {
        return false;
    }
    std::lock_guard lock{state_mutex};
    *buttons = pads[controller_num].buttons | pads[controller_num].cam_buttons;
    *x = pads[controller_num].x;
    *y = pads[controller_num].y;
    if (mod_pads[controller_num].active && in_gameplay()) {
        *buttons = mod_pads[controller_num].buttons;
        *x = mod_pads[controller_num].x;
        *y = mod_pads[controller_num].y;
    }
    return true;
}

winback::input::RawPad winback::input::read_raw(int controller_num) {
    if (controller_num < 0 || controller_num >= max_players) {
        return {};
    }
    std::lock_guard lock{state_mutex};
    const PadState& p = pads[controller_num];
    return {p.pad_buttons, p.other_buttons, p.x, p.y, p.right_x, p.right_y};
}

void winback::input::set_mod_pad(int controller_num, uint16_t buttons, float x, float y) {
    if (controller_num >= 0 && controller_num < max_players) {
        std::lock_guard lock{state_mutex};
        mod_pads[controller_num] = {true, buttons, x, y};
    }
}

void winback::input::set_rumble(int controller_num, bool rumble) {
    if (controller_num >= 0 && controller_num < max_players && controllers[controller_num]) {
        SDL_GameControllerRumble(controllers[controller_num], rumble ? 0xC000 : 0, rumble ? 0xC000 : 0, rumble ? 1000 : 0);
    }
}

ultramodern::input::connected_device_info_t winback::input::get_device_info(int controller_num) {
    // Port 1 is always connected (keyboard); other ports need a gamepad.
    bool connected = controller_num == 0 || (controller_num == 1 && scripted_player2()) ||
        (controller_num < max_players && controllers[controller_num] != nullptr);
    return {
        .connected_device = connected ? ultramodern::input::Device::Controller : ultramodern::input::Device::None,
        .connected_pak = connected ? ultramodern::input::Pak::RumblePak : ultramodern::input::Pak::None,
    };
}

// --- Controls ----------------------------------------------------------------

winback::input::Layout winback::input::preset_layout(Preset preset) {
    Layout l{};
    if (preset == Preset::N64) {
        // The recomp's first layout, from before the PS2 one: the N64 pad's
        // buttons where an Xbox pad has them, and the same buttons in menus as
        // in play.
        l.buttons[PadA] = Bind::A;
        l.buttons[PadB] = Bind::CDown;
        l.buttons[PadX] = Bind::B;
        l.buttons[PadY] = Bind::CLeft;
        l.buttons[PadLB] = Bind::L;
        l.buttons[PadRB] = Bind::R;
        l.buttons[PadLT] = Bind::Z;
        l.buttons[PadRT] = Bind::R;
        l.right_stick = RightStick::CButtons;
        l.menus = MenuButtons::AsPlay;
    }
    else {
        // The PlayStation 2 release's default layout.
        l.buttons[PadA] = Bind::B;
        l.buttons[PadB] = Bind::CUp;
        l.buttons[PadX] = Bind::A;
        l.buttons[PadY] = Bind::CDown;
        l.buttons[PadLB] = Bind::Z;
        l.buttons[PadRB] = Bind::R;
        l.buttons[PadLT] = Bind::LookLeft;
        l.buttons[PadRT] = Bind::LookRight;
        l.buttons[PadR3] = Bind::CameraReset;
        l.right_stick = RightStick::Camera;
        l.menus = MenuButtons::AB;
    }
    l.buttons[PadStart] = Bind::Start;
    l.buttons[PadDUp] = Bind::DUp;
    l.buttons[PadDDown] = Bind::DDown;
    l.buttons[PadDLeft] = Bind::DLeft;
    l.buttons[PadDRight] = Bind::DRight;
    return l;
}

winback::input::Layout winback::input::active_layout(const ControlSettings& settings) {
    return settings.preset == Preset::Custom ? settings.custom : preset_layout(settings.preset);
}

namespace {

// settings.json keys for the custom layout's buttons, in PadInput order.
const char* const pad_keys[PadCount] = {
    "a", "b", "x", "y", "lb", "rb", "lt", "rt", "start", "l3", "r3",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

} // namespace

void winback::input::load_controls() {
    ControlSettings c;
    nlohmann::json j = winback::settings_file::read();
    try {
        c.preset = j.value("controls", c.preset);
        auto cc = j.find("custom_controls");
        if (cc != j.end() && cc->is_object()) {
            for (int i = 0; i < PadCount; i++) {
                c.custom.buttons[i] = cc->value(pad_keys[i], c.custom.buttons[i]);
            }
            c.custom.right_stick = cc->value("right_stick", c.custom.right_stick);
            c.custom.menus = cc->value("game_menus", c.custom.menus);
        }
    }
    catch (const nlohmann::json::exception& e) {
        std::fprintf(stderr, "[input] ignoring controls in settings.json: %s\n", e.what());
    }
    std::lock_guard lock{controls_mutex};
    controls = c;
}

winback::input::ControlSettings winback::input::get_controls() {
    std::lock_guard lock{controls_mutex};
    return controls;
}

void winback::input::set_controls(const ControlSettings& c) {
    {
        std::lock_guard lock{controls_mutex};
        controls = c;
    }
    nlohmann::json cc;
    for (int i = 0; i < PadCount; i++) {
        cc[pad_keys[i]] = c.custom.buttons[i];
    }
    cc["right_stick"] = c.custom.right_stick;
    cc["game_menus"] = c.custom.menus;
    nlohmann::json j;
    j["controls"] = c.preset;
    j["custom_controls"] = cc;
    winback::settings_file::update(j);
}
