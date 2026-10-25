// -----------------------------------------------------------------------------
// WebGL 2.0 RHI backend - skeleton implementation.
//
// All overrides are intentionally minimal: enough so the file compiles on
// Emscripten (USE_WEBGL2=1, FULL_ES3=1) and the engine can boot to a cleared
// canvas. Each WEBGL2_TODO marker pinpoints work to flesh out incrementally.
//
// Reference backends:
//   - dx12/dx12_rhi.cpp
//   - vulkan/vulkan_rhi.cpp
// -----------------------------------------------------------------------------

#if defined(__EMSCRIPTEN__)

    #include "Runtime/Function/Render/Interface/WebGL2/WebGL2RHI.h"

    #include "Runtime/Function/Render/Interface/RHIFactory.h"
    #include "Runtime/Core/Base/Macro.h"
    #include "Runtime/Function/Render/WindowSystem.h"

    #include <algorithm>
    #include <cstring>
    #include <emscripten/emscripten.h>
    #include <emscripten/html5.h>

namespace ZEngine
{
    namespace WebGL2
    {

    #define WEBGL2_NOOP()    ((void)0)
    #define WEBGL2_TODO(msg) ((void)0)  // hook your logger here

        // ---------- EngineSystem ----------

        std::vector<std::type_index> WebGL2RHI::GetDependencies() const
        {
            return {};
        }

        bool WebGL2RHI::Initialize()
        {
            m_ShaderCompiler = std::make_unique<WebGL2ShaderCompiler>();
            for (uint8_t i = 0; i < k_max_frames_in_flight; ++i)
            {
                m_CommandBufferPtrs[i] = &m_CommandBuffers[i];
                m_FrameFencePtrs[i] = &m_FrameFences[i];
            }
            m_CurrentCommandBuffer = &m_CommandBuffers[0];

            // Adopt the current WebGL2 context if the host made one, otherwise
            // create one on #canvas (see CreateGLContext).
            if (!CreateGLContext(GET_SYSTEM(WindowSystem)))
            {
                LOG_FATAL(ZRender, "WebGL2RHI: failed to create / acquire a WebGL2 context on #canvas");
                return false;
            }
            CreateSwapchain();
            return true;
        }

        void WebGL2RHI::Shutdown()
        {
            DestroyGLContext();
            m_ShaderCompiler.reset();
        }
        void WebGL2RHI::PrepareContext()
        {
            WEBGL2_NOOP();
        }
        bool WebGL2RHI::IsPointLightShadowEnabled()
        {
            return false;
        }
        void WebGL2RHI::NotifyWindowFocusGained()
        {
            // WebGL2 has no swapchain surface chain to invalidate (the drawable
            // is canvas framebuffer 0, recomposited by the browser). The one
            // thing worth doing on focus regain is re-sync the drawable size:
            // the canvas may have been resized while the tab was backgrounded.
            // CreateSwapchain() re-reads the canvas and re-issues glViewport.
            CreateSwapchain();
        }

        // ---------- Allocation ----------

        bool WebGL2RHI::AllocateCommandBuffers(const RHICommandBufferAllocateInfo*, RHICommandBuffer*& out)
        {
            out = m_CommandBufferPtrs[m_CurrentFrameIndex];
            return out != nullptr;
        }

        bool WebGL2RHI::AllocateDescriptorSets(const RHIDescriptorSetAllocateInfo*, RHIDescriptorSet*& out)
        {
            out = new WebGL2DescriptorSet();
            return true;
        }

        // ---------- Swapchain ----------

        void WebGL2RHI::CreateSwapchain()
        {
            // The "swapchain" on WebGL2 is the default framebuffer (the canvas itself,
            // FBO 0). All we need to track is its size so the engine's viewport / RT
            // sizing math has correct numbers. The actual present is implicit on
            // requestAnimationFrame return.
            int w = 0;
            int h = 0;
            if (emscripten_get_canvas_element_size("#canvas", &w, &h) != EMSCRIPTEN_RESULT_SUCCESS || w <= 0 || h <= 0)
            {
                // Fall back to whatever the GL context advertises (may be the default 300x150).
                w = m_DrawableWidth > 0 ? m_DrawableWidth : 1280;
                h = m_DrawableHeight > 0 ? m_DrawableHeight : 720;
            }
            m_DrawableWidth = w;
            m_DrawableHeight = h;
            m_SwapchainExtent.width = static_cast<uint32_t>(w);
            m_SwapchainExtent.height = static_cast<uint32_t>(h);
            m_DefaultFramebuffer = 0;  // canvas
            glViewport(0, 0, w, h);
        }

        void WebGL2RHI::RecreateSwapchain()
        {
            // Picked up next frame from the canvas size; emscripten resizes the
            // drawing buffer for us when JS sets canvas.width / canvas.height.
            CreateSwapchain();
        }
        void WebGL2RHI::CreateSwapchainImageViews()
        {
            WEBGL2_NOOP();
        }
        void WebGL2RHI::CreateFramebufferImageAndView()
        {
            WEBGL2_TODO("attach depth/stencil rb");
        }

        // ---------- Sampler / shader ----------

        RHISampler* WebGL2RHI::GetOrCreateDefaultSampler(RHIDefaultSamplerType)
        {
            return new WebGL2Sampler();
        }
        RHISampler* WebGL2RHI::GetOrCreateMipmapSampler(uint32_t, uint32_t)
        {
            return new WebGL2Sampler();
        }

        RHIShader* WebGL2RHI::CreateShaderModule(const std::vector<unsigned char>& code)
        {
            auto* s = new WebGL2Shader();
            s->setStage(GL_VERTEX_SHADER, std::string(reinterpret_cast<const char*>(code.data()), code.size()));
            return s;
        }

        RHIShader* WebGL2RHI::CreateShaderModuleFromFile(const std::string& file_path,
                                                         ShaderStage shader_stage,
                                                         const std::vector<std::string>& include_paths,
                                                         const ShaderMacros& /*macros*/,
                                                         std::vector<uint8_t>& output_binary,
                                                         const std::string& /*entry_point*/,
                                                         bool /*embed_debug*/)
        {
            (void)output_binary;  // WebGL2 does not produce binary blobs
            if (!m_ShaderCompiler)
            {
                return nullptr;
            }
            std::map<std::string, std::string> macro_map;
            // TODO: pass embed_debug to WebGL2ShaderCompiler if host-side
            //       debug info becomes useful (currently emcc compiles GLSL
            //       to GL at runtime, debug info is browser-side).
            auto r = m_ShaderCompiler->CompileFromFile(file_path, shader_stage, include_paths, macro_map);
            if (!r.success)
            {
                return nullptr;
            }
            auto* s = new WebGL2Shader();
            s->setStage(GL_VERTEX_SHADER, r.glsl_source);
            return s;
        }

        RHIShader* WebGL2RHI::CreateShaderModuleFromSource(const std::string& source_code, ShaderStage shader_stage, const std::string& shader_name, const std::vector<std::string>& include_paths, const ShaderMacros& /*macros*/)
        {
            if (!m_ShaderCompiler)
            {
                return nullptr;
            }
            auto r = m_ShaderCompiler->CompileFromSource(source_code, shader_stage, shader_name, include_paths);
            if (!r.success)
            {
                return nullptr;
            }
            auto* s = new WebGL2Shader();
            s->setStage(GL_VERTEX_SHADER, r.glsl_source);
            return s;
        }

        // ---------- Buffer ----------

        void WebGL2RHI::CreateBuffer(RHIDeviceSize size, RHIBufferUsageFlags, RHIMemoryPropertyFlags, RHIBuffer*& out_buffer, RHIDeviceMemory*& out_memory)
        {
            auto* buffer = new WebGL2Buffer();
            auto* memory = new WebGL2DeviceMemory();
            GLuint name = 0;
            glGenBuffers(1, &name);
            glBindBuffer(GL_ARRAY_BUFFER, name);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(size), nullptr, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            buffer->setHandle(name, GL_ARRAY_BUFFER, size);
            memory->setOwner(buffer);
            out_buffer = buffer;
            out_memory = memory;
        }

        void WebGL2RHI::CreateBufferAndInitialize(RHIBufferUsageFlags usage, RHIMemoryPropertyFlags props, RHIBuffer*& out_buffer, RHIDeviceMemory*& out_memory, RHIDeviceSize size, void* data, int datasize)
        {
            CreateBuffer(size, usage, props, out_buffer, out_memory);
            if (data != nullptr && datasize > 0)
            {
                auto* b = static_cast<WebGL2Buffer*>(out_buffer);
                glBindBuffer(GL_ARRAY_BUFFER, b->getHandle());
                glBufferSubData(GL_ARRAY_BUFFER, 0, datasize, data);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }
        }

        void WebGL2RHI::CopyBuffer(RHIBuffer* src, RHIBuffer* dst, RHIDeviceSize sOff, RHIDeviceSize dOff, RHIDeviceSize size)
        {
            glBindBuffer(GL_COPY_READ_BUFFER, static_cast<WebGL2Buffer*>(src)->getHandle());
            glBindBuffer(GL_COPY_WRITE_BUFFER, static_cast<WebGL2Buffer*>(dst)->getHandle());
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(sOff), static_cast<GLintptr>(dOff), static_cast<GLsizeiptr>(size));
            glBindBuffer(GL_COPY_READ_BUFFER, 0);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }

        // ---------- Image ----------

        void WebGL2RHI::CreateImage(uint32_t w, uint32_t h, RHIFormat fmt, RHIImageTiling, RHIImageUsageFlags, RHIMemoryPropertyFlags, RHIImage*& out_image, RHIDeviceMemory*& out_memory, RHIImageCreateFlags, uint32_t array_layers, uint32_t miplevels)
        {
            auto* image = new WebGL2Image();
            GLuint name = 0;
            glGenTextures(1, &name);
            GLenum target = (array_layers == 6) ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
            GLenum ifmt = ToGLInternalFormat(fmt);
            GLenum gl_fmt = ToGLFormat(fmt);
            GLenum gl_type = ToGLType(fmt);
            GLsizei mips = miplevels ? static_cast<GLsizei>(miplevels) : 1;
            glBindTexture(target, name);
            glTexStorage2D(target, mips, ifmt, w, h);
            glBindTexture(target, 0);
            image->setHandle(name, target, ifmt, gl_fmt, gl_type, w, h, array_layers, mips);
            out_image = image;
            out_memory = nullptr;
        }

        void WebGL2RHI::CreateImageView(RHIImage* image, RHIFormat, RHIImageAspectFlags, RHIImageViewType view_type, uint32_t layer_count, uint32_t miplevels, RHIImageView*& out_view)
        {
            auto* v = new WebGL2ImageView();
            v->setImage(static_cast<WebGL2Image*>(image), view_type, 0, layer_count, 0, miplevels);
            out_view = v;
        }

        void WebGL2RHI::CreateGlobalImage(RHIImage*& image, RHIImageView*& view, void*, uint32_t w, uint32_t h, void* pixels, RHIFormat fmt, uint32_t miplevels)
        {
            RHIDeviceMemory* mem = nullptr;
            CreateImage(w, h, fmt, RHI_IMAGE_TILING_OPTIMAL, 0u, 0u, image, mem, 0u, 1u, miplevels);
            if (pixels != nullptr)
            {
                auto* gi = static_cast<WebGL2Image*>(image);
                glBindTexture(gi->getTarget(), gi->GetTexture());
                glTexSubImage2D(gi->getTarget(), 0, 0, 0, w, h, gi->getFormat(), gi->getType(), pixels);
                if (miplevels > 1)
                {
                    glGenerateMipmap(gi->getTarget());
                }
                glBindTexture(gi->getTarget(), 0);
            }
            CreateImageView(image, fmt, 0, RHI_IMAGE_VIEW_TYPE_2D, 1, miplevels ? miplevels : 1, view);
        }

