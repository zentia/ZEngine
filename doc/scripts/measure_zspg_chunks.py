# -*- coding: utf-8 -*-
"""measure_zspg_chunks.py —— 量一批 .zspg 容器里各 chunk 的实际字节数。

用途
----
容器布局每改一次（v3 拆 UV、v6 裁剪多边形、v7 影响流…），都要回答同一个问题：
**改完到底省了多少**。靠"我觉得会省"不算数 —— 这里直接从真实资产烘焙出来的
容器里量，给出的数字可以写进 doc/GPU_SPINE_V7_ANIMATION.md 当结论。

用法
----
    python measure_zspg_chunks.py <容器目录>

    目录由 SpineCook 批量烘焙得到：
      ZEditor -batchmode -suite=SpineCook <Assets 根目录> -o <out> --recursive

输出
----
    * 顶点 / 影响条目总数、均值影响数
    * chunk 3（顶点）、21（bindPos）、22（bone+weight）、4（UV）、5（索引）的字节数
    * v7 实际 vs "v6 若同样全部能烘焙（weighted 定长 48 B）" 的比值
    * v9 动画块：chunk 11-16 的字节数、时间轴类型分布、曲线类型分布

⚠️ 两个容易踩的点
----------------
1. 魔数是 `0x5A535047`（'ZSPG'），**按小端落盘** ⇒ 文件头四字节是 `47 50 53 5A`
   （'GPSZ'）。别拿 `d[:4] != b"ZSPG"` 判 —— 那样会得到 0 个文件，而且**不报错**。
2. 只统计容器**实际写出来的** chunk：空 chunk 不落盘（规格 §9），
   所以"没有裁剪"的资产不会贡献 chunk 9，直接求和不会虚高。
"""

import collections
import os
import struct
import sys

kMagic = 0x5A535047  # 'ZSPG'
kHeaderSize = 64
kChunkEntrySize = 16

# header 里的分段信息（SpineGpuHeader 0x2C / 0x30 / 0x38）
kOffChunkCount = 0x2C
kOffRigidVertexCount = 0x30
kOffWeightedVertexCount = 0x38
kOffBoneCount = 0x10

# chunk 10 = SpineGpuBone[]，36 B/条（v8）
kBoneSetupSize = 36
kOffBoneParent = 32
kOffBoneTransformMode = 34
kOffBonePad = 35

# 逐值抄 spine-cpp TransformMode.h:34-40
kTransformModeNames = ["normal", "onlyTranslation", "noRotationOrReflection",
                       "noScale", "noScaleOrReflection"]

# ---- v9 动画块（chunk 11-16）----
kAnimDirSize = 20        # SpineGpuAnim
kTimelineEntrySize = 32  # SpineGpuTimeline
kBezierSize = 16         # SpineGpuBezier

# 逐值抄 spine-cpp TimelineType.h:35-49
kTimelineNames = [
    "rotate", "translate", "scale", "shear", "attachment", "color", "deform",
    "event", "drawOrder", "ikConstraint", "transformConstraint",
    "pathConstraintPosition", "pathConstraintSpacing", "pathConstraintMix", "twoColor",
]
# 逐值抄 CurveTimeline.cpp:42-44
kCurveNames = ["linear", "stepped", "bezier"]


def Mb(n):
    return "%.2f MB" % (n / 1048576.0)


