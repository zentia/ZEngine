#pragma once

// The WebGL 2.0 RHI resource wrappers are Emscripten-only. The translation
// unit is compiled out on native toolchains so accidental inclusion does not
// drag <GLES3/gl3.h> into the native build.
#if defined(__EMSCRIPTEN__)

// -----------------------------------------------------------------------------
// WebGL 2.0 RHI resource wrappers.
//
// Mirrors the role of dx12/dx12_rhi_resource.h: every concrete class extends
// the corresponding RHI* base from rhi_struct.h so the engine can keep using
// the shared type system, while the actual GPU handle is a GLuint name issued
// by the WebGL 2.0 (≈ OpenGL ES 3.0) context.
//
// Targets Emscripten + WebGL2 (FULL_ES3=1). Use the GLES3 headers instead of
// desktop GL so the same code compiles in mini-game (微信/抖音/QQ/支付宝) hosts.
// -----------------------------------------------------------------------------

    #include "Runtime/Function/Render/Interface/RHIStruct.h"

    #include <GLES3/gl3.h>
    #include <cstdint>
    #include <map>
    #include <string>
    #include <unordered_map>
    #include <vector>

namespace ZEngine
{
    namespace WebGL2
    {

        // ---------------------------------------------------------------------------
        // Sync / queue / command primitives.
        //
        // WebGL 2 has no real command buffers. We model the abstraction by recording
        // commands into a simple in-memory list that the RHI replays on submit; this
        // keeps the engine-side render graph oblivious to the backend.
        // ---------------------------------------------------------------------------

        class WebGL2Queue final : public RHIQueue
        {
        };

        class WebGL2CommandPool final : public RHICommandPool
        {
        };

        class WebGL2CommandBuffer final : public RHICommandBuffer
        {
        public:
            enum class CommandKind : uint8_t
            {
                BeginRenderPass,
                EndRenderPass,
                BindPipeline,
                SetViewport,
                SetScissor,
                BindVertexBuffers,
                BindIndexBuffer,
                BindDescriptorSets,
                Draw,
                DrawIndexed,
                Dispatch,  // ignored on WebGL2 – kept for API parity
                CopyBuffer,
                CopyImageToBuffer,
                CopyImageToImage,
                BlitImage,
                ClearAttachments,
                PipelineBarrier,  // implicit in GL, but recorded for tooling
                PushDebugGroup,
                PopDebugGroup,
            };

            struct Command
            {
                CommandKind kind {CommandKind::BeginRenderPass};
                // Heterogeneous payload kept opaque on purpose; the RHI knows how to
                // interpret each variant in SubmitRendering(). Backed by an arena so
                // we don't pay malloc per draw.
                size_t payload_offset {0};
                size_t payload_size {0};
            };

            void reset()
            {
                m_Commands.clear();
                m_Arena.clear();
            }
            std::vector<Command>& commands() { return m_Commands; }
            const std::vector<Command>& commands() const { return m_Commands; }
            std::vector<uint8_t>& arena() { return m_Arena; }
            const std::vector<uint8_t>& arena() const { return m_Arena; }

        private:
            std::vector<Command> m_Commands;
            std::vector<uint8_t> m_Arena;
        };

        class WebGL2Fence final : public RHIFence
        {
        public:
            void setSync(GLsync sync) { m_Sync = sync; }
            GLsync getSync() const { return m_Sync; }
            void setSignaled(bool v) { m_Signaled = v; }
            bool isSignaled() const { return m_Signaled; }

        private:
            GLsync m_Sync {nullptr};
            bool m_Signaled {false};
        };

        class WebGL2Semaphore final : public RHISemaphore
        {
            // WebGL2 has a single in-order context, so semaphores collapse to no-ops.
        };

        // ---------------------------------------------------------------------------
        // Buffer / memory.
        //
        // In WebGL2 every buffer object owns its memory, so RHIBuffer and
        // RHIDeviceMemory are aliased to the same GLuint. We still produce a separate
        // WebGL2DeviceMemory wrapper to keep map/unmap semantics with a CPU shadow
        // copy (WebGL2 has no glMapBuffer; we have to glBufferSubData on unmap).
        // ---------------------------------------------------------------------------

