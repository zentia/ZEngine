// 容器的读与写。与 C++ `ReadSpineGpuContainer` / `WriteSpineGpuContainer`
// （SpineGpuImporter.cpp）逐字节对应 —— 这是整个规格的落点：
// **同一个 .zspg，C++ 写的 C# 能读、C# 写的 C++ 能读，且再写一遍字节不变**。
//
// 三条容易漏、漏了就静默错的规矩：
//   1. **chunk 按下标找会错位**（v2 → v3 重排过号）⇒ 只能按 id 找。
//   2. chunk 在文件里的**顺序不是 id 顺序**：写端按
//      1,2,10,11,12,13,14,15,16,17,3,21,22,4,5,9,6,7,8 追加
//      （v9 的动画块 11-16 紧跟骨骼 setup，v10 的槽位 setup 17 紧跟动画块；
//      影响流紧跟顶点块；9 夹在 5 与 6 之间，是历史顺序），
//      C# 必须照抄，否则字节对不上。
//   3. 顶点 / UV chunk 的起点要 **8 B 对齐**（ByteAddressBuffer::Load2 的前提），
//      其余 4 B 对齐。对齐产生的空档必须填 0 —— 不填就是"未初始化填充"，
//      跨语言一比对就露。

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Text;

namespace ZEngine.SpineGpu
{
    public static class SpineGpuContainerIo
    {
        // -------------------------------------------------------------
        // 读
        // -------------------------------------------------------------
        public static SpineGpuAsset Read(byte[] data)
        {
            SpineGpuLayout.Validate();
            var c = SpineGpuConstants.ChunkTableEntrySize;
            if (data == null)
            {
                throw new SpineGpuContainerException("container: 数据是 null");
            }
            if (data.Length < SpineGpuConstants.HeaderSize)
            {
                throw new SpineGpuContainerException(
                    "container: 长度 " + data.Length + " < header 64 B");
            }

            var asset = new SpineGpuAsset();
            var h = asset.Header;
            h.Magic = ReadU32(data, SpineGpuOffsets.HdrMagic);
            h.Version = ReadU32(data, SpineGpuOffsets.HdrVersion);
            h.SpineRuntime = ReadU32(data, SpineGpuOffsets.HdrSpineRuntime);
            h.Flags = (SpineGpuFlags)ReadU32(data, SpineGpuOffsets.HdrFlags);
            h.BoneCount = ReadU32(data, SpineGpuOffsets.HdrBoneCount);
            h.SlotCount = ReadU32(data, SpineGpuOffsets.HdrSlotCount);
            h.DrawItemCount = ReadU32(data, SpineGpuOffsets.HdrDrawItemCount);
            h.VertexCount = ReadU32(data, SpineGpuOffsets.HdrVertexCount);
            h.IndexCount = ReadU32(data, SpineGpuOffsets.HdrIndexCount);
            h.AtlasPageCount = ReadU32(data, SpineGpuOffsets.HdrAtlasPageCount);
            h.BoneStride = ReadU32(data, SpineGpuOffsets.HdrBoneStride);
            h.ChunkCount = ReadU32(data, SpineGpuOffsets.HdrChunkCount);
            h.RigidVertexCount = ReadU32(data, SpineGpuOffsets.HdrRigidVertexCount);
            h.WeightedSegmentOffset = ReadU32(data, SpineGpuOffsets.HdrWeightedSegmentOffset);
            h.WeightedVertexCount = ReadU32(data, SpineGpuOffsets.HdrWeightedVertexCount);
            h.SkinCount = ReadU32(data, SpineGpuOffsets.HdrSkinCount);

            if (h.Magic != SpineGpuConstants.Magic)
            {
                throw new SpineGpuContainerException("container: 魔数不是 'ZSPG'");
            }
            if (h.Version != SpineGpuConstants.FormatVersion)
            {
                // 规格 §11：版本变了必须明确拒绝，不做静默兼容。
                throw new SpineGpuContainerException(
                    "container: 版本 " + h.Version + " != " + SpineGpuConstants.FormatVersion +
                    "。规格 §11：版本变了必须明确拒绝，不做静默兼容。");
            }

            long need = (long)SpineGpuConstants.HeaderSize + (long)h.ChunkCount * c;
            if (need > data.Length)
            {
                throw new SpineGpuContainerException("container: chunk 表越界");
            }

            // id → (offset, size)。**按 id 索引，不按下标。**
            var byId = new Dictionary<uint, ChunkSpan>();
            for (uint i = 0; i < h.ChunkCount; ++i)
            {
                int e = (int)(SpineGpuConstants.HeaderSize + i * c);
                uint id = ReadU32(data, e + SpineGpuOffsets.ChunkId);
                uint off = ReadU32(data, e + SpineGpuOffsets.ChunkOffset);
                uint size = ReadU32(data, e + SpineGpuOffsets.ChunkSize);
                if ((long)off + size > data.Length)
                {
                    throw new SpineGpuContainerException("container: chunk " + id + " 越界");
                }
                byId[id] = new ChunkSpan { Offset = off, Size = size };
            }

            asset.RigidVertexCount = h.RigidVertexCount;
            asset.WeightedSegmentOffset = h.WeightedSegmentOffset;
            asset.WeightedVertexCount = h.WeightedVertexCount;

            // ---- 顶点 ----
            if (TryGet(byId, (uint)SpineGpuChunkId.VertexBuffer, out uint vo, out uint vs))
            {
                asset.VertexBuffer = new byte[vs];
                Buffer.BlockCopy(data, (int)vo, asset.VertexBuffer, 0, (int)vs);
            }

            // ---- 影响流（chunk 21 / 22）。两条流**必须条目数相同** ----
            // 它们共用同一个下标（权重顶点的 influence_offset）。对不上 = 两条流配错了对，
            // 不报错，只是每个影响取到别人的 bindPos / 骨骼 ⇒ 形状静默错。
            uint bindCount = 0;
            uint boneCount = 0;
            if (TryGet(byId, (uint)SpineGpuChunkId.InfluenceBind, out uint ibo, out uint ibs))
            {
                if (ibs % SpineGpuConstants.InfluenceBindStride != 0)
                {
                    throw new SpineGpuContainerException(
                        "container: influence_bind 大小不是 " + SpineGpuConstants.InfluenceBindStride +
                        " B 的整数倍（一条是 2 float）");
                }
                bindCount = ibs / SpineGpuConstants.InfluenceBindStride;
                asset.InfluenceBind = new float[(ibs / 4u)];
                for (uint i = 0; i < asset.InfluenceBind.Length; ++i)
                {
                    asset.InfluenceBind[i] = ReadF32(data, (int)(ibo + i * 4u));
                }
            }
            if (TryGet(byId, (uint)SpineGpuChunkId.InfluenceBone, out uint ieo, out uint ies))
            {
                if (ies % SpineGpuConstants.InfluenceBoneStride != 0)
                {
                    throw new SpineGpuContainerException(
                        "container: influence_bone 大小不是 " + SpineGpuConstants.InfluenceBoneStride +
                        " B 的整数倍（一条是 4 B）");
                }
                boneCount = ies / SpineGpuConstants.InfluenceBoneStride;
                asset.InfluenceBone = new byte[ies];
                Buffer.BlockCopy(data, (int)ieo, asset.InfluenceBone, 0, (int)ies);
            }
            if (bindCount != boneCount)
            {
                throw new SpineGpuContainerException(
                    "container: 影响流两条 chunk 条目数不同（bind " + bindCount + " vs bone " + boneCount +
                    "）—— 它们必须共用同一个下标");
            }

            // ---- UV ----
            if (TryGet(byId, (uint)SpineGpuChunkId.UvBuffer, out uint uo, out uint us))
            {
                if (us % SpineGpuConstants.UvStride != 0)
                {
                    throw new SpineGpuContainerException(
                        "container: uv_buffer 大小不是 " + SpineGpuConstants.UvStride + " B 的整数倍");
                }
                asset.UvBuffer = new float[us / 4u];
                for (uint i = 0; i < asset.UvBuffer.Length; ++i)
                {
                    asset.UvBuffer[i] = ReadF32(data, (int)(uo + i * 4u));
                }
            }

            // ---- 索引（uint16，除非 kIndex32）----
            if (TryGet(byId, (uint)SpineGpuChunkId.IndexBuffer, out uint io, out uint isz))
            {
                int stride = (h.Flags & SpineGpuFlags.Index32) != 0 ? 4 : 2;
                if (isz % (uint)stride != 0)
                {
                    throw new SpineGpuContainerException("container: index_buffer 大小不是 " + stride + " B 的整数倍");
                }
                asset.IndexBuffer = new ushort[isz / (uint)stride];
                for (int i = 0; i < asset.IndexBuffer.Length; ++i)
                {
                    asset.IndexBuffer[i] = stride == 2
                        ? ReadU16(data, (int)(io + (uint)i * 2u))
                        : (ushort)ReadU32(data, (int)(io + (uint)i * 4u));
                }
            }

            // ---- 裁剪多边形（chunk 9，12 B/条）----
            if (TryGet(byId, (uint)SpineGpuChunkId.ClipData, out uint co, out uint cs))
            {
                if (cs % SpineGpuConstants.ClipPolygonSize != 0)
                {
                    throw new SpineGpuContainerException(
                        "container: clip_data 大小不是 " + SpineGpuConstants.ClipPolygonSize +
                        " B 的整数倍（v6 起一条是 12 B）");
                }
                uint n = cs / SpineGpuConstants.ClipPolygonSize;
                for (uint i = 0; i < n; ++i)
                {
                    int p = (int)(co + i * SpineGpuConstants.ClipPolygonSize);
                    asset.ClipPolygons.Add(new SpineGpuClipPolygon
                    {
                        SlotIndex = ReadU16(data, p + SpineGpuOffsets.ClipSlotIndex),
                        SkinId = ReadU16(data, p + SpineGpuOffsets.ClipSkinId),
                        VertexFormat = (SpineVertexFormat)data[p + SpineGpuOffsets.ClipVertexFormat],
                        VertexCount = ReadU16(data, p + SpineGpuOffsets.ClipVertexCount),
                        VertexOffset = ReadU32(data, p + SpineGpuOffsets.ClipVertexOffset),
                    });
                }
            }

            // ---- 绘制项（28 B/项）----
            if (TryGet(byId, (uint)SpineGpuChunkId.DrawItems, out uint doff, out uint dsz))
            {
                if (dsz % SpineGpuConstants.DrawItemSize != 0)
                {
                    throw new SpineGpuContainerException(
                        "container: draw_items 大小不是 " + SpineGpuConstants.DrawItemSize +
                        " B 的整数倍（v6 起一项是 28 B：多了 clip_index）");
                }
                uint n = dsz / SpineGpuConstants.DrawItemSize;
                for (uint i = 0; i < n; ++i)
                {
                    int p = (int)(doff + i * SpineGpuConstants.DrawItemSize);
                    asset.DrawItems.Add(new SpineDrawItem
                    {
                        IndexOffset = ReadU32(data, p + SpineGpuOffsets.ItemIndexOffset),
                        IndexCount = ReadU32(data, p + SpineGpuOffsets.ItemIndexCount),
                        UvOffset = ReadU32(data, p + SpineGpuOffsets.ItemUvOffset),
                        VertexFormat = (SpineVertexFormat)data[p + SpineGpuOffsets.ItemVertexFormat],
                        BlendMode = (SpineBlendMode)data[p + SpineGpuOffsets.ItemBlendMode],
                        AtlasPage = ReadU16(data, p + SpineGpuOffsets.ItemAtlasPage),
                        SlotIndex = ReadU16(data, p + SpineGpuOffsets.ItemSlotIndex),
                        AttachmentId = ReadU16(data, p + SpineGpuOffsets.ItemAttachmentId),
                        SkinId = ReadU16(data, p + SpineGpuOffsets.ItemSkinId),
                        Kind = (SpineDrawItemKind)data[p + SpineGpuOffsets.ItemKind],
                        ClipIndex = ReadU16(data, p + SpineGpuOffsets.ItemClipIndex),
                    });
                }
            }

            // ---- slot_bone ----
            if (TryGet(byId, (uint)SpineGpuChunkId.SlotBone, out uint so, out uint ss))
            {
                asset.SlotBone = new ushort[ss / 2u];
                for (int i = 0; i < asset.SlotBone.Length; ++i)
                {
                    asset.SlotBone[i] = ReadU16(data, (int)(so + (uint)i * 2u));
                }
            }

            // ---- 骨骼 setup（chunk 10，36 B/骨）----
            // ⚠️ "chunk 不存在"不报错（空 chunk 不落盘），所以必须拿 header.BoneCount 对：
            // 少了它读端会得到 0 根骨骼，而后面所有 bone_idx 全部越界 —— 越界在 shader 里
            // 不报错，只是蒙皮取到 0 号骨骼。
            bool hasBoneSetup = TryGet(byId, (uint)SpineGpuChunkId.BoneSetup, out uint bso, out uint bss);
            if (asset.Header.BoneCount != 0 && !hasBoneSetup)
            {
                throw new SpineGpuContainerException(
                    "container: header 说有 " + asset.Header.BoneCount + " 根骨骼，但没有 chunk 10（bone_setup）");
            }
            if (hasBoneSetup)
            {
                if (bss % SpineGpuConstants.BoneSetupSize != 0)
                {
                    throw new SpineGpuContainerException(
                        "container: bone_setup 大小不是 " + SpineGpuConstants.BoneSetupSize + " B 的整数倍");
                }
                uint n = bss / SpineGpuConstants.BoneSetupSize;
                if (n != asset.Header.BoneCount)
                {
                    throw new SpineGpuContainerException(
                        "container: bone_setup 有 " + n + " 条，但 header.bone_count 是 " + asset.Header.BoneCount);
                }
                for (uint i = 0; i < n; ++i)
                {
                    int p = (int)(bso + i * SpineGpuConstants.BoneSetupSize);
                    var b = new SpineGpuBone
                    {
                        X = ReadF32(data, p + SpineGpuOffsets.BoneX),
                        Y = ReadF32(data, p + SpineGpuOffsets.BoneY),
                        Rotation = ReadF32(data, p + SpineGpuOffsets.BoneRotation),
                        ScaleX = ReadF32(data, p + SpineGpuOffsets.BoneScaleX),
                        ScaleY = ReadF32(data, p + SpineGpuOffsets.BoneScaleY),
                        ShearX = ReadF32(data, p + SpineGpuOffsets.BoneShearX),
                        ShearY = ReadF32(data, p + SpineGpuOffsets.BoneShearY),
                        Length = ReadF32(data, p + SpineGpuOffsets.BoneLength),
                        Parent = (short)ReadU16(data, p + SpineGpuOffsets.BoneParent),
                        TransformMode = (SpineTransformMode)data[p + SpineGpuOffsets.BoneTransformMode],
                    };
                    if (data[p + SpineGpuOffsets.BonePad] != 0)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 根骨骼的 pad 不是 0");
                    }
                    if (b.Parent >= (short)i || b.Parent < -1)
                    {
                        throw new SpineGpuContainerException(
                            "container: 第 " + i + " 根骨骼的父下标是 " + b.Parent + "，但父骨骼必须排在自己之前");
                    }
                    if (b.TransformMode > SpineTransformMode.NoScaleOrReflection)
                    {
                        throw new SpineGpuContainerException(
                            "container: 第 " + i + " 根骨骼的 transform 模式 " + (int)b.TransformMode + " 越界（只有 0..4）");
                    }
                    asset.Bones.Add(b);
                }
            }

