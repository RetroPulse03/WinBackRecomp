// RmlUi render interface on RT64's render hardware interface.
//
// Adapted from Zelda64Recomp's src/ui/ui_renderer.cpp (GPL-3.0). The menu
// only uses generated textures (font glyphs) and solid or gradient geometry,
// so image loading and the MSAA resolve pass are left out. RmlUi 6 hands over
// premultiplied vertex colours; the pixel shader premultiplies the texture,
// so blending is ONE / INV_SRC_ALPHA.

#include <cassert>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>

#include "rt64_render_hooks.h"
#include "rt64_render_interface_builders.h"

#include "RmlUi/Core/RenderInterfaceCompatibility.h"

#include "ui_renderer.hpp"

#include "InterfaceVS.hlsl.spirv.h"
#include "InterfacePS.hlsl.spirv.h"
#ifdef _WIN32
#include "InterfaceVS.hlsl.dxil.h"
#include "InterfacePS.hlsl.dxil.h"
#define SHADER_BLOB(name, format) \
    ((format) == RT64::RenderShaderFormat::DXIL ? name##BlobDXIL : name##BlobSPIRV)
#define SHADER_SIZE(name, format) \
    ((format) == RT64::RenderShaderFormat::DXIL ? std::size(name##BlobDXIL) : std::size(name##BlobSPIRV))
#else
#define SHADER_BLOB(name, format) name##BlobSPIRV
#define SHADER_SIZE(name, format) std::size(name##BlobSPIRV)
#endif

namespace {

struct PushConstants {
    Rml::Matrix4f transform;
    Rml::Vector2f translation;
};

struct Texture {
    std::unique_ptr<RT64::RenderTexture> texture;
    std::unique_ptr<RT64::RenderDescriptorSet> set;
    bool transitioned = false;
};

// A mapped upload buffer that grows as needed; replaced buffers stay alive
// until the next frame's command list.
struct DynamicBuffer {
    std::unique_ptr<RT64::RenderBuffer> buffer;
    uint32_t size = 0;
    uint32_t used = 0;
    uint8_t* mapped = nullptr;
    RT64::RenderBufferFlags flags = RT64::RenderBufferFlag::NONE;
};

constexpr RT64::RenderFormat texture_format = RT64::RenderFormat::R8G8B8A8_UNORM;
constexpr RT64::RenderFormat swap_chain_format = RT64::RenderFormat::B8G8R8A8_UNORM;
constexpr uint32_t bytes_per_pixel = 4;

} // namespace

class winback::ui::RmlRenderer::Impl final : public Rml::RenderInterfaceCompatibility {
public:
    Impl(RT64::RenderInterface* rhi, RT64::RenderDevice* device) : rhi(rhi), device(device) {
        vertex_buffer.flags = RT64::RenderBufferFlag::VERTEX;
        index_buffer.flags = RT64::RenderBufferFlag::INDEX;
        resize(vertex_buffer, 512 * sizeof(Rml::Vertex), false);
        resize(index_buffer, 1024 * sizeof(int), false);

        RT64::RenderSamplerDesc sampler_desc;
        sampler_desc.minFilter = RT64::RenderFilter::LINEAR;
        sampler_desc.magFilter = RT64::RenderFilter::LINEAR;
        sampler_desc.addressU = RT64::RenderTextureAddressMode::CLAMP;
        sampler_desc.addressV = RT64::RenderTextureAddressMode::CLAMP;
        sampler_desc.addressW = RT64::RenderTextureAddressMode::CLAMP;
        sampler = device->createSampler(sampler_desc);

        RT64::RenderShaderFormat format = rhi->getCapabilities().shaderFormat;
        vertex_shader = device->createShader(SHADER_BLOB(InterfaceVS, format), SHADER_SIZE(InterfaceVS, format), "VSMain", format);
        pixel_shader = device->createShader(SHADER_BLOB(InterfacePS, format), SHADER_SIZE(InterfacePS, format), "PSMain", format);

        RT64::RenderDescriptorSetBuilder sampler_builder;
        sampler_builder.begin();
        sampler_builder.addImmutableSampler(1, sampler.get());
        sampler_builder.addConstantBuffer(3, 1); // D3D12 rejects an empty descriptor set.
        sampler_builder.end();
        sampler_set = sampler_builder.create(device);

        texture_set_builder = std::make_unique<RT64::RenderDescriptorSetBuilder>();
        texture_set_builder->begin();
        texture_index = texture_set_builder->addTexture(2);
        texture_set_builder->end();

        RT64::RenderPipelineLayoutBuilder layout_builder;
        layout_builder.begin(false, true);
        layout_builder.addPushConstant(0, 0, sizeof(PushConstants), RT64::RenderShaderStageFlag::VERTEX);
        layout_builder.addDescriptorSet(sampler_builder);
        layout_builder.addDescriptorSet(*texture_set_builder);
        layout_builder.end();
        layout = layout_builder.create(device);

        RT64::RenderInputElement elements[] = {
            {"POSITION", 0, 0, RT64::RenderFormat::R32G32_FLOAT, 0, offsetof(Rml::Vertex, position)},
            {"COLOR", 0, 1, RT64::RenderFormat::R8G8B8A8_UNORM, 0, offsetof(Rml::Vertex, colour)},
            {"TEXCOORD", 0, 2, RT64::RenderFormat::R32G32_FLOAT, 0, offsetof(Rml::Vertex, tex_coord)},
        };
        RT64::RenderGraphicsPipelineDesc desc;
        desc.renderTargetBlend[0] = RT64::RenderBlendDesc{
            .blendEnabled = true,
            .srcBlend = RT64::RenderBlend::ONE,
            .dstBlend = RT64::RenderBlend::INV_SRC_ALPHA,
            .blendOp = RT64::RenderBlendOperation::ADD,
            .srcBlendAlpha = RT64::RenderBlend::ONE,
            .dstBlendAlpha = RT64::RenderBlend::INV_SRC_ALPHA,
            .blendOpAlpha = RT64::RenderBlendOperation::ADD,
        };
        desc.renderTargetFormat[0] = swap_chain_format;
        desc.renderTargetCount = 1;
        desc.cullMode = RT64::RenderCullMode::NONE;
        desc.inputSlots = &vertex_slot;
        desc.inputSlotsCount = 1;
        desc.inputElements = elements;
        desc.inputElementsCount = uint32_t(std::size(elements));
        desc.pipelineLayout = layout.get();
        desc.primitiveTopology = RT64::RenderPrimitiveTopology::TRIANGLE_LIST;
        desc.vertexShader = vertex_shader.get();
        desc.pixelShader = pixel_shader.get();
        pipeline = device->createGraphicsPipeline(desc);

        copy_queue = device->createCommandQueue(RT64::RenderCommandListType::COPY);
        copy_list = copy_queue->createCommandList(RT64::RenderCommandListType::COPY);
        copy_fence = device->createCommandFence();
    }

    void RenderGeometry(Rml::Vertex* vertices, int num_vertices, int* indices, int num_indices,
                        Rml::TextureHandle texture, const Rml::Vector2f& translation) override {
        if (!textures.contains(texture)) {
            // Handle 0 is untextured geometry: a single white pixel.
            if (texture != 0) {
                return;
            }
            const Rml::byte white[] = {255, 255, 255, 255};
            create_texture(0, white, {1, 1});
        }

        uint32_t vertex_bytes = num_vertices * sizeof(*vertices);
        uint32_t index_bytes = num_indices * sizeof(*indices);
        uint32_t vertex_offset = allocate(vertex_buffer, vertex_bytes);
        uint32_t index_offset = allocate(index_buffer, index_bytes);
        std::memcpy(vertex_buffer.mapped + vertex_offset, vertices, vertex_bytes);
        std::memcpy(index_buffer.mapped + index_offset, indices, index_bytes);

        list->setViewports(RT64::RenderViewport{0, 0, float(width), float(height)});
        if (scissor_enabled) {
            list->setScissors(RT64::RenderRect{scissor_x, scissor_y, scissor_x + scissor_w, scissor_y + scissor_h});
        }
        else {
            list->setScissors(RT64::RenderRect{0, 0, width, height});
        }

        RT64::RenderIndexBufferView index_view{index_buffer.buffer->at(index_offset), index_bytes, RT64::RenderFormat::R32_UINT};
        list->setIndexBuffer(&index_view);
        RT64::RenderVertexBufferView vertex_view{vertex_buffer.buffer->at(vertex_offset), vertex_bytes};
        list->setVertexBuffers(0, &vertex_view, 1, &vertex_slot);

        Texture& tex = textures.at(texture);
        if (!tex.transitioned) {
            list->barriers(RT64::RenderBarrierStage::GRAPHICS, RT64::RenderTextureBarrier(tex.texture.get(), RT64::RenderTextureLayout::SHADER_READ));
            tex.transitioned = true;
        }
        list->setGraphicsDescriptorSet(tex.set.get(), 1);

        PushConstants constants{.transform = mvp, .translation = translation};
        list->setGraphicsPushConstants(0, &constants);
        list->drawIndexedInstanced(num_indices, 1, 0, 0, 0);
    }

    void EnableScissorRegion(bool enable) override {
        scissor_enabled = enable;
    }

    void SetScissorRegion(int x, int y, int w, int h) override {
        scissor_x = x;
        scissor_y = y;
        scissor_w = w;
        scissor_h = h;
    }

    // The menu doesn't load image files.
    bool LoadTexture(Rml::TextureHandle&, Rml::Vector2i&, const Rml::String&) override {
        return false;
    }

    bool GenerateTexture(Rml::TextureHandle& handle, const Rml::byte* source, const Rml::Vector2i& size) override {
        if (size.x == 0 || size.y == 0) {
            handle = 0;
            return true;
        }
        handle = next_texture++;
        return create_texture(handle, source, size);
    }

    void ReleaseTexture(Rml::TextureHandle handle) override {
        if (handle != 0) {
            textures.erase(handle);
        }
    }

    void SetTransform(const Rml::Matrix4f* transform) override {
        this->transform = transform ? *transform : Rml::Matrix4f::Identity();
        mvp = projection * this->transform;
    }

    void start(RT64::RenderCommandList* list, int width, int height) {
        this->list = list;
        this->width = width;
        this->height = height;
        list->setPipeline(pipeline.get());
        list->setGraphicsPipelineLayout(layout.get());
        list->setGraphicsDescriptorSet(sampler_set.get(), 0);
        projection = Rml::Matrix4f::ProjectOrtho(0.0f, float(width), float(height), 0.0f, -10000, 10000);
        mvp = projection * transform;
        // Command lists aren't double buffered, so last frame's buffers are free.
        stale_buffers.clear();
        begin(vertex_buffer);
        begin(index_buffer);
    }

    void end() {
        finish(vertex_buffer);
        finish(index_buffer);
        list = nullptr;
    }

private:
    void begin(DynamicBuffer& b) {
        b.used = 0;
        b.mapped = static_cast<uint8_t*>(b.buffer->map());
    }

    void finish(DynamicBuffer& b) {
        b.buffer->unmap();
        b.mapped = nullptr;
    }

    void resize(DynamicBuffer& b, uint32_t size, bool map) {
        if (b.mapped != nullptr) {
            b.buffer->unmap();
            b.mapped = nullptr;
        }
        if (b.buffer != nullptr) {
            stale_buffers.emplace_back(std::move(b.buffer));
        }
        b.buffer = device->createBuffer(RT64::RenderBufferDesc::UploadBuffer(size, b.flags));
        b.size = size;
        b.used = 0;
        if (map) {
            b.mapped = static_cast<uint8_t*>(b.buffer->map());
        }
    }

    uint32_t allocate(DynamicBuffer& b, uint32_t bytes) {
        if (b.used + bytes > b.size) {
            uint32_t total = b.used + bytes;
            resize(b, total + total / 2, true);
        }
        uint32_t offset = b.used;
        b.used += bytes;
        return offset;
    }

    bool create_texture(Rml::TextureHandle handle, const Rml::byte* source, const Rml::Vector2i& size) {
        auto texture = device->createTexture(RT64::RenderTextureDesc::Texture2D(size.x, size.y, 1, texture_format));
        if (texture == nullptr) {
            return false;
        }
        // Rows of a placed footprint are aligned to 256 bytes.
        uint32_t row_pitch = size.x * bytes_per_pixel;
        uint32_t padded_pitch = (row_pitch + 255) & ~255u;
        uint64_t upload_size = uint64_t(padded_pitch) * size.y;
        if (upload_size > copy_buffer_size) {
            copy_buffer_size = upload_size * 3 / 2;
            copy_buffer = device->createBuffer(RT64::RenderBufferDesc::UploadBuffer(copy_buffer_size));
        }
        auto* dst = static_cast<uint8_t*>(copy_buffer->map());
        for (int row = 0; row < size.y; row++) {
            std::memcpy(dst + size_t(row) * padded_pitch, source + size_t(row) * row_pitch, row_pitch);
        }
        copy_buffer->unmap();

        copy_list->begin();
        copy_list->barriers(RT64::RenderBarrierStage::COPY, RT64::RenderTextureBarrier(texture.get(), RT64::RenderTextureLayout::COPY_DEST));
        copy_list->copyTextureRegion(
            RT64::RenderTextureCopyLocation::Subresource(texture.get()),
            RT64::RenderTextureCopyLocation::PlacedFootprint(copy_buffer.get(), texture_format, size.x, size.y, 1, padded_pitch / bytes_per_pixel));
        copy_list->end();
        copy_queue->executeCommandLists(copy_list.get(), copy_fence.get());
        copy_queue->waitForCommandFence(copy_fence.get());

        auto set = texture_set_builder->create(device);
        set->setTexture(texture_index, texture.get(), RT64::RenderTextureLayout::SHADER_READ);
        textures[handle] = Texture{std::move(texture), std::move(set), false};
        return true;
    }

    RT64::RenderInterface* rhi;
    RT64::RenderDevice* device;
    int width = 0;
    int height = 0;
    bool scissor_enabled = false;
    int scissor_x = 0, scissor_y = 0, scissor_w = 0, scissor_h = 0;
    Rml::Matrix4f projection = Rml::Matrix4f::Identity();
    Rml::Matrix4f transform = Rml::Matrix4f::Identity();
    Rml::Matrix4f mvp = Rml::Matrix4f::Identity();
    std::unordered_map<Rml::TextureHandle, Texture> textures;
    Rml::TextureHandle next_texture = 1;
    DynamicBuffer vertex_buffer;
    DynamicBuffer index_buffer;
    std::vector<std::unique_ptr<RT64::RenderBuffer>> stale_buffers;
    std::unique_ptr<RT64::RenderSampler> sampler;
    std::unique_ptr<RT64::RenderShader> vertex_shader;
    std::unique_ptr<RT64::RenderShader> pixel_shader;
    std::unique_ptr<RT64::RenderDescriptorSet> sampler_set;
    std::unique_ptr<RT64::RenderDescriptorSetBuilder> texture_set_builder;
    uint32_t texture_index = 0;
    std::unique_ptr<RT64::RenderPipelineLayout> layout;
    std::unique_ptr<RT64::RenderPipeline> pipeline;
    std::unique_ptr<RT64::RenderCommandQueue> copy_queue;
    std::unique_ptr<RT64::RenderCommandList> copy_list;
    std::unique_ptr<RT64::RenderCommandFence> copy_fence;
    std::unique_ptr<RT64::RenderBuffer> copy_buffer;
    uint64_t copy_buffer_size = 0;
    RT64::RenderInputSlot vertex_slot{0, sizeof(Rml::Vertex)};
    RT64::RenderCommandList* list = nullptr;
};

winback::ui::RmlRenderer::RmlRenderer(RT64::RenderInterface* rhi, RT64::RenderDevice* device)
    : impl(std::make_unique<Impl>(rhi, device)) {}

winback::ui::RmlRenderer::~RmlRenderer() = default;

Rml::RenderInterface* winback::ui::RmlRenderer::rml_interface() {
    return impl->GetAdaptedInterface();
}

void winback::ui::RmlRenderer::start(RT64::RenderCommandList* list, int width, int height) {
    impl->start(list, width, height);
}

void winback::ui::RmlRenderer::end() {
    impl->end();
}
