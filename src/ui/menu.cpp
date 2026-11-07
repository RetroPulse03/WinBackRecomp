// The recomp's overlay menu, drawn with RmlUi on top of the game.
//
// The window thread turns controller and keyboard input into menu actions
// and updates the menu state; RT64's present thread draws it through the
// render hooks (ui_renderer.cpp), rebuilding the visible rows whenever the
// state changes. Entries: Level Select (stage, then stage start or
// checkpoint), Display (window mode and renderer options), Controls
// (controller layout), Mods (on/off, settings and restarting to apply),
// Return to Game and Exit Game.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "RmlUi/Core.h"

#include "rt64_render_hooks.h"
#include "rt64_render_interface.h"

#include "sdl_include.hpp"
#include "menu.hpp"
#include "ui_renderer.hpp"
#include "level_select.hpp"
#include "display.hpp"
#include "input.hpp"
#include "app.hpp"
#include "mods.hpp"
#include "ultramodern/ultramodern.hpp"

#include "Lato_Bold_ttf.h"
#include "Lato_BoldItalic_ttf.h"
#include "Lato_BlackItalic_ttf.h"
#include "menu_rml.h"

namespace {

enum class Screen { Main, Stages, Checkpoints, Display, Controls, Mods, ModSettings, ConfirmRestart, ConfirmExit };
enum class Action { Toggle, Up, Down, Left, Right, Accept, Back };

constexpr int visible_rows = 10;
enum MainItem { MainLevelSelect, MainDisplay, MainControls, MainMods, MainReturn, MainExit };
const char* const main_items[] = {"Level Select", "Display", "Controls", "Mods", "Return to Game", "Exit Game"};
const char* const exit_items[] = {"Exit Game", "Cancel"};
const char* const restart_items[] = {"Restart", "Cancel"};

// Display screen rows. Each option's values are its enum's values in order;
// Off/On options are bools.
enum DisplayItem { DisplayWindowMode, DisplayResolution, DisplayWidescreen, DisplayCrop, DisplayInteger, DisplayAntialiasing };
struct DisplayOption {
    const char* name;
    const char* note;
    std::vector<const char*> values;
};
const DisplayOption display_options[] = {
    {"Window Mode", "Borderless covers the screen with a window and switches away instantly. Fullscreen"
                    " takes over the screen; it can help with stutter on some PCs. F11 or Alt+Enter also"
                    " switch between windowed and full screen.",
     {"Windowed", "Borderless", "Fullscreen"}},
    {"Resolution", "How sharply 3D is drawn. Original keeps the N64's look: 240 lines, or 216 with the"
                   " overscan cropped. Double draws twice as many. Match Window draws at the window's size.",
     {"Original", "Double", "Match Window"}},
    {"Widescreen", "On shows a 16:9 picture with more of the world at the sides. Off keeps the N64's 4:3.",
     {"Off", "On"}},
    {"Crop Overscan", "WinBack leaves a black border around its picture, which old TVs hid past the edge"
                      " of the screen. On cuts the border off so the picture fills more of the window.",
     {"Off", "On"}},
    {"Integer Scale", "On enlarges the picture only in whole steps, so every game pixel is the same size."
                      " The rest of the window stays black.",
     {"Off", "On"}},
    {"Anti-Aliasing", "Smooths jagged edges on 3D models. Higher settings cost more graphics power.",
     {"Off", "2x MSAA", "4x MSAA", "8x MSAA"}},
};

int display_value(const winback::display::Settings& d, int option) {
    switch (option) {
        case DisplayWindowMode: return int(d.window_mode);
        case DisplayResolution: return int(d.resolution);
        case DisplayWidescreen: return int(d.widescreen);
        case DisplayCrop: return int(d.crop_overscan);
        case DisplayInteger: return int(d.integer_scale);
        default: return int(d.antialiasing);
    }
}

// The value as shown. Original and Double give the number of lines shown, which
// cropping the overscan reduces from 240 to the 216 the game draws.
std::string display_label(const winback::display::Settings& d, int option) {
    int v = display_value(d, option);
    std::string label = display_options[option].values[v];
    if (option == DisplayResolution && v <= int(ultramodern::renderer::Resolution::Original2x)) {
        int lines = d.crop_overscan ? 216 : 240;
        label += " (" + std::to_string(lines * (v + 1)) + "p)";
    }
    return label;
}

void set_display_value(winback::display::Settings& d, int option, int value) {
    namespace R = ultramodern::renderer;
    switch (option) {
        case DisplayWindowMode: d.window_mode = winback::display::WindowMode(value); break;
        case DisplayResolution: d.resolution = R::Resolution(value); break;
        case DisplayWidescreen: d.widescreen = value != 0; break;
        case DisplayCrop: d.crop_overscan = value != 0; break;
        case DisplayInteger: d.integer_scale = value != 0; break;
        default: d.antialiasing = R::Antialiasing(value); break;
    }
}

// Controls screen rows: the preset, each controller button, the right stick and
// the game's menus, then (for Custom only) the two resets.
namespace In = winback::input;
constexpr int ControlsPreset = 0;
constexpr int ControlsFirstButton = 1;
constexpr int ControlsRightStick = ControlsFirstButton + In::PadCount;
constexpr int ControlsMenus = ControlsRightStick + 1;
constexpr int ControlsResetPS2 = ControlsMenus + 1;
constexpr int ControlsResetN64 = ControlsResetPS2 + 1;

int controls_rows(const In::ControlSettings& c) {
    return c.preset == In::Preset::Custom ? ControlsResetN64 + 1 : ControlsMenus + 1;
}

const char* const preset_names[] = {"PS2 Style", "N64 Style", "Custom"};
static_assert(std::size(preset_names) == size_t(In::Preset::Count));
// What each N64 button does in play, where it's known.
const char* const bind_names[] = {
    "None", "A (Action)", "B (Reload)", "Z (Crouch)", "R (Aim)", "L", "Start (Pause)",
    "C-Up (Weapon)", "C-Down (Lock On)", "C-Left (Camera)", "C-Right (Camera)",
    "D-Up", "D-Down (C4)", "D-Left", "D-Right", "Camera Behind",
};
static_assert(std::size(bind_names) == size_t(In::Bind::Count));
const char* const stick_names[] = {"Camera", "C Buttons", "None"};
static_assert(std::size(stick_names) == size_t(In::RightStick::Count));

// Steps an enum value by delta, wrapping around.
template <typename E>
void step_enum(E& v, int delta) {
    int n = int(E::Count);
    v = E((int(v) + delta + n) % n);
}

struct State {
    bool open = false;
    Screen screen = Screen::Main;
    int main_sel = 0;
    int stage_sel = 0;
    int row_sel = 0;     // Checkpoint screen: 0 = stage start, then checkpoints.
    int display_sel = 0;
    int exit_sel = 1;
    winback::display::Settings display;
    int controls_sel = 0;
    winback::input::ControlSettings controls;
    int mods_sel = 0;    // Mods, then broken mods, then Restart.
    int option_sel = 0;  // Mod settings: 0 = on/off, then the mod's options.
    int restart_sel = 1;
    std::shared_ptr<const std::vector<winback::mods::Mod>> mods;
    std::shared_ptr<const std::vector<winback::mods::BrokenMod>> broken_mods;
    bool restart_needed = false;
    uint64_t version = 0;
};

// Work done after the state lock is released.
struct Effects {
    bool display = false;
    winback::display::Settings display_settings;
    bool controls = false;
    winback::input::ControlSettings controls_settings;
    bool quit = false;
    bool restart = false;
};

std::mutex state_mutex;
State state;
std::atomic<bool> open_flag{false};

int row_count(const State& s) {
    switch (s.screen) {
        case Screen::Main: return int(std::size(main_items));
        case Screen::Stages: return int(winback::levels::stages().size());
        case Screen::Checkpoints: return 1 + int(winback::levels::stages()[s.stage_sel].checkpoints.size());
        case Screen::Display: return int(std::size(display_options));
        case Screen::Controls: return controls_rows(s.controls);
        case Screen::Mods: return int(s.mods->size() + s.broken_mods->size()) + 1;
        case Screen::ModSettings: return 1 + int((*s.mods)[s.mods_sel].options.size());
        case Screen::ConfirmRestart: return int(std::size(restart_items));
        case Screen::ConfirmExit: return int(std::size(exit_items));
    }
    return 0;
}

int& selection(State& s) {
    switch (s.screen) {
        case Screen::Main: return s.main_sel;
        case Screen::Stages: return s.stage_sel;
        case Screen::Display: return s.display_sel;
        case Screen::Controls: return s.controls_sel;
        case Screen::Mods: return s.mods_sel;
        case Screen::ModSettings: return s.option_sel;
        case Screen::ConfirmRestart: return s.restart_sel;
        case Screen::ConfirmExit: return s.exit_sel;
        default: return s.row_sel;
    }
}

void set_open(State& s, bool open) {
    s.open = open;
    if (open) {
        s.screen = Screen::Main;
    }
    open_flag = open;
}

// Steps the selected display option by delta, wrapping around.
void change_display(State& s, int delta, Effects& fx) {
    const DisplayOption& opt = display_options[s.display_sel];
    int n = int(opt.values.size());
    int v = (display_value(s.display, s.display_sel) + delta + n) % n;
    set_display_value(s.display, s.display_sel, v);
    fx.display = true;
    fx.display_settings = s.display;
}

// Steps the selected controls row by delta. Only the preset can change unless
// it is Custom.
void change_controls(State& s, int delta, Effects& fx) {
    In::ControlSettings& c = s.controls;
    int row = s.controls_sel;
    if (row == ControlsPreset) {
        step_enum(c.preset, delta);
    }
    else if (c.preset != In::Preset::Custom) {
        return;
    }
    else if (row < ControlsRightStick) {
        step_enum(c.custom.buttons[row - ControlsFirstButton], delta);
    }
    else if (row == ControlsRightStick) {
        step_enum(c.custom.right_stick, delta);
    }
    else if (row == ControlsMenus) {
        step_enum(c.custom.menus, delta);
    }
    else {
        return;
    }
    fx.controls = true;
    fx.controls_settings = c;
}

// Accept on the Controls screen: the resets, or the next value.
void accept_controls(State& s, Effects& fx) {
    if (s.controls_sel == ControlsResetPS2 || s.controls_sel == ControlsResetN64) {
        s.controls.custom = In::preset_layout(s.controls_sel == ControlsResetPS2 ? In::Preset::PS2 : In::Preset::N64);
        fx.controls = true;
        fx.controls_settings = s.controls;
    }
    else {
        change_controls(s, 1, fx);
    }
}

void refresh_mods(State& s) {
    s.mods = std::make_shared<const std::vector<winback::mods::Mod>>(winback::mods::list());
    s.broken_mods = std::make_shared<const std::vector<winback::mods::BrokenMod>>(winback::mods::broken());
    s.restart_needed = winback::mods::restart_needed();
}

bool on_mod_row(const State& s) {
    return s.mods_sel < int(s.mods->size());
}

// Flips the selected mod on or off, or steps the selected mod setting.
void change_mod(State& s, int delta) {
    const winback::mods::Mod& mod = (*s.mods)[s.mods_sel];
    if (s.screen == Screen::Mods || s.option_sel == 0) {
        winback::mods::set_enabled(mod.id, !mod.enabled);
    }
    else if (!mod.options[s.option_sel - 1].disabled) {
        winback::mods::step_option(mod.id, mod.options[s.option_sel - 1].id, delta);
    }
    refresh_mods(s);
    // A setting can hide others.
    s.option_sel = std::min(s.option_sel, int((*s.mods)[s.mods_sel].options.size()));
}

Effects apply_locked(Action a) {
    Effects fx;
    State& s = state;
    if (a == Action::Toggle) {
        set_open(s, !s.open);
        s.version++;
        return fx;
    }
    if (!s.open) {
        return fx;
    }
    int count = row_count(s);
    int& sel = selection(s);
    switch (a) {
        case Action::Up: sel = (sel + count - 1) % count; break;
        case Action::Down: sel = (sel + 1) % count; break;
        case Action::Left:
        case Action::Right:
            if (s.screen == Screen::Display) change_display(s, a == Action::Left ? -1 : 1, fx);
            else if (s.screen == Screen::Controls) change_controls(s, a == Action::Left ? -1 : 1, fx);
            else if (s.screen == Screen::ModSettings || (s.screen == Screen::Mods && on_mod_row(s))) {
                change_mod(s, a == Action::Left ? -1 : 1);
            }
            else if (s.screen == Screen::Mods) break;
            else if (a == Action::Left) sel = std::max(sel - visible_rows, 0);
            else sel = std::min(sel + visible_rows, count - 1);
            break;
        case Action::Back:
            if (s.screen == Screen::Main) set_open(s, false);
            else if (s.screen == Screen::Checkpoints) s.screen = Screen::Stages;
            else if (s.screen == Screen::ModSettings || s.screen == Screen::ConfirmRestart) s.screen = Screen::Mods;
            else s.screen = Screen::Main;
            break;
        case Action::Accept:
            if (s.screen == Screen::Main) {
                switch (s.main_sel) {
                    case MainLevelSelect: s.screen = Screen::Stages; break;
                    case MainDisplay:
                        s.screen = Screen::Display;
                        s.display = winback::display::get();
                        break;
                    case MainControls:
                        s.screen = Screen::Controls;
                        s.controls = winback::input::get_controls();
                        s.controls_sel = 0;
                        break;
                    case MainMods:
                        s.screen = Screen::Mods;
                        s.mods_sel = 0;
                        refresh_mods(s);
                        break;
                    case MainExit:
                        s.screen = Screen::ConfirmExit;
                        s.exit_sel = 1;
                        break;
                    default: set_open(s, false); break;
                }
            }
            else if (s.screen == Screen::Display) {
                change_display(s, 1, fx);
            }
            else if (s.screen == Screen::Controls) {
                accept_controls(s, fx);
            }
            else if (s.screen == Screen::Mods) {
                if (on_mod_row(s)) {
                    s.screen = Screen::ModSettings;
                    s.option_sel = 0;
                }
                else if (s.mods_sel == count - 1) {
                    s.screen = Screen::ConfirmRestart;
                    s.restart_sel = 1;
                }
            }
            else if (s.screen == Screen::ModSettings) {
                change_mod(s, 1);
            }
            else if (s.screen == Screen::ConfirmRestart) {
                if (s.restart_sel == 0) fx.restart = true;
                else s.screen = Screen::Mods;
            }
            else if (s.screen == Screen::ConfirmExit) {
                if (s.exit_sel == 0) fx.quit = true;
                else s.screen = Screen::Main;
            }
            else if (s.screen == Screen::Stages) {
                s.screen = Screen::Checkpoints;
                s.row_sel = 0;
            }
            else {
                winback::levels::request_jump(s.stage_sel, s.row_sel - 1);
                set_open(s, false);
            }
            break;
        default:
            break;
    }
    s.version++;
    return fx;
}

void apply(Action a) {
    Effects fx;
    {
        std::lock_guard lock{state_mutex};
        fx = apply_locked(a);
    }
    if (fx.display) {
        winback::display::set(fx.display_settings);
    }
    if (fx.controls) {
        winback::input::set_controls(fx.controls_settings);
    }
    if (fx.quit) {
        std::printf("[menu] exit game\n");
        winback::exit_app(false);
    }
    if (fx.restart) {
        std::printf("[menu] restart\n");
        winback::exit_app(true);
    }
}

// --- Input (window thread) ---------------------------------------------------

// A held direction repeats after a pause, like a keyboard.
struct Repeat {
    bool held = false;
    std::chrono::steady_clock::time_point next{};
};

bool direction_held(Action a) {
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    SDL_Scancode key = a == Action::Up ? SDL_SCANCODE_UP : a == Action::Down ? SDL_SCANCODE_DOWN
                     : a == Action::Left ? SDL_SCANCODE_LEFT : SDL_SCANCODE_RIGHT;
    if (k[key]) {
        return true;
    }
    SDL_GameControllerButton button = a == Action::Up ? SDL_CONTROLLER_BUTTON_DPAD_UP
                                    : a == Action::Down ? SDL_CONTROLLER_BUTTON_DPAD_DOWN
                                    : a == Action::Left ? SDL_CONTROLLER_BUTTON_DPAD_LEFT
                                    : SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_GameController* c = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
        if (c == nullptr) {
            continue;
        }
        if (SDL_GameControllerGetButton(c, button)) {
            return true;
        }
        float x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        float y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
        if ((a == Action::Up && y < -0.5f) || (a == Action::Down && y > 0.5f) ||
            (a == Action::Left && x < -0.5f) || (a == Action::Right && x > 0.5f)) {
            return true;
        }
    }
    return false;
}