            // ---- 动画（chunk 11-16，v9）----
            // 校验的重点不是"能不能读出来"，而是**下标引用有没有越界**：越界在 C# 里
            // 不抛异常（下标只是 uint），只会拿到别人的数据 —— 也就是"播 A 动画用了
            // B 动画的帧"。所以每个区间都显式查一遍（与 C++ 读端逐条对应）。
            {
                bool hasDir = TryGet(byId, (uint)SpineGpuChunkId.AnimDir, out uint ado, out uint ads);
                bool hasTl = TryGet(byId, (uint)SpineGpuChunkId.Timelines, out uint tlo, out uint tls);
                bool hasFrames = TryGet(byId, (uint)SpineGpuChunkId.Frames, out uint fro, out uint frs);
                bool hasCurve = TryGet(byId, (uint)SpineGpuChunkId.CurveTypes, out uint cuo, out uint cus);
                bool hasBez = TryGet(byId, (uint)SpineGpuChunkId.Beziers, out uint bzo, out uint bzs);
                bool hasPay = TryGet(byId, (uint)SpineGpuChunkId.TimelinePayload, out uint plo, out uint pls);

                if (hasDir)
                {
                    if (ads % SpineGpuConstants.AnimDirSize != 0)
                    {
                        throw new SpineGpuContainerException(
                            "container: 动画目录大小不是 " + SpineGpuConstants.AnimDirSize +
                            " B 的整数倍（一条是 20 B）");
                    }
                    uint n = ads / SpineGpuConstants.AnimDirSize;
                    for (uint i = 0; i < n; ++i)
                    {
                        int p = (int)(ado + i * SpineGpuConstants.AnimDirSize);
                        var a = new SpineGpuAnim
                        {
                            NameOffset = ReadU32(data, p + SpineGpuOffsets.AnimNameOffset),
                            TimelineFirst = ReadU32(data, p + SpineGpuOffsets.AnimTimelineFirst),
                            TimelineCount = ReadU32(data, p + SpineGpuOffsets.AnimTimelineCount),
                            Duration = ReadF32(data, p + SpineGpuOffsets.AnimDuration),
                            Pad = ReadU32(data, p + SpineGpuOffsets.AnimPad),
                        };
                        if (a.Pad != 0)
                        {
                            throw new SpineGpuContainerException(
                                "container: 动画目录第 " + i + " 条的 pad 不是 0");
                        }
                        asset.Anims.Add(a);
                    }
                }

                if (hasTl)
                {
                    if (tls % SpineGpuConstants.TimelineEntrySize != 0)
                    {
                        throw new SpineGpuContainerException(
                            "container: 时间轴表大小不是 " + SpineGpuConstants.TimelineEntrySize +
                            " B 的整数倍（一条是 32 B）");
                    }
                    uint n = tls / SpineGpuConstants.TimelineEntrySize;
                    for (uint i = 0; i < n; ++i)
                    {
                        int p = (int)(tlo + i * SpineGpuConstants.TimelineEntrySize);
                        asset.Timelines.Add(new SpineGpuTimeline
                        {
                            Type = ReadU16(data, p + SpineGpuOffsets.TlType),
                            Target = ReadU16(data, p + SpineGpuOffsets.TlTarget),
                            FrameCount = ReadU32(data, p + SpineGpuOffsets.TlFrameCount),
                            FrameFirst = ReadU32(data, p + SpineGpuOffsets.TlFrameFirst),
                            CurveFirst = ReadU32(data, p + SpineGpuOffsets.TlCurveFirst),
                            BezierFirst = ReadU32(data, p + SpineGpuOffsets.TlBezierFirst),
                            BezierCount = ReadU32(data, p + SpineGpuOffsets.TlBezierCount),
                            PayloadOffset = ReadU32(data, p + SpineGpuOffsets.TlPayloadOffset),
                            PayloadCount = ReadU32(data, p + SpineGpuOffsets.TlPayloadCount),
                        });
                    }
                }

                uint frameFloatCount = 0;
                if (hasFrames)
                {
                    if (frs % 4u != 0)
                    {
                        throw new SpineGpuContainerException(
                            "container: 帧数据大小不是 4 B 的整数倍（一条是 1 float）");
                    }
                    frameFloatCount = frs / 4u;
                    asset.Frames = new float[frameFloatCount];
                    for (uint i = 0; i < frameFloatCount; ++i)
                    {
                        asset.Frames[i] = ReadF32(data, (int)(fro + i * 4u));
                    }
                }
                uint curveCount = 0;
                if (hasCurve)
                {
                    curveCount = cus;
                    asset.CurveTypes = new byte[cus];
                    Buffer.BlockCopy(data, (int)cuo, asset.CurveTypes, 0, (int)cus);
                }
                uint bezierCount = 0;
                if (hasBez)
                {
                    if (bzs % SpineGpuConstants.BezierSize != 0)
                    {
                        throw new SpineGpuContainerException(
                            "container: bezier 块大小不是 " + SpineGpuConstants.BezierSize +
                            " B 的整数倍（一条是 4 float）");
                    }
                    bezierCount = bzs / SpineGpuConstants.BezierSize;
                    for (uint i = 0; i < bezierCount; ++i)
                    {
                        int p = (int)(bzo + i * SpineGpuConstants.BezierSize);
                        asset.Beziers.Add(new SpineGpuBezier
                        {
                            Cx1 = ReadF32(data, p + SpineGpuOffsets.BezCx1),
                            Cy1 = ReadF32(data, p + SpineGpuOffsets.BezCy1),
                            Cx2 = ReadF32(data, p + SpineGpuOffsets.BezCx2),
                            Cy2 = ReadF32(data, p + SpineGpuOffsets.BezCy2),
                        });
                    }
                }
                uint payloadSize = 0;
                if (hasPay)
                {
                    payloadSize = pls;
                    asset.TimelinePayload = new byte[pls];
                    Buffer.BlockCopy(data, (int)plo, asset.TimelinePayload, 0, (int)pls);
                }

                // ---- 区间校验 ----
                for (int i = 0; i < asset.Timelines.Count; ++i)
                {
                    var t = asset.Timelines[i];
                    var type = (SpineTimelineType)t.Type;
                    uint entries = SpineTimelineInfo.Entries(type);
                    if (entries == 0)
                    {
                        throw new SpineGpuContainerException(
                            "container: 第 " + i + " 条时间轴的类型 " + t.Type + " 不认识（TimelineType 只到 14）");
                    }
                    if (t.FrameCount == 0)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 条时间轴有 0 帧");
                    }
                    ulong frameEnd = (ulong)t.FrameFirst + (ulong)t.FrameCount * entries;
                    if (frameEnd > frameFloatCount)
                    {
                        throw new SpineGpuContainerException(
                            "container: 第 " + i + " 条时间轴的帧区间 [ " + t.FrameFirst + ", " + frameEnd +
                            " ) 超出帧块（" + frameFloatCount + " 个 float）");
                    }
                    uint segments = SpineTimelineInfo.HasCurve(type) ? (t.FrameCount - 1u) : 0u;
                    ulong curveEnd = (ulong)t.CurveFirst + segments;
                    if (curveEnd > curveCount)
                    {
                        throw new SpineGpuContainerException(
                            "container: 第 " + i + " 条时间轴的曲线区间超出曲线块（" + curveCount + " 段）");
                    }
                    ulong bezEnd = (ulong)t.BezierFirst + t.BezierCount;
                    if (bezEnd > bezierCount)
                    {
                        throw new SpineGpuContainerException(
                            "container: 第 " + i + " 条时间轴的 bezier 区间超出块（" + bezierCount + " 条）");
                    }
                    // 曲线块里 bezier 段的个数必须**等于** BezierCount ——
                    // 不等意味着"某段标了 bezier 却没有控制点"，采样时会取到上一条的。
                    uint bezInSegments = 0;
                    for (uint s = 0; s < segments; ++s)
                    {
                        byte ct = asset.CurveTypes[t.CurveFirst + s];
                        if (ct == (byte)SpineCurveType.Bezier)
                        {
                            ++bezInSegments;
                        }
                        if (ct > (byte)SpineCurveType.Bezier)
                        {
                            throw new SpineGpuContainerException(
                                "container: 第 " + i + " 条时间轴第 " + s +
                                " 段的曲线类型越界（0 linear / 1 stepped / 2 bezier）");
                        }
                    }
                    if (bezInSegments != t.BezierCount)
                    {
                        throw new SpineGpuContainerException(
                            "container: 第 " + i + " 条时间轴标了 " + bezInSegments + " 段 bezier，但 bezier_count 是 " +
                            t.BezierCount);
                    }
                    // 变长载荷：逐帧走一遍，顺带校验帧数（走不完 = 载荷被截断）
                    if (type == SpineTimelineType.Attachment || type == SpineTimelineType.Deform ||
                        type == SpineTimelineType.DrawOrder)
                    {
                        uint walked = 0;
                        uint pos = t.PayloadOffset;
                        for (uint fr = 0; fr < t.PayloadCount; ++fr)
                        {
                            if (type == SpineTimelineType.Attachment)
                            {
                                if (pos + 4u > payloadSize)
                                {
                                    throw new SpineGpuContainerException(
                                        "container: 第 " + i + " 条时间轴的载荷被截断");
                                }
                                pos += 4u;
                            }
                            else if (type == SpineTimelineType.Deform)
                            {
                                if (pos + 8u > payloadSize)
                                {
                                    throw new SpineGpuContainerException(
                                        "container: 第 " + i + " 条时间轴的载荷被截断");
                                }
                                uint cnt = ReadU32(asset.TimelinePayload, (int)(pos + 4u));
                                pos += 8u;
                                if (pos + cnt * 4u > payloadSize)
                                {
                                    throw new SpineGpuContainerException(
                                        "container: 第 " + i + " 条时间轴第 " + fr + " 帧的 deform 值被截断");
                                }
                                pos += cnt * 4u;
                            }
                            else
                            {
                                if (pos + 4u > payloadSize)
                                {
                                    throw new SpineGpuContainerException(
                                        "container: 第 " + i + " 条时间轴的载荷被截断");
                                }
                                uint n2 = ReadU32(asset.TimelinePayload, (int)pos);
                                pos += 4u;
                                if (pos + n2 * 8u > payloadSize)
                                {
                                    throw new SpineGpuContainerException(
                                        "container: 第 " + i + " 条时间轴第 " + fr + " 帧的 drawOrder 偏移被截断");
                                }
                                pos += n2 * 8u;
                            }
                            ++walked;
                        }
                        if (walked != t.FrameCount)
                        {
                            throw new SpineGpuContainerException(
                                "container: 第 " + i + " 条时间轴的载荷有 " + walked + " 帧，但 frame_count 是 " +
                                t.FrameCount);
                        }
                    }
                    else if (t.PayloadCount != 0)
                    {
                        throw new SpineGpuContainerException(
                            "container: 第 " + i + " 条时间轴是定长类型，却带了一份变长载荷");
                    }
                }

