// 容器的读与写。与 C++ `ReadSpineGpuContainer` / `WriteSpineGpuContainer`
// （SpineGpuImporter.cpp）逐字节对应 —— 这是整个规格的落点：
// **同一个 .zspg，C++ 写的 C# 能读、C# 写的 C++ 能读，且再写一遍字节不变**。
//
// 三条容易漏、漏了就静默错的规矩：
//   1. **chunk 按下标找会错位**（v2 → v3 重排过号）⇒ 只能按 id 找。
//   2. chunk 在文件里的**顺序不是 id 顺序**：写端按 1,2,3,21,22,4,5,9,6,7,8 追加
//      （影响流紧跟顶点块；9 夹在 5 与 6 之间，是历史顺序），
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

            // ---- 字符串：骨骼名 → 皮肤名 → 槽位名 → 附件名 → 图集页名 ----
            if (TryGet(byId, (uint)SpineGpuChunkId.Strings, out uint sto, out uint sts))
            {
                var all = SplitCStrings(data, (int)sto, (int)sts);
                int i = 0;
                Take(all, h.BoneCount, asset.BoneNames, ref i);
                Take(all, h.SkinCount, asset.SkinNames, ref i);
                Take(all, h.SlotCount, asset.SlotNames, ref i);
                Take(all, (uint)attachmentCount, asset.AttachmentNames, ref i);
                Take(all, h.AtlasPageCount, asset.AtlasPageNames, ref i);
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
            Put(a.AttachmentNames);
            Put(a.AtlasPageNames);
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
