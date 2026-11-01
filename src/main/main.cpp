// WinBack Recompiled - host application.
//
// Creates the SDL window, wires RT64 / audio / input into N64ModernRuntime,
// finds the user's ROM on first run and boots straight into the game.
//
// Options: --rom <path>   ROM to use (only needed once; it is stored)
//          --fullscreen
//          --developer    RT64 debug UI
//          --console      show log output (also on when WINBACK_DEBUG is set)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "sdl_include.hpp"

#ifdef _WIN32
#include <Windows.h>
#include <commdlg.h>
#include <timeapi.h>
#endif

#include "librecomp/game.hpp"
#include "ultramodern/config.hpp"
#include "ultramodern/error_handling.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

#include "winback_game.hpp"
#include "rt64_context.hpp"
#include "audio.hpp"
#include "input.hpp"
#include "menu.hpp"

namespace fs = std::filesystem;

namespace {

constexpr const char* app_title = "WinBack Recompiled";
// Exit code when no valid ROM was given; recomplauncher.json maps it to a message.
constexpr int exit_no_rom = 2;
const recomp::Version project_version{
    WINBACK_VERSION_MAJOR, WINBACK_VERSION_MINOR, WINBACK_VERSION_PATCH, WINBACK_VERSION_SUFFIX};

SDL_Window* window = nullptr;
bool fullscreen = false;
bool developer = false;
bool game_started = false;
std::string rom_arg;

fs::path exe_dir() {
    char* base = SDL_GetBasePath();
    fs::path p = base ? fs::path(base) : fs::current_path();
    SDL_free(base);
    return p;
}

void message_box(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, app_title, msg, window);
}

#ifdef _WIN32
// Asks the player for their ROM with the standard Open dialog.
std::vector<fs::path> pick_rom() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrTitle = L"Select your WinBack - Covert Operations (USA) ROM";
    ofn.lpstrFilter = L"N64 ROM (*.z64)\0*.z64\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        return {};
    }
    return {fs::path(file)};
}
#endif

bool select_rom_from(const std::vector<fs::path>& candidates) {
    for (const fs::path& c : candidates) {
        if (recomp::select_rom(c, winback::game_id) == recomp::RomValidationError::Good) {
            std::printf("[winback] using ROM %s\n", c.string().c_str());
            return true;
        }
    }
    return false;
}

// Stores a verified copy of the ROM next to the exe on first run: --rom, then
// any .z64 next to the exe or in the working directory, then one the player picks.
bool ensure_rom() {
    if (recomp::is_rom_valid(winback::game_id)) {
        return true;
    }
    std::vector<fs::path> candidates;
    if (!rom_arg.empty()) {
        candidates.emplace_back(rom_arg);
    }
    for (const fs::path& dir : {exe_dir(), fs::current_path(), fs::current_path() / "rom"}) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            auto ext = e.path().extension().string();
            if (ext == ".z64" || ext == ".Z64") {
                candidates.push_back(e.path());
            }
        }
    }
    if (select_rom_from(candidates)) {
        return true;
    }
#ifdef _WIN32
    while (true) {
        std::vector<fs::path> picked = pick_rom();
        if (picked.empty()) {
            break;
        }
        if (select_rom_from(picked)) {
            return true;
        }
        message_box("That file isn't a WinBack - Covert Operations (USA) ROM.\n\n"
                    "A big-endian .z64 dump of the USA release is needed.");
    }
#endif
    message_box("WinBack - Covert Operations (USA) ROM not found.\n\n"
                "Place your big-endian .z64 ROM next to the executable, or run with --rom <path>.");
    return false;
}

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        message_box(SDL_GetError());
        std::exit(EXIT_FAILURE);
    }
    winback::audio::init();
    return nullptr;
}

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(__linux__)
    flags |= SDL_WINDOW_VULKAN;
#endif
    window = SDL_CreateWindow(app_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960, flags);
    if (window == nullptr) {
        message_box(SDL_GetError());
        std::exit(EXIT_FAILURE);
    }
#if defined(_WIN32)
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(window, &wm);
    return ultramodern::renderer::WindowHandle{wm.info.win.window, GetCurrentThreadId()};
#else
    return window;
#endif
}