        class WebGL2Buffer final : public RHIBuffer
        {
        public:
            void setHandle(GLuint name, GLenum target, RHIDeviceSize size)
            {
                m_Name = name;
                m_Target = target;
                m_Size = size;
            }
            GLuint getHandle() const { return m_Name; }
            GLenum getTarget() const { return m_Target; }
            RHIDeviceSize getSize() const { return m_Size; }

        private:
            GLuint m_Name {0};
            GLenum m_Target {GL_ARRAY_BUFFER};
            RHIDeviceSize m_Size {0};
        };

        class WebGL2DeviceMemory final : public RHIDeviceMemory
        {
        public:
            void setOwner(WebGL2Buffer* buffer) { m_Buffer = buffer; }
            WebGL2Buffer* getOwner() const { return m_Buffer; }

            // CPU-side staging. WebGL2 cannot persistently map; we allocate a host
            // buffer on map and flush via glBufferSubData on unmap.
            std::vector<uint8_t>& staging() { return m_Staging; }
            bool& mapped() { return m_Mapped; }
            RHIDeviceSize& mappedOffset() { return m_MappedOffset; }
            RHIDeviceSize& mappedSize() { return m_MappedSize; }

        private:
            WebGL2Buffer* m_Buffer {nullptr};
            std::vector<uint8_t> m_Staging;
            bool m_Mapped {false};
            RHIDeviceSize m_MappedOffset {0};
            RHIDeviceSize m_MappedSize {0};
        };

        // ---------------------------------------------------------------------------
        // Image / image view / sampler.
        // ---------------------------------------------------------------------------

        class WebGL2Image final : public RHIImage
        {
        public:
            void setHandle(GLuint texture, GLenum target, GLenum internal_format, GLenum format, GLenum type, uint32_t width, uint32_t height, uint32_t array_layers, uint32_t mip_levels)
            {
                m_Texture = texture;
                m_Target = target;
                m_InternalFormat = internal_format;
                m_Format = format;
                m_Type = type;
                m_Width = width;
                m_Height = height;
                m_ArrayLayers = array_layers;
                m_MipLevels = mip_levels;
            }

            GLuint GetTexture() const { return m_Texture; }
            GLenum getTarget() const { return m_Target; }
            GLenum getInternalFormat() const { return m_InternalFormat; }
            GLenum getFormat() const { return m_Format; }
            GLenum getType() const { return m_Type; }
            uint32_t getWidth() const { return m_Width; }
            uint32_t getHeight() const { return m_Height; }
            uint32_t getArrayLayers() const { return m_ArrayLayers; }
            uint32_t getMipLevels() const { return m_MipLevels; }

            // Some images are render-buffer backed (depth/stencil-only attachments
            // that never need to be sampled). m_Renderbuffer is non-zero in that case.
            void setRenderbuffer(GLuint rb) { m_Renderbuffer = rb; }
            GLuint getRenderbuffer() const { return m_Renderbuffer; }

        private:
            GLuint m_Texture {0};
            GLuint m_Renderbuffer {0};
            GLenum m_Target {GL_TEXTURE_2D};
            GLenum m_InternalFormat {GL_RGBA8};
            GLenum m_Format {GL_RGBA};
            GLenum m_Type {GL_UNSIGNED_BYTE};
            uint32_t m_Width {0};
            uint32_t m_Height {0};
            uint32_t m_ArrayLayers {1};
            uint32_t m_MipLevels {1};
        };

        class WebGL2ImageView final : public RHIImageView
        {
        public:
            void setImage(WebGL2Image* image, RHIImageViewType view_type, uint32_t base_layer, uint32_t layer_count, uint32_t base_mip, uint32_t mip_levels)
            {
                m_Image = image;
                m_ViewType = view_type;
                m_BaseLayer = base_layer;
                m_LayerCount = layer_count;
                m_BaseMip = base_mip;
                m_MipLevels = mip_levels;
            }

