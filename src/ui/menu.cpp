// The recomp's overlay menu, drawn with RmlUi on top of the game.
//
// The window thread turns controller and keyboard input into menu actions
// and updates the menu state; RT64's present thread draws it through the
// render hooks (ui_renderer.cpp), rebuilding the visible rows whenever the
// state changes. Entries: Level Select (stage, then stage start or
// checkpoint), Display (window mode and renderer options), Return to Game and
// Exit Game.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
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
#include "ultramodern/ultramodern.hpp"

#include "Lato_Bold_ttf.h"
#include "Lato_BoldItalic_ttf.h"
#include "Lato_BlackItalic_ttf.h"
#include "menu_rml.h"

namespace {

enum class Screen { Main, Stages, Checkpoints, Display, ConfirmExit };
enum class Action { Toggle, Up, Down, Left, Right, Accept, Back };

constexpr int visible_rows = 10;
enum MainItem { MainLevelSelect, MainDisplay, MainReturn, MainExit };
const char* const main_items[] = {"LEVEL SELECT", "DISPLAY", "RETURN TO GAME", "EXIT GAME"};
const char* const exit_items[] = {"EXIT GAME", "CANCEL"};

// Display screen rows. Each option's values are its enum's values in order.
struct DisplayOption {
    const char* name;
    const char* note;
    std::vector<const char*> values;
};
const DisplayOption display_options[] = {
    {"WINDOW MODE", "BORDERLESS covers the screen with a window and switches away instantly. FULLSCREEN"
                    " takes over the screen; it can help with stutter on some PCs. F11 or Alt+Enter also"
                    " switch between windowed and full screen.",
     {"WINDOWED", "BORDERLESS", "FULLSCREEN"}},
    {"RESOLUTION", "How sharply 3D is drawn. MATCH WINDOW draws at the window's size. The N64 drew 240"
                   " lines; ORIGINAL keeps that look.",
     {"ORIGINAL (240P)", "DOUBLE (480P)", "MATCH WINDOW"}},
    {"ASPECT RATIO", "WIDESCREEN shows more of the scene to the sides when the window is wider than 4:3.",
     {"ORIGINAL (4:3)", "WIDESCREEN"}},
    {"ANTI-ALIASING", "Smooths jagged edges on 3D models. Higher settings cost more graphics power.",
     {"OFF", "2X MSAA", "4X MSAA", "8X MSAA"}},
};

int display_value(const winback::display::Settings& d, int option) {
    switch (option) {
        case 0: return int(d.window_mode);
        case 1: return int(d.resolution);
        case 2: return int(d.aspect_ratio);
        default: return int(d.antialiasing);
    }
}

void set_display_value(winback::display::Settings& d, int option, int value) {
    namespace R = ultramodern::renderer;
    switch (option) {
        case 0: d.window_mode = winback::display::WindowMode(value); break;
        case 1: d.resolution = R::Resolution(value); break;
        case 2: d.aspect_ratio = R::AspectRatio(value); break;
        default: d.antialiasing = R::Antialiasing(value); break;
    }
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
    uint64_t version = 0;
};

// Work done after the state lock is released.
struct Effects {
    bool display = false;
    winback::display::Settings display_settings;
    bool quit = false;
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
        case Screen::ConfirmExit: return int(std::size(exit_items));
    }
    return 0;
}