                // 动画的区间必须落在时间轴表里
                for (int i = 0; i < asset.Anims.Count; ++i)
                {
                    var a = asset.Anims[i];
                    ulong end = (ulong)a.TimelineFirst + a.TimelineCount;
                    if (end > (ulong)asset.Timelines.Count)
                    {
                        throw new SpineGpuContainerException(
                            "container: 第 " + i + " 个动画的时间轴区间 [ " + a.TimelineFirst + ", " + end +
                            " ) 超出时间轴表（" + asset.Timelines.Count + " 条）");
                    }
                }
            }

            // ---- attachments：拿到附件数，才能切分字符串表 ----
            int attachmentCount = 0;
            if (TryGet(byId, (uint)SpineGpuChunkId.Attachments, out uint ao, out uint asz))
            {
                if (asz % SpineGpuConstants.AttachmentRecordSize != 0)
                {
                    throw new SpineGpuContainerException(
                        "container: attachments 大小不是 " + SpineGpuConstants.AttachmentRecordSize + " B 的整数倍");
                }
                uint n = asz / SpineGpuConstants.AttachmentRecordSize;
                for (uint i = 0; i < n; ++i)
                {
                    int p = (int)(ao + i * SpineGpuConstants.AttachmentRecordSize);
                    asset.AttachmentRecords.Add(new SpineGpuAttachmentRecord
                    {
                        SlotIndex = ReadU16(data, p + SpineGpuOffsets.AttSlotIndex),
                        AttachmentId = ReadU16(data, p + SpineGpuOffsets.AttAttachmentId),
                        SkinId = ReadU16(data, p + SpineGpuOffsets.AttSkinId),
                    });
                }
                attachmentCount = (int)n;
            }

