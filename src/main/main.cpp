// WinBack Recompiled - host application.
//
// Creates the SDL window, wires RT64 / audio / input into N64ModernRuntime,
// finds the user's ROM on first run and boots straight into the game.
//
// Options: --rom <path>   ROM to use (only needed once; it is stored)
//          --fullscreen
//          --developer    RT64 debug UI
//          --console      show log output (also on when WINBACK_DEBUG is set)
//          --prepare      store the ROM given with --rom and exit, with no window
//                         (RecompLauncher's setup step)
//
// Without a console or redirected output, the log goes to logs/winback.log.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sdl_include.hpp"

#ifdef _WIN32
#include <Windows.h>
#include <commdlg.h>
#include <timeapi.h>
#include <tlhelp32.h>
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
#include "display.hpp"
#include "app.hpp"
#include "mods.hpp"

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
// Nobody may be there to answer a dialog: RecompLauncher's setup step and Big Picture mode, where
// the launcher has already asked for the ROM. A ROM problem ends in exit_no_rom instead.
bool no_dialogs = false;
std::atomic<bool> mod_load_failed{false};
std::string rom_arg;

fs::path exe_dir() {
    char* base = SDL_GetBasePath();
    fs::path p = base ? fs::path(base) : fs::current_path();
    SDL_free(base);
    return p;
}

void message_box(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
    // The runtime reports mods that fail to load here, and then the game can't
    // start. They're turned off and the game restarts without them.
    if (std::strncmp(msg, "Error loading mods", 18) == 0) {
        std::string text = std::string(msg) + "\nThese mods will be turned off, and the game will restart.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, app_title, text.c_str(), window);
        mod_load_failed = true;
        return;
    }
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
    // The launcher's pick is the one to use or refuse, not a reason to go looking for another.
    bool search = rom_arg.empty() || !no_dialogs;
    for (const fs::path& dir : {exe_dir(), fs::current_path(), fs::current_path() / "rom"}) {
        if (!search) {
            break;
        }
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
    if (no_dialogs) {
        std::fprintf(stderr, "[winback] no usable ROM\n");
        return false;
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
    winback::display::attach_window(window);
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
    if (mod_load_failed) {
        winback::mods::turn_off_failed();
        winback::exit_app(true);
    }
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (winback::menu::handle_event(ev)) {
            continue;
        }
        switch (ev.type) {
            case SDL_QUIT:
                winback::exit_app(false);
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.scancode == SDL_SCANCODE_F11 ||
                    (ev.key.keysym.scancode == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                    winback::display::toggle_fullscreen();
                }
                break;
            default:
                winback::input::handle_event(ev);
                break;
        }
    }
    winback::menu::update();
}

#ifdef _WIN32
bool has_output() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    return GetConsoleWindow() != nullptr || (out != nullptr && out != INVALID_HANDLE_VALUE);
}

// Release builds use the GUI subsystem. Attach to the launching terminal, or
// open a new console, so logs and crash messages stay visible when asked for.
// Output already redirected to a file or pipe is left alone.
void open_console() {
    if (has_output()) {
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
}

// Keeps the log in logs/winback.log, which RecompLauncher sends with a crash report.
void open_log() {
    std::error_code ec;
    fs::create_directories(exe_dir() / "logs", ec);
    fs::path path = exe_dir() / "logs" / "winback.log";
    std::ofstream truncate(path);
    truncate.close();
    _wfreopen(path.c_str(), L"a", stdout);
    _wfreopen(path.c_str(), L"a", stderr);
}

// Waits out a restarted game while a launcher watches this process. The launcher takes this
// process ending as the end of the session, so this one stays, hidden and stopped, and ends with
// the new one's exit code. Nothing past this point needs the other threads.
[[noreturn]] void stand_by_for(HANDLE process) {
    DWORD id = GetCurrentThreadId();
    HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threads != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(threads, &entry); more; more = Thread32Next(threads, &entry)) {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == id) {
                continue;
            }
            if (HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID)) {
                SuspendThread(thread);
                CloseHandle(thread);
            }
        }
        CloseHandle(threads);
    }
    DWORD code = EXIT_SUCCESS;
    WaitForSingleObject(process, INFINITE);
    GetExitCodeProcess(process, &code);
    TerminateProcess(GetCurrentProcess(), code);
    std::_Exit(int(code));
}
#endif

} // namespace

void winback::exit_app(bool restart) {
    winback::mods::finish();
#ifdef _WIN32
    if (restart) {
        // The new process inherits the environment; scripted menu presses must not run twice.
        SetEnvironmentVariableW(L"WINBACK_MENUPRESS", nullptr);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring command_line = GetCommandLineW();
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        bool launcher = std::getenv("RECOMPLAUNCHER") != nullptr;
        if (launcher) {
            // Before the new process starts, so the two don't draw or play sound together.
            SDL_HideWindow(window);
            winback::audio::pause();
        }
        if (CreateProcessW(path, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            if (launcher) {
                stand_by_for(pi.hProcess);
            }
            CloseHandle(pi.hProcess);
        }
        else {
            message_box("Couldn't restart the game. Please start it again yourself.");
        }
    }
    TerminateProcess(GetCurrentProcess(), EXIT_SUCCESS);
#endif
    std::_Exit(EXIT_SUCCESS);
}

int main(int argc, char** argv) {
    bool console = std::getenv("WINBACK_DEBUG") != nullptr;
    // RecompLauncher's controller-focused Big Picture mode.
    const char* launcher_mode = std::getenv("RECOMPLAUNCHER_MODE");
    fullscreen = launcher_mode != nullptr && std::strcmp(launcher_mode, "bigpicture") == 0;
    no_dialogs = fullscreen;
    bool prepare = false;
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
        else if (std::strcmp(argv[i], "--prepare") == 0) {
            prepare = true;
            no_dialogs = true;
        }
    }

#ifdef _WIN32
    if (console) {
        open_console();
    }
    // The launcher's setup step reads progress from the output it gave us.
    else if (!prepare && (std::getenv("RECOMPLAUNCHER") != nullptr || !has_output())) {
        open_log();
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
    if (prepare) {
        std::printf("progress 0.1 Checking your ROM\n");
    }
    if (!ensure_rom()) {
        return exit_no_rom;
    }
    if (prepare) {
        std::printf("progress 1 Your ROM is ready\n");
        return EXIT_SUCCESS;
    }
    winback::display::load(developer, fullscreen);
    winback::menu::set_render_hooks();
    winback::mods::register_exports();

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
