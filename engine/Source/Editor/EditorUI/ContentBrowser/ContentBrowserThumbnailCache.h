#pragma once

struct EditorFileNode;

#include <filesystem>

// Lazy GPU thumbnail cache for Content Browser tiles (Texture2D / MeshData / Material).
// Returns a UIGpuResources handle suitable for SImage::Texture, or nullptr to fall
// back to the type-colored placeholder quad.
namespace ContentBrowserThumbnailCache
{
    void InvalidateAll();
    void InvalidatePath(const std::filesystem::path& path);

    // Drain async mesh thumbnail jobs (call once per frame from Content Browser).
    // Returns true when at least one thumbnail became ready.
    bool Tick(int max_mesh_thumbnails_per_frame = 2);

    void* ResolveForNode(const EditorFileNode* node);
}