// WINBACK_MENUPRESS=action@seconds[,...]: scripted menu presses for tests,
// e.g. "select@20,down@21,a@22". Actions: select up down left right a b.
void poll_scripted_presses() {
    struct Press { Action action; double at; bool done; };
    static std::vector<Press> presses = [] {
        std::vector<Press> out;
        const char* env = std::getenv("WINBACK_MENUPRESS");
        if (env == nullptr) {
            return out;
        }
        static const std::pair<const char*, Action> names[] = {
            {"select", Action::Toggle}, {"up", Action::Up}, {"down", Action::Down},
            {"left", Action::Left}, {"right", Action::Right}, {"a", Action::Accept}, {"b", Action::Back},
        };
        std::string spec = env;
        size_t pos = 0;
        while (pos < spec.size()) {
            size_t comma = spec.find(',', pos);
            std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            size_t at = item.find('@');
            for (const auto& [name, action] : names) {
                if (at != std::string::npos && item.compare(0, at, name) == 0 && std::strlen(name) == at) {
                    out.push_back({action, std::atof(item.c_str() + at + 1), false});
                }
            }
            if (comma == std::string::npos) {
                break;
            }
            pos = comma + 1;
        }
        return out;
    }();
    static const auto start = std::chrono::steady_clock::now();
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    for (auto& p : presses) {
        if (!p.done && t >= p.at) {
            p.done = true;
            apply(p.action);
        }
    }
}

