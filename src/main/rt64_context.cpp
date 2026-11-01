// RT64 renderer backend for N64ModernRuntime.
//
// RT64 interprets the game's display lists (F3DEX2/L3DEX2 2.08) in HLE and
// renders them with Direct3D 12 or Vulkan.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#ifndef HLSL_CPU
#define HLSL_CPU
#endif
#include "hle/rt64_application.h"

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

using UC = RT64::UserConfiguration;
namespace R = ultramodern::renderer;

void apply_user_config(RT64::Application* app, const R::GraphicsConfig& cfg) {
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
    switch (cfg.ar_option) {
        case R::AspectRatio::Expand: app->userConfig.aspectRatio = UC::AspectRatio::Expand; break;
        case R::AspectRatio::Manual: app->userConfig.aspectRatio = UC::AspectRatio::Manual; break;
        default:                     app->userConfig.aspectRatio = UC::AspectRatio::Original; break;
    }
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
        app->state->rsp->reset();
        app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
        app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
    }

    // Only used to draw under a launcher UI before the game starts; this host
    // boots straight into the game.
    void send_dummy_workload(uint32_t) override {}

    void update_screen() override {
        winback::stats::screen_updates++;
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
        constexpr int reference_height = 240;
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

std::unique_ptr<ultramodern::renderer::RendererContext>
winback::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<RT64Context>(rdram, window_handle, developer_mode);
}