            WebGL2Image* getImage() const { return m_Image; }
            RHIImageViewType getViewType() const { return m_ViewType; }
            uint32_t getBaseLayer() const { return m_BaseLayer; }
            uint32_t getLayerCount() const { return m_LayerCount; }
            uint32_t getBaseMip() const { return m_BaseMip; }
            uint32_t getMipLevels() const { return m_MipLevels; }

        private:
            WebGL2Image* m_Image {nullptr};
            RHIImageViewType m_ViewType {RHI_IMAGE_VIEW_TYPE_2D};
            uint32_t m_BaseLayer {0};
            uint32_t m_LayerCount {1};
            uint32_t m_BaseMip {0};
            uint32_t m_MipLevels {1};
        };

        class WebGL2Sampler final : public RHISampler
        {
        public:
            void setHandle(GLuint sampler) { m_Sampler = sampler; }
            GLuint getHandle() const { return m_Sampler; }

        private:
            GLuint m_Sampler {0};
        };

        // ---------------------------------------------------------------------------
        // Shader / pipeline.
        //
        // WebGL 2.0 has no monolithic PSO. We pre-link a GL program, then carry the
        // fixed-function state (rasterizer, blend, depth-stencil, vertex layout) on
        // the pipeline object so the RHI can flush it lazily on each draw.
        // ---------------------------------------------------------------------------

        class WebGL2Shader final : public RHIShader
        {
        public:
            void setStage(GLenum stage, std::string glsl_source)
            {
                m_Stage = stage;
                m_Source = std::move(glsl_source);
            }
            GLenum getStage() const { return m_Stage; }
            const std::string& getSource() const { return m_Source; }

        private:
            GLenum m_Stage {GL_VERTEX_SHADER};
            std::string m_Source;
        };

        class WebGL2DescriptorSetLayout final : public RHIDescriptorSetLayout
        {
        public:
            void setBindings(const RHIDescriptorSetLayoutBinding* bindings, uint32_t count)
            {
                m_Bindings.assign(bindings, bindings + count);
            }
            const std::vector<RHIDescriptorSetLayoutBinding>& getBindings() const { return m_Bindings; }

        private:
            std::vector<RHIDescriptorSetLayoutBinding> m_Bindings;
        };

        class WebGL2DescriptorPool final : public RHIDescriptorPool
        {
        };

        // In WebGL2 we don't have descriptor sets. We keep the engine-facing object
        // alive but treat it as a CPU-side bag of (binding -> resource) records that
        // the pipeline binder iterates on draw.
        class WebGL2DescriptorSet final : public RHIDescriptorSet
        {
        public:
            struct UniformBufferBinding
            {
                WebGL2Buffer* buffer {nullptr};
                GLintptr offset {0};
                GLsizeiptr range {0};
            };
            struct SampledImageBinding
            {
                WebGL2ImageView* view {nullptr};
                WebGL2Sampler* sampler {nullptr};
            };

            void setLayout(WebGL2DescriptorSetLayout* layout) { m_Layout = layout; }
            WebGL2DescriptorSetLayout* getLayout() const { return m_Layout; }

            std::unordered_map<uint32_t, UniformBufferBinding>& uboBindings() { return m_Ubos; }
            std::unordered_map<uint32_t, SampledImageBinding>& imageBindings() { return m_Images; }

        private:
            WebGL2DescriptorSetLayout* m_Layout {nullptr};
            std::unordered_map<uint32_t, UniformBufferBinding> m_Ubos;
            std::unordered_map<uint32_t, SampledImageBinding> m_Images;
        };

        class WebGL2PipelineLayout final : public RHIPipelineLayout
        {
        public:
            void setLayouts(std::vector<WebGL2DescriptorSetLayout*> layouts) { m_Layouts = std::move(layouts); }
            const std::vector<WebGL2DescriptorSetLayout*>& getLayouts() const { return m_Layouts; }

        private:
            std::vector<WebGL2DescriptorSetLayout*> m_Layouts;
        };

