#pragma once

#include <cstdint>
#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace winback {
    std::unique_ptr<ultramodern::renderer::RendererContext>
    create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

    // How the game's picture is placed in the window. crop_overscan hides the black border the game
    // leaves around its picture; integer_scale scales it only by whole multiples. Takes effect on the
    // next screen update.
    void set_framing(bool crop_overscan, bool integer_scale);
}
