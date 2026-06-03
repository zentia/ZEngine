#if defined(__APPLE__)

#include "Runtime/UI/Core/WindowUI.h"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#define Component AppleComponent
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#undef Component

// NOTE (de-ImGui, 2026-06): the editor UI is now recorded as native ZSlate
// batches by WindowUI::PreRender (BatchedUIRenderer -> UiRenderBatch), exactly
// like Windows (DX12) and Vulkan. This macOS pass no longer renders ImGui draw
// data; it owns a CAMetalLayer purely to clear + present the editor window.
//
// Rasterizing the native batch on macOS still needs two pieces that do not yet
// exist:
//   1. a Metal pipeline branch in ZSlateEditorOverlay::EnsurePipeline / DrawBatch
//      (today only GraphicsAPI::DirectX12 and GraphicsAPI::Vulkan are handled), and
//   2. MetalRHI swapchain / present integration (MetalRHI::CreateSwapchain et al.
//      are currently empty stubs, which is precisely why this bespoke CAMetalLayer
//      path exists at all).
//
// Until that lands the macOS editor window presents a cleared frame. This is the
// Metal half of the imgui-unlink work; once it is done this whole file should be
// deleted and the __APPLE__ branch in EditorUIPass.cpp folded into the shared
// native-overlay path.

namespace
{
    id<MTLDevice>            g_editor_metal_device                 = nil;
    id<MTLCommandQueue>      g_editor_metal_command_queue          = nil;
    CAMetalLayer*            g_editor_metal_layer                  = nil;
    MTLRenderPassDescriptor* g_editor_metal_render_pass_descriptor = nil;

    bool ensureEditorMetalContext(GLFWwindow* window)
    {
        if (window == nullptr)
        {
            return false;
        }

        if (g_editor_metal_device == nil)
        {
            g_editor_metal_device = MTLCreateSystemDefaultDevice();
        }
        if (g_editor_metal_device == nil)
        {
            return false;
        }

        if (g_editor_metal_command_queue == nil)
        {
            g_editor_metal_command_queue = [g_editor_metal_device newCommandQueue];
        }
        if (g_editor_metal_command_queue == nil)
        {
            return false;
        }

        if (g_editor_metal_layer == nil)
        {
            NSWindow* ns_window = glfwGetCocoaWindow(window);
            if (ns_window == nil || ns_window.contentView == nil)
            {
                return false;
            }

            g_editor_metal_layer             = [CAMetalLayer layer];
            g_editor_metal_layer.device      = g_editor_metal_device;
            g_editor_metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            ns_window.contentView.layer      = g_editor_metal_layer;
            ns_window.contentView.wantsLayer = YES;
        }

        if (g_editor_metal_render_pass_descriptor == nil)
        {
            g_editor_metal_render_pass_descriptor = [MTLRenderPassDescriptor new];
        }

        return g_editor_metal_render_pass_descriptor != nil;
    }
}

bool EditorMetalUIInitialize(GLFWwindow* window)
{
    // No ImGui backend init anymore -- just bring up the Metal device/layer so the
    // editor window can present.
    return ensureEditorMetalContext(window);
}

bool EditorMetalUIRender(GLFWwindow* window, WindowUI* window_ui)
{
    if (window_ui == nullptr || !EditorMetalUIInitialize(window))
    {
        return false;
    }

    // Record this frame's native ZSlate batch + run per-frame UI logic. The batch
    // is not yet rasterized on Metal (see file note), but keeping the call live
    // preserves window state, input hit-testing geometry and any per-frame logic,
    // and means the day the Metal overlay pipeline lands the batch is already here.
    window_ui->PreRender();

    @autoreleasepool
    {
        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        if (display_w <= 0 || display_h <= 0)
        {
            return false;
        }

        g_editor_metal_layer.drawableSize = CGSizeMake(display_w, display_h);
        id<CAMetalDrawable> drawable = [g_editor_metal_layer nextDrawable];
        if (drawable == nil)
        {
            return false;
        }

        id<MTLCommandBuffer> command_buffer = [g_editor_metal_command_queue commandBuffer];
        if (command_buffer == nil)
        {
            return false;
        }

        g_editor_metal_render_pass_descriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.02, 0.02, 0.02, 1.0);
        g_editor_metal_render_pass_descriptor.colorAttachments[0].texture     = drawable.texture;
        g_editor_metal_render_pass_descriptor.colorAttachments[0].loadAction  = MTLLoadActionClear;
        g_editor_metal_render_pass_descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLRenderCommandEncoder> render_encoder =
            [command_buffer renderCommandEncoderWithDescriptor:g_editor_metal_render_pass_descriptor];
        if (render_encoder == nil)
        {
            return false;
        }

        // Clear-only for now (native ZSlate Metal raster pending; see file note).
        [render_encoder pushDebugGroup:@"ZEditor UI (native ZSlate; Metal raster pending)"];
        [render_encoder popDebugGroup];
        [render_encoder endEncoding];
        [command_buffer presentDrawable:drawable];
        [command_buffer commit];
    }

    return true;
}

#endif