// --- Drawing (render thread) -------------------------------------------------

struct Ui {
    std::unique_ptr<winback::ui::RmlRenderer> renderer;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    uint64_t drawn_version = ~0ull;
    int width = 0;
    int height = 0;
};
Ui* ui = nullptr;

std::string two_digits(int n) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d", n);
    return buf;
}

std::string plate(const std::string& text, bool selected, const std::string& tag = "", const char* tag_class = "") {
    std::string out = "<div class=\"plate";
    out += selected ? " selected\">" : "\">";
    if (!tag.empty()) {
        out += std::string("<span class=\"tag ") + tag_class + "\">" + tag + "</span>";
    }
    out += text + "</div>";
    return out;
}

// Text from a mod's files, safe to place in RML.
std::string escaped(const std::string& text) {
    return Rml::StringUtilities::EncodeRml(text);
}

void set_text(const char* id, const std::string& rml) {
    if (Rml::Element* e = ui->document->GetElementById(id)) {
        e->SetInnerRML(rml);
    }
}

// The title's letters fade from orange to silver, left to right, like the
// area names on the game's pause screen.
void set_title(const std::string& title) {
    struct Stop { float at; int r, g, b; };
    static const Stop stops[] = {{0.0f, 0xF0, 0x7C, 0x1E}, {0.4f, 0xC9, 0x8E, 0x5A}, {0.6f, 0xB9, 0xBD, 0xB4}, {1.0f, 0xE6, 0xEC, 0xEE}};
    std::string rml;
    size_t n = title.size();
    for (size_t i = 0; i < n; i++) {
        float t = n > 1 ? float(i) / float(n - 1) : 0.0f;
        int k = 0;
        while (k < 2 && t > stops[k + 1].at) {
            k++;
        }
        float f = (t - stops[k].at) / (stops[k + 1].at - stops[k].at);
        auto mix = [f](int a, int b) { return int(a + (b - a) * f + 0.5f); };
        char color[8];
        std::snprintf(color, sizeof(color), "#%02X%02X%02X",
            mix(stops[k].r, stops[k + 1].r), mix(stops[k].g, stops[k + 1].g), mix(stops[k].b, stops[k + 1].b));
        rml += std::string("<span style=\"color: ") + color + "\">" + (title[i] == ' ' ? std::string("&#160;") : std::string(1, title[i])) + "</span>";
    }
    set_text("title", rml);
}