void update_gfx(void*) {
    if (!game_started) {
        game_started = true;
        recomp::start_game(winback::game_id, "");
    }
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (winback::menu::handle_event(ev)) {
            continue;
        }
        switch (ev.type) {
            case SDL_QUIT:
                ultramodern::quit();
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.scancode == SDL_SCANCODE_F11 ||
                    (ev.key.keysym.scancode == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                    auto cfg = ultramodern::renderer::get_graphics_config();
                    cfg.wm_option = cfg.wm_option == ultramodern::renderer::WindowMode::Fullscreen
                        ? ultramodern::renderer::WindowMode::Windowed
                        : ultramodern::renderer::WindowMode::Fullscreen;
                    ultramodern::renderer::set_graphics_config(cfg);
                }
                break;
            default:
                winback::input::handle_event(ev);
                break;
        }
    }
    winback::menu::update();
}

void set_default_graphics_config() {
    namespace R = ultramodern::renderer;
    R::GraphicsConfig cfg{};
    cfg.developer_mode = developer;
    cfg.res_option = R::Resolution::Auto;
    cfg.wm_option = fullscreen ? R::WindowMode::Fullscreen : R::WindowMode::Windowed;
    cfg.hr_option = R::HUDRatioMode::Clamp16x9;
    // Vulkan on every platform (RT64's Auto picks D3D12 on Windows). D3D12 stays
    // available through WINBACK_GFX_API=d3d12.
    cfg.api_option = R::GraphicsApi::Vulkan;
    cfg.ar_option = R::AspectRatio::Expand;
    cfg.msaa_option = R::Antialiasing::MSAA2X;
    cfg.rr_option = R::RefreshRate::Display;
    cfg.hpfb_option = R::HighPrecisionFramebuffer::Auto;
    cfg.rr_manual_value = 60;
    cfg.ds_option = 1;
    R::set_graphics_config(cfg);
}

#ifdef _WIN32
// Release builds use the GUI subsystem. Attach to the launching terminal, or
// open a new console, so logs and crash messages stay visible when asked for.
// Output already redirected to a file or pipe is left alone.
void open_console() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleWindow() != nullptr || (out != nullptr && out != INVALID_HANDLE_VALUE)) {
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
}
#endif

} // namespace

int main(int argc, char** argv) {
    bool console = std::getenv("WINBACK_DEBUG") != nullptr;
    // RecompLauncher's controller-focused Big Picture mode.
    const char* launcher_mode = std::getenv("RECOMPLAUNCHER_MODE");
    fullscreen = launcher_mode != nullptr && std::strcmp(launcher_mode, "bigpicture") == 0;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom_arg = argv[++i];
        }
        else if (std::strcmp(argv[i], "--fullscreen") == 0) {
            fullscreen = true;
        }
        else if (std::strcmp(argv[i], "--developer") == 0) {
            developer = true;
        }
        else if (std::strcmp(argv[i], "--console") == 0) {
            console = true;
        }
    }

#ifdef _WIN32
    if (console) {
        open_console();
    }
    timeBeginPeriod(1);
#endif
    // Unbuffered, so log files keep everything up to a crash or a forced exit.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Portable: the stored ROM, saves, mods and renderer data all live in the
    // executable's folder, never in per-user system folders.
    recomp::register_config_path(exe_dir());
    if (!winback::register_game()) {
        message_box("Failed to register the game with the runtime.");
        return EXIT_FAILURE;
    }
    recomp::check_all_stored_roms();
    if (!ensure_rom()) {
        return exit_no_rom;
    }
    set_default_graphics_config();
    winback::menu::set_render_hooks();

    recomp::Configuration cfg{};
    cfg.argc = argc;
    cfg.argv = argv;
    cfg.project_version = project_version;
    cfg.rsp_callbacks = winback::rsp_callbacks();
    cfg.renderer_callbacks = {
        .create_render_context = winback::create_render_context,
    };
    cfg.audio_callbacks = {
        .queue_samples = winback::audio::queue_samples,
        .get_frames_remaining = winback::audio::get_frames_remaining,
        .set_frequency = winback::audio::set_frequency,
    };
    cfg.input_callbacks = {
        .poll_input = winback::input::poll,
        .get_input = winback::input::get,
        .set_rumble = winback::input::set_rumble,
        .get_connected_device_info = winback::input::get_device_info,
    };
    cfg.gfx_callbacks = {
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };
    cfg.error_handling_callbacks = {
        .message_box = message_box,
    };
    recomp::start(cfg);

    SDL_Quit();
    return EXIT_SUCCESS;
}