int& selection(State& s) {
    switch (s.screen) {
        case Screen::Main: return s.main_sel;
        case Screen::Stages: return s.stage_sel;
        case Screen::Display: return s.display_sel;
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
            else if (a == Action::Left) sel = std::max(sel - visible_rows, 0);
            else sel = std::min(sel + visible_rows, count - 1);
            break;
        case Action::Back:
            if (s.screen == Screen::Main) set_open(s, false);
            else if (s.screen == Screen::Checkpoints) s.screen = Screen::Stages;
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
    if (fx.quit) {
        std::printf("[menu] exit game\n");
        ultramodern::quit();
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
    out += first > 0 ? "<div class=\"more\">&#x2191; MORE</div>" : "<div class=\"more\"></div>";
    for (int i = first; i < last; i++) {
        out += make_row(i, i == sel);
    }
    if (last < count) {
        out += "<div class=\"more\">&#x2193; MORE</div>";
    }
    return out;
}

void rebuild(const State& s) {
    namespace L = winback::levels;
    std::string hint = "<span class=\"key\">A</span> CHOOSE &#160;&#160;&#160; <span class=\"key\">B</span> BACK"
                       " &#160;&#160;&#160; <span class=\"key\">SELECT</span> CLOSE";
    if (s.screen == Screen::Main) {
        set_title("RECOMP MENU");
        set_text("subtitle", "WinBack Recompiled");
        set_text("list", rows(int(std::size(main_items)), s.main_sel,
            [](int i, bool sel) { return plate(main_items[i], sel); }));
        static const char* const notes[] = {
            "Jump to the start of any stage, or to one of its checkpoints.",
            "Window mode, resolution and other picture settings.",
            "Close this menu.",
            "Close WinBack Recompiled.",
        };
        set_text("detail", std::string("<div class=\"head\">") + main_items[s.main_sel] + "</div>"
            "<div class=\"note\">" + notes[s.main_sel] + "</div>");
    }
    else if (s.screen == Screen::Display) {
        set_title("DISPLAY");
        set_text("subtitle", "Picture settings");
        set_text("list", rows(int(std::size(display_options)), s.display_sel, [&](int i, bool sel) {
            const DisplayOption& opt = display_options[i];
            return plate(opt.name, sel, std::string("&lt; ") + opt.values[display_value(s.display, i)] + " &gt;", "value");
        }));
        const DisplayOption& opt = display_options[s.display_sel];
        set_text("detail", std::string("<div class=\"head\">") + opt.name + "</div>"
            "<div>" + opt.values[display_value(s.display, s.display_sel)] + "</div>"
            "<div class=\"note\">" + opt.note + "</div>");
        hint = "<span class=\"key\">LEFT/RIGHT</span> CHANGE &#160;&#160;&#160; <span class=\"key\">B</span> BACK"
               " &#160;&#160;&#160; <span class=\"key\">SELECT</span> CLOSE";
    }
    else if (s.screen == Screen::ConfirmExit) {
        set_title("EXIT GAME");
        set_text("subtitle", "Close WinBack Recompiled?");
        set_text("list", rows(int(std::size(exit_items)), s.exit_sel,
            [](int i, bool sel) { return plate(exit_items[i], sel); }));
        set_text("detail", "<div class=\"head\">EXIT GAME</div><div class=\"note\">Progress since your last"
            " Controller Pak save is lost. Recorded checkpoints are kept.</div>");
    }
    else if (s.screen == Screen::Stages) {
        const auto& stages = L::stages();
        set_title("LEVEL SELECT");
        set_text("subtitle", "Choose a stage");
        set_text("list", rows(int(stages.size()), s.stage_sel, [&](int i, bool sel) {
            return plate("STAGE " + two_digits(stages[i].number) + " &#8211; " + stages[i].area, sel);
        }));
        const L::Stage& st = stages[s.stage_sel];
        int recorded = 0;
        for (const auto& cp : st.checkpoints) {
            recorded += L::is_recorded(st.number, cp) ? 1 : 0;
        }
        set_text("detail", "<div class=\"head\">STAGE " + two_digits(st.number) + "</div>"
            "<div class=\"area\">" + st.area + "</div>"
            "<div>Checkpoints: " + std::to_string(st.checkpoints.size()) + "</div>"
            "<div>Recorded: " + std::to_string(recorded) + "</div>"
            "<div class=\"note\">A checkpoint is RECORDED once you reach it in play. Until then it loads"
            " a BASIC version: a fresh stage with you placed at the checkpoint.</div>");
    }
    else {
        const L::Stage& st = L::stages()[s.stage_sel];
        set_title("STAGE " + two_digits(st.number));
        set_text("subtitle", st.area);
        set_text("list", rows(1 + int(st.checkpoints.size()), s.row_sel, [&](int i, bool sel) {
            if (i == 0) {
                return plate("STAGE START", sel);
            }
            bool rec = L::is_recorded(st.number, st.checkpoints[i - 1]);
            return plate("CHECKPOINT " + two_digits(i), sel, rec ? "RECORDED" : "BASIC", rec ? "recorded" : "basic");
        }));
        if (s.row_sel == 0) {
            set_text("detail", "<div class=\"head\">STAGE START</div><div class=\"note\">Starts the stage from the beginning,"
                " as the game's own mission start does.</div>");
        }
        else {
            const L::Checkpoint& cp = st.checkpoints[s.row_sel - 1];
            bool rec = L::is_recorded(st.number, cp);
            set_text("detail", "<div class=\"head\">CHECKPOINT " + two_digits(s.row_sel) + "</div>"
                "<div>" + (rec ? "RECORDED" : "BASIC") + "</div>"
                "<div class=\"area\">Map " + two_digits(cp.map) + ", checkpoint " + std::to_string(cp.id) + "</div>"
                "<div class=\"note\">" + (rec ? "Loads the snapshot saved when you reached this checkpoint."
                                              : "Not reached yet: loads a fresh stage with you placed here.") + "</div>");
        }
    }
    set_text("hint", hint);
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