// Rows [first, first + visible_rows) around the selection.
std::string rows(int count, int sel, auto&& make_row) {
    int first = std::clamp(sel - visible_rows / 2, 0, std::max(count - visible_rows, 0));
    int last = std::min(first + visible_rows, count);
    std::string out;
    out += first > 0 ? "<div class=\"more\">&#x2191; More</div>" : "<div class=\"more\"></div>";
    for (int i = first; i < last; i++) {
        out += make_row(i, i == sel);
    }
    if (last < count) {
        out += "<div class=\"more\">&#x2193; More</div>";
    }
    return out;
}

// Text the game keeps in capitals, such as area names, as "Outer Ground".
std::string title_case(const std::string& text) {
    std::string out = text;
    bool word_start = true;
    for (char& ch : out) {
        bool letter = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        if (letter && !word_start && ch >= 'A' && ch <= 'Z') {
            ch = char(ch - 'A' + 'a');
        }
        word_start = !letter && ch != '\'';
    }
    return out;
}

// The key hints along the bottom.
std::string hint_key(const char* key, const char* action) {
    return std::string("<span class=\"key\">") + key + "</span> " + action;
}
std::string hints(std::initializer_list<std::string> items) {
    std::string out;
    for (const std::string& item : items) {
        out += (out.empty() ? "" : " &#160;&#160;&#160; ") + item;
    }
    return out;
}

