#pragma once

// RmlUi render interface that draws through RT64's render hardware interface
// into the swap chain, after RT64 has drawn the game.

#include <memory>

namespace RT64 {
    struct RenderInterface;
    struct RenderDevice;
    struct RenderCommandList;
    struct RenderFramebuffer;
}

namespace Rml {
    class RenderInterface;
}

namespace winback::ui {
    class RmlRenderer {
    public:
        RmlRenderer(RT64::RenderInterface* rhi, RT64::RenderDevice* device);
        ~RmlRenderer();
        Rml::RenderInterface* rml_interface();
        void start(RT64::RenderCommandList* list, int width, int height);
        void end();

    private:
        class Impl;
        std::unique_ptr<Impl> impl;
    };
}
