#version 450

// ===========================================================================
// VirtualTexture 离屏光栅验证——片元着色器：第一个**片元阶段**的页表消费者。
//
// vt_resolve.comp 在 compute 里逐整数 texel 查页表；这里换成真正的光栅化：UV 由光栅化器
// 逐像素插值得到（每个像素中心），再走同一份 vt_common.h 的 VTSampleUV。它验证两件
// compute 版验证不了的事：
//   * vt_common.h 在 fragment 阶段能用（sampler2D / texelFetch 在两个阶段的限制不同）；
//   * 输出尺寸与虚拟纹理尺寸不一致（放大 / 缩小）时，插值 UV → texel 的换算是对的。
//
// 不驻留的页输出 fallback（调用方指定，测试里 alpha=0，与源纹理的 alpha=255 区分开）。
// ===========================================================================

#extension GL_GOOGLE_include_directive : enable

#include "vt_common.h"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform _vt_blit_params
{
    uvec4 lookup;   // x = page_size, y = pool_width_pages, z = pages_x, w = pages_y
    uvec4 extent;   // x = virtual width, y = virtual height, z/w = unused
    vec4 fallback;  // 不驻留页输出的颜色
    uvec4 v2;       // x = page_border, y = mip_count, z = filter (0 = point mip 0, 1 = mip select + bilinear), w = unused
} u_params;

layout(set = 0, binding = 1) uniform sampler2D u_page_table;
layout(set = 0, binding = 2) uniform sampler2D u_physical_pool;

void main()
{
    VTLookupParams p;
    p.page_size = u_params.lookup.x;
    p.pool_width_pages = u_params.lookup.y;
    p.pages_x = u_params.lookup.z;
    p.pages_y = u_params.lookup.w;

    p.border = u_params.v2.x;
    p.mip_count = u_params.v2.y;
    p.virtual_size = uvec2(u_params.extent.xy);

    bool resident = false;
    if (u_params.v2.z == 0u)
    {
        // v1: 整数 texel / 点采样，mip 0。
        out_color = VTSampleUV(u_page_table, u_physical_pool, p, v_uv, p.virtual_size, u_params.fallback, resident);
    }
    else
    {
        // v2: 屏幕空间导数 → mip；缺页回退到更粗的 mip；mip 内双线性（gutter 保证跨页边不串页）。
        uint used_mip;
        const float lod = VTLod(p, dFdx(v_uv), dFdy(v_uv));
        out_color = VTSampleBilinear(u_page_table, u_physical_pool, p, v_uv, lod, u_params.fallback, resident, used_mip);
    }
}