        class WebGL2RenderPass final : public RHIRenderPass
        {
        public:
            void setAttachments(const RHIAttachmentDescription* attachments, uint32_t count)
            {
                m_Attachments.assign(attachments, attachments + count);
            }
            const std::vector<RHIAttachmentDescription>& getAttachments() const { return m_Attachments; }

        private:
            std::vector<RHIAttachmentDescription> m_Attachments;
        };

        class WebGL2Framebuffer final : public RHIFramebuffer
        {
        public:
            void SetSize(uint32_t w, uint32_t h, uint32_t layers)
            {
                m_Width = w;
                m_Height = h;
                m_Layers = layers;
            }
            void setHandle(GLuint fbo) { m_Fbo = fbo; }
            void setRenderPass(WebGL2RenderPass* rp) { m_RenderPass = rp; }
            void addColorAttachment(WebGL2ImageView* view) { m_Colors.push_back(view); }
            void setDepthStencilAttachment(WebGL2ImageView* view) { m_DepthStencil = view; }

            GLuint getHandle() const { return m_Fbo; }
            WebGL2RenderPass* GetRenderPass() const { return m_RenderPass; }
            const std::vector<WebGL2ImageView*>& getColorAttachments() const { return m_Colors; }
            WebGL2ImageView* getDepthStencilAttachment() const { return m_DepthStencil; }
            uint32_t getWidth() const { return m_Width; }
            uint32_t getHeight() const { return m_Height; }

        private:
            GLuint m_Fbo {0};
            WebGL2RenderPass* m_RenderPass {nullptr};
            std::vector<WebGL2ImageView*> m_Colors;
            WebGL2ImageView* m_DepthStencil {nullptr};
            uint32_t m_Width {0};
            uint32_t m_Height {0};
            uint32_t m_Layers {1};
        };

        class WebGL2Pipeline final : public RHIPipeline
        {
        public:
            struct VertexBinding
            {
                uint32_t binding {0};
                uint32_t stride {0};
                GLenum input_rate {GL_FALSE};  // GL_FALSE = per-vertex; GL_TRUE = per-instance
            };
            struct VertexAttribute
            {
                uint32_t location {0};
                uint32_t binding {0};
                GLint components {4};
                GLenum type {GL_FLOAT};
                GLboolean normalized {GL_FALSE};
                uint32_t offset {0};
            };

            void setProgram(GLuint program) { m_Program = program; }
            GLuint getProgram() const { return m_Program; }

            void setLayout(WebGL2PipelineLayout* layout) { m_Layout = layout; }
            WebGL2PipelineLayout* getLayout() const { return m_Layout; }

            void setPrimitiveTopology(GLenum topo) { m_Topology = topo; }
            GLenum getPrimitiveTopology() const { return m_Topology; }

            std::vector<VertexBinding>& vertexBindings() { return m_VBindings; }
            std::vector<VertexAttribute>& vertexAttributes() { return m_VAttribs; }

            // Pipeline-baked fixed-function state. Flush these on bind.
            bool depth_test_enable {true};
            bool depth_write_enable {true};
            GLenum depth_func {GL_LESS};
            bool cull_enable {true};
            GLenum cull_face {GL_BACK};
            GLenum front_face {GL_CCW};
            bool blend_enable {false};
            GLenum blend_src_rgb {GL_ONE};
            GLenum blend_dst_rgb {GL_ZERO};
            GLenum blend_src_a {GL_ONE};
            GLenum blend_dst_a {GL_ZERO};
            GLenum blend_op_rgb {GL_FUNC_ADD};
            GLenum blend_op_a {GL_FUNC_ADD};

        private:
            GLuint m_Program {0};
            WebGL2PipelineLayout* m_Layout {nullptr};
            GLenum m_Topology {GL_TRIANGLES};
            std::vector<VertexBinding> m_VBindings;
            std::vector<VertexAttribute> m_VAttribs;
        };

    }  // namespace WebGL2
}  // namespace ZEngine

#endif  // __EMSCRIPTEN__