def main():
    if len(sys.argv) < 2:
        print("用法：python measure_zspg_chunks.py <容器目录>")
        return 1

    root = sys.argv[1]
    sizes = collections.Counter()
    offsets = {}          # chunk id -> (offset, size)，只留最后一个文件（用来取骨骼名以外的字段）
    files = 0
    rigid_vertices = 0
    weighted_vertices = 0
    bone_total = 0
    mode_hist = collections.Counter()
    max_depth = 0
    bad_pad = 0

    # ---- v9 动画 ----
    anim_total = 0
    timeline_total = 0
    frame_floats = 0
    tl_type_hist = collections.Counter()
    curve_hist = collections.Counter()
    bezier_total = 0
    files_with_anim = 0
    bad_anim_pad = 0
    deform_frames = 0
    deform_floats = 0
    attachment_frames = 0
    draworder_frames = 0

    for dirpath, _, filenames in os.walk(root):
        for fn in filenames:
            if not fn.endswith(".zspg"):
                continue
            with open(os.path.join(dirpath, fn), "rb") as f:
                d = f.read()
            # ⚠️ 小端：文件头四字节是 'GPSZ' 不是 'ZSPG'（见文件头的踩坑说明）
            if len(d) < kHeaderSize or struct.unpack_from("<I", d, 0)[0] != kMagic:
                continue
            files += 1
            rigid_vertices += struct.unpack_from("<I", d, kOffRigidVertexCount)[0]
            weighted_vertices += struct.unpack_from("<I", d, kOffWeightedVertexCount)[0]
            bone_total += struct.unpack_from("<I", d, kOffBoneCount)[0]
            nchunk = struct.unpack_from("<I", d, kOffChunkCount)[0]
            offsets = {}
            for i in range(nchunk):
                cid, off, size = struct.unpack_from("<III", d, kHeaderSize + i * kChunkEntrySize)
                sizes[cid] += size
                offsets[cid] = (off, size)

            # ---- chunk 10：骨骼 setup ----
            if 10 in offsets:
                bo, bs = offsets[10]
                n = bs // kBoneSetupSize
                parents = []
                for k in range(n):
                    p = bo + k * kBoneSetupSize
                    mode_hist[d[p + kOffBoneTransformMode]] += 1
                    if d[p + kOffBonePad] != 0:
                        bad_pad += 1
                    parents.append(struct.unpack_from("<h", d, p + kOffBoneParent)[0])
                # 树深：有 parent < index 这条不变量在，一遍正向扫就能算
                depth = [0] * n
                for k in range(n):
                    par = parents[k]
                    depth[k] = 1 if par < 0 else depth[par] + 1
                if depth:
                    max_depth = max(max_depth, max(depth))

            # ---- chunk 11：动画目录（20 B/条）----
            if 11 in offsets:
                ao, asz = offsets[11]
                n = asz // kAnimDirSize
                anim_total += n
                files_with_anim += 1
                for k in range(n):
                    p = ao + k * kAnimDirSize
                    # pad @16 必须写 0，否则跨语言逐字节对拍会红
                    if struct.unpack_from("<I", d, p + 16)[0] != 0:
                        bad_anim_pad += 1

            # ---- chunk 12：时间轴表（32 B/条）----
            if 12 in offsets:
                to, tsz = offsets[12]
                n = tsz // kTimelineEntrySize
                timeline_total += n
                for k in range(n):
                    p = to + k * kTimelineEntrySize
                    tl_type_hist[struct.unpack_from("<H", d, p)[0]] += 1

            # ---- chunk 14：曲线类型（u8/段）----
            if 14 in offsets:
                co, csz = offsets[14]
                for k in range(csz):
                    curve_hist[d[co + k]] += 1

            # ---- chunk 16：变长载荷，按时间轴类型拆分 ----
            # deform（type 6）是这里的大头：每帧 [u32 start][u32 count][f32 × count]。
            # 量出"帧数 / float 数"才能回答"S6 的 deform 缓冲到底有多大"。
            if 12 in offsets and 16 in offsets:
                to, tsz = offsets[12]
                po, psz = offsets[16]
                for k in range(tsz // kTimelineEntrySize):
                    p = to + k * kTimelineEntrySize
                    ttype = struct.unpack_from("<H", d, p)[0]
                    frame_count = struct.unpack_from("<I", d, p + 4)[0]
                    payload_offset = struct.unpack_from("<I", d, p + 24)[0]
                    payload_count = struct.unpack_from("<I", d, p + 28)[0]
                    if ttype == 6:      # deform
                        deform_frames += payload_count
                        q = po + payload_offset
                        for _ in range(payload_count):
                            start, cnt = struct.unpack_from("<II", d, q)
                            q += 8 + cnt * 4
                            deform_floats += cnt
                    elif ttype == 4:    # attachment：每帧一个 u32
                        attachment_frames += payload_count
                        q = po + payload_offset + payload_count * 4
                        del q
                    elif ttype == 8:    # drawOrder：每帧 [u32 n][(u32,u32) × n]
                        draworder_frames += payload_count
                        q = po + payload_offset
                        for _ in range(payload_count):
                            n, = struct.unpack_from("<I", d, q)
                            q += 4 + n * 8

    if files == 0:
        print("目录里没有 .zspg：" + root)
        return 1

    # 这两块是定长的，条目数直接从字节数反推（header 里没有它们的位置）
    frame_floats = sizes[13] // 4
    bezier_total = sizes[15] // kBezierSize

    influences = sizes[22] // 4  # chunk 22 是 4 B/条
    print("容器数        : %d" % files)
    print("rigid 顶点    : %d" % rigid_vertices)
    print("weighted 顶点 : %d" % weighted_vertices)
    print("影响条目      : %d  (= chunk22 / 4)" % influences)
    if weighted_vertices:
        print("均值影响数    : %.3f" % (influences / float(weighted_vertices)))
    print()
    print("骨骼总数      : %d  (chunk 10，36 B/骨)" % bone_total)
    if bone_total:
        print("chunk 10 字节 : %s" % Mb(sizes[10]))
        print("均骨骼数/资产 : %.1f" % (bone_total / float(files)))
        print("最大树深      : %d" % max_depth)
        print("transform 模式分布（TransformMode.h:34-40）：")
        for v, c in sorted(mode_hist.items()):
            name = kTransformModeNames[v] if 0 <= v < len(kTransformModeNames) else "<越界 %d>" % v
            print("    %d %-24s %7d  %5.2f%%" % (v, name, c, 100.0 * c / bone_total))
        # pad 必须是 0：非 0 说明写端没清零，跨语言逐字节对拍会红
        print("pad 非 0 的骨骼: %d  （必须 0）" % bad_pad)
    print()

    print("chunk 3  顶点      : %s  (rigid 8B + weighted 8B 引用)" % Mb(sizes[3]))
    print("chunk 21 bindPos   : %s" % Mb(sizes[21]))
    print("chunk 22 bone/w    : %s" % Mb(sizes[22]))
    print("  => 蒙皮合计      : %s" % Mb(sizes[3] + sizes[21] + sizes[22]))
    print("chunk 4  UV        : %s" % Mb(sizes[4]))
    print("chunk 5  索引      : %s" % Mb(sizes[5]))
    print()

    # ---- v9 动画块 ----
    print("---- v9 动画（chunk 11-16）----")
    print("有动画的容器  : %d / %d" % (files_with_anim, files))
    print("动画总数      : %d  (chunk 11 / 20 B)" % anim_total)
    print("时间轴总数    : %d  (chunk 12 / 32 B)" % timeline_total)
    print("帧 float      : %d  (chunk 13)" % frame_floats)
    print("曲线段        : %d  (chunk 14)" % sizes[14])
    print("bezier 段     : %d  (chunk 15 / 16 B)" % bezier_total)
    print("变长载荷      : %s  (chunk 16)" % Mb(sizes[16]))
    print("  deform 帧   : %d  帧，共 %d 个 float（均值 %.1f/帧 ⇒ 稀疏，没展开成整帧）"
          % (deform_frames, deform_floats,
             deform_floats / float(deform_frames) if deform_frames else 0.0))
    print("  attachment  : %d 帧" % attachment_frames)
    print("  drawOrder   : %d 帧" % draworder_frames)
    print("  动画块合计  : %s" % Mb(sizes[11] + sizes[12] + sizes[13] +
                                    sizes[14] + sizes[15] + sizes[16]))
    if files_with_anim:
        print("均动画数/资产 : %.1f" % (anim_total / float(files_with_anim)))
    print("动画目录 pad 非 0: %d  （必须 0）" % bad_anim_pad)
    if timeline_total:
        print("时间轴类型分布（TimelineType.h:35-49）：")
        for v, c in sorted(tl_type_hist.items()):
            name = kTimelineNames[v] if 0 <= v < len(kTimelineNames) else "<越界 %d>" % v
            print("    %2d %-24s %7d  %5.2f%%" % (v, name, c, 100.0 * c / timeline_total))
    if sizes[14]:
        print("曲线类型分布（CurveTimeline.cpp:42-44）：")
        for v, c in sorted(curve_hist.items()):
            name = kCurveNames[v] if 0 <= v < len(kCurveNames) else "<越界 %d>" % v
            print("    %d %-10s %7d  %5.2f%%" % (v, name, c, 100.0 * c / sizes[14]))
    print()

    v7_skin = sizes[3] + sizes[21] + sizes[22]
    v6_skin = rigid_vertices * 8 + weighted_vertices * 48
    v7_all = v7_skin + sizes[4] + sizes[5]
    v6_all = v6_skin + sizes[4] + sizes[5]
    print("蒙皮相关（v7 实测）      : %s" % Mb(v7_skin))
    print("蒙皮相关（v6 定长 48B）  : %s" % Mb(v6_skin))
    print("  比值                   : %.3f" % (v7_skin / float(v6_skin)))
    print("顶点+蒙皮+UV+索引（v7）  : %s" % Mb(v7_all))
    print("同上（v6 若能全部烘焙）  : %s" % Mb(v6_all))
    print("  比值                   : %.3f" % (v7_all / float(v6_all)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
