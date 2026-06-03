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

    #include "Runtime/Core/Base/Macro.h"
    #include "Runtime/Function/Render/WindowSystem.h"

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

            // Pick up the WebGL2 context that the emscripten GLFW shim created
            // inside glfwCreateWindow(). WindowSystem is registered ahead of any
            // RHI in RegisterRuntime.cpp so it has already run at this point.
            if (!CreateGLContext(GET_SYSTEM(WindowSystem)))
            {
                LOG_FATAL(ZRender, "WebGL2RHI: failed to acquire current WebGL2 context");
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
                                                         std::vector<uint8_t>& output_spirv_code,
                                                         const std::string& /*entry_point*/)
        {
            if (!m_ShaderCompiler)
            {
                return nullptr;
            }
            std::map<std::string, std::string> macro_map;
            auto r = m_ShaderCompiler->CompileFromFile(file_path, shader_stage, include_paths, macro_map);
            if (!r.success)
            {
                return nullptr;
            }
            auto* s = new WebGL2Shader();
            s->setStage(GL_VERTEX_SHADER, r.glsl_source);
            output_spirv_code.clear();
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
            return true;
        }
        bool WebGL2RHI::EndCommandBufferPFN(RHICommandBuffer*)
        {
            return true;
        }

        void WebGL2RHI::CmdBeginRenderPassPFN(RHICommandBuffer*, const RHIRenderPassBeginInfo*, RHISubpassContents)
        {
            WEBGL2_TODO("record");
        }
        void WebGL2RHI::CmdNextSubpassPFN(RHICommandBuffer*, RHISubpassContents)
        {
            WEBGL2_NOOP();
        }
        void WebGL2RHI::CmdEndRenderPassPFN(RHICommandBuffer*)
        {
            WEBGL2_TODO("record");
        }
        void WebGL2RHI::CmdBindPipelinePFN(RHICommandBuffer*, RHIPipelineBindPoint, RHIPipeline*)
        {
            WEBGL2_TODO("record");
        }
        void WebGL2RHI::CmdSetViewportPFN(RHICommandBuffer*, uint32_t, uint32_t, const RHIViewport*)
        {
            WEBGL2_TODO("record");
        }
        void WebGL2RHI::CmdSetScissorPFN(RHICommandBuffer*, uint32_t, uint32_t, const RHIRect2D*)
        {
            WEBGL2_TODO("record");
        }
        void WebGL2RHI::CmdBindVertexBuffersPFN(RHICommandBuffer*, uint32_t, uint32_t, RHIBuffer* const*, const RHIDeviceSize*)
        {
            WEBGL2_TODO("record");
        }
        void WebGL2RHI::CmdBindIndexBufferPFN(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize, RHIIndexType)
        {
            WEBGL2_TODO("record");
        }
        void WebGL2RHI::CmdBindDescriptorSetsPFN(RHICommandBuffer*, RHIPipelineBindPoint, RHIPipelineLayout*, uint32_t, uint32_t, const RHIDescriptorSet* const*, uint32_t, const uint32_t*)
        {
            WEBGL2_TODO("record");
        }
        void WebGL2RHI::CmdDrawIndexedPFN(RHICommandBuffer*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
        {
            WEBGL2_TODO("record");
        }
        void WebGL2RHI::CmdClearAttachmentsPFN(RHICommandBuffer*, uint32_t, const RHIClearAttachment*, uint32_t, const RHIClearRect*)
        {
            WEBGL2_TODO("record");
        }

        bool WebGL2RHI::BeginCommandBuffer(RHICommandBuffer* cb, const RHICommandBufferBeginInfo* info)
        {
            return BeginCommandBufferPFN(cb, info);
        }

        void WebGL2RHI::CmdCopyImageToBuffer(RHICommandBuffer*, RHIImage*, RHIImageLayout, RHIBuffer*, uint32_t, const RHIBufferImageCopy*)
        {
            WEBGL2_TODO("readback via FBO + glReadPixels");
        }
        void WebGL2RHI::CmdCopyImageToImage(RHICommandBuffer*, RHIImage*, RHIImageAspectFlagBits, RHIImage*, RHIImageAspectFlagBits, uint32_t, uint32_t)
        {
            WEBGL2_TODO("blit via FBO");
        }
        void WebGL2RHI::CmdBlitImage(RHICommandBuffer*, RHIImage*, RHIImageLayout, RHIImage*, RHIImageLayout, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, RHIFilter)
        {
            WEBGL2_TODO("glBlitFramebuffer");
        }
        void WebGL2RHI::CmdCopyBuffer(RHICommandBuffer*, RHIBuffer*, RHIBuffer*, uint32_t, RHIBufferCopy*)
        {
            WEBGL2_TODO("record copy ops");
        }
        void WebGL2RHI::CmdDraw(RHICommandBuffer*, uint32_t, uint32_t, uint32_t, uint32_t)
        {
            WEBGL2_TODO("record draw");
        }
        void WebGL2RHI::CmdDispatch(RHICommandBuffer*, uint32_t, uint32_t, uint32_t)
        { /* unsupported */
        }
        void WebGL2RHI::CmdDispatchIndirect(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize)
        { /* unsupported */
        }
        void WebGL2RHI::CmdPipelineBarrier(RHICommandBuffer*, RHIPipelineStageFlags, RHIPipelineStageFlags, RHIDependencyFlags, uint32_t, const RHIMemoryBarrier*, uint32_t, const RHIBufferMemoryBarrier*, uint32_t, const RHIImageMemoryBarrier*)
        { /* implicit in GL */
        }
        bool WebGL2RHI::EndCommandBuffer(RHICommandBuffer*)
        {
            return true;
        }

        void WebGL2RHI::UpdateDescriptorSets(uint32_t, const RHIWriteDescriptorSet*, uint32_t, const RHICopyDescriptorSet*)
        {
            WEBGL2_TODO("populate set bag");
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
        void WebGL2RHI::PushEvent(RHICommandBuffer*, const char*, const float*)
        {
            WEBGL2_NOOP();
        }
        void WebGL2RHI::PopEvent(RHICommandBuffer*)
        {
            WEBGL2_NOOP();
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
            // The emscripten GLFW shim already created and made-current a WebGL2
            // context inside glfwCreateWindow() (we set GLFW_OPENGL_ES_API + 3.0 in
            // WindowSystem.cpp). All we need to do is read the handle so we can
            // re-make-current it from any thread / re-entry point if needed.
            EMSCRIPTEN_WEBGL_CONTEXT_HANDLE handle = emscripten_webgl_get_current_context();
            if (handle <= 0)
            {
                // No GL context is current — most likely WindowSystem::Initialize()
                // did not run, or the shim failed to honour the OPENGL_ES_API hint.
                m_GlContextHandle = nullptr;
                return false;
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
        void WebGL2RHI::ExecuteCommandList(WebGL2CommandBuffer*)
        { /* TODO */
        }
        void WebGL2RHI::FlushPipelineState(WebGL2Pipeline*)
        { /* TODO */
        }
        void WebGL2RHI::BindDescriptorSets(WebGL2PipelineLayout*, uint32_t, uint32_t, const RHIDescriptorSet* const*)
        { /* TODO */
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

    }  // namespace WebGL2
}  // namespace ZEngine

#endif  // __EMSCRIPTEN__