// RecompLauncher says which controller is plugged in; otherwise it's the first one SDL finds.
bool playstation_pad() {
    if (const char* pad = std::getenv("RECOMPLAUNCHER_PAD")) {
        return std::strcmp(pad, "playstation") == 0;
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameControllerType type = SDL_GameControllerTypeForIndex(i);
            return type == SDL_CONTROLLER_TYPE_PS3 || type == SDL_CONTROLLER_TYPE_PS4 || type == SDL_CONTROLLER_TYPE_PS5;
        }
    }
    return false;
}

// The hints are written with Xbox's button names, which share their positions with these.
std::string for_this_pad(std::string hint) {
    if (playstation_pad()) {
        for (const auto& [from, to] : {std::pair{">A<", ">Cross<"}, std::pair{">B<", ">Circle<"}}) {
            size_t at = hint.find(from);
            if (at != std::string::npos) {
                hint.replace(at, std::strlen(from), to);
            }
        }
    }
    return hint;
}

// A controller button's name on the pad in use.
const char* pad_name(int pad) {
    static const char* const xbox[] = {
        "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Start", "Left Stick Press", "Right Stick Press",
        "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
    };
    static const char* const playstation[] = {
        "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "Start", "L3", "R3",
        "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
    };
    static_assert(std::size(xbox) == size_t(In::PadCount) && std::size(playstation) == size_t(In::PadCount));
    return (playstation_pad() ? playstation : xbox)[pad];
}

std::string menus_label(In::MenuButtons m) {
    if (m == In::MenuButtons::AsPlay) {
        return "Same as Play";
    }
    return std::string(pad_name(In::PadA)) + " / " + pad_name(In::PadB);
}

