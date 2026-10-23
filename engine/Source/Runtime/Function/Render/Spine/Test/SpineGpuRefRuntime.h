#pragma once

// =====================================================================
// spine-cpp 参考实现的宿主胶水（SpineGpuRefParityTest 与 SpineGpuBenchmark 共用）。
// ⚠️ 含**非 inline 的定义**（getDefaultExtension / AtlasAttachmentLoader 桩）⇒ 每个可执行文件只能有
//    一个翻译单元 include 它（两个 exe 都是单 .cpp，正好）。
// =====================================================================

#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"

#include <spine/AtlasAttachmentLoader.h>
#include <spine/AttachmentLoader.h>
#include <spine/BoundingBoxAttachment.h>
#include <spine/ClippingAttachment.h>
#include <spine/Extension.h>
#include <spine/MeshAttachment.h>
#include <spine/PathAttachment.h>
#include <spine/PointAttachment.h>
#include <spine/RegionAttachment.h>

// ---------------------------------------------------------------------
// getDefaultExtension()
//
// spine-cpp 把"默认分配器由宿主提供"做成了一个**未定义的 extern**：
// `Extension.cpp:49` 只调用它，实现留给了引擎各自 runtime（Unity 那边在
// 别处给）。它不在 SpineReference.lib 里 ⇒ 宿主（本测试）必须给一个。
// `DefaultSpineExtension` 是随库实现的（malloc/free + fopen），直接用。
// ---------------------------------------------------------------------
namespace spine
{
SpineExtension* getDefaultExtension()
{
    static DefaultSpineExtension g_default_extension;
    return &g_default_extension;
}

// -------------------------------------------------------------------
// AtlasAttachmentLoader 的"永不调用"桩
//
// SkeletonBinary.cpp:98 的 `SkeletonBinary(Atlas*)` 里 `new AtlasAttachmentLoader(...)`。
// 我们只用 `SkeletonBinary(AttachmentLoader*)`，但 Debug 下 MSVC 链接器按 **.obj**
// 粒度拉符号 ⇒ 那个构造函数还是得能解析。而 `AtlasAttachmentLoader.cpp` 依赖
// `Atlas.cpp`，后者吃 Unity 的 baselib 头（该头文件在引擎里根本不存在）⇒ 编不过。
// ⇒ 这里补一份空实现；它对应的构造路径我们**永远不走**。
// -------------------------------------------------------------------
AtlasAttachmentLoader::AtlasAttachmentLoader(Atlas*) : _atlas(nullptr) {}
RegionAttachment* AtlasAttachmentLoader::newRegionAttachment(Skin&, const String&, const String&)
{
    return nullptr;
}
MeshAttachment* AtlasAttachmentLoader::newMeshAttachment(Skin&, const String&, const String&) { return nullptr; }
BoundingBoxAttachment* AtlasAttachmentLoader::newBoundingBoxAttachment(Skin&, const String&) { return nullptr; }
PathAttachment* AtlasAttachmentLoader::newPathAttachment(Skin&, const String&) { return nullptr; }
PointAttachment* AtlasAttachmentLoader::newPointAttachment(Skin&, const String&) { return nullptr; }
ClippingAttachment* AtlasAttachmentLoader::newClippingAttachment(Skin&, const String&) { return nullptr; }
void AtlasAttachmentLoader::configureAttachment(Attachment*) {}
AtlasRegion* AtlasAttachmentLoader::findRegion(const String&) { return nullptr; }
const SpineRTTI AtlasAttachmentLoader::rtti(6);
const SpineRTTI& AtlasAttachmentLoader::getRTTI() const { return rtti; }
}   // namespace spine

