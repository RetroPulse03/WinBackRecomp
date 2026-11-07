// RT64 renderer backend for N64ModernRuntime.
//
// RT64 interprets the game's display lists (F3DEX2/L3DEX2 2.08) in HLE and
// renders them with Direct3D 12 or Vulkan.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#ifndef HLSL_CPU
#define HLSL_CPU
#endif
#include "hle/rt64_application.h"
#include "librecomp/game.hpp"

#include "ultramodern/config.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

#include "rt64_context.hpp"
#include "debug_stats.hpp"

namespace {

uint8_t DMEM[0x1000];
uint8_t IMEM[0x1000];
uint8_t dummy_rom_header[0x40];

unsigned int MI_INTR_REG = 0;
unsigned int DPC_START_REG = 0;
unsigned int DPC_END_REG = 0;
unsigned int DPC_CURRENT_REG = 0;
unsigned int DPC_STATUS_REG = 0;
unsigned int DPC_CLOCK_REG = 0;
unsigned int DPC_BUFBUSY_REG = 0;
unsigned int DPC_PIPEBUSY_REG = 0;
unsigned int DPC_TMEM_REG = 0;

void check_interrupts_stub() {}

// WINBACK_DLDUMP: logs every fill/texture rectangle of at least 32x32 pixels
// with the RDP state it is drawn with, for chasing render bugs. Walks the
// F3DEX2 display list itself; RT64 is unaffected. As in the microcode, a
// texture rectangle is sent when G_RDPHALF_2 runs, not at G_TEXRECT.
void dump_rects(const uint8_t* rdram, uint32_t dl) {
    static const bool enabled = std::getenv("WINBACK_DLDUMP") != nullptr;
    static uint32_t frame = 0;
    if (!enabled) {
        return;
    }
    frame++;
    auto word = [rdram](uint32_t addr) { return *reinterpret_cast<const uint32_t*>(rdram + (addr & 0x3FFFFFC)); };
    uint32_t segments[16] = {};
    auto resolve = [&segments](uint32_t addr) { return segments[(addr >> 24) & 0xF] + (addr & 0xFFFFFF); };
    uint32_t stack[18];
    int depth = 0;
    uint32_t combine0 = 0, combine1 = 0, omh = 0, oml = 0, timg0 = 0, timg1 = 0, prim = 0, env = 0, fill = 0;
    uint32_t texrect0 = 0, texrect1 = 0;
    auto log_rect = [&](const char* kind, uint32_t w0, uint32_t w1) {
        uint32_t x1 = (w0 >> 12) & 0xFFF, y1 = w0 & 0xFFF;
        uint32_t x0 = (w1 >> 12) & 0xFFF, y0 = w1 & 0xFFF;
        if (x1 >= x0 + 32 * 4 && y1 >= y0 + 32 * 4) {
            std::fprintf(stderr, "[dl %u] %s (%u,%u)-(%u,%u) cc %08X %08X om %08X %08X timg %08X %08X prim %08X env %08X fill %08X\n",
                frame, kind, x0 / 4, y0 / 4, x1 / 4, y1 / 4, combine0, combine1, omh, oml, timg0, timg1, prim, env, fill);
        }
    };
    for (int guard = 0; guard < 200000; guard++) {
        uint32_t w0 = word(dl), w1 = word(dl + 4);
        uint8_t op = w0 >> 24;
        dl += 8;
        switch (op) {
            case 0xDE: // G_DL
                if (((w0 >> 16) & 0xFF) == 0 && depth < 18) {
                    stack[depth++] = dl;
                }
                dl = resolve(w1);
                break;
            case 0xDF: // G_ENDDL
                if (depth == 0) {
                    return;
                }
                dl = stack[--depth];
                break;
            case 0xDB: // G_MOVEWORD
                if (((w0 >> 16) & 0xFF) == 0x06) {
                    segments[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0xFFFFFF;
                }
                break;
            case 0xE2: case 0xE3: { // G_SETOTHERMODE_L / _H
                uint32_t len = (w0 & 0xFF) + 1;
                uint32_t shift = 32 - ((w0 >> 8) & 0xFF) - len;
                uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
                uint32_t& om = op == 0xE2 ? oml : omh;
                om = (om & ~mask) | (w1 & mask);
                break;
            }
            case 0xEF: omh = w0 & 0xFFFFFF; oml = w1; break; // G_RDPSETOTHERMODE
            case 0xFC: combine0 = w0; combine1 = w1; break;
            case 0xFD: timg0 = w0; timg1 = resolve(w1); break;
            case 0xFA: prim = w1; break;
            case 0xFB: env = w1; break;
            case 0xF7: fill = w1; break;
            case 0xE4: case 0xE5: texrect0 = w0; texrect1 = w1; break; // G_TEXRECT / G_TEXRECTFLIP
            case 0xF1: // G_RDPHALF_2
                if ((texrect0 >> 24) == 0xE4 || (texrect0 >> 24) == 0xE5) {
                    log_rect((texrect0 >> 24) == 0xE4 ? "TEXRECT" : "TEXRECTFLIP", texrect0, texrect1);
                }
                break;
            case 0xF6: log_rect("FILLRECT", w0, w1); break;
            case 0xED: // G_SETSCISSOR
                std::fprintf(stderr, "[dl %u] SCISSOR (%u,%u)-(%u,%u)\n", frame,
                    ((w0 >> 12) & 0xFFF) / 4, (w0 & 0xFFF) / 4, ((w1 >> 12) & 0xFFF) / 4, (w1 & 0xFFF) / 4);
                break;
        }
    }
}

using UC = RT64::UserConfiguration;
namespace R = ultramodern::renderer;

// The area WinBack draws its picture in, as margins in VI coordinates (640x480). The game clears and
// draws only (14,14)-(305,229) of its 320x240 framebuffer (the FILLRECTs in a WINBACK_DLDUMP log), and
// leaves the rest black for the TV's overscan to hide.
constexpr UC::Framing game_picture_margins{28, 28, 28, 20};

constexpr uint32_t framing_crop = 1;
constexpr uint32_t framing_integer = 2;
std::atomic<uint32_t> framing_flags = 0;
std::atomic<bool> framing_changed = false;

UC::Framing current_framing() {
    uint32_t flags = framing_flags.load();
    UC::Framing f = (flags & framing_crop) ? game_picture_margins : UC::Framing{};
    f.integerScale = (flags & framing_integer) != 0;
    return f;
}

void apply_user_config(RT64::Application* app, const R::GraphicsConfig& cfg) {
    app->userConfig.framing = current_framing();
    switch (cfg.res_option) {
        case R::Resolution::Original:
            app->userConfig.resolution = UC::Resolution::Manual;
            app->userConfig.resolutionMultiplier = std::max(cfg.ds_option, 1);
            app->userConfig.downsampleMultiplier = std::max(cfg.ds_option, 1);
            break;
        case R::Resolution::Original2x:
            app->userConfig.resolution = UC::Resolution::Manual;
            app->userConfig.resolutionMultiplier = 2.0 * std::max(cfg.ds_option, 1);
            app->userConfig.downsampleMultiplier = std::max(cfg.ds_option, 1);
            break;
        default:
            app->userConfig.resolution = UC::Resolution::WindowIntegerScale;
            app->userConfig.downsampleMultiplier = 1;
            break;
    }
    app->userConfig.aspectTarget = 16.0 / 9.0;
    switch (cfg.ar_option) {
        case R::AspectRatio::Expand: app->userConfig.aspectRatio = UC::AspectRatio::Expand; break;
        case R::AspectRatio::Manual: app->userConfig.aspectRatio = UC::AspectRatio::Manual; break;
        default:                     app->userConfig.aspectRatio = UC::AspectRatio::Original; break;
    }
    // Extended GBI origins (the HUD's edge anchors, src/game/hud.cpp) at the edges of the widened picture.
    app->userConfig.extAspectRatio = app->userConfig.aspectRatio == UC::AspectRatio::Original ? UC::AspectRatio::Original : UC::AspectRatio::Expand;
    switch (cfg.msaa_option) {
        case R::Antialiasing::MSAA2X: app->userConfig.antialiasing = UC::Antialiasing::MSAA2X; break;
        case R::Antialiasing::MSAA4X: app->userConfig.antialiasing = UC::Antialiasing::MSAA4X; break;
        case R::Antialiasing::MSAA8X: app->userConfig.antialiasing = UC::Antialiasing::MSAA8X; break;
        default:                      app->userConfig.antialiasing = UC::Antialiasing::None; break;
    }
    switch (cfg.rr_option) {
        case R::RefreshRate::Display: app->userConfig.refreshRate = UC::RefreshRate::Display; break;
        case R::RefreshRate::Manual:  app->userConfig.refreshRate = UC::RefreshRate::Manual; break;
        default:                      app->userConfig.refreshRate = UC::RefreshRate::Original; break;
    }
    app->userConfig.refreshRateTarget = cfg.rr_manual_value;
    switch (cfg.hpfb_option) {
        case R::HighPrecisionFramebuffer::On:   app->userConfig.internalColorFormat = UC::InternalColorFormat::High; break;
        case R::HighPrecisionFramebuffer::Auto: app->userConfig.internalColorFormat = UC::InternalColorFormat::Automatic; break;
        default:                                app->userConfig.internalColorFormat = UC::InternalColorFormat::Standard; break;
    }
    app->userConfig.displayBuffering = UC::DisplayBuffering::Triple;
}

R::SetupResult map_setup_result(RT64::Application::SetupResult r) {
    switch (r) {
        case RT64::Application::SetupResult::Success:                  return R::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound: return R::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:       return R::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:      return R::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:   return R::SetupResult::GraphicsDeviceNotFound;
    }
    return R::SetupResult::GraphicsDeviceNotFound;
}

class RT64Context final : public R::RendererContext {
public:
    RT64Context(uint8_t* rdram, R::WindowHandle window_handle, bool developer_mode) {
        RT64::Application::Core core{};
#if defined(_WIN32)
        core.window = window_handle.window;
#elif defined(__linux__)
        core.window = window_handle;
#elif defined(__APPLE__)
        core.window.window = window_handle.window;
        core.window.view = window_handle.view;
#endif
        core.checkInterrupts = check_interrupts_stub;
        core.HEADER = dummy_rom_header;
        core.RDRAM = rdram;
        core.DMEM = DMEM;
        core.IMEM = IMEM;
        core.MI_INTR_REG = &MI_INTR_REG;
        core.DPC_START_REG = &DPC_START_REG;
        core.DPC_END_REG = &DPC_END_REG;
        core.DPC_CURRENT_REG = &DPC_CURRENT_REG;
        core.DPC_STATUS_REG = &DPC_STATUS_REG;
        core.DPC_CLOCK_REG = &DPC_CLOCK_REG;
        core.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
        core.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
        core.DPC_TMEM_REG = &DPC_TMEM_REG;

        R::ViRegs* vi = R::get_vi_regs();
        core.VI_STATUS_REG = &vi->VI_STATUS_REG;
        core.VI_ORIGIN_REG = &vi->VI_ORIGIN_REG;
        core.VI_WIDTH_REG = &vi->VI_WIDTH_REG;
        core.VI_INTR_REG = &vi->VI_INTR_REG;
        core.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
        core.VI_TIMING_REG = &vi->VI_TIMING_REG;
        core.VI_V_SYNC_REG = &vi->VI_V_SYNC_REG;
        core.VI_H_SYNC_REG = &vi->VI_H_SYNC_REG;
        core.VI_LEAP_REG = &vi->VI_LEAP_REG;
        core.VI_H_START_REG = &vi->VI_H_START_REG;
        core.VI_V_START_REG = &vi->VI_V_START_REG;
        core.VI_V_BURST_REG = &vi->VI_V_BURST_REG;
        core.VI_X_SCALE_REG = &vi->VI_X_SCALE_REG;
        core.VI_Y_SCALE_REG = &vi->VI_Y_SCALE_REG;

        RT64::ApplicationConfiguration app_config;
        app_config.useConfigurationFile = false;
        // Keep RT64's log and caches in the game folder instead of %LOCALAPPDATA%.
        app_config.detectDataPath = false;
        app_config.dataPath = recomp::get_config_path() / "rt64";
        app = std::make_unique<RT64::Application>(core, app_config);

        const auto& cfg = R::get_graphics_config();
        apply_user_config(app.get(), cfg);
        app->userConfig.developerMode = developer_mode;
        switch (cfg.api_option) {
            case R::GraphicsApi::D3D12:  app->userConfig.graphicsAPI = UC::GraphicsAPI::D3D12; break;
            case R::GraphicsApi::Vulkan: app->userConfig.graphicsAPI = UC::GraphicsAPI::Vulkan; break;
            default:                     app->userConfig.graphicsAPI = UC::GraphicsAPI::Automatic; break;
        }
        // WINBACK_GFX_API=vulkan|d3d12 forces a backend (testing).
        if (const char* api = std::getenv("WINBACK_GFX_API")) {
            std::string v = api;
            if (v == "vulkan") app->userConfig.graphicsAPI = UC::GraphicsAPI::Vulkan;
            else if (v == "d3d12") app->userConfig.graphicsAPI = UC::GraphicsAPI::D3D12;
        }

        uint32_t thread_id = 0;
#ifdef _WIN32
        thread_id = window_handle.thread_id;
#endif
        setup_result = map_setup_result(app->setup(thread_id));
        switch (app->chosenGraphicsAPI) {
            case UC::GraphicsAPI::D3D12:  chosen_api = R::GraphicsApi::D3D12; break;
            case UC::GraphicsAPI::Vulkan: chosen_api = R::GraphicsApi::Vulkan; break;
            default:                      chosen_api = R::GraphicsApi::Auto; break;
        }
        if (setup_result != R::SetupResult::Success) {
            app = nullptr;
            return;
        }
        app->setFullScreen(cfg.wm_option == R::WindowMode::Fullscreen);
    }

    bool valid() override { return static_cast<bool>(app); }

    bool update_config(const R::GraphicsConfig& old_cfg, const R::GraphicsConfig& new_cfg) override {
        if (old_cfg == new_cfg) {
            return false;
        }
        if (new_cfg.wm_option != old_cfg.wm_option) {
            app->setFullScreen(new_cfg.wm_option == R::WindowMode::Fullscreen);
        }
        apply_user_config(app.get(), new_cfg);
        app->updateUserConfig(true);
        if (new_cfg.msaa_option != old_cfg.msaa_option) {
            app->updateMultisampling();
        }
        return true;
    }

    void enable_instant_present() override {
        app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        app->updateEnhancementConfig();
    }

    void send_dl(const OSTask* task) override {
        winback::stats::gfx_tasks++;
        dump_rects(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF);
        app->state->rsp->reset();
        app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
        app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
    }

    // Only used to draw under a launcher UI before the game starts; this host
    // boots straight into the game.
    void send_dummy_workload(uint32_t) override {}

    void update_screen() override {
        winback::stats::screen_updates++;
        if (framing_changed.exchange(false)) {
            app->userConfig.framing = current_framing();
            app->updateUserConfig(false);
        }
        app->updateScreen();
    }

    void shutdown() override {
        if (app) {
            app->end();
        }
    }

    uint32_t get_display_framerate() const override {
        return app->presentQueue->ext.sharedResources->swapChainRate;
    }

    float get_resolution_scale() const override {
        const UC::Framing& framing = app->userConfig.framing;
        const int reference_height = std::max(240 - (240 * (framing.cropTop + framing.cropBottom)) / 480, 1);
        switch (app->userConfig.resolution) {
            case UC::Resolution::WindowIntegerScale: {
                int h = app->sharedQueueResources->swapChainHeight;
                return h > 0 ? std::max(float((h + reference_height - 1) / reference_height), 1.0f) : 1.0f;
            }
            case UC::Resolution::Manual:
                return float(app->userConfig.resolutionMultiplier);
            default:
                return 1.0f;
        }
    }

private:
    std::unique_ptr<RT64::Application> app;
};

} // namespace

void winback::set_framing(bool crop_overscan, bool integer_scale) {
    framing_flags = (crop_overscan ? framing_crop : 0) | (integer_scale ? framing_integer : 0);
    framing_changed = true;
}

std::unique_ptr<ultramodern::renderer::RendererContext>
winback::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<RT64Context>(rdram, window_handle, developer_mode);
}