void rebuild_controls(const State& s) {
    const In::ControlSettings& c = s.controls;
    bool custom = c.preset == In::Preset::Custom;
    In::Layout layout = In::active_layout(c);
    set_title("Controls");
    set_text("subtitle", "Controller layout");
    // Name and value of each row; values that can change get arrows.
    auto row = [&](int i, std::string& name, std::string& value) {
        bool fixed = !custom && i != ControlsPreset;
        if (i == ControlsPreset) {
            name = "Preset";
            value = preset_names[size_t(c.preset)];
        }
        else if (i < ControlsRightStick) {
            name = pad_name(i - ControlsFirstButton);
            value = bind_names[size_t(layout.buttons[i - ControlsFirstButton])];
        }
        else if (i == ControlsRightStick) {
            name = "Right Stick";
            value = stick_names[size_t(layout.right_stick)];
        }
        else if (i == ControlsMenus) {
            name = "Game Menus";
            value = menus_label(layout.menus);
        }
        else {
            name = i == ControlsResetPS2 ? "Reset to PS2 Style" : "Reset to N64 Style";
            value.clear();
            return false;
        }
        return !fixed;
    };
    set_text("list", rows(controls_rows(c), s.controls_sel, [&](int i, bool sel) {
        std::string name, value;
        bool changeable = row(i, name, value);
        if (value.empty()) {
            return plate(name, sel);
        }
        return changeable ? plate(name, sel, "&lt; " + value + " &gt;", "value") : plate(name, sel, value, "basic");
    }));
    std::string name, value;
    row(s.controls_sel, name, value);
    std::string note;
    int i = s.controls_sel;
    if (i == ControlsPreset) {
        note = "PS2 Style - The PlayStation 2 release's layout.<br/><br/>"
               "N64 Style - The Nintendo 64 original release's layout.<br/><br/>"
               "Custom - Choose what each button does.";
    }
    else if (i < ControlsRightStick) {
        note = "The game button this controller button presses. Camera Behind turns the camera to face the way"
               " the player faces.";
    }
    else if (i == ControlsRightStick) {
        note = "Camera - Turns the camera by how far the stick is pushed.<br/><br/>"
               "C Buttons - Presses C-Up, C-Down, C-Left and C-Right, as the N64's C buttons do.";
    }
    else if (i == ControlsMenus) {
        note = menus_label(In::MenuButtons::AB) + " - Confirms and goes back in the game's menus and on the pause"
               " screen, whatever those buttons do in play.<br/><br/>"
               "Same as Play - Uses the buttons above there too. The game confirms with A and goes back with B.";
    }
    else {
        note = std::string("Sets every custom button back to ") + (i == ControlsResetPS2 ? "PS2" : "N64") + " Style.";
    }
    if (!custom && i != ControlsPreset) {
        note += "<br/><br/>Choose the Custom preset to change it.";
    }
    set_text("detail", "<div class=\"head\">" + name + "</div>" + (value.empty() ? "" : "<div>" + value + "</div>")
        + "<div class=\"note\">" + note + "</div>");
}