// ---------------------------------------------------------------------
// 参考侧的"空"附件加载器
//
// 我们只比**骨骼**，不需要图集 ⇒ 不需要 `AtlasAttachmentLoader`。
// 但 path 约束要读 `PathAttachment` 的顶点与 lengths —— 那部分由
// `SkeletonBinary::readVertices()` 填，跟图集无关 ⇒ 空加载器照样对。
// ---------------------------------------------------------------------
//
// v15：给了图集就按 `AtlasAttachmentLoader`（AtlasAttachmentLoader.cpp:51-96）配 region / mesh 的
// 图集参数 —— 端到端比 UV / 裁剪要用参考侧的 UV。找不到 region 时**照样建附件**（参考加载器
// 会返回 NULL ⇒ 附件集合变了，骨骼对拍就没法比），只是不配 UV。
class RefAttachmentLoader : public spine::AttachmentLoader
{
public:
    explicit RefAttachmentLoader(const SpineAtlas* atlas = nullptr) : m_Atlas(atlas) {}

    spine::RegionAttachment* newRegionAttachment(spine::Skin& skin, const spine::String& name,
                                                 const spine::String& path) override
    {
        (void)skin;
        spine::RegionAttachment* a = new (__FILE__, __LINE__) spine::RegionAttachment(name);
        a->setPath(path);
        if (const SpineAtlasRegion* r = Find(path))
        {
            a->setUVs(r->u, r->v, r->u2, r->v2, r->rotate);
            a->setRegionOffsetX(static_cast<float>(r->offset_x));
            a->setRegionOffsetY(static_cast<float>(r->offset_y));
            a->setRegionWidth(static_cast<float>(r->width));
            a->setRegionHeight(static_cast<float>(r->height));
            a->setRegionOriginalWidth(static_cast<float>(r->orig_w));
            a->setRegionOriginalHeight(static_cast<float>(r->orig_h));
        }
        return a;
    }
    spine::MeshAttachment* newMeshAttachment(spine::Skin& skin, const spine::String& name,
                                             const spine::String& path) override
    {
        (void)skin;
        spine::MeshAttachment* a = new (__FILE__, __LINE__) spine::MeshAttachment(name);
        a->setPath(path);
        if (const SpineAtlasRegion* r = Find(path))
        {
            a->setRegionU(r->u);
            a->setRegionV(r->v);
            a->setRegionU2(r->u2);
            a->setRegionV2(r->v2);
            a->setRegionRotate(r->rotate);
            a->setRegionOffsetX(static_cast<float>(r->offset_x));
            a->setRegionOffsetY(static_cast<float>(r->offset_y));
            a->setRegionWidth(static_cast<float>(r->width));
            a->setRegionHeight(static_cast<float>(r->height));
            a->setRegionOriginalWidth(static_cast<float>(r->orig_w));
            a->setRegionOriginalHeight(static_cast<float>(r->orig_h));
        }
        return a;
    }
    spine::BoundingBoxAttachment* newBoundingBoxAttachment(spine::Skin& skin, const spine::String& name) override
    {
        (void)skin;
        return new (__FILE__, __LINE__) spine::BoundingBoxAttachment(name);
    }
    spine::PathAttachment* newPathAttachment(spine::Skin& skin, const spine::String& name) override
    {
        (void)skin;
        return new (__FILE__, __LINE__) spine::PathAttachment(name);
    }
    spine::PointAttachment* newPointAttachment(spine::Skin& skin, const spine::String& name) override
    {
        (void)skin;
        return new (__FILE__, __LINE__) spine::PointAttachment(name);
    }
    spine::ClippingAttachment* newClippingAttachment(spine::Skin& skin, const spine::String& name) override
    {
        (void)skin;
        return new (__FILE__, __LINE__) spine::ClippingAttachment(name);
    }
    void configureAttachment(spine::Attachment* attachment) override { (void)attachment; }

private:
    const SpineAtlasRegion* Find(const spine::String& path) const
    {
        // spine::String 空串时 buffer() 是 NULL
        return m_Atlas != nullptr && path.buffer() != nullptr ? FindAtlasRegion(*m_Atlas, path.buffer()) : nullptr;
    }
    const SpineAtlas* m_Atlas;
};
