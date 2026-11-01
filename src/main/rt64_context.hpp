#pragma once

#include <cstdint>
#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace winback {
    std::unique_ptr<ultramodern::renderer::RendererContext>
    create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
}