        void WebGL2RHI::CreateCubeMap(RHIImage*& image, RHIImageView*& view, void*, uint32_t w, uint32_t h, std::array<void*, 6> pixels, RHIFormat fmt, uint32_t miplevels)
        {
            RHIDeviceMemory* mem = nullptr;
            CreateImage(w, h, fmt, RHI_IMAGE_TILING_OPTIMAL, 0u, 0u, image, mem, 0u, 6u, miplevels);
            auto* gi = static_cast<WebGL2Image*>(image);
            glBindTexture(GL_TEXTURE_CUBE_MAP, gi->GetTexture());
            for (uint32_t i = 0; i < 6; ++i)
            {
                if (pixels[i] != nullptr)
                {
                    glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, 0, 0, w, h, gi->getFormat(), gi->getType(), pixels[i]);
                }
            }
            if (miplevels > 1)
            {
                glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
            }
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            CreateImageView(image, fmt, 0, RHI_IMAGE_VIEW_TYPE_CUBE, 6, miplevels ? miplevels : 1, view);
        }

        // ---------- Pool / layout / pipeline ----------

        void WebGL2RHI::CreateCommandPool()
        {
            WEBGL2_NOOP();
        }
        bool WebGL2RHI::CreateCommandPool(const RHICommandPoolCreateInfo*, RHICommandPool*& out)
        {
            out = &m_CommandPool;
            return true;
        }
        bool WebGL2RHI::CreateDescriptorPool(const RHIDescriptorPoolCreateInfo*, RHIDescriptorPool*& o)
        {
            o = new WebGL2DescriptorPool();
            return true;
        }

        bool WebGL2RHI::CreateDescriptorSetLayout(const RHIDescriptorSetLayoutCreateInfo* info, RHIDescriptorSetLayout*& out)
        {
            auto* l = new WebGL2DescriptorSetLayout();
            l->setBindings(info->pBindings, info->bindingCount);
            out = l;
            return true;
        }

        bool WebGL2RHI::CreateFence(const RHIFenceCreateInfo*, RHIFence*& out)
        {
            out = new WebGL2Fence();
            return true;
        }

        bool WebGL2RHI::CreateFramebuffer(const RHIFramebufferCreateInfo* info, RHIFramebuffer*& out)
        {
            auto* fb = new WebGL2Framebuffer();
            fb->SetSize(info->width, info->height, info->layers);
            GLuint name = 0;
            glGenFramebuffers(1, &name);
            fb->setHandle(name);
            // TODO: walk attachments, glFramebufferTexture2D / glFramebufferRenderbuffer.
            out = fb;
            return true;
        }

        bool WebGL2RHI::CreateGraphicsPipelines(RHIPipelineCache*, uint32_t, const RHIGraphicsPipelineCreateInfo*, RHIPipeline*& out)
        {
            // TODO: glCreateProgram + attach VS/FS shaders + link, populate vertex layout,
            //       blend/depth/cull from create info.
            out = new WebGL2Pipeline();
            return true;
        }

        bool WebGL2RHI::CreateComputePipelines(RHIPipelineCache*, uint32_t, const RHIComputePipelineCreateInfo*, RHIPipeline*& out)
        {
            out = nullptr;
            return false;  // unsupported on WebGL2
        }

        bool WebGL2RHI::CreatePipelineLayout(const RHIPipelineLayoutCreateInfo* info, RHIPipelineLayout*& out)
        {
            auto* l = new WebGL2PipelineLayout();
            std::vector<WebGL2DescriptorSetLayout*> ls(info->setLayoutCount);
            for (uint32_t i = 0; i < info->setLayoutCount; ++i)
            {
                ls[i] = static_cast<WebGL2DescriptorSetLayout*>(info->pSetLayouts[i]);
            }
            l->setLayouts(std::move(ls));
            out = l;
            return true;
        }

        bool WebGL2RHI::CreateRenderPass(const RHIRenderPassCreateInfo* info, RHIRenderPass*& out)
        {
            auto* p = new WebGL2RenderPass();
            p->setAttachments(info->pAttachments, info->attachmentCount);
            out = p;
            return true;
        }
        void WebGL2RHI::DestroyRenderPass(RHIRenderPass* renderPass)
        {
            // RHIRenderPass has no virtual dtor; delete through the concrete
            // type (same pattern as DX12RHI::DestroyRenderPass).
            delete static_cast<WebGL2RenderPass*>(renderPass);
        }

        bool WebGL2RHI::CreateSampler(const RHISamplerCreateInfo*, RHISampler*& out)
        {
            auto* s = new WebGL2Sampler();
            GLuint name = 0;
            glGenSamplers(1, &name);
            // TODO: translate filter / wrap / lod via glSamplerParameteri.
            s->setHandle(name);
            out = s;
            return true;
        }

        bool WebGL2RHI::CreateSemaphore(const RHISemaphoreCreateInfo*, RHISemaphore*& out)
        {
            out = new WebGL2Semaphore();
            return true;
        }

        // ---------- Command recording (record-only stubs) ----------

        bool WebGL2RHI::WaitForFencesPFN(uint32_t, RHIFence* const*, RHIBool32, uint64_t)
        {
            return true;
        }
        bool WebGL2RHI::ResetFencesPFN(uint32_t, RHIFence* const*)
        {
            return true;
        }
        bool WebGL2RHI::ResetCommandPoolPFN(RHICommandPool*, RHICommandPoolResetFlags)
        {
            return true;
        }

        bool WebGL2RHI::BeginCommandBufferPFN(RHICommandBuffer* cb, const RHICommandBufferBeginInfo*)
        {
            static_cast<WebGL2CommandBuffer*>(cb)->reset();
            m_CurrentCommandBuffer = static_cast<WebGL2CommandBuffer*>(cb);
            // Reset the CPU-side GL binding mirror so a fresh command stream
            // doesn't inherit stale vertex/index/descriptor bindings.
            std::memset(m_BoundArrayBuffers, 0, sizeof(m_BoundArrayBuffers));
            std::memset(m_BoundArrayOffsets, 0, sizeof(m_BoundArrayOffsets));
            m_BoundIndexBuffer = 0;
            m_BoundIndexOffset = 0;
            m_BoundIndexType = GL_UNSIGNED_SHORT;
            std::memset(m_BoundDescriptorSets, 0, sizeof(m_BoundDescriptorSets));
            m_BoundDescriptorSetCount = 0;
            return true;
        }
        bool WebGL2RHI::EndCommandBufferPFN(RHICommandBuffer*)
        {
            return true;
        }

        void WebGL2RHI::CmdBeginRenderPassPFN(RHICommandBuffer* commandBuffer, const RHIRenderPassBeginInfo* p, RHISubpassContents)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::BeginRenderPassPayload pl {};
            // Bind the pass's framebuffer (0 = canvas) so replay issues GL calls
            // against it. The clear values are recorded for replay too.
            if (p != nullptr && p->framebuffer != nullptr)
            {
                pl.framebuffer = static_cast<WebGL2Framebuffer*>(p->framebuffer)->getHandle();
                m_ActiveFramebuffer = static_cast<WebGL2Framebuffer*>(p->framebuffer);
            }
            else
            {
                pl.framebuffer = m_DefaultFramebuffer;  // canvas
                m_ActiveFramebuffer = nullptr;
            }
            pl.render_area_x = p ? p->renderArea.offset.x : 0;
            pl.render_area_y = p ? p->renderArea.offset.y : 0;
            pl.render_area_w = p ? p->renderArea.extent.width : 0;
            pl.render_area_h = p ? p->renderArea.extent.height : 0;
            pl.clear_value_count = p ? std::min(p->clearValueCount, WebGL2CommandBuffer::k_max_clear_attachments) : 0;
            for (uint32_t i = 0; i < pl.clear_value_count; ++i)
            {
                const RHIClearValue& cv = p->pClearValues[i];
                WebGL2CommandBuffer::RecordedClearColor& rc = pl.clear_values[i];
                rc.aspect = RHI_IMAGE_ASPECT_COLOR_BIT;
                rc.attachment = i;
                rc.color[0] = cv.color.float32[0];
                rc.color[1] = cv.color.float32[1];
                rc.color[2] = cv.color.float32[2];
                rc.color[3] = cv.color.float32[3];
                rc.depth = cv.depthStencil.depth;
                rc.stencil = cv.depthStencil.stencil;
            }
            cb->record(WebGL2CommandBuffer::CommandKind::BeginRenderPass, pl);
        }
        void WebGL2RHI::CmdNextSubpassPFN(RHICommandBuffer*, RHISubpassContents)
        {
            // Single-subpass on WebGL2; nothing to advance.
        }
        void WebGL2RHI::CmdEndRenderPassPFN(RHICommandBuffer* commandBuffer)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::EndRenderPassPayload pl {};
            cb->record(WebGL2CommandBuffer::CommandKind::EndRenderPass, pl);
            m_ActiveFramebuffer = nullptr;
        }
        void WebGL2RHI::CmdBindPipelinePFN(RHICommandBuffer* commandBuffer, RHIPipelineBindPoint, RHIPipeline* pipeline)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::BindPipelinePayload pl {};
            pl.pipeline = static_cast<WebGL2Pipeline*>(pipeline);
            cb->record(WebGL2CommandBuffer::CommandKind::BindPipeline, pl);
        }
        void WebGL2RHI::CmdSetViewportPFN(RHICommandBuffer* commandBuffer, uint32_t, uint32_t, const RHIViewport* pViewports)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::SetViewportPayload pl {};
            if (pViewports != nullptr)
            {
                pl.x = pViewports->x;
                pl.y = pViewports->y;
                pl.w = pViewports->width;
                pl.h = pViewports->height;
                pl.min_depth = pViewports->minDepth;
                pl.max_depth = pViewports->maxDepth;
            }
            cb->record(WebGL2CommandBuffer::CommandKind::SetViewport, pl);
        }
        void WebGL2RHI::CmdSetScissorPFN(RHICommandBuffer* commandBuffer, uint32_t, uint32_t, const RHIRect2D* pScissors)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::SetScissorPayload pl {};
            if (pScissors != nullptr)
            {
                pl.x = pScissors->offset.x;
                pl.y = pScissors->offset.y;
                pl.w = pScissors->extent.width;
                pl.h = pScissors->extent.height;
            }
            cb->record(WebGL2CommandBuffer::CommandKind::SetScissor, pl);
        }
        void WebGL2RHI::CmdBindVertexBuffersPFN(RHICommandBuffer* commandBuffer, uint32_t firstBinding, uint32_t bindingCount, RHIBuffer* const* pBuffers, const RHIDeviceSize* pOffsets)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::BindVertexBuffersPayload pl {};
            pl.first_binding = firstBinding;
            pl.binding_count = std::min(bindingCount, WebGL2CommandBuffer::k_max_vertex_bindings);
            for (uint32_t i = 0; i < pl.binding_count; ++i)
            {
                RHIBuffer* buf = pBuffers ? pBuffers[i] : nullptr;
                pl.bindings[i].buffer = buf ? static_cast<WebGL2Buffer*>(buf)->getHandle() : 0;
                pl.bindings[i].offset = pOffsets ? static_cast<GLintptr>(pOffsets[i]) : 0;
            }
            cb->record(WebGL2CommandBuffer::CommandKind::BindVertexBuffers, pl);
        }
        void WebGL2RHI::CmdBindIndexBufferPFN(RHICommandBuffer* commandBuffer, RHIBuffer* buffer, RHIDeviceSize offset, RHIIndexType indexType)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::BindIndexBufferPayload pl {};
            pl.buffer = buffer ? static_cast<WebGL2Buffer*>(buffer)->getHandle() : 0;
            pl.offset = static_cast<GLintptr>(offset);
            pl.index_type = ToGLIndexType(indexType);
            cb->record(WebGL2CommandBuffer::CommandKind::BindIndexBuffer, pl);
        }
        void WebGL2RHI::CmdBindDescriptorSetsPFN(RHICommandBuffer* commandBuffer, RHIPipelineBindPoint, RHIPipelineLayout*, uint32_t firstSet, uint32_t descriptorSetCount, const RHIDescriptorSet* const* pDescriptorSets, uint32_t, const uint32_t*)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::BindDescriptorSetsPayload pl {};
            pl.first_set = firstSet;
            pl.set_count = std::min(descriptorSetCount, WebGL2CommandBuffer::k_max_descriptor_sets);
            for (uint32_t i = 0; i < pl.set_count; ++i)
            {
                pl.sets[i].set = pDescriptorSets ? pDescriptorSets[i] : nullptr;
            }
            cb->record(WebGL2CommandBuffer::CommandKind::BindDescriptorSets, pl);
        }
        void WebGL2RHI::CmdDrawIndexedPFN(RHICommandBuffer* commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::DrawIndexedPayload pl {};
            pl.index_count = indexCount;
            pl.instance_count = instanceCount;
            pl.first_index = firstIndex;
            pl.vertex_offset = vertexOffset;
            pl.first_instance = firstInstance;
            cb->record(WebGL2CommandBuffer::CommandKind::DrawIndexed, pl);
        }
        void WebGL2RHI::CmdClearAttachmentsPFN(RHICommandBuffer* commandBuffer, uint32_t attachmentCount, const RHIClearAttachment* pAttachments, uint32_t, const RHIClearRect*)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::ClearAttachmentsPayload pl {};
            pl.count = std::min(attachmentCount, WebGL2CommandBuffer::k_max_clear_attachments);
            for (uint32_t i = 0; i < pl.count; ++i)
            {
                pl.attachments[i].aspect = pAttachments[i].aspectMask;
                pl.attachments[i].attachment = pAttachments[i].colorAttachment;
                pl.attachments[i].color[0] = pAttachments[i].clearValue.color.float32[0];
                pl.attachments[i].color[1] = pAttachments[i].clearValue.color.float32[1];
                pl.attachments[i].color[2] = pAttachments[i].clearValue.color.float32[2];
                pl.attachments[i].color[3] = pAttachments[i].clearValue.color.float32[3];
                pl.attachments[i].depth = pAttachments[i].clearValue.depthStencil.depth;
                pl.attachments[i].stencil = pAttachments[i].clearValue.depthStencil.stencil;
            }
            cb->record(WebGL2CommandBuffer::CommandKind::ClearAttachments, pl);
        }

        bool WebGL2RHI::BeginCommandBuffer(RHICommandBuffer* cb, const RHICommandBufferBeginInfo* info)
        {
            return BeginCommandBufferPFN(cb, info);
        }

        void WebGL2RHI::CmdCopyImageToBuffer(RHICommandBuffer* commandBuffer, RHIImage* srcImage, RHIImageLayout, RHIBuffer* dstBuffer, uint32_t regionCount, const RHIBufferImageCopy* pRegions)
        {
            // WebGL2 reads pixels back into a PBO via glReadPixels; the actual
            // read happens at replay (needs an FBO bound to the source image).
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            auto* gi = static_cast<WebGL2Image*>(srcImage);
            WebGL2CommandBuffer::CopyImageToBufferPayload pl {};
            pl.src_texture = gi->GetTexture();
            pl.src_target = gi->getTarget();
            pl.dst_buffer = dstBuffer ? static_cast<WebGL2Buffer*>(dstBuffer)->getHandle() : 0;
            if (pRegions != nullptr && regionCount > 0)
            {
                pl.dst_offset = static_cast<GLintptr>(pRegions[0].bufferOffset);
                pl.level = pRegions[0].imageSubresource.mipLevel;
                pl.x = pRegions[0].imageOffset.x;
                pl.y = pRegions[0].imageOffset.y;
                pl.w = pRegions[0].imageExtent.width;
                pl.h = pRegions[0].imageExtent.height;
            }
            pl.fmt = gi->getFormat();
            pl.type = gi->getType();
            cb->record(WebGL2CommandBuffer::CommandKind::CopyImageToBuffer, pl);
        }
        void WebGL2RHI::CmdCopyImageToImage(RHICommandBuffer* commandBuffer, RHIImage* srcImage, RHIImageAspectFlagBits, RHIImage* dstImage, RHIImageAspectFlagBits, uint32_t width, uint32_t height)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            auto* src = static_cast<WebGL2Image*>(srcImage);
            auto* dst = static_cast<WebGL2Image*>(dstImage);
            WebGL2CommandBuffer::CopyImageToImagePayload pl {};
            pl.src_texture = src->GetTexture();
            pl.src_target = src->getTarget();
            pl.dst_texture = dst->GetTexture();
            pl.dst_target = dst->getTarget();
            pl.w = width;
            pl.h = height;
            cb->record(WebGL2CommandBuffer::CommandKind::CopyImageToImage, pl);
        }
        void WebGL2RHI::CmdBlitImage(RHICommandBuffer* commandBuffer, RHIImage* srcImage, RHIImageLayout, RHIImage* dstImage, RHIImageLayout, uint32_t srcX0, uint32_t srcY0, uint32_t srcX1, uint32_t srcY1, uint32_t dstX0, uint32_t dstY0, uint32_t dstX1, uint32_t dstY1, RHIFilter filter)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            auto* src = static_cast<WebGL2Image*>(srcImage);
            auto* dst = static_cast<WebGL2Image*>(dstImage);
            WebGL2CommandBuffer::BlitImagePayload pl {};
            pl.src_texture = src->GetTexture();
            pl.src_target = src->getTarget();
            pl.dst_texture = dst->GetTexture();
            pl.dst_target = dst->getTarget();
            pl.src_x0 = static_cast<int32_t>(srcX0);
            pl.src_y0 = static_cast<int32_t>(srcY0);
            pl.src_x1 = static_cast<int32_t>(srcX1);
            pl.src_y1 = static_cast<int32_t>(srcY1);
            pl.dst_x0 = static_cast<int32_t>(dstX0);
            pl.dst_y0 = static_cast<int32_t>(dstY0);
            pl.dst_x1 = static_cast<int32_t>(dstX1);
            pl.dst_y1 = static_cast<int32_t>(dstY1);
            pl.filter = ToGLFilter(filter);
            cb->record(WebGL2CommandBuffer::CommandKind::BlitImage, pl);
        }
        void WebGL2RHI::CmdCopyBuffer(RHICommandBuffer* commandBuffer, RHIBuffer* srcBuffer, RHIBuffer* dstBuffer, uint32_t regionCount, RHIBufferCopy* pRegions)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::CopyBufferPayload pl {};
            pl.region_count = std::min(regionCount, WebGL2CommandBuffer::k_max_buffer_copy_regions);
            for (uint32_t i = 0; i < pl.region_count; ++i)
            {
                pl.regions[i].src = srcBuffer ? static_cast<WebGL2Buffer*>(srcBuffer)->getHandle() : 0;
                pl.regions[i].dst = dstBuffer ? static_cast<WebGL2Buffer*>(dstBuffer)->getHandle() : 0;
                pl.regions[i].src_offset = static_cast<GLintptr>(pRegions[i].srcOffset);
                pl.regions[i].dst_offset = static_cast<GLintptr>(pRegions[i].dstOffset);
                pl.regions[i].size = static_cast<GLsizeiptr>(pRegions[i].size);
            }
            cb->record(WebGL2CommandBuffer::CommandKind::CopyBuffer, pl);
        }
        void WebGL2RHI::CmdDraw(RHICommandBuffer* commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::DrawPayload pl {};
            pl.vertex_count = vertexCount;
            pl.instance_count = instanceCount;
            pl.first_vertex = firstVertex;
            pl.first_instance = firstInstance;
            cb->record(WebGL2CommandBuffer::CommandKind::Draw, pl);
        }
        void WebGL2RHI::CmdDispatch(RHICommandBuffer*, uint32_t, uint32_t, uint32_t)
        { /* unsupported */
        }
        void WebGL2RHI::CmdDispatchIndirect(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize)
        { /* unsupported */
        }
        // WebGL2 has no indirect draw of any kind (there is no
        // glMultiDrawElementsIndirect equivalent), and Lumen/Nanite are not
        // compiled for Emscripten anyway. Silent no-ops, matching
        // CmdDispatchIndirect: an unsupported draw does nothing rather than fail.
        void WebGL2RHI::CmdDrawIndexedIndirect(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize, uint32_t, uint32_t)
        { /* unsupported */
        }
        void WebGL2RHI::CmdDrawIndirect(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize, uint32_t, uint32_t)
        { /* unsupported */
        }
        void WebGL2RHI::CmdPipelineBarrier(RHICommandBuffer*, RHIPipelineStageFlags, RHIPipelineStageFlags, RHIDependencyFlags, uint32_t, const RHIMemoryBarrier*, uint32_t, const RHIBufferMemoryBarrier*, uint32_t, const RHIImageMemoryBarrier*)
        { /* implicit in GL */
        }
        bool WebGL2RHI::EndCommandBuffer(RHICommandBuffer*)
        {
            return true;
        }

        void WebGL2RHI::UpdateDescriptorSets(uint32_t descriptorWriteCount, const RHIWriteDescriptorSet* pDescriptorWrites, uint32_t, const RHICopyDescriptorSet*)
        {
            // Populate the CPU-side descriptor-set bag. Each write targets one
            // binding of one set; the binding's GL point is derived from the
            // set's layout at replay time (BindDescriptorSets). A null resource
            // leaves the binding untouched (matches Vulkan/DX12 skip semantics).
            for (uint32_t i = 0; i < descriptorWriteCount; ++i)
            {
                const RHIWriteDescriptorSet& w = pDescriptorWrites[i];
                if (w.dstSet == nullptr)
                {
                    continue;
                }
                auto* set = static_cast<WebGL2DescriptorSet*>(w.dstSet);
                const uint32_t binding = w.dstBinding;
                if (w.descriptorType == RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                    w.descriptorType == RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
                {
                    if (w.pBufferInfo == nullptr || w.pBufferInfo->buffer == nullptr)
                    {
                        continue;
                    }
                    WebGL2DescriptorSet::UniformBufferBinding ub;
                    ub.buffer = static_cast<WebGL2Buffer*>(w.pBufferInfo->buffer);
                    ub.offset = static_cast<GLintptr>(w.pBufferInfo->offset);
                    ub.range = static_cast<GLsizeiptr>(w.pBufferInfo->range);
                    set->uboBindings()[binding] = ub;
                }
                else if (w.descriptorType == RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                         w.descriptorType == RHI_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                         w.descriptorType == RHI_DESCRIPTOR_TYPE_SAMPLER)
                {
                    if (w.pImageInfo == nullptr || w.pImageInfo->imageView == nullptr)
                    {
                        continue;
                    }
                    WebGL2DescriptorSet::SampledImageBinding sib;
                    sib.view = static_cast<WebGL2ImageView*>(w.pImageInfo->imageView);
                    sib.sampler = w.pImageInfo->sampler
                        ? static_cast<WebGL2Sampler*>(w.pImageInfo->sampler) : nullptr;
                    set->imageBindings()[binding] = sib;
                }
            }
        }

        bool WebGL2RHI::QueueSubmit(RHIQueue*, uint32_t submitCount, const RHISubmitInfo* pSubmits, RHIFence*)
        {
            for (uint32_t i = 0; i < submitCount; ++i)
            {
                for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; ++j)
                {
                    ExecuteCommandList(static_cast<WebGL2CommandBuffer*>(pSubmits[i].pCommandBuffers[j]));
                }
            }
            return true;
        }

        bool WebGL2RHI::QueueWaitIdle(RHIQueue*)
        {
            glFinish();
            return true;
        }
        void WebGL2RHI::ResetCommandPool()
        {
            for (auto& cb : m_CommandBuffers)
            {
                cb.reset();
            }
        }
        void WebGL2RHI::WaitForFences()
        {
            glFinish();
        }

        // ---------- Query ----------

        void WebGL2RHI::GetPhysicalDeviceProperties(RHIPhysicalDeviceProperties* props)
        {
            if (props)
                std::memset(props, 0, sizeof(*props));
        }

        RHICommandBuffer* WebGL2RHI::GetCurrentCommandBuffer() const
        {
            return m_CurrentCommandBuffer;
        }
        RHICommandBuffer* const* WebGL2RHI::GetCommandBufferList() const
        {
            return m_CommandBufferPtrs;
        }
        RHICommandPool* WebGL2RHI::GetCommandPoor() const
        {
            return const_cast<WebGL2CommandPool*>(&m_CommandPool);
        }
        RHIDescriptorPool* WebGL2RHI::GetDescriptorPoor() const
        {
            return nullptr;
        }
        RHIFence* const* WebGL2RHI::GetFenceList() const
        {
            return m_FrameFencePtrs;
        }
        QueueFamilyIndices WebGL2RHI::GetQueueFamilyIndices() const
        {
            return m_QueueIndices;
        }
        RHIQueue* WebGL2RHI::GetGraphicsQueue() const
        {
            return const_cast<WebGL2Queue*>(&m_GraphicsQueue);
        }
        RHIQueue* WebGL2RHI::GetComputeQueue() const
        {
            return const_cast<WebGL2Queue*>(&m_GraphicsQueue);
        }

        RHISwapChainDesc WebGL2RHI::GetSwapchainInfo()
        {
            RHISwapChainDesc d {};
            d.image_format = m_SwapchainImageFormat;
            d.extent = m_SwapchainExtent;
            d.imageViews = m_SwapchainImageviews;
            return d;
        }
        RHIDepthImageDesc WebGL2RHI::GetDepthImageInfo() const
        {
            RHIDepthImageDesc d {};
            d.depth_image_format = m_DepthImageFormat;
            d.depth_image = m_DepthImage;
            d.depth_image_view = m_DepthImageView;
            return d;
        }

        uint8_t WebGL2RHI::GetMaxFramesInFlight() const
        {
            return k_max_frames_in_flight;
        }
        uint8_t WebGL2RHI::GetCurrentFrameIndex() const
        {
            return m_CurrentFrameIndex;
        }
        void WebGL2RHI::SetCurrentFrameIndex(uint8_t i)
        {
            m_CurrentFrameIndex = i;
        }

        // ---------- Frame ----------

        RHICommandBuffer* WebGL2RHI::BeginSingleTimeCommands()
        {
            return m_CommandBufferPtrs[m_CurrentFrameIndex];
        }
        void WebGL2RHI::EndSingleTimeCommands(RHICommandBuffer*)
        {
            glFlush();
        }
        bool WebGL2RHI::PrepareBeforePass(std::function<void()>)
        {
            return true;
        }
        void WebGL2RHI::SubmitRendering(std::function<void()>)
        {
            glFlush();
            m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % k_max_frames_in_flight;
            m_CurrentCommandBuffer = &m_CommandBuffers[m_CurrentFrameIndex];
        }
        void WebGL2RHI::PushEvent(RHICommandBuffer* commandBuffer, const char* name, const float*)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::PushDebugGroupPayload pl {};
            if (name != nullptr)
            {
                std::strncpy(pl.name, name, sizeof(pl.name) - 1);
            }
            cb->record(WebGL2CommandBuffer::CommandKind::PushDebugGroup, pl);
        }
        void WebGL2RHI::PopEvent(RHICommandBuffer* commandBuffer)
        {
            auto* cb = static_cast<WebGL2CommandBuffer*>(commandBuffer);
            WebGL2CommandBuffer::PopDebugGroupPayload pl {};
            cb->record(WebGL2CommandBuffer::CommandKind::PopDebugGroup, pl);
        }

        // ---------- Destroy ----------

        void WebGL2RHI::clear()
        {
            WEBGL2_TODO("free everything");
        }
        void WebGL2RHI::ClearSwapchain()
        {
            WEBGL2_NOOP();
        }
        void WebGL2RHI::DestroyDefaultSampler(RHIDefaultSamplerType)
        {
            WEBGL2_NOOP();
        }
        void WebGL2RHI::DestroyMipmappedSampler()
        {
            WEBGL2_NOOP();
        }
        void WebGL2RHI::DestroyShaderModule(RHIShader* s)
        {
            delete s;
        }
        void WebGL2RHI::DestroySemaphore(RHISemaphore* s)
        {
            delete s;
        }
        void WebGL2RHI::DestroySampler(RHISampler* s)
        {
            GLuint n = static_cast<WebGL2Sampler*>(s)->getHandle();
            if (n != 0)
            {
                glDeleteSamplers(1, &n);
            }
            delete s;
        }
        void WebGL2RHI::DestroyInstance(RHIInstance*)
        {
            WEBGL2_NOOP();
        }
        void WebGL2RHI::DestroyImageView(RHIImageView* v)
        {
            delete v;
        }
        void WebGL2RHI::DestroyImage(RHIImage* image)
        {
            auto* g = static_cast<WebGL2Image*>(image);
            GLuint t = g->GetTexture();
            if (t != 0)
            {
                glDeleteTextures(1, &t);
            }
            GLuint r = g->getRenderbuffer();
            if (r != 0)
            {
                glDeleteRenderbuffers(1, &r);
            }
            delete image;
        }
        void WebGL2RHI::DestroyFramebuffer(RHIFramebuffer* fb)
        {
            GLuint n = static_cast<WebGL2Framebuffer*>(fb)->getHandle();
            if (n != 0)
            {
                glDeleteFramebuffers(1, &n);
            }
            delete fb;
        }
        void WebGL2RHI::DestroyFence(RHIFence* f)
        {
            auto* g = static_cast<WebGL2Fence*>(f);
            if (g->getSync() != nullptr)
            {
                glDeleteSync(g->getSync());
            }
            delete f;
        }
        void WebGL2RHI::DestroyDevice()
        {
            WEBGL2_NOOP();
        }
        void WebGL2RHI::DestroyCommandPool(RHICommandPool*)
        {
            WEBGL2_NOOP();
        }
        void WebGL2RHI::DestroyBuffer(RHIBuffer*& buffer)
        {
            auto* g = static_cast<WebGL2Buffer*>(buffer);
            GLuint n = g->getHandle();
            if (n != 0)
            {
                glDeleteBuffers(1, &n);
            }
            delete g;
            buffer = nullptr;
        }
        void WebGL2RHI::FreeCommandBuffers(RHICommandPool*, uint32_t, RHICommandBuffer*)
        {
            WEBGL2_NOOP();
        }

        // ---------- Memory ----------

        void WebGL2RHI::FreeMemory(RHIDeviceMemory*& memory)
        {
            delete memory;
            memory = nullptr;
        }

        bool WebGL2RHI::MapMemory(RHIDeviceMemory* memory, RHIDeviceSize offset, RHIDeviceSize size, RHIMemoryMapFlags, void** ppData)
        {
            auto* m = static_cast<WebGL2DeviceMemory*>(memory);
            if (m == nullptr || m->getOwner() == nullptr)
            {
                return false;
            }
            const RHIDeviceSize map_size = (size == 0) ? m->getOwner()->getSize() : size;
            m->staging().resize(static_cast<size_t>(map_size));
            m->mapped() = true;
            m->mappedOffset() = offset;
            m->mappedSize() = map_size;
            *ppData = m->staging().data();
            return true;
        }

        void WebGL2RHI::UnmapMemory(RHIDeviceMemory* memory)
        {
            auto* m = static_cast<WebGL2DeviceMemory*>(memory);
            if (m == nullptr || m->getOwner() == nullptr || !m->mapped())
            {
                return;
            }
            auto* buf = m->getOwner();
            glBindBuffer(GL_ARRAY_BUFFER, buf->getHandle());
            glBufferSubData(GL_ARRAY_BUFFER,
                            static_cast<GLintptr>(m->mappedOffset()),
                            static_cast<GLsizeiptr>(m->mappedSize()),
                            m->staging().data());
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            m->mapped() = false;
        }

        void WebGL2RHI::InvalidateMappedMemoryRanges(void*, RHIDeviceMemory*, RHIDeviceSize, RHIDeviceSize)
        {
            WEBGL2_NOOP();
        }
        void WebGL2RHI::FlushMappedMemoryRanges(void*, RHIDeviceMemory*, RHIDeviceSize, RHIDeviceSize)
        {
            WEBGL2_NOOP();
        }

        // ---------- Semaphore ----------

        RHISemaphore*& WebGL2RHI::GetTextureCopySemaphore(uint32_t)
        {
            static RHISemaphore* dummy {nullptr};
            return dummy;
        }

        // ---------- Multi-viewport ----------

        void WebGL2RHI::RegisterViewport(const int, const RHIViewport&)
        {
            WEBGL2_NOOP();
        }
        void WebGL2RHI::UpdateViewport(ViewportType, const RHIViewport&)
        {
            WEBGL2_NOOP();
        }
        RHIViewport* WebGL2RHI::GetViewport(ViewportType)
        {
            return nullptr;
        }

        void WebGL2RHI::CreateViewportRenderTexture(const std::string& id, uint32_t w, uint32_t h)
        {
            ViewportRenderTexture rt {};
            rt.width = w;
            rt.height = h;
            m_ViewportRenderTextures.emplace(id, rt);
        }
        void WebGL2RHI::UpdateViewportRenderTexture(const std::string& id, uint32_t w, uint32_t h)
        {
            auto it = m_ViewportRenderTextures.find(id);
            if (it == m_ViewportRenderTextures.end())
            {
                CreateViewportRenderTexture(id, w, h);
                return;
            }
            it->second.width = w;
            it->second.height = h;
        }
        void WebGL2RHI::DestroyViewportRenderTexture(const std::string& id)
        {
            m_ViewportRenderTextures.erase(id);
        }
        RHI::ViewportRenderTexture* WebGL2RHI::GetViewportRenderTexture(const std::string& id)
        {
            auto it = m_ViewportRenderTextures.find(id);
            return (it == m_ViewportRenderTextures.end()) ? nullptr : &it->second;
        }

        // ---------- Internal ----------

        bool WebGL2RHI::CreateGLContext(WindowSystem* /*window_system*/)
        {
            // The engine no longer links GLFW (native Win32 / Cocoa window layer,
            // doc/platform_windowing_plan.md), so nothing creates a context for us
            // on Web: WindowSystem has no application there. Adopt a context the
            // host page already made current (an embedding mini-game shell may do
            // that); otherwise create a WebGL2 context on #canvas ourselves.
            EMSCRIPTEN_WEBGL_CONTEXT_HANDLE handle = emscripten_webgl_get_current_context();
            if (handle <= 0)
            {
                EmscriptenWebGLContextAttributes attributes;
                emscripten_webgl_init_context_attributes(&attributes);
                attributes.majorVersion = 2;
                attributes.minorVersion = 0;
                attributes.alpha = EM_FALSE;
                attributes.depth = EM_TRUE;
                attributes.stencil = EM_TRUE;
                attributes.antialias = EM_FALSE;
                handle = emscripten_webgl_create_context("#canvas", &attributes);
                if (handle <= 0 || emscripten_webgl_make_context_current(handle) != EMSCRIPTEN_RESULT_SUCCESS)
                {
                    if (handle > 0)
                    {
                        emscripten_webgl_destroy_context(handle);
                    }
                    m_GlContextHandle = nullptr;
                    return false;
                }
            }
            m_GlContextHandle = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));

            // Seed the drawable size from the canvas. CreateSwapchain() will refresh.
            int w = 0;
            int h = 0;
            if (emscripten_get_canvas_element_size("#canvas", &w, &h) == EMSCRIPTEN_RESULT_SUCCESS)
            {
                m_DrawableWidth = w;
                m_DrawableHeight = h;
            }
            return true;
        }

        void WebGL2RHI::DestroyGLContext()
        {
            if (m_GlContextHandle != nullptr)
            {
                EMSCRIPTEN_WEBGL_CONTEXT_HANDLE h =
                    static_cast<EMSCRIPTEN_WEBGL_CONTEXT_HANDLE>(reinterpret_cast<uintptr_t>(m_GlContextHandle));
                emscripten_webgl_destroy_context(h);
                m_GlContextHandle = nullptr;
            }
        }
        void WebGL2RHI::ExecuteCommandList(WebGL2CommandBuffer* cb)
        {
            if (cb == nullptr)
            {
                return;
            }
            const std::vector<WebGL2CommandBuffer::Command>& cmds = cb->commands();
            const uint8_t* arena = cmds.empty() ? nullptr : cb->arena().data();
            for (const auto& cmd : cmds)
            {
                ReplayCommand(cb, cmd);
            }
        }

        void WebGL2RHI::ReplayCommand(WebGL2CommandBuffer* cb, const WebGL2CommandBuffer::Command& cmd)
        {
            // The arena base is cb->arena().data(); each payload is a fixed POD
            // copied verbatim by record(), so the reinterpret cast is safe.
            // arena().data() is a prvalue pointer, so we must dereference it to
            // obtain a reference (reinterpret_cast to a reference type requires an
            // lvalue).
            #define PL(T) (*reinterpret_cast<const WebGL2CommandBuffer::T*>(cb->arena().data() + cmd.payload_offset))

            switch (cmd.kind)
            {
                case WebGL2CommandBuffer::CommandKind::BeginRenderPass:
                {
                    const auto& p = PL(BeginRenderPassPayload);
                    glBindFramebuffer(GL_FRAMEBUFFER, p.framebuffer);
                    glViewport(p.render_area_x, p.render_area_y,
                               static_cast<GLsizei>(p.render_area_w),
                               static_cast<GLsizei>(p.render_area_h));
                    // Clear the attachments the pass asked for.
                    GLbitfield mask = 0;
                    for (uint32_t i = 0; i < p.clear_value_count; ++i)
                    {
                        const auto& cv = p.clear_values[i];
                        if (cv.aspect & RHI_IMAGE_ASPECT_COLOR_BIT)
                        {
                            glClearColor(cv.color[0], cv.color[1], cv.color[2], cv.color[3]);
                            mask |= GL_COLOR_BUFFER_BIT;
                        }
                        if (cv.aspect & RHI_IMAGE_ASPECT_DEPTH_BIT)
                        {
                            glClearDepthf(cv.depth);
                            mask |= GL_DEPTH_BUFFER_BIT;
                        }
                        if (cv.aspect & RHI_IMAGE_ASPECT_STENCIL_BIT)
                        {
                            glClearStencil(cv.stencil);
                            mask |= GL_STENCIL_BUFFER_BIT;
                        }
                    }
                    if (mask != 0)
                    {
                        glClear(mask);
                    }
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::EndRenderPass:
                {
                    // Detach from the pass's framebuffer so subsequent draws
                    // don't render into a destroyed FBO.
                    glBindFramebuffer(GL_FRAMEBUFFER, m_DefaultFramebuffer);
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::BindPipeline:
                {
                    const auto& p = PL(BindPipelinePayload);
                    m_CurrentPipeline = p.pipeline;
                    if (p.pipeline != nullptr)
                    {
                        glUseProgram(p.pipeline->getProgram());
                    }
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::SetViewport:
                {
                    const auto& p = PL(SetViewportPayload);
                    glViewport(static_cast<GLint>(p.x), static_cast<GLint>(p.y),
                               static_cast<GLsizei>(p.w), static_cast<GLsizei>(p.h));
                    glDepthRangef(p.min_depth, p.max_depth);
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::SetScissor:
                {
                    const auto& p = PL(SetScissorPayload);
                    glScissor(static_cast<GLint>(p.x), static_cast<GLint>(p.y),
                              static_cast<GLsizei>(p.w), static_cast<GLsizei>(p.h));
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::BindVertexBuffers:
                {
                    const auto& p = PL(BindVertexBuffersPayload);
                    for (uint32_t i = 0; i < p.binding_count; ++i)
                    {
                        const GLuint buf = p.bindings[i].buffer;
                        const GLintptr off = p.bindings[i].offset;
                        if (buf == 0)
                        {
                            continue;
                        }
                        const uint32_t binding = p.first_binding + i;
                        // Mirror the binding so a later Draw can set attributes.
                        if (binding < WebGL2CommandBuffer::k_max_vertex_bindings)
                        {
                            m_BoundArrayBuffers[binding] = buf;
                            m_BoundArrayOffsets[binding] = off;
                        }
                        glBindBuffer(GL_ARRAY_BUFFER, buf);
                    }
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::BindIndexBuffer:
                {
                    const auto& p = PL(BindIndexBufferPayload);
                    m_BoundIndexBuffer = p.buffer;
                    m_BoundIndexOffset = p.offset;
                    m_BoundIndexType = p.index_type;
                    if (p.buffer != 0)
                    {
                        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p.buffer);
                    }
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::BindDescriptorSets:
                {
                    const auto& p = PL(BindDescriptorSetsPayload);
                    for (uint32_t i = 0; i < p.set_count; ++i)
                    {
                        const uint32_t set_idx = p.first_set + i;
                        if (set_idx < WebGL2CommandBuffer::k_max_descriptor_sets)
                        {
                            m_BoundDescriptorSets[set_idx] = p.sets[i].set;
                        }
                    }
                    m_BoundDescriptorSetCount = p.first_set + p.set_count;
                    // Re-apply against the currently bound program's layout.
                    if (m_CurrentPipeline != nullptr)
                    {
                        // The recorded sets are stored as RecordedDescriptorSet;
                        // extract the base RHIDescriptorSet* pointers for the bind.
                        const RHIDescriptorSet* ptrs[WebGL2CommandBuffer::k_max_descriptor_sets] {};
                        for (uint32_t i = 0; i < p.set_count; ++i)
                        {
                            ptrs[i] = p.sets[i].set;
                        }
                        BindDescriptorSets(m_CurrentPipeline->getLayout(),
                                           p.first_set, p.set_count, ptrs);
                    }
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::Draw:
                {
                    const auto& p = PL(DrawPayload);
                    FlushPipelineState(m_CurrentPipeline);
                    glDrawArrays(m_CurrentPipeline ? m_CurrentPipeline->getPrimitiveTopology() : GL_TRIANGLES,
                                 static_cast<GLint>(p.first_vertex),
                                 static_cast<GLsizei>(p.vertex_count));
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::DrawIndexed:
                {
                    const auto& p = PL(DrawIndexedPayload);
                    FlushPipelineState(m_CurrentPipeline);
                    if (m_BoundIndexBuffer != 0)
                    {
                        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_BoundIndexBuffer);
                    }
                    const GLsizei index_stride = (m_BoundIndexType == GL_UNSIGNED_INT) ? 4 : 2;
                    glDrawElements(m_CurrentPipeline ? m_CurrentPipeline->getPrimitiveTopology() : GL_TRIANGLES,
                                   static_cast<GLsizei>(p.index_count),
                                   m_BoundIndexType,
                                   reinterpret_cast<void*>(static_cast<uintptr_t>(p.first_index * index_stride)));
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::ClearAttachments:
                {
                    const auto& p = PL(ClearAttachmentsPayload);
                    for (uint32_t i = 0; i < p.count; ++i)
                    {
                        const auto& a = p.attachments[i];
                        if (a.aspect & RHI_IMAGE_ASPECT_COLOR_BIT)
                        {
                            glClearColor(a.color[0], a.color[1], a.color[2], a.color[3]);
                            glClear(GL_COLOR_BUFFER_BIT);
                        }
                        if (a.aspect & RHI_IMAGE_ASPECT_DEPTH_BIT)
                        {
                            glClearDepthf(a.depth);
                            glClear(GL_DEPTH_BUFFER_BIT);
                        }
                        if (a.aspect & RHI_IMAGE_ASPECT_STENCIL_BIT)
                        {
                            glClearStencil(a.stencil);
                            glClear(GL_STENCIL_BUFFER_BIT);
                        }
                    }
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::CopyBuffer:
                {
                    const auto& p = PL(CopyBufferPayload);
                    for (uint32_t i = 0; i < p.region_count; ++i)
                    {
                        const auto& r = p.regions[i];
                        glBindBuffer(GL_COPY_READ_BUFFER, r.src);
                        glBindBuffer(GL_COPY_WRITE_BUFFER, r.dst);
                        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                                            r.src_offset, r.dst_offset, r.size);
                    }
                    glBindBuffer(GL_COPY_READ_BUFFER, 0);
                    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::CopyImageToImage:
                {
                    const auto& p = PL(CopyImageToImagePayload);
                    // Bind src to texture unit 0, dst to FBO color attachment.
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(p.src_target, p.src_texture);
                    GLint prev_fbo = 0;
                    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
                    glBindFramebuffer(GL_FRAMEBUFFER, m_ActiveFramebuffer ? m_ActiveFramebuffer->getHandle() : m_DefaultFramebuffer);
                    // Default draw/read buffer is GL_COLOR_ATTACHMENT0 on a
                    // framebuffer with a single color attachment.
                    glReadBuffer(GL_COLOR_ATTACHMENT0);
                    glBlitFramebuffer(0, 0, static_cast<GLsizei>(p.w), static_cast<GLsizei>(p.h),
                                      0, 0, static_cast<GLsizei>(p.w), static_cast<GLsizei>(p.h),
                                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
                    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
                    glBindTexture(p.src_target, 0);
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::BlitImage:
                {
                    const auto& p = PL(BlitImagePayload);
                    GLint prev_fbo = 0;
                    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
                    // Src -> read FBO, dst -> draw FBO (both default-attached for now).
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ActiveFramebuffer ? m_ActiveFramebuffer->getHandle() : m_DefaultFramebuffer);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ActiveFramebuffer ? m_ActiveFramebuffer->getHandle() : m_DefaultFramebuffer);
                    glBlitFramebuffer(p.src_x0, p.src_y0, p.src_x1, p.src_y1,
                                      p.dst_x0, p.dst_y0, p.dst_x1, p.dst_y1,
                                      GL_COLOR_BUFFER_BIT, p.filter);
                    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::CopyImageToBuffer:
                {
                    // Readback needs the source image bound to an FBO; record the
                    // PBO + offset for a later glReadPixels path. For now flush
                    // the pixels into the PBO from the default framebuffer.
                    const auto& p = PL(CopyImageToBufferPayload);
                    GLint prev_fbo = 0;
                    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
                    glBindFramebuffer(GL_FRAMEBUFFER, m_ActiveFramebuffer ? m_ActiveFramebuffer->getHandle() : m_DefaultFramebuffer);
                    glBindBuffer(GL_PIXEL_PACK_BUFFER, p.dst_buffer);
                    glReadPixels(p.x, p.y, static_cast<GLsizei>(p.w), static_cast<GLsizei>(p.h),
                                 p.fmt, p.type, reinterpret_cast<void*>(static_cast<uintptr_t>(p.dst_offset)));
                    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::PushDebugGroup:
                {
                    // GL ES 3.0 has no scoped debug marker (that's a GL 4.3+
                    // / ARB_gl_spirv feature). The name is kept for tooling.
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::PopDebugGroup:
                {
                    break;
                }
                case WebGL2CommandBuffer::CommandKind::Dispatch:
                case WebGL2CommandBuffer::CommandKind::PipelineBarrier:
                {
                    // Implicit in GL / unsupported on WebGL2.
                    break;
                }
            }
            #undef PL
        }

        void WebGL2RHI::FlushPipelineState(WebGL2Pipeline* pipeline)
        {
            if (pipeline == nullptr)
            {
                return;
            }
            // Re-apply the vertex attribute pointers the pipeline layout expects
            // from the most recently bound vertex buffers, then enable the
            // fixed-function state baked into the pipeline.
            for (const auto& attrib : pipeline->vertexAttributes())
            {
                if (attrib.binding >= WebGL2CommandBuffer::k_max_vertex_bindings)
                {
                    continue;
                }
                GLuint buf = m_BoundArrayBuffers[attrib.binding];
                if (buf == 0)
                {
                    continue;
                }
                glBindBuffer(GL_ARRAY_BUFFER, buf);
                glVertexAttribPointer(attrib.location, attrib.components, attrib.type,
                                        attrib.normalized, static_cast<GLsizei>(attrib.offset),
                                        reinterpret_cast<void*>(static_cast<uintptr_t>(m_BoundArrayOffsets[attrib.binding])));
                glEnableVertexAttribArray(attrib.location);
            }

            // Depth test
            if (pipeline->depth_test_enable)
            {
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(pipeline->depth_func);
                glDepthMask(pipeline->depth_write_enable ? GL_TRUE : GL_FALSE);
            }
            else
            {
                glDisable(GL_DEPTH_TEST);
            }
            // Cull
            if (pipeline->cull_enable)
            {
                glEnable(GL_CULL_FACE);
                glCullFace(pipeline->cull_face);
                glFrontFace(pipeline->front_face);
            }
            else
            {
                glDisable(GL_CULL_FACE);
            }
            // Blend
            if (pipeline->blend_enable)
            {
                glEnable(GL_BLEND);
                glBlendFuncSeparate(pipeline->blend_src_rgb, pipeline->blend_dst_rgb,
                                    pipeline->blend_src_a, pipeline->blend_dst_a);
                glBlendEquationSeparate(pipeline->blend_op_rgb, pipeline->blend_op_a);
            }
            else
            {
                glDisable(GL_BLEND);
            }
        }

        void WebGL2RHI::BindDescriptorSets(WebGL2PipelineLayout* layout,
                                           uint32_t first_set,
                                           uint32_t count,
                                           const RHIDescriptorSet* const* sets)
        {
            if (layout == nullptr || sets == nullptr)
            {
                return;
            }
            const std::vector<WebGL2DescriptorSetLayout*>& layouts = layout->getLayouts();
            for (uint32_t s = 0; s < count; ++s)
            {
                const RHIDescriptorSet* rset = sets[s];
                if (rset == nullptr)
                {
                    continue;
                }
                auto* set = const_cast<WebGL2DescriptorSet*>(
                    static_cast<const WebGL2DescriptorSet*>(rset));
                // Map this set's layout bindings to GL binding points. The layout
                // index (s + first_set) selects the GL binding base; each binding
                // within it uses its declared binding number as the GL point.
                const uint32_t set_binding_base = first_set + s;
                for (const auto& lb : set->getLayout() ? set->getLayout()->getBindings() : std::vector<RHIDescriptorSetLayoutBinding>())
                {
                    const uint32_t gl_point = set_binding_base * 16 + lb.binding;
                    if (lb.descriptorType == RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                        lb.descriptorType == RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
                    {
                        auto it = set->uboBindings().find(lb.binding);
                        if (it == set->uboBindings().end())
                        {
                            continue;
                        }
                        const WebGL2DescriptorSet::UniformBufferBinding& ub = it->second;
                        glBindBufferRange(GL_UNIFORM_BUFFER, gl_point, ub.buffer->getHandle(),
                                          ub.offset, ub.range);
                    }
                    else if (lb.descriptorType == RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                             lb.descriptorType == RHI_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                    {
                        auto it = set->imageBindings().find(lb.binding);
                        if (it == set->imageBindings().end())
                        {
                            continue;
                        }
                        const WebGL2DescriptorSet::SampledImageBinding& sib = it->second;
                        const GLuint tex = sib.view ? sib.view->getImage()->GetTexture() : 0;
                        const GLenum target = sib.view ? sib.view->getImage()->getTarget() : GL_TEXTURE_2D;
                        const uint32_t unit = gl_point & 15u;  // GL active texture units
                        glActiveTexture(GL_TEXTURE0 + unit);
                        glBindTexture(target, tex);
                        if (sib.sampler != nullptr && sib.sampler->getHandle() != 0)
                        {
                            glBindSampler(unit, sib.sampler->getHandle());
                        }
                    }
                }
            }
        }

        GLenum WebGL2RHI::ToGLInternalFormat(RHIFormat fmt)
        {
            switch (fmt)
            {
                case RHI_FORMAT_R8G8B8A8_UNORM:
                    return GL_RGBA8;
                case RHI_FORMAT_R8G8B8A8_SRGB:
                    return GL_SRGB8_ALPHA8;
                case RHI_FORMAT_R16G16B16A16_SFLOAT:
                    return GL_RGBA16F;
                case RHI_FORMAT_R32G32B32A32_SFLOAT:
                    return GL_RGBA32F;
                case RHI_FORMAT_D24_UNORM_S8_UINT:
                    return GL_DEPTH24_STENCIL8;
                case RHI_FORMAT_D32_SFLOAT:
                    return GL_DEPTH_COMPONENT32F;
                default:
                    return GL_RGBA8;
            }
        }
        GLenum WebGL2RHI::ToGLFormat(RHIFormat fmt)
        {
            switch (fmt)
            {
                case RHI_FORMAT_R8G8B8A8_UNORM:
                case RHI_FORMAT_R8G8B8A8_SRGB:
                case RHI_FORMAT_R16G16B16A16_SFLOAT:
                case RHI_FORMAT_R32G32B32A32_SFLOAT:
                    return GL_RGBA;
                case RHI_FORMAT_D24_UNORM_S8_UINT:
                    return GL_DEPTH_STENCIL;
                case RHI_FORMAT_D32_SFLOAT:
                    return GL_DEPTH_COMPONENT;
                default:
                    return GL_RGBA;
            }
        }
        GLenum WebGL2RHI::ToGLType(RHIFormat fmt)
        {
            switch (fmt)
            {
                case RHI_FORMAT_R8G8B8A8_UNORM:
                case RHI_FORMAT_R8G8B8A8_SRGB:
                    return GL_UNSIGNED_BYTE;
                case RHI_FORMAT_R16G16B16A16_SFLOAT:
                    return GL_HALF_FLOAT;
                case RHI_FORMAT_R32G32B32A32_SFLOAT:
                    return GL_FLOAT;
                case RHI_FORMAT_D24_UNORM_S8_UINT:
                    return GL_UNSIGNED_INT_24_8;
                case RHI_FORMAT_D32_SFLOAT:
                    return GL_FLOAT;
                default:
                    return GL_UNSIGNED_BYTE;
            }
        }
        GLenum WebGL2RHI::ToGLTopology(RHIPipelineBindPoint, int)
        {
            return GL_TRIANGLES;
        }
        GLenum WebGL2RHI::ToGLIndexType(RHIIndexType t)
        {
            switch (t)
            {
                case RHI_INDEX_TYPE_UINT16:
                    return GL_UNSIGNED_SHORT;
                case RHI_INDEX_TYPE_UINT32:
                    return GL_UNSIGNED_INT;
                default:
                    return GL_UNSIGNED_SHORT;
            }
        }
        GLenum WebGL2RHI::ToGLCompareOp(RHICompareOp op)
        {
            switch (op)
            {
                case RHI_COMPARE_OP_NEVER:         return GL_NEVER;
                case RHI_COMPARE_OP_LESS:          return GL_LESS;
                case RHI_COMPARE_OP_EQUAL:         return GL_EQUAL;
                case RHI_COMPARE_OP_LESS_OR_EQUAL: return GL_LEQUAL;
                case RHI_COMPARE_OP_GREATER:       return GL_GREATER;
                case RHI_COMPARE_OP_NOT_EQUAL:     return GL_NOTEQUAL;
                case RHI_COMPARE_OP_GREATER_OR_EQUAL: return GL_GEQUAL;
                case RHI_COMPARE_OP_ALWAYS:        return GL_ALWAYS;
                default:                           return GL_ALWAYS;
            }
        }
        GLenum WebGL2RHI::ToGLCullMode(RHICullModeFlags cull)
        {
            // GL's cull takes a single face; the RHI uses a flag set. Front-and-
            // back culling has no GL equivalent in ES 3.0 (only GL_ARB), so we
            // fall back to back-face culling (the most common case).
            if (cull == RHI_CULL_MODE_FRONT_AND_BACK)
                return GL_BACK;
            if (cull & RHI_CULL_MODE_FRONT_BIT)
                return GL_FRONT;
            if (cull & RHI_CULL_MODE_BACK_BIT)
                return GL_BACK;
            return GL_BACK;  // RHI_CULL_MODE_NONE -> disabled by caller
        }
        GLenum WebGL2RHI::ToGLFrontFace(RHIFrontFace ff)
        {
            switch (ff)
            {
                case RHI_FRONT_FACE_COUNTER_CLOCKWISE: return GL_CCW;
                case RHI_FRONT_FACE_CLOCKWISE:         return GL_CW;
                default:                               return GL_CCW;
            }
        }
        GLenum WebGL2RHI::ToGLPolygonMode(RHIPolygonMode pm)
        {
            // glPolygonMode is a desktop-GL feature; GL_FILL/GL_LINE/GL_POINT
            // are not defined in GLES 3.0. WebGL2 can't change polygon mode, so
            // this helper is a no-op default (the pipeline flush never consumes
            // it). Kept for interface parity with the other ToGL* translators.
            (void)pm;
            return 0;
        }
        GLenum WebGL2RHI::ToGLBlendFactor(RHIBlendFactor f)
        {
            switch (f)
            {
                case RHI_BLEND_FACTOR_ZERO:                 return GL_ZERO;
                case RHI_BLEND_FACTOR_ONE:                  return GL_ONE;
                case RHI_BLEND_FACTOR_SRC_COLOR:            return GL_SRC_COLOR;
                case RHI_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:  return GL_ONE_MINUS_SRC_COLOR;
                case RHI_BLEND_FACTOR_DST_COLOR:            return GL_DST_COLOR;
                case RHI_BLEND_FACTOR_ONE_MINUS_DST_COLOR:  return GL_ONE_MINUS_DST_COLOR;
                case RHI_BLEND_FACTOR_SRC_ALPHA:            return GL_SRC_ALPHA;
                case RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
                case RHI_BLEND_FACTOR_DST_ALPHA:            return GL_DST_ALPHA;
                case RHI_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:  return GL_ONE_MINUS_DST_ALPHA;
                case RHI_BLEND_FACTOR_CONSTANT_COLOR:       return GL_CONSTANT_COLOR;
                case RHI_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return GL_ONE_MINUS_CONSTANT_COLOR;
                case RHI_BLEND_FACTOR_CONSTANT_ALPHA:       return GL_CONSTANT_ALPHA;
                case RHI_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return GL_ONE_MINUS_CONSTANT_ALPHA;
                case RHI_BLEND_FACTOR_SRC_ALPHA_SATURATE:   return GL_SRC_ALPHA_SATURATE;
                default:                                    return GL_ONE;
            }
        }
        GLenum WebGL2RHI::ToGLBlendOp(RHIBlendOp op)
        {
            switch (op)
            {
                case RHI_BLEND_OP_ADD:            return GL_FUNC_ADD;
                case RHI_BLEND_OP_SUBTRACT:       return GL_FUNC_SUBTRACT;
                case RHI_BLEND_OP_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
                case RHI_BLEND_OP_MIN:
                    // GL_FUNC_MIN (0x8092) is an EXT on ES 3.0, not core; fall
                    // back to ADD when the constant isn't defined.
                #ifdef GL_FUNC_MIN
                    return GL_FUNC_MIN;
                #else
                    return GL_FUNC_ADD;
                #endif
                case RHI_BLEND_OP_MAX:
                #ifdef GL_FUNC_MAX
                    return GL_FUNC_MAX;
                #else
                    return GL_FUNC_ADD;
                #endif
                default:                          return GL_FUNC_ADD;
            }
        }
        GLenum WebGL2RHI::ToGLFilter(RHIFilter f)
        {
            switch (f)
            {
                case RHI_FILTER_NEAREST: return GL_NEAREST;
                case RHI_FILTER_LINEAR:  return GL_LINEAR;
                default:                 return GL_LINEAR;
            }
        }
        GLenum WebGL2RHI::ToGLAddressMode(RHISamplerAddressMode a)
        {
            switch (a)
            {
                case RHI_SAMPLER_ADDRESS_MODE_REPEAT:            return GL_REPEAT;
                case RHI_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:   return GL_MIRRORED_REPEAT;
                case RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:     return GL_CLAMP_TO_EDGE;
                case RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
                #ifdef GL_CLAMP_TO_BORDER
                    return GL_CLAMP_TO_BORDER;
                #else
                    return GL_CLAMP_TO_EDGE;
                #endif
                case RHI_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
                #ifdef GL_MIRROR_CLAMP_TO_EDGE
                    return GL_MIRROR_CLAMP_TO_EDGE;
                #else
                    return GL_CLAMP_TO_EDGE;
                #endif
                default:                                         return GL_CLAMP_TO_EDGE;
            }
        }
        GLenum WebGL2RHI::ToGLMipmapMode(RHISamplerMipmapMode m)
        {
            switch (m)
            {
                case RHI_SAMPLER_MIPMAP_MODE_NEAREST: return GL_NEAREST_MIPMAP_NEAREST;
                case RHI_SAMPLER_MIPMAP_MODE_LINEAR:  return GL_LINEAR_MIPMAP_LINEAR;
                default:                              return GL_LINEAR_MIPMAP_LINEAR;
            }
        }

    }  // namespace WebGL2
}  // namespace ZEngine

// ---- audit #4：WebGL2 后端自注册进 RHIFactory 注册表 ----
// 本翻译单元只在 Emscripten 编译（整个文件包在 __EMSCRIPTEN__ 守卫里），
// 故注册器放在命名空间外、守卫内。
namespace {
    struct WebGL2RHIBackendRegistrar
    {
        WebGL2RHIBackendRegistrar()
        {
            RHIFactory::RegisterBackend(GraphicsAPI::WebGL2,
                [](WindowSystem* window_system) -> RHI* { return new ZEngine::WebGL2::WebGL2RHI(); });
        }
    } s_webgl2_rhi_registrar;
}

#endif  // __EMSCRIPTEN__