void rebuild(const State& s) {
    namespace L = winback::levels;
    const std::string choose_hint = hints({hint_key("A", "Choose"), hint_key("B", "Back"), hint_key("Select", "Close")});
    const std::string change_hint = hints({hint_key("Left/Right", "Change"), hint_key("B", "Back"), hint_key("Select", "Close")});
    std::string hint = choose_hint;
    if (s.screen == Screen::Main) {
        set_title("Recomp Menu");
        set_text("subtitle", "WinBack Recompiled");
        set_text("list", rows(int(std::size(main_items)), s.main_sel,
            [](int i, bool sel) { return plate(main_items[i], sel); }));
        static const char* const notes[] = {
            "Jump to the start of any stage, or to one of its checkpoints.",
            "Window mode, resolution and other picture settings.",
            "Choose a controller layout, or set your own buttons.",
            "Turn mods on or off and change their settings.",
            "Close this menu.",
            "Close WinBack Recompiled.",
        };
        set_text("detail", std::string("<div class=\"head\">") + main_items[s.main_sel] + "</div>"
            "<div class=\"note\">" + notes[s.main_sel] + "</div>");
    }
    else if (s.screen == Screen::Display) {
        set_title("Display");
        set_text("subtitle", "Picture settings");
        set_text("list", rows(int(std::size(display_options)), s.display_sel, [&](int i, bool sel) {
            const DisplayOption& opt = display_options[i];
            return plate(opt.name, sel, std::string("&lt; ") + display_label(s.display, i) + " &gt;", "value");
        }));
        const DisplayOption& opt = display_options[s.display_sel];
        set_text("detail", std::string("<div class=\"head\">") + opt.name + "</div>"
            "<div>" + display_label(s.display, s.display_sel) + "</div>"
            "<div class=\"note\">" + opt.note + "</div>");
        hint = change_hint;
    }
    else if (s.screen == Screen::Controls) {
        rebuild_controls(s);
        hint = s.controls_sel >= ControlsResetPS2 ? choose_hint : change_hint;
    }
    else if (s.screen == Screen::Mods) {
        int mod_count = int(s.mods->size());
        int broken_count = int(s.broken_mods->size());
        set_title("Mods");
        set_text("subtitle", mod_count + broken_count == 0 ? "No mods found" : "Installed mods");
        set_text("list", rows(mod_count + broken_count + 1, s.mods_sel, [&](int i, bool sel) {
            if (i < mod_count) {
                bool on = (*s.mods)[i].enabled;
                return plate(escaped((*s.mods)[i].name), sel, on ? "On" : "Off", on ? "recorded" : "basic");
            }
            if (i < mod_count + broken_count) {
                return plate(escaped((*s.broken_mods)[i - mod_count].name), sel, "Error", "basic");
            }
            return plate("Restart Game", sel, s.restart_needed ? "Changes waiting" : "", "value");
        }));
        if (s.mods_sel < mod_count) {
            const winback::mods::Mod& m = (*s.mods)[s.mods_sel];
            set_text("detail", "<div class=\"head\">" + escaped(m.name) + "</div>"
                "<div>Version " + escaped(m.version) + "</div>"
                "<div class=\"area\">" + escaped(m.authors) + "</div>"
                "<div class=\"note\">" + escaped(m.description) + "</div>");
            hint = hints({hint_key("Left/Right", "On/Off"), hint_key("A", "Open"), hint_key("B", "Back")});
        }
        else if (s.mods_sel < mod_count + broken_count) {
            const winback::mods::BrokenMod& b = (*s.broken_mods)[s.mods_sel - mod_count];
            set_text("detail", "<div class=\"head\">" + escaped(b.name) + "</div>"
                "<div class=\"note\">This mod couldn't be opened: " + escaped(b.reason) + "</div>");
        }
        else {
            set_text("detail", std::string("<div class=\"head\">Restart Game</div><div class=\"note\">")
                + (s.restart_needed ? "Turning mods on or off takes effect after a restart."
                                    : "Mods added to the mods folder appear after a restart.") + "</div>");
        }
    }
    else if (s.screen == Screen::ModSettings) {
        const winback::mods::Mod& m = (*s.mods)[s.mods_sel];
        set_title("Mod Settings");
        set_text("subtitle", escaped(m.name));
        set_text("list", rows(1 + int(m.options.size()), s.option_sel, [&](int i, bool sel) {
            if (i == 0) {
                return plate("Enabled", sel, std::string("&lt; ") + (m.enabled ? "On" : "Off") + " &gt;", "value");
            }
            const winback::mods::Option& o = m.options[i - 1];
            bool fixed = o.disabled || o.type == winback::mods::OptionType::String;
            return plate(escaped(o.name), sel, fixed ? escaped(o.value) : "&lt; " + escaped(o.value) + " &gt;", fixed ? "basic" : "value");
        }));
        if (s.option_sel == 0) {
            set_text("detail", "<div class=\"head\">Enabled</div><div class=\"note\">Turning a mod on or off"
                " takes effect after a restart. Use Restart Game on the Mods screen.</div>");
        }
        else {
            const winback::mods::Option& o = m.options[s.option_sel - 1];
            std::string note = escaped(o.description);
            if (o.type == winback::mods::OptionType::String) {
                note += " Text can't be changed in this menu yet.";
            }
            set_text("detail", "<div class=\"head\">" + escaped(o.name) + "</div>"
                "<div>" + escaped(o.value) + "</div><div class=\"note\">" + note + "</div>");
        }
        hint = change_hint;
    }
    else if (s.screen == Screen::ConfirmRestart) {
        set_title("Restart Game");
        set_text("subtitle", "Restart WinBack Recompiled?");
        set_text("list", rows(int(std::size(restart_items)), s.restart_sel,
            [](int i, bool sel) { return plate(restart_items[i], sel); }));
        set_text("detail", "<div class=\"head\">Restart Game</div><div class=\"note\">Progress since your last"
            " Controller Pak save is lost. Recorded checkpoints are kept.</div>");
    }
    else if (s.screen == Screen::ConfirmExit) {
        set_title("Exit Game");
        set_text("subtitle", "Close WinBack Recompiled?");
        set_text("list", rows(int(std::size(exit_items)), s.exit_sel,
            [](int i, bool sel) { return plate(exit_items[i], sel); }));
        set_text("detail", "<div class=\"head\">Exit Game</div><div class=\"note\">Progress since your last"
            " Controller Pak save is lost. Recorded checkpoints are kept.</div>");
    }
    else if (s.screen == Screen::Stages) {
        const auto& stages = L::stages();
        set_title("Level Select");
        set_text("subtitle", "Choose a stage");
        set_text("list", rows(int(stages.size()), s.stage_sel, [&](int i, bool sel) {
            return plate("Stage " + two_digits(stages[i].number + 1) + " &#8211; " + title_case(stages[i].area), sel);
        }));
        const L::Stage& st = stages[s.stage_sel];
        int recorded = 0;
        for (const auto& cp : st.checkpoints) {
            recorded += L::is_recorded(st.number, cp) ? 1 : 0;
        }
        set_text("detail", "<div class=\"head\">Stage " + two_digits(st.number + 1) + "</div>"
            "<div class=\"area\">" + title_case(st.area) + "</div>"
            "<div>Checkpoints: " + std::to_string(st.checkpoints.size()) + "</div>"
            "<div>Recorded: " + std::to_string(recorded) + "</div>"
            "<div class=\"note\">A checkpoint is Recorded once you reach it in play. Until then it loads"
            " a Basic version: a fresh stage with you placed at the checkpoint.</div>");
    }
    else {
        const L::Stage& st = L::stages()[s.stage_sel];
        set_title("Stage " + two_digits(st.number + 1));
        set_text("subtitle", title_case(st.area));
        set_text("list", rows(1 + int(st.checkpoints.size()), s.row_sel, [&](int i, bool sel) {
            if (i == 0) {
                return plate("Stage Start", sel);
            }
            bool rec = L::is_recorded(st.number, st.checkpoints[i - 1]);
            return plate("Checkpoint " + two_digits(i), sel, rec ? "Recorded" : "Basic", rec ? "recorded" : "basic");
        }));
        if (s.row_sel == 0) {
            set_text("detail", "<div class=\"head\">Stage Start</div><div class=\"note\">Starts the stage from the beginning,"
                " as the game's own mission start does.</div>");
        }
        else {
            const L::Checkpoint& cp = st.checkpoints[s.row_sel - 1];
            bool rec = L::is_recorded(st.number, cp);
            set_text("detail", "<div class=\"head\">Checkpoint " + two_digits(s.row_sel) + "</div>"
                "<div>" + (rec ? "Recorded" : "Basic") + "</div>"
                "<div class=\"area\">Map " + two_digits(cp.map) + ", checkpoint " + std::to_string(cp.id) + "</div>"
                "<div class=\"note\">" + (rec ? "Loads the snapshot saved when you reached this checkpoint."
                                              : "Not reached yet: loads a fresh stage with you placed here.") + "</div>");
        }
    }
    set_text("hint", for_this_pad(hint));
}