            // ---- 槽位 setup（chunk 17，v10）----
            // ⚠️ "chunk 不存在"本身**不**报错（v9 及更早的容器没有它），但"存在而条数
            //    不等于 header.slot_count"必须报错：少一条 ⇒ 后面的槽位读到别人的颜色，
            //    而"颜色略偏"是那种跑起来才发现、且查不出来的错。
            // 字符串块的大小要在**读 chunk 17 之前**拿到：槽位的 setup 附件名偏移
            // 必须落在它里面，而字符串块是在槽位块之后才被切分的。
            TryGet(byId, (uint)SpineGpuChunkId.Strings, out uint _, out uint stsAll);
            if (TryGet(byId, (uint)SpineGpuChunkId.SlotSetup, out uint sso, out uint sss))
            {
                if (sss % SpineGpuConstants.SlotSetupSize != 0)
                {
                    throw new SpineGpuContainerException("container: slot_setup 大小不是 " +
                        SpineGpuConstants.SlotSetupSize + " B 的整数倍（一条是 40 B）");
                }
                uint n = sss / SpineGpuConstants.SlotSetupSize;
                if (n != h.SlotCount)
                {
                    throw new SpineGpuContainerException("container: slot_setup 有 " + n +
                        " 条，但 header.slot_count 是 " + h.SlotCount);
                }
                for (uint i = 0; i < n; ++i)
                {
                    int p = (int)(sso + i * SpineGpuConstants.SlotSetupSize);
                    var s = new SpineGpuSlot
                    {
                        ColorR = ReadF32(data, p + SpineGpuOffsets.SlotColorR),
                        ColorG = ReadF32(data, p + SpineGpuOffsets.SlotColorG),
                        ColorB = ReadF32(data, p + SpineGpuOffsets.SlotColorB),
                        ColorA = ReadF32(data, p + SpineGpuOffsets.SlotColorA),
                        DarkR = ReadF32(data, p + SpineGpuOffsets.SlotDarkR),
                        DarkG = ReadF32(data, p + SpineGpuOffsets.SlotDarkG),
                        DarkB = ReadF32(data, p + SpineGpuOffsets.SlotDarkB),
                        DarkA = ReadF32(data, p + SpineGpuOffsets.SlotDarkA),
                        SetupAttachmentNameOffset = ReadU32(data, p + SpineGpuOffsets.SlotSetupAttachmentNameOffset),
                        Flags = (SpineGpuSlotFlags)ReadU32(data, p + SpineGpuOffsets.SlotFlags),
                    };
                    // setup 附件名偏移必须落在字符串块里；NoAttachmentName = "没挂附件"。
                    if (s.SetupAttachmentNameOffset != SpineGpuConstants.NoAttachmentName &&
                        (!byId.ContainsKey((uint)SpineGpuChunkId.Strings) ||
                         s.SetupAttachmentNameOffset >= stsAll))
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 个槽位的 setup 附件名偏移 " +
                            s.SetupAttachmentNameOffset + " 超出字符串块");
                    }
                    // 已知位之外的 flag 必须清零 —— 留着脏字节会让跨语言逐字节对拍对不上。
                    if ((s.Flags & ~SpineGpuSlotFlags.HasDark) != 0)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 个槽位的 flags 有未知位（" +
                            s.Flags + "，目前只有 bit0 = has_dark）");
                    }
                    asset.Slots.Add(s);
                }
            }

            // ---- 约束 setup（chunk 18，v11）----
            // 块头带 ik/tf/path/bone_pool 四个 count：**没有**它们的话，"chunk 不存在"
            // 和"chunk 被截断"在字节上分不开（v11 之前 chunk 10 / 11 / 17 都栽过）。
            if (TryGet(byId, (uint)SpineGpuChunkId.ConstraintSetup, out uint cso, out uint css))
            {
                if (css < SpineGpuConstants.ConstraintHeaderSize)
                {
                    throw new SpineGpuContainerException("container: constraint_setup 比块头（" +
                        SpineGpuConstants.ConstraintHeaderSize + " B）还小");
                }
                uint ikN = ReadU32(data, (int)cso + 0);
                uint tfN = ReadU32(data, (int)cso + 4);
                uint pathN = ReadU32(data, (int)cso + 8);
                uint poolN = ReadU32(data, (int)cso + 12);

                ulong want = SpineGpuConstants.ConstraintHeaderSize
                             + (ulong)ikN * SpineGpuConstants.IkConstraintSize
                             + (ulong)tfN * SpineGpuConstants.TransformConstraintSize
                             + (ulong)pathN * SpineGpuConstants.PathConstraintSize
                             + (ulong)poolN * 2u;
                if (want != css)
                {
                    throw new SpineGpuContainerException("container: constraint_setup 大小 " + css +
                        " B 与块头算出来的 " + want + " B 不一致（ik=" + ikN + " tf=" + tfN +
                        " path=" + pathN + " pool=" + poolN + "）");
                }

                uint off = SpineGpuConstants.ConstraintHeaderSize;
                for (uint i = 0; i < ikN; ++i)
                {
                    int p = (int)(cso + off);
                    var k = new SpineGpuIkConstraint
                    {
                        Order = ReadU16(data, p + SpineGpuOffsets.IkOrder),
                        BoneCount = ReadU16(data, p + SpineGpuOffsets.IkBoneCount),
                        BoneFirst = ReadU32(data, p + SpineGpuOffsets.IkBoneFirst),
                        Target = ReadU32(data, p + SpineGpuOffsets.IkTarget),
                        Mix = ReadF32(data, p + SpineGpuOffsets.IkMix),
                        BendDir = (short)ReadU16(data, p + SpineGpuOffsets.IkBendDir),
                        Flags = (SpineGpuIkFlags)ReadU16(data, p + SpineGpuOffsets.IkFlags),
                    };
                    ushort ikKnown = (ushort)(SpineGpuIkFlags.Compress | SpineGpuIkFlags.Stretch |
                                                   SpineGpuIkFlags.Uniform);
                    if (((ushort)k.Flags & (ushort)~ikKnown) != 0)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 条 IK 约束的 flags 有未知位（" +
                            k.Flags + "）");
                    }
                    if (k.BendDir != 1 && k.BendDir != -1)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 条 IK 约束的 bend_dir 是 " +
                            k.BendDir + "（只可能是 1 / -1）");
                    }
                    // ⚠️ target 是**骨骼**下标，越界不报错 —— 解算时读到别人的变换矩阵。
                    if (k.Target >= asset.Header.BoneCount)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 条 IK 约束的目标骨骼 " +
                            k.Target + " 越界（只有 " + asset.Header.BoneCount + " 根骨骼）");
                    }
                    asset.Iks.Add(k);
                    off += SpineGpuConstants.IkConstraintSize;
                }
                for (uint i = 0; i < tfN; ++i)
                {
                    int p = (int)(cso + off);
                    var t = new SpineGpuTransformConstraint
                    {
                        Order = ReadU16(data, p + SpineGpuOffsets.TfOrder),
                        BoneCount = ReadU16(data, p + SpineGpuOffsets.TfBoneCount),
                        BoneFirst = ReadU32(data, p + SpineGpuOffsets.TfBoneFirst),
                        Target = ReadU32(data, p + SpineGpuOffsets.TfTarget),
                        OffsetRotation = ReadF32(data, p + SpineGpuOffsets.TfOffsetRotation),
                        OffsetX = ReadF32(data, p + SpineGpuOffsets.TfOffsetX),
                        OffsetY = ReadF32(data, p + SpineGpuOffsets.TfOffsetY),
                        OffsetScaleX = ReadF32(data, p + SpineGpuOffsets.TfOffsetScaleX),
                        OffsetScaleY = ReadF32(data, p + SpineGpuOffsets.TfOffsetScaleY),
                        OffsetShearY = ReadF32(data, p + SpineGpuOffsets.TfOffsetShearY),
                        RotateMix = ReadF32(data, p + SpineGpuOffsets.TfRotateMix),
                        TranslateMix = ReadF32(data, p + SpineGpuOffsets.TfTranslateMix),
                        ScaleMix = ReadF32(data, p + SpineGpuOffsets.TfScaleMix),
                        ShearMix = ReadF32(data, p + SpineGpuOffsets.TfShearMix),
                        Flags = (SpineGpuTransformFlags)ReadU16(data, p + SpineGpuOffsets.TfFlags),
                        Pad = ReadU16(data, p + SpineGpuOffsets.TfPad),
                    };
                    ushort tfKnown = (ushort)(SpineGpuTransformFlags.Local | SpineGpuTransformFlags.Relative);
                    if (((ushort)t.Flags & (ushort)~tfKnown) != 0)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i +
                            " 条 transform 约束的 flags 有未知位（" + t.Flags + "）");
                    }
                    if (t.Pad != 0)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i +
                            " 条 transform 约束的 pad 不是 0");
                    }
                    if (t.Target >= asset.Header.BoneCount)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i +
                            " 条 transform 约束的目标骨骼 " + t.Target + " 越界（只有 " +
                            asset.Header.BoneCount + " 根骨骼）");
                    }
                    asset.Transforms.Add(t);
                    off += SpineGpuConstants.TransformConstraintSize;
                }
                for (uint i = 0; i < pathN; ++i)
                {
                    int p = (int)(cso + off);
                    var pc = new SpineGpuPathConstraint
                    {
                        Order = ReadU16(data, p + SpineGpuOffsets.PathOrder),
                        BoneCount = ReadU16(data, p + SpineGpuOffsets.PathBoneCount),
                        BoneFirst = ReadU32(data, p + SpineGpuOffsets.PathBoneFirst),
                        TargetSlot = ReadU32(data, p + SpineGpuOffsets.PathTargetSlot),
                        PositionMode = (SpineGpuPathPositionMode)ReadU8(data, p + SpineGpuOffsets.PathPositionMode),
                        SpacingMode = (SpineGpuPathSpacingMode)ReadU8(data, p + SpineGpuOffsets.PathSpacingMode),
                        RotateMode = (SpineGpuPathRotateMode)ReadU8(data, p + SpineGpuOffsets.PathRotateMode),
                        Pad = ReadU8(data, p + SpineGpuOffsets.PathPad),
                        OffsetRotation = ReadF32(data, p + SpineGpuOffsets.PathOffsetRotation),
                        Position = ReadF32(data, p + SpineGpuOffsets.PathPosition),
                        Spacing = ReadF32(data, p + SpineGpuOffsets.PathSpacing),
                        RotateMix = ReadF32(data, p + SpineGpuOffsets.PathRotateMix),
                        TranslateMix = ReadF32(data, p + SpineGpuOffsets.PathTranslateMix),
                        PathIndex = ReadU32(data, p + SpineGpuOffsets.PathIndex),
                    };
                    if (pc.PositionMode != SpineGpuPathPositionMode.Fixed &&
                        pc.PositionMode != SpineGpuPathPositionMode.Percent)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i +
                            " 条 path 约束的 positionMode 是 " + pc.PositionMode);
                    }
                    if ((byte)pc.SpacingMode > (byte)SpineGpuPathSpacingMode.Percent)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i +
                            " 条 path 约束的 spacingMode 是 " + pc.SpacingMode);
                    }
                    if ((byte)pc.RotateMode > (byte)SpineGpuPathRotateMode.ChainScale)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i +
                            " 条 path 约束的 rotateMode 是 " + pc.RotateMode);
                    }
                    if (pc.Pad != 0)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 条 path 约束的 pad 不是 0");
                    }
                    // ⚠️ 是**槽位**下标（`SkeletonBinary.cpp:249` 取 `_slots[...]`）——
                    //    与 IK / transform 的 target 同名但 index space 不同。
                    if (pc.TargetSlot >= asset.Header.SlotCount)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 条 path 约束的目标槽位 " +
                            pc.TargetSlot + " 越界（只有 " + asset.Header.SlotCount + " 个槽位）");
                    }
                    asset.Paths.Add(pc);
                    off += SpineGpuConstants.PathConstraintSize;
                }
                asset.ConstraintBonePool = new ushort[poolN];
                for (uint i = 0; i < poolN; ++i)
                {
                    ushort bb = ReadU16(data, (int)(cso + off));
                    if (bb >= asset.Header.BoneCount)
                    {
                        throw new SpineGpuContainerException("container: 约束骨骼池第 " + i + " 条是 " + bb +
                            "，越界（只有 " + asset.Header.BoneCount + " 根骨骼）");
                    }
                    asset.ConstraintBonePool[i] = bb;
                    off += 2;
                }
                // 每条约束的 [BoneFirst, BoneFirst + BoneCount) 必须落在池里 ——
                // 越界不报错，只是约束拿到别人的骨骼列表。
                Action<uint, uint, string, uint> checkRange = (first, count, what, i) =>
                {
                    ulong end = (ulong)first + count;
                    if (end > poolN)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 条 " + what +
                            " 约束的骨骼区间 [ " + first + ", " + end + " ) 超出骨骼池（" + poolN + " 条）");
                    }
                };
                for (uint i = 0; i < asset.Iks.Count; ++i)
                {
                    checkRange(asset.Iks[(int)i].BoneFirst, asset.Iks[(int)i].BoneCount, "IK", i);
                }
                for (uint i = 0; i < asset.Transforms.Count; ++i)
                {
                    checkRange(asset.Transforms[(int)i].BoneFirst, asset.Transforms[(int)i].BoneCount,
                               "transform", i);
                }
                for (uint i = 0; i < asset.Paths.Count; ++i)
                {
                    checkRange(asset.Paths[(int)i].BoneFirst, asset.Paths[(int)i].BoneCount, "path", i);
                }
                // order 必须是 0..N-1 的排列（Bake 侧已校验；这里是"读别人的容器"也要挡住）
                {
                    var orders = new List<uint>();
                    foreach (var k in asset.Iks) orders.Add(k.Order);
                    foreach (var t in asset.Transforms) orders.Add(t.Order);
                    foreach (var pc2 in asset.Paths) orders.Add(pc2.Order);
                    orders.Sort();
                    for (int i = 0; i < orders.Count; ++i)
                    {
                        if (orders[i] != (uint)i)
                        {
                            throw new SpineGpuContainerException("container: 约束的 order 不是 0.." +
                                (orders.Count - 1) + " 的一个排列（第 " + i + " 个是 " + orders[i] + "）");
                        }
                    }
                }
            }

            // ---- 执行顺序（chunk 19，v11）----
            // ⚠️ chunk 19 缺失 = 这个资产**没有约束**（写端只在有约束时落它），此时
            //    updateCache 的结果恒等于 0..bone_count-1。反过来：有约束却没有 chunk 19
            //    ⇒ 容器坏了，必须报错（静默退化会让约束全都不跑，"姿势不动"也不报错）。
            {
                bool anyConstraint = asset.Iks.Count > 0 || asset.Transforms.Count > 0 || asset.Paths.Count > 0;
                if (TryGet(byId, (uint)SpineGpuChunkId.UpdateOrder, out uint uoo, out uint uos))
                {
                    if (uos < SpineGpuConstants.UpdateOrderHeaderSize)
                    {
                        throw new SpineGpuContainerException("container: update_order 比块头（" +
                            SpineGpuConstants.UpdateOrderHeaderSize + " B）还小");
                    }
                    uint stepN = ReadU32(data, (int)uoo + 0);
                    uint resetN = ReadU32(data, (int)uoo + 4);
                    ulong want = SpineGpuConstants.UpdateOrderHeaderSize
                                 + (ulong)stepN * SpineGpuConstants.UpdateStepSize + (ulong)resetN * 2u;
                    if (want != uos)
                    {
                        throw new SpineGpuContainerException("container: update_order 大小 " + uos +
                            " B 与块头算出来的 " + want + " B 不一致");
                    }
                    uint off = SpineGpuConstants.UpdateOrderHeaderSize;
                    for (uint i = 0; i < stepN; ++i)
                    {
                        int p = (int)(uoo + off);
                        var s = new SpineGpuUpdateStep
                        {
                            Kind = (SpineGpuUpdateKind)ReadU8(data, p + SpineGpuOffsets.StepKind),
                            Pad = ReadU8(data, p + SpineGpuOffsets.StepPad),
                            Index = ReadU16(data, p + SpineGpuOffsets.StepIndex),
                        };
                        if ((byte)s.Kind > (byte)SpineGpuUpdateKind.Path)
                        {
                            throw new SpineGpuContainerException("container: 第 " + i + " 步的 kind 是 " +
                                s.Kind + "（只到 3 = path）");
                        }
                        if (s.Pad != 0)
                        {
                            throw new SpineGpuContainerException("container: 第 " + i + " 步的 pad 不是 0");
                        }
                        // 下标必须落在对应表里 —— 越界不报错，只是解算器跑到别人的对象上。
                        uint limit = s.Kind == SpineGpuUpdateKind.Bone ? h.BoneCount
                            : s.Kind == SpineGpuUpdateKind.Ik ? (uint)asset.Iks.Count
                            : s.Kind == SpineGpuUpdateKind.Transform ? (uint)asset.Transforms.Count
                            : (uint)asset.Paths.Count;
                        if (s.Index >= limit)
                        {
                            throw new SpineGpuContainerException("container: 第 " + i + " 步的 index " +
                                s.Index + " 超出该类对象的个数 " + limit);
                        }
                        asset.UpdateSteps.Add(s);
                        off += SpineGpuConstants.UpdateStepSize;
                    }
                    asset.UpdateReset = new ushort[resetN];
                    for (uint i = 0; i < resetN; ++i)
                    {
                        ushort b = ReadU16(data, (int)(uoo + off));
                        if (b >= h.BoneCount)
                        {
                            throw new SpineGpuContainerException("container: update_reset 第 " + i +
                                " 项的骨骼下标 " + b + " 超出骨骼数 " + h.BoneCount);
                        }
                        asset.UpdateReset[i] = b;
                        off += 2;
                    }
                }
                else if (anyConstraint)
                {
                    throw new SpineGpuContainerException("container: 有约束（ik=" + asset.Iks.Count +
                        " tf=" + asset.Transforms.Count + " path=" + asset.Paths.Count +
                        "）却没有 chunk 19（update_order）—— 约束会全部不跑，表现是「姿势不动」而不是报错");
                }
                else
                {
                    // 可证等价：无约束时 updateCache = 正向遍历全部骨骼，而 parent < index
                    // ⇒ 结果就是 0..bone_count-1，reset 为空。
                    for (uint i = 0; i < h.BoneCount; ++i)
                    {
                        asset.UpdateSteps.Add(new SpineGpuUpdateStep
                        {
                            Kind = SpineGpuUpdateKind.Bone,
                            Pad = 0,
                            Index = (ushort)i,
                        });
                    }
                    asset.UpdateReset = System.Array.Empty<ushort>();
                }
            }

            // ---- PathAttachment 几何（chunk 20，v11）----
            if (TryGet(byId, (uint)SpineGpuChunkId.PathAttachments, out uint pao, out uint pas))
            {
                if (pas < SpineGpuConstants.PathAttHeaderSize)
                {
                    throw new SpineGpuContainerException("container: path_attachments 比块头（" +
                        SpineGpuConstants.PathAttHeaderSize + " B）还小");
                }
                uint attN = ReadU32(data, (int)pao + 0);
                uint boneN = ReadU32(data, (int)pao + 4);
                uint vertN = ReadU32(data, (int)pao + 8);
                uint lenN = ReadU32(data, (int)pao + 12);
                ulong want = SpineGpuConstants.PathAttHeaderSize
                             + (ulong)attN * SpineGpuConstants.PathAttachmentSize
                             + (ulong)boneN * 2u + (ulong)vertN * 4u + (ulong)lenN * 4u;
                if (want != pas)
                {
                    throw new SpineGpuContainerException("container: path_attachments 大小 " + pas +
                        " B 与块头算出来的 " + want + " B 不一致");
                }
                uint off = SpineGpuConstants.PathAttHeaderSize;
                for (uint i = 0; i < attN; ++i)
                {
                    int p = (int)(pao + off);
                    var a = new SpineGpuPathAttachment
                    {
                        SlotIndex = ReadU16(data, p + SpineGpuOffsets.PathAttSlotIndex),
                        SkinIndex = ReadU16(data, p + SpineGpuOffsets.PathAttSkinIndex),
                        Flags = (SpineGpuPathAttachmentFlags)ReadU16(data, p + SpineGpuOffsets.PathAttFlags),
                        Pad = ReadU16(data, p + SpineGpuOffsets.PathAttPad),
                        VertexCount = ReadU32(data, p + SpineGpuOffsets.PathAttVertexCount),
                        BoneOffset = ReadU32(data, p + SpineGpuOffsets.PathAttBoneOffset),
                        BoneCount = ReadU32(data, p + SpineGpuOffsets.PathAttBoneCount),
                        VertexOffset = ReadU32(data, p + SpineGpuOffsets.PathAttVertexOffset),
                        VertexFloatCount = ReadU32(data, p + SpineGpuOffsets.PathAttVertexFloatCount),
                        LengthOffset = ReadU32(data, p + SpineGpuOffsets.PathAttLengthOffset),
                        LengthCount = ReadU32(data, p + SpineGpuOffsets.PathAttLengthCount),
                        NameOffset = ReadU32(data, p + SpineGpuOffsets.PathAttNameOffset),
                    };
                    ushort paKnown = (ushort)(SpineGpuPathAttachmentFlags.Closed |
                                                   SpineGpuPathAttachmentFlags.ConstantSpeed |
                                                   SpineGpuPathAttachmentFlags.Weighted);
                    if (((ushort)a.Flags & (ushort)~paKnown) != 0)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i +
                            " 条 path 附件的 flags 有未知位（" + a.Flags + "）");
                    }
                    if (a.Pad != 0)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 条 path 附件的 pad 不是 0");
                    }
                    if (a.SlotIndex >= h.SlotCount)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 条 path 附件的槽位下标 " +
                            a.SlotIndex + " 超出槽位数 " + h.SlotCount);
                    }
                    Action<uint, uint, uint, string> inPool = (first, count, pool, what) =>
                    {
                        if ((ulong)first + count > pool)
                        {
                            throw new SpineGpuContainerException("container: 第 " + i + " 条 path 附件的 " + what +
                                " 区间 [ " + first + ", " + ((ulong)first + count) + " ) 超出池（" + pool + "）");
                        }
                    };
                    inPool(a.BoneOffset, a.BoneCount, boneN, "骨骼");
                    inPool(a.VertexOffset, a.VertexFloatCount, vertN, "顶点");
                    inPool(a.LengthOffset, a.LengthCount, lenN, "lengths");
                    // lengths 条数 = 点数 / 3（SkeletonBinary.cpp:550）—— Bake 侧也是这个式子。
                    if (a.LengthCount != a.VertexCount / 3u)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 条 path 附件的 lengths 有 " +
                            a.LengthCount + " 条，但点数 " + a.VertexCount + " 对应 " +
                            (a.VertexCount / 3u) + " 条");
                    }
                    asset.PathAttachments.Add(a);
                    off += SpineGpuConstants.PathAttachmentSize;
                }
                asset.PathBonePool = new ushort[boneN];
                for (uint i = 0; i < boneN; ++i)
                {
                    ushort b = ReadU16(data, (int)(pao + off));
                    if (b >= h.BoneCount)
                    {
                        throw new SpineGpuContainerException("container: path 附件的骨骼池第 " + i + " 项 " +
                            b + " 超出骨骼数 " + h.BoneCount);
                    }
                    asset.PathBonePool[i] = b;
                    off += 2;
                }
                asset.PathVertices = new float[vertN];
                for (uint i = 0; i < vertN; ++i)
                {
                    asset.PathVertices[i] = ReadF32(data, (int)(pao + off));
                    off += 4;
                }
                asset.PathLengths = new float[lenN];
                for (uint i = 0; i < lenN; ++i)
                {
                    asset.PathLengths[i] = ReadF32(data, (int)(pao + off));
                    off += 4;
                }
            }

            // ---- 交叉检查：path 约束的 PathIndex 必须落在 chunk 20 里 ----
            // ⚠️ NoPathAttachment 是合法的（"这个槽位上没有 path 附件"），而 0 **也**是
            //    合法的下标 ⇒ 不能拿"非 0"判断，必须显式比 NoPathAttachment。
            for (int i = 0; i < asset.Paths.Count; ++i)
            {
                var pcx = asset.Paths[i];
                if (pcx.PathIndex != SpineGpuConstants.NoPathAttachment &&
                    pcx.PathIndex >= (uint)asset.PathAttachments.Count)
                {
                    throw new SpineGpuContainerException("container: 第 " + i + " 条 path 约束的 path_index " +
                        pcx.PathIndex + " 越界（chunk 20 只有 " + asset.PathAttachments.Count + " 条）");
                }
            }

            // ---- 交叉检查（v12）：指向 path 附件的 deform 时间轴 ----
            // 越界或帧写出 deform 数组都是坏容器：解算会读到别的 path 的曲线，不报错。
            for (int i = 0; i < asset.Timelines.Count; ++i)
            {
                var t = asset.Timelines[i];
                if ((SpineTimelineType)t.Type != SpineTimelineType.Deform ||
                    (t.Target & SpineGpuConstants.DeformTargetPathBit) == 0)
                {
                    continue;
                }
                int k = t.Target & SpineGpuConstants.DeformTargetIndexMask;
                if (k >= asset.PathAttachments.Count)
                {
                    throw new SpineGpuContainerException("container: 第 " + i + " 条 deform 时间轴指向 path 附件 " + k +
                        "，越界（chunk 20 只有 " + asset.PathAttachments.Count + " 条）");
                }
                uint deformLen = SpineGpuConstants.PathDeformLength(asset.PathAttachments[k]);
                long pos = t.PayloadOffset;   // 载荷已在时间轴校验里走过一遍
                for (uint fr = 0; fr < t.PayloadCount; ++fr)
                {
                    uint start = ReadU32(asset.TimelinePayload, (int)pos);
                    uint count = ReadU32(asset.TimelinePayload, (int)(pos + 4));
                    pos += 8 + (long)count * 4;
                    if ((ulong)start + count > deformLen)
                    {
                        throw new SpineGpuContainerException("container: 第 " + i + " 条 deform 时间轴第 " + fr +
                            " 帧写到 [" + start + ", " + (start + count) + ")，超出 path 附件 " + k +
                            " 的 deform 长度 " + deformLen);
                    }
                }
            }

            // ---- 字符串：骨骼名 → 皮肤名 → 槽位名 → 槽位 setup 附件名 → 附件名 →
            //              图集页名 → 动画名 → path 附件名（v11）→ 动画附件名 ----
            if (TryGet(byId, (uint)SpineGpuChunkId.Strings, out uint sto, out uint sts))
            {
                var all = SplitCStrings(data, (int)sto, (int)sts);
                int i = 0;
                Take(all, h.BoneCount, asset.BoneNames, ref i);
                Take(all, h.SkinCount, asset.SkinNames, ref i);
                Take(all, h.SlotCount, asset.SlotNames, ref i);
                Take(all, h.SlotCount, asset.SlotSetupAttachmentNames, ref i);
                Take(all, (uint)attachmentCount, asset.AttachmentNames, ref i);
                Take(all, h.AtlasPageCount, asset.AtlasPageNames, ref i);
                // 槽位 setup 附件名是 chunk 17 的复位基线 ⇒ 少一条就会让后面的槽位
                // 挂上别人的附件名（不报错，只是"这个槽位画错了图"）。
                if (asset.SlotSetupAttachmentNames.Count != h.SlotCount)
                {
                    throw new SpineGpuContainerException("container: 槽位 setup 附件名只有 " +
                        asset.SlotSetupAttachmentNames.Count + " 条，但 header.slot_count 是 " + h.SlotCount);
                }
                // 动画名：数量 = chunk 11 的条目数（header 里没有 anim_count 的位置 ——
                // 64 B 的 header 早就排满了，而定长块的大小本身就是计数）。
                Take(all, (uint)asset.Anims.Count, asset.AnimNames, ref i);
                // v11 path 附件名：数量 = chunk 20 的条目数（一一对应，不去重）。
                Take(all, (uint)asset.PathAttachments.Count, asset.PathAttachmentNames, ref i);
                if (asset.PathAttachmentNames.Count != asset.PathAttachments.Count)
                {
                    throw new SpineGpuContainerException("container: path 附件名只有 " +
                        asset.PathAttachmentNames.Count + " 条，但 chunk 20 有 " +
                        asset.PathAttachments.Count + " 条");
                }
                // 剩下的全是"动画附件名"。**只按偏移访问**，所以不需要知道有几条 ⇒
                // 不校验"是否刚好读完"（多出来的是合法数据，不是错误）。
                while (i < all.Count)
                {
                    asset.AnimAttachmentNames.Add(all[i++]);
                }
                // ⚠️ 但每个 name_offset 必须真的落在这一块里 —— 指到前面几组（比如骨骼名）
                // 不报错，只是动画挂载了错误名字的附件。
                for (int k = 0; k < asset.Anims.Count; ++k)
                {
                    if (asset.Anims[k].NameOffset >= sts)
                    {
                        throw new SpineGpuContainerException(
                            "container: 第 " + k + " 个动画的 name_offset " + asset.Anims[k].NameOffset +
                            " 超出字符串块（大小 " + sts + "）");
                    }
                }
                // path 附件的名字同理：指到骨骼名那组不报错，只是"换装后摆错了曲线"。
                for (int k = 0; k < asset.PathAttachments.Count; ++k)
                {
                    if (asset.PathAttachments[k].NameOffset >= sts)
                    {
                        throw new SpineGpuContainerException(
                            "container: 第 " + k + " 条 path 附件的 name_offset " +
                            asset.PathAttachments[k].NameOffset + " 超出字符串块（大小 " + sts + "）");
                    }
                }
            }

            return asset;
        }

        // -------------------------------------------------------------
        // 写
        // -------------------------------------------------------------
        public static byte[] Write(SpineGpuAsset asset)
        {
            SpineGpuLayout.Validate();
            if (asset == null)
            {
                throw new SpineGpuContainerException("container: 资产是 null");
            }

            // ⚠️ 下面 Add 的**顺序就是**落盘顺序，照抄 C++：chunk 9（裁剪多边形）
            // 夹在 5（索引）与 6（绘制项）之间 —— 是历史顺序，不是 id 顺序。
            // 换了顺序文件照样能读，但字节不再逐字节相同 ⇒ 跨语言对拍立刻红。
            var ids = new List<uint>();
            var blobs = new List<byte[]>();
            var aligns = new List<uint>();

            void Add(uint id, byte[] blob, uint align)
            {
                if (blob == null || blob.Length == 0)
                {
                    return;   // 空 chunk 不写（C++ 同）
                }
                ids.Add(id);
                blobs.Add(blob);
                aligns.Add(align);
            }

            const uint A4 = SpineGpuConstants.ChunkAlignment;
            const uint A8 = SpineGpuConstants.VertexSegmentAlignment;

            Add((uint)SpineGpuChunkId.Strings, BuildStrings(asset), A4);
            Add((uint)SpineGpuChunkId.SlotBone, U16Blob(asset.SlotBone), A4);

            // 骨骼 setup 36 B/条。位置紧挨 slot_bone —— 落盘顺序必须与 C++ 一致
            // （C++ 那边是 1,2,10,3,21,22,4,5,9,6,7,8），否则跨语言逐字节对拍会红。
            // pad 恒写 0：整块 new 出来本来就是 0，这里仍然显式留空以说明"它参与字节"。
            if (asset.Bones.Count > 0)
            {
                var b = new byte[asset.Bones.Count * (int)SpineGpuConstants.BoneSetupSize];
                for (int i = 0; i < asset.Bones.Count; ++i)
                {
                    var s = asset.Bones[i];
                    int o = i * (int)SpineGpuConstants.BoneSetupSize;
                    WriteF32(b, o + SpineGpuOffsets.BoneX, s.X);
                    WriteF32(b, o + SpineGpuOffsets.BoneY, s.Y);
                    WriteF32(b, o + SpineGpuOffsets.BoneRotation, s.Rotation);
                    WriteF32(b, o + SpineGpuOffsets.BoneScaleX, s.ScaleX);
                    WriteF32(b, o + SpineGpuOffsets.BoneScaleY, s.ScaleY);
                    WriteF32(b, o + SpineGpuOffsets.BoneShearX, s.ShearX);
                    WriteF32(b, o + SpineGpuOffsets.BoneShearY, s.ShearY);
                    WriteF32(b, o + SpineGpuOffsets.BoneLength, s.Length);
                    WriteU16(b, o + SpineGpuOffsets.BoneParent, (ushort)(short)s.Parent);
                    b[o + SpineGpuOffsets.BoneTransformMode] = (byte)s.TransformMode;
                }
                Add((uint)SpineGpuChunkId.BoneSetup, b, A4);
            }

            // ---- v9：动画（id 11..16，紧跟骨骼 setup 后面）----
            // 五块之间全是**下标**引用，落盘顺序无所谓，但读端要按"先目录后表"的顺序校验。
            // ⚠️ 顺序必须与 C++ 一致（1,2,10,11,12,13,14,15,16,3,21,22,4,5,9,6,7,8），
            //    否则跨语言逐字节对拍会红。
            if (asset.Anims.Count > 0)
            {
                var b = new byte[asset.Anims.Count * (int)SpineGpuConstants.AnimDirSize];
                for (int i = 0; i < asset.Anims.Count; ++i)
                {
                    var a = asset.Anims[i];
                    int o = i * (int)SpineGpuConstants.AnimDirSize;
                    WriteU32(b, o + SpineGpuOffsets.AnimNameOffset, a.NameOffset);
                    WriteU32(b, o + SpineGpuOffsets.AnimTimelineFirst, a.TimelineFirst);
                    WriteU32(b, o + SpineGpuOffsets.AnimTimelineCount, a.TimelineCount);
                    WriteF32(b, o + SpineGpuOffsets.AnimDuration, a.Duration);
                    WriteU32(b, o + SpineGpuOffsets.AnimPad, a.Pad);
                }
                Add((uint)SpineGpuChunkId.AnimDir, b, A4);
            }
            if (asset.Timelines.Count > 0)
            {
                var b = new byte[asset.Timelines.Count * (int)SpineGpuConstants.TimelineEntrySize];
                for (int i = 0; i < asset.Timelines.Count; ++i)
                {
                    var t = asset.Timelines[i];
                    int o = i * (int)SpineGpuConstants.TimelineEntrySize;
                    WriteU16(b, o + SpineGpuOffsets.TlType, t.Type);
                    WriteU16(b, o + SpineGpuOffsets.TlTarget, t.Target);
                    WriteU32(b, o + SpineGpuOffsets.TlFrameCount, t.FrameCount);
                    WriteU32(b, o + SpineGpuOffsets.TlFrameFirst, t.FrameFirst);
                    WriteU32(b, o + SpineGpuOffsets.TlCurveFirst, t.CurveFirst);
                    WriteU32(b, o + SpineGpuOffsets.TlBezierFirst, t.BezierFirst);
                    WriteU32(b, o + SpineGpuOffsets.TlBezierCount, t.BezierCount);
                    WriteU32(b, o + SpineGpuOffsets.TlPayloadOffset, t.PayloadOffset);
                    WriteU32(b, o + SpineGpuOffsets.TlPayloadCount, t.PayloadCount);
                }
                Add((uint)SpineGpuChunkId.Timelines, b, A4);
            }
            Add((uint)SpineGpuChunkId.Frames, F32Blob(asset.Frames), A4);
            if (asset.CurveTypes.Length > 0)
            {
                var b = new byte[asset.CurveTypes.Length];
                Buffer.BlockCopy(asset.CurveTypes, 0, b, 0, asset.CurveTypes.Length);
                Add((uint)SpineGpuChunkId.CurveTypes, b, A4);
            }
            if (asset.Beziers.Count > 0)
            {
                var b = new byte[asset.Beziers.Count * (int)SpineGpuConstants.BezierSize];
                for (int i = 0; i < asset.Beziers.Count; ++i)
                {
                    var z = asset.Beziers[i];
                    int o = i * (int)SpineGpuConstants.BezierSize;
                    WriteF32(b, o + SpineGpuOffsets.BezCx1, z.Cx1);
                    WriteF32(b, o + SpineGpuOffsets.BezCy1, z.Cy1);
                    WriteF32(b, o + SpineGpuOffsets.BezCx2, z.Cx2);
                    WriteF32(b, o + SpineGpuOffsets.BezCy2, z.Cy2);
                }
                Add((uint)SpineGpuChunkId.Beziers, b, A4);
            }
            if (asset.TimelinePayload.Length > 0)
            {
                var b = new byte[asset.TimelinePayload.Length];
                Buffer.BlockCopy(asset.TimelinePayload, 0, b, 0, asset.TimelinePayload.Length);
                Add((uint)SpineGpuChunkId.TimelinePayload, b, A4);
            }

            // ---- v10：槽位 setup（id 17，40 B/槽），紧跟动画块后面 ----
            // 落盘顺序必须与 C++ 一致（1,2,10,11,12,13,14,15,16,17,3,21,22,4,5,9,6,7,8）。
            if (asset.Slots.Count > 0)
            {
                var b = new byte[asset.Slots.Count * (int)SpineGpuConstants.SlotSetupSize];
                for (int i = 0; i < asset.Slots.Count; ++i)
                {
                    var s = asset.Slots[i];
                    int o = i * (int)SpineGpuConstants.SlotSetupSize;
                    WriteF32(b, o + SpineGpuOffsets.SlotColorR, s.ColorR);
                    WriteF32(b, o + SpineGpuOffsets.SlotColorG, s.ColorG);
                    WriteF32(b, o + SpineGpuOffsets.SlotColorB, s.ColorB);
                    WriteF32(b, o + SpineGpuOffsets.SlotColorA, s.ColorA);
                    WriteF32(b, o + SpineGpuOffsets.SlotDarkR, s.DarkR);
                    WriteF32(b, o + SpineGpuOffsets.SlotDarkG, s.DarkG);
                    WriteF32(b, o + SpineGpuOffsets.SlotDarkB, s.DarkB);
                    WriteF32(b, o + SpineGpuOffsets.SlotDarkA, s.DarkA);
                    WriteU32(b, o + SpineGpuOffsets.SlotSetupAttachmentNameOffset, s.SetupAttachmentNameOffset);
                    WriteU32(b, o + SpineGpuOffsets.SlotFlags, (uint)s.Flags);
                }
                Add((uint)SpineGpuChunkId.SlotSetup, b, A4);
            }

            // ---- v11：约束（id 18 / 19 / 20），紧跟槽位 setup 后面 ----
            // 落盘顺序必须与 C++ 一致（1,2,10,11..16,17,18,19,20,3,21,22,4,5,9,6,7,8）。
            bool hasConstraints = asset.Iks.Count > 0 || asset.Transforms.Count > 0 || asset.Paths.Count > 0;
            if (hasConstraints)
            {
                int poolN = asset.ConstraintBonePool.Length;
                int total = (int)SpineGpuConstants.ConstraintHeaderSize
                            + asset.Iks.Count * (int)SpineGpuConstants.IkConstraintSize
                            + asset.Transforms.Count * (int)SpineGpuConstants.TransformConstraintSize
                            + asset.Paths.Count * (int)SpineGpuConstants.PathConstraintSize
                            + poolN * 2;
                var b = new byte[total];
                WriteU32(b, 0, (uint)asset.Iks.Count);
                WriteU32(b, 4, (uint)asset.Transforms.Count);
                WriteU32(b, 8, (uint)asset.Paths.Count);
                WriteU32(b, 12, (uint)poolN);
                int o = (int)SpineGpuConstants.ConstraintHeaderSize;
                for (int i = 0; i < asset.Iks.Count; ++i)
                {
                    var k = asset.Iks[i];
                    WriteU16(b, o + SpineGpuOffsets.IkOrder, k.Order);
                    WriteU16(b, o + SpineGpuOffsets.IkBoneCount, k.BoneCount);
                    WriteU32(b, o + SpineGpuOffsets.IkBoneFirst, k.BoneFirst);
                    WriteU32(b, o + SpineGpuOffsets.IkTarget, k.Target);
                    WriteF32(b, o + SpineGpuOffsets.IkMix, k.Mix);
                    WriteU16(b, o + SpineGpuOffsets.IkBendDir, (ushort)k.BendDir);
                    WriteU16(b, o + SpineGpuOffsets.IkFlags, (ushort)k.Flags);
                    o += (int)SpineGpuConstants.IkConstraintSize;
                }
                for (int i = 0; i < asset.Transforms.Count; ++i)
                {
                    var t = asset.Transforms[i];
                    WriteU16(b, o + SpineGpuOffsets.TfOrder, t.Order);
                    WriteU16(b, o + SpineGpuOffsets.TfBoneCount, t.BoneCount);
                    WriteU32(b, o + SpineGpuOffsets.TfBoneFirst, t.BoneFirst);
                    WriteU32(b, o + SpineGpuOffsets.TfTarget, t.Target);
                    WriteF32(b, o + SpineGpuOffsets.TfOffsetRotation, t.OffsetRotation);
                    WriteF32(b, o + SpineGpuOffsets.TfOffsetX, t.OffsetX);
                    WriteF32(b, o + SpineGpuOffsets.TfOffsetY, t.OffsetY);
                    WriteF32(b, o + SpineGpuOffsets.TfOffsetScaleX, t.OffsetScaleX);
                    WriteF32(b, o + SpineGpuOffsets.TfOffsetScaleY, t.OffsetScaleY);
                    WriteF32(b, o + SpineGpuOffsets.TfOffsetShearY, t.OffsetShearY);
                    WriteF32(b, o + SpineGpuOffsets.TfRotateMix, t.RotateMix);
                    WriteF32(b, o + SpineGpuOffsets.TfTranslateMix, t.TranslateMix);
                    WriteF32(b, o + SpineGpuOffsets.TfScaleMix, t.ScaleMix);
                    WriteF32(b, o + SpineGpuOffsets.TfShearMix, t.ShearMix);
                    WriteU16(b, o + SpineGpuOffsets.TfFlags, (ushort)t.Flags);
                    WriteU16(b, o + SpineGpuOffsets.TfPad, t.Pad);
                    o += (int)SpineGpuConstants.TransformConstraintSize;
                }
                for (int i = 0; i < asset.Paths.Count; ++i)
                {
                    var pc = asset.Paths[i];
                    WriteU16(b, o + SpineGpuOffsets.PathOrder, pc.Order);
                    WriteU16(b, o + SpineGpuOffsets.PathBoneCount, pc.BoneCount);
                    WriteU32(b, o + SpineGpuOffsets.PathBoneFirst, pc.BoneFirst);
                    WriteU32(b, o + SpineGpuOffsets.PathTargetSlot, pc.TargetSlot);
                    b[o + SpineGpuOffsets.PathPositionMode] = (byte)pc.PositionMode;
                    b[o + SpineGpuOffsets.PathSpacingMode] = (byte)pc.SpacingMode;
                    b[o + SpineGpuOffsets.PathRotateMode] = (byte)pc.RotateMode;
                    b[o + SpineGpuOffsets.PathPad] = pc.Pad;
                    WriteF32(b, o + SpineGpuOffsets.PathOffsetRotation, pc.OffsetRotation);
                    WriteF32(b, o + SpineGpuOffsets.PathPosition, pc.Position);
                    WriteF32(b, o + SpineGpuOffsets.PathSpacing, pc.Spacing);
                    WriteF32(b, o + SpineGpuOffsets.PathRotateMix, pc.RotateMix);
                    WriteF32(b, o + SpineGpuOffsets.PathTranslateMix, pc.TranslateMix);
                    WriteU32(b, o + SpineGpuOffsets.PathIndex, pc.PathIndex);
                    o += (int)SpineGpuConstants.PathConstraintSize;
                }
                for (int i = 0; i < poolN; ++i)
                {
                    WriteU16(b, o, asset.ConstraintBonePool[i]);
                    o += 2;
                }
                Add((uint)SpineGpuChunkId.ConstraintSetup, b, A4);
            }

            // ⚠️ 只在**有约束**时落 chunk 19：无约束时 updateCache 的结果恒等于
            //    0..bone_count-1（parent < index ⇒ 正向遍历即拓扑序），写下来纯属冗余。
            //    读端按同一条规则重建 ⇒ 读→写能逐字节相同。
            if (hasConstraints)
            {
                int total = (int)SpineGpuConstants.UpdateOrderHeaderSize
                            + asset.UpdateSteps.Count * (int)SpineGpuConstants.UpdateStepSize
                            + asset.UpdateReset.Length * 2;
                var b = new byte[total];
                WriteU32(b, 0, (uint)asset.UpdateSteps.Count);
                WriteU32(b, 4, (uint)asset.UpdateReset.Length);
                int o = (int)SpineGpuConstants.UpdateOrderHeaderSize;
                for (int i = 0; i < asset.UpdateSteps.Count; ++i)
                {
                    var s = asset.UpdateSteps[i];
                    b[o + SpineGpuOffsets.StepKind] = (byte)s.Kind;
                    b[o + SpineGpuOffsets.StepPad] = s.Pad;
                    WriteU16(b, o + SpineGpuOffsets.StepIndex, s.Index);
                    o += (int)SpineGpuConstants.UpdateStepSize;
                }
                for (int i = 0; i < asset.UpdateReset.Length; ++i)
                {
                    WriteU16(b, o, asset.UpdateReset[i]);
                    o += 2;
                }
                Add((uint)SpineGpuChunkId.UpdateOrder, b, A4);
            }

            if (asset.PathAttachments.Count > 0)
            {
                int total = (int)SpineGpuConstants.PathAttHeaderSize
                            + asset.PathAttachments.Count * (int)SpineGpuConstants.PathAttachmentSize
                            + asset.PathBonePool.Length * 2
                            + asset.PathVertices.Length * 4 + asset.PathLengths.Length * 4;
                var b = new byte[total];
                WriteU32(b, 0, (uint)asset.PathAttachments.Count);
                WriteU32(b, 4, (uint)asset.PathBonePool.Length);
                WriteU32(b, 8, (uint)asset.PathVertices.Length);
                WriteU32(b, 12, (uint)asset.PathLengths.Length);
                int o = (int)SpineGpuConstants.PathAttHeaderSize;
                for (int i = 0; i < asset.PathAttachments.Count; ++i)
                {
                    var a = asset.PathAttachments[i];
                    WriteU16(b, o + SpineGpuOffsets.PathAttSlotIndex, a.SlotIndex);
                    WriteU16(b, o + SpineGpuOffsets.PathAttSkinIndex, a.SkinIndex);
                    WriteU16(b, o + SpineGpuOffsets.PathAttFlags, (ushort)a.Flags);
                    WriteU16(b, o + SpineGpuOffsets.PathAttPad, a.Pad);
                    WriteU32(b, o + SpineGpuOffsets.PathAttVertexCount, a.VertexCount);
                    WriteU32(b, o + SpineGpuOffsets.PathAttBoneOffset, a.BoneOffset);
                    WriteU32(b, o + SpineGpuOffsets.PathAttBoneCount, a.BoneCount);
                    WriteU32(b, o + SpineGpuOffsets.PathAttVertexOffset, a.VertexOffset);
                    WriteU32(b, o + SpineGpuOffsets.PathAttVertexFloatCount, a.VertexFloatCount);
                    WriteU32(b, o + SpineGpuOffsets.PathAttLengthOffset, a.LengthOffset);
                    WriteU32(b, o + SpineGpuOffsets.PathAttLengthCount, a.LengthCount);
                    WriteU32(b, o + SpineGpuOffsets.PathAttNameOffset, a.NameOffset);
                    o += (int)SpineGpuConstants.PathAttachmentSize;
                }
                for (int i = 0; i < asset.PathBonePool.Length; ++i)
                {
                    WriteU16(b, o, asset.PathBonePool[i]);
                    o += 2;
                }
                for (int i = 0; i < asset.PathVertices.Length; ++i)
                {
                    WriteF32(b, o, asset.PathVertices[i]);
                    o += 4;
                }
                for (int i = 0; i < asset.PathLengths.Length; ++i)
                {
                    WriteF32(b, o, asset.PathLengths[i]);
                    o += 4;
                }
                Add((uint)SpineGpuChunkId.PathAttachments, b, A4);
            }

            Add((uint)SpineGpuChunkId.VertexBuffer, asset.VertexBuffer, A8);
            // 影响流：两条**紧跟**顶点块写（落盘顺序必须与 C++ 一致，否则跨语言对拍会红）。
            Add((uint)SpineGpuChunkId.InfluenceBind, F32Blob(asset.InfluenceBind), A8);
            Add((uint)SpineGpuChunkId.InfluenceBone, asset.InfluenceBone, A4);
            Add((uint)SpineGpuChunkId.UvBuffer, F32Blob(asset.UvBuffer), A8);
            // ⚠️ 索引恒定按 uint16 落盘：导入器目前**从不**置 kIndex32，C++ 写端也是
            // 无条件写 u16。读端那边按 flag 两种都认，将来真要 32 位索引时
            // **两端一起改**（改一端 = 字节立刻对不上，对拍会红，不会静默）。
            Add((uint)SpineGpuChunkId.IndexBuffer, U16Blob(asset.IndexBuffer), A4);

            // 裁剪多边形 12 B/条。未用字节（clip.pad）必须写 0 —— 落盘是靠
            // "先整块清零再逐字段填"，不是靠 struct 的内存布局。
            if (asset.ClipPolygons.Count > 0)
            {
                var b = new byte[asset.ClipPolygons.Count * (int)SpineGpuConstants.ClipPolygonSize];
                for (int i = 0; i < asset.ClipPolygons.Count; ++i)
                {
                    var p = asset.ClipPolygons[i];
                    int o = i * (int)SpineGpuConstants.ClipPolygonSize;
                    WriteU16(b, o + SpineGpuOffsets.ClipSlotIndex, p.SlotIndex);
                    WriteU16(b, o + SpineGpuOffsets.ClipSkinId, p.SkinId);
                    b[o + SpineGpuOffsets.ClipVertexFormat] = (byte)p.VertexFormat;
                    WriteU16(b, o + SpineGpuOffsets.ClipVertexCount, p.VertexCount);
                    WriteU32(b, o + (int)SpineGpuOffsets.ClipVertexOffset, p.VertexOffset);
                }
                Add((uint)SpineGpuChunkId.ClipData, b, A4);
            }

            if (asset.DrawItems.Count > 0)
            {
                var b = new byte[asset.DrawItems.Count * (int)SpineGpuConstants.DrawItemSize];
                for (int i = 0; i < asset.DrawItems.Count; ++i)
                {
                    var d = asset.DrawItems[i];
                    int o = i * (int)SpineGpuConstants.DrawItemSize;
                    WriteU32(b, o + SpineGpuOffsets.ItemIndexOffset, d.IndexOffset);
                    WriteU32(b, o + SpineGpuOffsets.ItemIndexCount, d.IndexCount);
                    WriteU32(b, o + SpineGpuOffsets.ItemUvOffset, d.UvOffset);
                    b[o + SpineGpuOffsets.ItemVertexFormat] = (byte)d.VertexFormat;
                    b[o + SpineGpuOffsets.ItemBlendMode] = (byte)d.BlendMode;
                    WriteU16(b, o + SpineGpuOffsets.ItemAtlasPage, d.AtlasPage);
                    WriteU16(b, o + SpineGpuOffsets.ItemSlotIndex, d.SlotIndex);
                    WriteU16(b, o + SpineGpuOffsets.ItemAttachmentId, d.AttachmentId);
                    WriteU16(b, o + SpineGpuOffsets.ItemSkinId, d.SkinId);
                    b[o + SpineGpuOffsets.ItemKind] = (byte)d.Kind;
                    WriteU16(b, o + SpineGpuOffsets.ItemClipIndex, d.ClipIndex);
                }
                Add((uint)SpineGpuChunkId.DrawItems, b, A4);
            }

            if (asset.AttachmentRecords.Count > 0)
            {
                var b = new byte[asset.AttachmentRecords.Count * (int)SpineGpuConstants.AttachmentRecordSize];
                for (int i = 0; i < asset.AttachmentRecords.Count; ++i)
                {
                    var r = asset.AttachmentRecords[i];
                    int o = i * (int)SpineGpuConstants.AttachmentRecordSize;
                    WriteU16(b, o + SpineGpuOffsets.AttSlotIndex, r.SlotIndex);
                    WriteU16(b, o + SpineGpuOffsets.AttAttachmentId, r.AttachmentId);
                    WriteU16(b, o + SpineGpuOffsets.AttSkinId, r.SkinId);
                }
                Add((uint)SpineGpuChunkId.Attachments, b, A4);
            }

            Add((uint)SpineGpuChunkId.AtlasPages, CStringsBlob(asset.AtlasPageNames), A4);

            int tableBytes = ids.Count * (int)SpineGpuConstants.ChunkTableEntrySize;
            int cursor = (int)SpineGpuConstants.HeaderSize + tableBytes;
            var offsets = new int[ids.Count];
            for (int i = 0; i < ids.Count; ++i)
            {
                cursor = (int)AlignUp((uint)cursor, aligns[i]);
                offsets[i] = cursor;
                cursor += blobs[i].Length;
            }

            // 整块先清零：对齐空档与未用字段都是 0，不靠"未初始化填充"碰运气。
            var outBuf = new byte[cursor];

            var h = asset.Header;
            WriteU32(outBuf, SpineGpuOffsets.HdrMagic, SpineGpuConstants.Magic);
            WriteU32(outBuf, SpineGpuOffsets.HdrVersion, SpineGpuConstants.FormatVersion);
            WriteU32(outBuf, SpineGpuOffsets.HdrSpineRuntime, h.SpineRuntime);
            WriteU32(outBuf, SpineGpuOffsets.HdrFlags, (uint)h.Flags);
            WriteU32(outBuf, SpineGpuOffsets.HdrBoneCount, h.BoneCount);
            WriteU32(outBuf, SpineGpuOffsets.HdrSlotCount, h.SlotCount);
            WriteU32(outBuf, SpineGpuOffsets.HdrDrawItemCount, h.DrawItemCount);
            WriteU32(outBuf, SpineGpuOffsets.HdrVertexCount, h.VertexCount);
            WriteU32(outBuf, SpineGpuOffsets.HdrIndexCount, h.IndexCount);
            WriteU32(outBuf, SpineGpuOffsets.HdrAtlasPageCount, h.AtlasPageCount);
            WriteU32(outBuf, SpineGpuOffsets.HdrBoneStride, h.BoneStride);
            WriteU32(outBuf, SpineGpuOffsets.HdrChunkCount, (uint)ids.Count);
            WriteU32(outBuf, SpineGpuOffsets.HdrRigidVertexCount, asset.RigidVertexCount);
            WriteU32(outBuf, SpineGpuOffsets.HdrWeightedSegmentOffset, asset.WeightedSegmentOffset);
            WriteU32(outBuf, SpineGpuOffsets.HdrWeightedVertexCount, asset.WeightedVertexCount);
            WriteU32(outBuf, SpineGpuOffsets.HdrSkinCount, (uint)asset.SkinNames.Count);

            for (int i = 0; i < ids.Count; ++i)
            {
                int e = (int)SpineGpuConstants.HeaderSize + i * (int)SpineGpuConstants.ChunkTableEntrySize;
                WriteU32(outBuf, e + SpineGpuOffsets.ChunkId, ids[i]);
                WriteU32(outBuf, e + SpineGpuOffsets.ChunkOffset, (uint)offsets[i]);
                WriteU32(outBuf, e + SpineGpuOffsets.ChunkSize, (uint)blobs[i].Length);
                WriteU32(outBuf, e + SpineGpuOffsets.ChunkPad, 0u);
                Buffer.BlockCopy(blobs[i], 0, outBuf, offsets[i], blobs[i].Length);
            }

            return outBuf;
        }

        // -------------------------------------------------------------
        // 内部
        // -------------------------------------------------------------
        private sealed class ChunkSpan
        {
            public uint Offset;
            public uint Size;
        }

        private static bool TryGet(Dictionary<uint, ChunkSpan> byId, uint id, out uint off, out uint size)
        {
            ChunkSpan s;
            if (byId.TryGetValue(id, out s))
            {
                off = s.Offset;
                size = s.Size;
                return true;
            }
            off = 0;
            size = 0;
            return false;
        }

        private static uint ReadU32(byte[] b, int at) => BinaryPrimitives.ReadUInt32LittleEndian(new ReadOnlySpan<byte>(b, at, 4));
        private static ushort ReadU16(byte[] b, int at) => BinaryPrimitives.ReadUInt16LittleEndian(new ReadOnlySpan<byte>(b, at, 2));
        // IEEE754 binary32 小端，与 C++ 的 memcpy 完全一致。
        // 走 int32 的位模式而不是 BitConverter.ToSingle(...) —— 后者按**本机**字节序，
        // 在大端机上会静默反着读。（netstandard2.1 没有 BinaryPrimitives.ReadSingle*，
        // 所以先读 int32 再 Int32BitsToSingle —— 位模式不变，纯 reinterpret。）
        private static byte ReadU8(byte[] b, int at) => b[at];
        private static float ReadF32(byte[] b, int at) =>
            BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(new ReadOnlySpan<byte>(b, at, 4)));

        private static void WriteF32(byte[] b, int at, float v) =>
            BinaryPrimitives.WriteInt32LittleEndian(new Span<byte>(b, at, 4), BitConverter.SingleToInt32Bits(v));

        private static void WriteU32(byte[] b, int at, uint v) => BinaryPrimitives.WriteUInt32LittleEndian(new Span<byte>(b, at, 4), v);
        private static void WriteU16(byte[] b, int at, ushort v) => BinaryPrimitives.WriteUInt16LittleEndian(new Span<byte>(b, at, 2), v);

        private static uint AlignUp(uint x, uint a) => (x + a - 1u) / a * a;

        private static byte[] U16Blob(ushort[] src)
        {
            if (src == null || src.Length == 0) return System.Array.Empty<byte>();
            var b = new byte[src.Length * 2];
            for (int i = 0; i < src.Length; ++i) WriteU16(b, i * 2, src[i]);
            return b;
        }

        private static byte[] F32Blob(float[] src)
        {
            if (src == null || src.Length == 0) return System.Array.Empty<byte>();
            var b = new byte[src.Length * 4];
            for (int i = 0; i < src.Length; ++i) WriteF32(b, i * 4, src[i]);
            return b;
        }

        private static byte[] CStringsBlob(List<string> src)
        {
            var ms = new System.IO.MemoryStream();
            foreach (var s in src)
            {
                var bytes = Encoding.UTF8.GetBytes(s ?? string.Empty);
                ms.Write(bytes, 0, bytes.Length);
                ms.WriteByte(0);
            }
            return ms.ToArray();
        }

        private static byte[] BuildStrings(SpineGpuAsset a)
        {
            var ms = new System.IO.MemoryStream();
            void Put(List<string> src)
            {
                foreach (var s in src)
                {
                    var bytes = Encoding.UTF8.GetBytes(s ?? string.Empty);
                    ms.Write(bytes, 0, bytes.Length);
                    ms.WriteByte(0);
                }
            }
            Put(a.BoneNames);
            Put(a.SkinNames);
            Put(a.SlotNames);
            // v10：槽位 setup 附件名，紧跟槽位名（条数 = slot_count，空名也占一条）。
            Put(a.SlotSetupAttachmentNames);
            Put(a.AttachmentNames);
            Put(a.AtlasPageNames);
            // v9：动画名 → 动画附件名，**排在最后** —— 前五组的数量各自能从
            // header / 附件记录数反推，这两组里只有"动画名"有计数（= 动画数），
            // 动画附件名只按偏移访问。顺序一变，name_offset 全部错位且不报错。
            Put(a.AnimNames);
            // v11：path 附件名必须排在"动画附件名"**前面** —— 后者是"剩下的全归它"，
            // 没有计数 ⇒ 排在它后面的组读端切不出来。
            Put(a.PathAttachmentNames);
            Put(a.AnimAttachmentNames);
            return ms.ToArray();
        }

        private static List<string> SplitCStrings(byte[] data, int off, int size)
        {
            var all = new List<string>();
            int p = off, end = off + size;
            while (p < end)
            {
                int z = p;
                while (z < end && data[z] != 0) ++z;
                all.Add(Encoding.UTF8.GetString(data, p, z - p));
                p = z + 1;
            }
            return all;
        }

        private static void Take(List<string> all, uint count, List<string> dst, ref int i)
        {
            for (uint k = 0; k < count && i < all.Count; ++k) dst.Add(all[i++]);
        }
    }
}