void init_hook(RT64::RenderInterface* rhi, RT64::RenderDevice* device) {
    ui = new Ui();
    ui->renderer = std::make_unique<winback::ui::RmlRenderer>(rhi, device);
    Rml::SetRenderInterface(ui->renderer->rml_interface());
    Rml::Initialise();
    auto font = [](const char* data, size_t size, Rml::Style::FontStyle style, Rml::Style::FontWeight weight) {
        Rml::LoadFontFace({reinterpret_cast<const Rml::byte*>(data), size}, "Lato", style, weight);
    };
    font(Lato_Bold_ttf, std::size(Lato_Bold_ttf), Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold);
    font(Lato_BoldItalic_ttf, std::size(Lato_BoldItalic_ttf), Rml::Style::FontStyle::Italic, Rml::Style::FontWeight::Bold);
    font(Lato_BlackItalic_ttf, std::size(Lato_BlackItalic_ttf), Rml::Style::FontStyle::Italic, Rml::Style::FontWeight(900));
    ui->context = Rml::CreateContext("menu", {1280, 960});
    ui->document = ui->context->LoadDocumentFromMemory(std::string(menu_rml, std::size(menu_rml)));
    if (ui->document != nullptr) {
        ui->document->Show();
    }
}

void draw_hook(RT64::RenderCommandList* list, RT64::RenderFramebuffer* framebuffer) {
    if (ui == nullptr || ui->document == nullptr) {
        return;
    }
    State s;
    {
        std::lock_guard lock{state_mutex};
        s = state;
    }
    if (!s.open) {
        return;
    }
    int width = int(framebuffer->getWidth());
    int height = int(framebuffer->getHeight());
    if (width != ui->width || height != ui->height) {
        ui->width = width;
        ui->height = height;
        ui->context->SetDimensions({width, height});
        // Layout is in dp: 1080 dp is the window height.
        ui->context->SetDensityIndependentPixelRatio(height / 1080.0f);
    }
    if (s.version != ui->drawn_version) {
        ui->drawn_version = s.version;
        rebuild(s);
    }
    ui->context->Update();
    ui->renderer->start(list, width, height);
    ui->context->Render();
    ui->renderer->end();
}

void deinit_hook() {
    if (ui != nullptr) {
        Rml::Shutdown();
        delete ui;
        ui = nullptr;
    }
}

} // namespace

void winback::menu::set_render_hooks() {
    RT64::SetRenderHooks(init_hook, draw_hook, deinit_hook);
}

bool winback::menu::is_open() {
    return open_flag;
}

bool winback::menu::handle_event(const SDL_Event& ev) {
    if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
        switch (ev.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_BACK: apply(Action::Toggle); return true;
            case SDL_CONTROLLER_BUTTON_A: if (is_open()) { apply(Action::Accept); return true; } break;
            case SDL_CONTROLLER_BUTTON_B: if (is_open()) { apply(Action::Back); return true; } break;
        }
    }
    else if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0) {
        switch (ev.key.keysym.scancode) {
            case SDL_SCANCODE_ESCAPE: apply(is_open() ? Action::Back : Action::Toggle); return true;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
                if (is_open() && !(ev.key.keysym.mod & KMOD_ALT)) { apply(Action::Accept); return true; }
                break;
            case SDL_SCANCODE_BACKSPACE: if (is_open()) { apply(Action::Back); return true; } break;
            default: break;
        }
    }
    return false;
}

void winback::menu::update() {
    poll_scripted_presses();
    static Repeat repeats[4];
    static const Action dirs[4] = {Action::Up, Action::Down, Action::Left, Action::Right};
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; i++) {
        bool held = is_open() && direction_held(dirs[i]);
        if (held && !repeats[i].held) {
            apply(dirs[i]);
            repeats[i].next = now + 400ms;
        }
        else if (held && now >= repeats[i].next) {
            apply(dirs[i]);
            repeats[i].next = now + 80ms;
        }
        repeats[i].held = held;
    }
}
