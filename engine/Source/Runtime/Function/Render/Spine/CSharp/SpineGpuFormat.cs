// =====================================================================
// GPU-Spine 中间格式 —— C# 侧的布局定义
// 规格：doc/GPU_SPINE_INTERCHANGE_FORMAT.md
// C++ 权威定义：SpineGpuFormat.h（本文件必须与它**逐字段一致**）
// ---------------------------------------------------------------------
// ⚠️ 这个文件是**照抄**，不是"参考"。
//    C++ 那边的每个偏移都有 static_assert 钉着，因为布局错一个字节不报错、
//    只是形状静默错。C# 没有编译期 offsetof，所以这里改用三道关：
//      1. 所有偏移写成**命名常量**（不是靠 struct 的内存布局隐式得出），
//         读写两端共用同一组常量 ⇒ 读与写不可能各自漂；
//      2. `SpineGpuLayout.Validate()` 在启动时把"常量之间是否自洽"跑一遍
//         （等价于 C++ 的 static_assert 那一层）；
//      3. 真正的跨语言钉子在对拍测试里：C++ 写的字节，C# 读回来再写一遍
//         必须逐字节相同（Test/Program.cs）。版本 / 记录大小一漂，
//         读端会在 `size % 记录大小 != 0` 上炸。
// =====================================================================

using System;

namespace ZEngine.SpineGpu
{
    public static class SpineGpuConstants
    {
        public const uint Magic = 0x5A535047u;   // 'ZSPG'

        // 版本史见 SpineGpuFormat.h::kFormatVersion。
        // v6 = 裁剪多边形进容器（绘制项 24 B → 28 B，chunk 9 起真正写数据）。
        // v7 = 蒙皮数据搬出顶点（weighted 顶点 48 B → 8 B 引用；新增 chunk 21 / 22）。
        // v8 = 骨骼树进容器（新增 chunk 10，36 B/骨）。header / 绘制项 / 顶点布局都没动，
        //      但没有它动画就无从解算（时间轴是相对 setup pose 的增量）。
        public const uint FormatVersion = 8;

        public const uint DefaultSkinId = 0;
        public const uint SpineRuntime38 = 30800;
        // 3.7.xx -> 30700。3.7 与 3.8 的 .skel 字节布局一致，只有槽位暗色判据不同
        // ⇒ 读端必须知道是哪一个（记错不会报错，只是少数槽位颜色不对）。
        public const uint SpineRuntime37 = 30700;
        // v7 起**不再是固定槽位数**，只是"单个顶点影响数的硬上限"（挡坏数据）。
        // 实测每顶点最大 15、均值 2.20 ⇒ 影响数现在是逐顶点变长的。
        public const int MaxInfluences = 255;

        public const uint BoneStrideCompact = 24;
        public const uint BoneStrideStd140 = 32;
        public const uint RootStrideCompact = 24;
        public const uint RootStrideStd140 = 32;

        public const uint DrawInstanceSize = 20;   // 每帧实例项
        public const uint DrawItemSize = 28;       // 静态绘制项（v6）
        public const uint ClipPolygonSize = 12;    // 裁剪多边形记录（v6）
        public const uint AttachmentRecordSize = 8;
        public const uint BoneSetupSize = 36;      // 骨骼 setup 记录（v8，chunk 10）

        // 骨骼数上限：parent 是 int16（要能表示 -1），bone_idx 是 uint16 ⇒ 取严的。
        public const uint MaxBones = 32767;

        // clip_index 的"没有"值。用 0xFFFF 而不是 0 —— 0 是一条**真实存在的**多边形，
        // 用它会让运行时拿第一条多边形去裁根本不该裁的项，且不报错。
        public const ushort NoClipIndex = 0xFFFF;

        public const uint RigidVertexStride = 8;
        // v7 起 weighted 顶点**只是引用**：(influence_offset u32, influence_count u32)。
        // 蒙皮数据在 InfluenceBindStride / InfluenceBoneStride 两条流里。
        public const uint WeightedVertexStride = 8;

        // 影响流两条。**分开**而不是交错成一条 12 B：Load2 要 8 B 对齐，
        // 12 B 步长会让第 1、3、5…条的 bindPos 落在 12、36…（不是 8 的倍数）。
        public const uint InfluenceBindStride = 8;   // float2 bindPos
        public const uint InfluenceBoneStride = 4;   // u16 boneIdx + u8 weight + u8 pad

        public const uint UvStride = 8;

        public const uint HeaderSize = 64;
        public const uint ChunkTableEntrySize = 16;
        public const uint VertexSegmentAlignment = 8;
        public const uint ChunkAlignment = 4;
    }

    public enum SpineVertexFormat : byte
    {
        Rigid = 0,
        // v7 起名字从 Weighted4 改成 Weighted：影响数**不再**是定长 4（枚举值仍是 1）。
        Weighted = 1,
    }

    // 与 spine-cpp BlendMode.h:35-38 逐值对应。
    public enum SpineBlendMode : byte
    {
        Normal = 0,
        Additive = 1,
        Multiply = 2,
        Screen = 3,
    }

    // 逐值抄 spine-cpp `TransformMode.h:34-40`（C++ 侧同名枚举）。
    // 语义在 `Bone.cpp:126-185` 的 `updateWorldTransform()` switch：
    // 决定父骨骼的世界变换里哪些分量传给子骨骼。5 个值都是实测会出现的。
    public enum SpineTransformMode : byte
    {
        Normal = 0,
        OnlyTranslation = 1,
        NoRotationOrReflection = 2,
        NoScale = 3,
        NoScaleOrReflection = 4,
    }

    public enum SpineDrawItemKind : byte
    {
        Static = 0,     // 可进 GPU 蒙皮路径
        Fallback = 1,   // CPU 路径（裁剪 / FFD 正在动 / 绘制顺序动画等）
    }

    [Flags]
    public enum SpineGpuFlags : uint
    {
        None = 0u,
        Index32 = 1u << 0,
        HasDrawOrderTimeline = 1u << 1,
        UsesTintBlack = 1u << 2,
        HasClipping = 1u << 3,
        // ⚠️ 只由 deform（FFD）点亮，**不由裁剪点亮** —— 语义不同：
        // kHasDeform = 顶点**逐帧**变 ⇒ 动态 VB 每帧重算；
        // 裁剪的动态 VB 只在多边形动的时候才需要重算。合成一个位，
        // 运行时会为纯裁剪的资产每帧重算一份根本没变的顶点。
        HasDeform = 1u << 4,
    }

    // ⚠️ v3 重新编过号（UV 缓冲插进来，后面整体 +1）。
    // 读端必须按 **id** 找 chunk，不许按下标 —— v2 的容器用 v3 读端读
    // 会拿到 index_buffer 当 UV，静默错位。
    public enum SpineGpuChunkId : uint
    {
        Strings = 1,
        SlotBone = 2,
        VertexBuffer = 3,
        UvBuffer = 4,
        IndexBuffer = 5,
        DrawItems = 6,
        Attachments = 7,
        AtlasPages = 8,
        ClipData = 9,
        BoneSetup = 10,     // v8：骨骼 setup（36 B/骨），下标 = header.bone_count 的序

        // v7：蒙皮数据流。id 从 21 起 —— 10..20 留给 v7 动画阶段
        // （骨骼树 / 时间轴 / path 几何），别占用。
        InfluenceBind = 21,   // float2 × 影响条目数
        InfluenceBone = 22,   // (boneIdx u16, weight u8, pad u8) × 影响条目数
    }

    // -----------------------------------------------------------------
    // 字段偏移。C++ 侧是 static_assert(offsetof(...))，这里是命名常量。
    // 读写两端都用它们 ⇒ 不可能出现"读按 28 B、写按 24 B"这种半边漂。
    // -----------------------------------------------------------------
    public static class SpineGpuOffsets
    {
        // header
        public const int HdrMagic = 0x00;
        public const int HdrVersion = 0x04;
        public const int HdrSpineRuntime = 0x08;
        public const int HdrFlags = 0x0C;
        public const int HdrBoneCount = 0x10;
        public const int HdrSlotCount = 0x14;
        public const int HdrDrawItemCount = 0x18;
        public const int HdrVertexCount = 0x1C;
        public const int HdrIndexCount = 0x20;
        public const int HdrAtlasPageCount = 0x24;
        public const int HdrBoneStride = 0x28;
        public const int HdrChunkCount = 0x2C;
        public const int HdrRigidVertexCount = 0x30;
        public const int HdrWeightedSegmentOffset = 0x34;
        public const int HdrWeightedVertexCount = 0x38;
        public const int HdrSkinCount = 0x3C;

        // chunk table entry
        public const int ChunkId = 0x00;
        public const int ChunkOffset = 0x04;
        public const int ChunkSize = 0x08;
        public const int ChunkPad = 0x0C;

        // attachment record
        public const int AttSlotIndex = 0;
        public const int AttAttachmentId = 2;
        public const int AttSkinId = 4;
        public const int AttPad = 6;

        // draw item
        public const int ItemIndexOffset = 0;
        public const int ItemIndexCount = 4;
        public const int ItemUvOffset = 8;
        public const int ItemVertexFormat = 12;
        public const int ItemBlendMode = 13;
        public const int ItemAtlasPage = 14;
        public const int ItemSlotIndex = 16;
        public const int ItemAttachmentId = 18;
        public const int ItemSkinId = 20;
        public const int ItemKind = 22;
        public const int ItemPad = 23;
        public const int ItemClipIndex = 24;   // v6
        public const int ItemPad2 = 26;        // v6

        // clip polygon
        public const int ClipSlotIndex = 0;
        public const int ClipSkinId = 2;
        public const int ClipVertexFormat = 4;
        public const int ClipPad = 5;
        public const int ClipVertexCount = 6;
        public const int ClipVertexOffset = 8;

        // weighted vertex（v7 起只是引用）
        public const int WvInfluenceOffset = 0;   // u32：影响流的**条目**下标
        public const int WvInfluenceCount = 4;    // u32：这个顶点有几个影响

        // 影响流一条骨骼记录（chunk 22，4 B）
        public const int IbBoneIdx = 0;    // u16
        public const int IbWeight = 2;     // u8，unorm8
        public const int IbPad = 3;        // u8，落盘必须写 0

        // 骨骼 setup 一条（chunk 10，36 B）
        public const int BoneX = 0;
        public const int BoneY = 4;
        public const int BoneRotation = 8;    // 度，不是弧度
        public const int BoneScaleX = 12;     // 默认 **1** 不是 0
        public const int BoneScaleY = 16;
        public const int BoneShearX = 20;
        public const int BoneShearY = 24;
        public const int BoneLength = 28;
        public const int BoneParent = 32;          // int16，-1 = 根，否则必须 < 自己的下标
        public const int BoneTransformMode = 34;   // SpineTransformMode
        public const int BonePad = 35;             // 落盘必须写 0
    }

    // C++ 用 static_assert 挡的那层"常量之间是否自洽"。C# 没有编译期 offsetof，
    // 所以放在启动路径上跑 —— 成本几微秒，换的是"改了一个常量忘了另一个"立刻炸。
    public static class SpineGpuLayout
    {
        public static void Validate()
        {
            Check(SpineGpuConstants.HeaderSize == 64, "header is 64 B");
            Check(SpineGpuConstants.ChunkTableEntrySize == 16, "chunk entry is 16 B");
            Check(SpineGpuConstants.DrawItemSize == 28, "draw item is 28 B (v6: +clip_index)");
            Check(SpineGpuConstants.ClipPolygonSize == 12, "clip polygon is 12 B");
            Check(SpineGpuConstants.AttachmentRecordSize == 8, "attachment record is 8 B");
            Check(SpineGpuConstants.RigidVertexStride == 8, "rigid vertex is 8 B");
            Check(SpineGpuConstants.WeightedVertexStride == 8, "weighted vertex is 8 B (v7: reference only)");
            Check(SpineGpuConstants.InfluenceBindStride == 8, "influence bind entry is 8 B");
            Check(SpineGpuConstants.InfluenceBoneStride == 4, "influence bone entry is 4 B");
            Check(SpineGpuConstants.UvStride == 8, "uv entry is 8 B");
            Check(SpineGpuConstants.WeightedVertexStride % 8 == 0,
                  "weighted stride must be 8-byte aligned (Load2 on bindPos)");
            Check(SpineGpuConstants.InfluenceBindStride % 8 == 0,
                  "influence bind stride must be 8-byte aligned (Load2 on bindPos)");
            Check(SpineGpuConstants.RigidVertexStride % 8 == 0,
                  "rigid stride must keep bindPos 8-byte aligned");
            Check(SpineGpuConstants.MaxInfluences == 255, "MAX_INFLUENCES is 255 (sanity bound only)");

            // 偏移必须单调且落在记录内 —— 这是 offsetof 断言能在运行时表达的那一半：
            // "字段没跑出结构体"。
            CheckOffset(SpineGpuOffsets.ItemClipIndex, 2, SpineGpuConstants.DrawItemSize, "draw item clip_index");
            CheckOffset(SpineGpuOffsets.ItemPad2, 2, SpineGpuConstants.DrawItemSize, "draw item pad2");
            CheckOffset(SpineGpuOffsets.ItemSkinId, 2, SpineGpuConstants.DrawItemSize, "draw item skin_id");
            CheckOffset(SpineGpuOffsets.ItemKind, 1, SpineGpuConstants.DrawItemSize, "draw item kind");
            CheckOffset(SpineGpuOffsets.ClipVertexOffset, 4, SpineGpuConstants.ClipPolygonSize, "clip vertex_offset");
            CheckOffset(SpineGpuOffsets.AttSkinId, 2, SpineGpuConstants.AttachmentRecordSize, "attachment skin_id");
            CheckOffset(SpineGpuOffsets.WvInfluenceCount, 4, SpineGpuConstants.WeightedVertexStride,
                        "weighted influence_count");
            CheckOffset(SpineGpuOffsets.IbWeight, 1, SpineGpuConstants.InfluenceBoneStride, "influence bone weight");
            CheckOffset(SpineGpuOffsets.HdrSkinCount, 4, SpineGpuConstants.HeaderSize, "header skin_count");
            CheckOffset(SpineGpuOffsets.BoneParent, 2, SpineGpuConstants.BoneSetupSize, "bone parent");
            CheckOffset(SpineGpuOffsets.BoneTransformMode, 1, SpineGpuConstants.BoneSetupSize, "bone transform_mode");
            CheckOffset(SpineGpuOffsets.BonePad, 1, SpineGpuConstants.BoneSetupSize, "bone pad");

            Check(SpineGpuConstants.BoneSetupSize == 36, "bone setup record is 36 B (v8)");
            Check(SpineGpuConstants.MaxBones == 32767, "MaxBones is 32767 (int16 parent)");

            Check(SpineGpuConstants.NoClipIndex == 0xFFFF, "NoClipIndex must be 0xFFFF, not 0");
        }

        private static void Check(bool cond, string what)
        {
            if (!cond)
            {
                throw new SpineGpuContainerException("GPU-Spine 布局自洽检查失败：" + what);
            }
        }

        private static void CheckOffset(int offset, int width, uint recordSize, string what)
        {
            if (offset < 0 || offset + width > recordSize)
            {
                throw new SpineGpuContainerException(
                    "GPU-Spine 布局自洽检查失败：" + what + " @" + offset + " 跑出了 " + recordSize + " B 的记录");
            }
        }
    }

    public sealed class SpineGpuHeader
    {
        public uint Magic;
        public uint Version;
        public uint SpineRuntime;
        public SpineGpuFlags Flags;
        public uint BoneCount;
        public uint SlotCount;
        public uint DrawItemCount;
        public uint VertexCount;
        public uint IndexCount;
        public uint AtlasPageCount;
        public uint BoneStride;
        public uint ChunkCount;
        public uint RigidVertexCount;         // 0x30
        public uint WeightedSegmentOffset;    // 0x34，8 B 对齐
        public uint WeightedVertexCount;      // 0x38
        public uint SkinCount;                // 0x3C
    }

    public sealed class SpineGpuAttachmentRecord
    {
        public ushort SlotIndex;
        public ushort AttachmentId;
        public ushort SkinId;
    }

    public sealed class SpineDrawItem
    {
        public uint IndexOffset;
        public uint IndexCount;
        // ⚠️ UV **条目**的下标，不是字节偏移（chunk 4 里一条 8 B）。
        public uint UvOffset;
        public SpineVertexFormat VertexFormat;
        public SpineBlendMode BlendMode;
        public ushort AtlasPage;
        public ushort SlotIndex;
        public ushort AttachmentId;
        public ushort SkinId;
        public SpineDrawItemKind Kind;
        // chunk 9 的行号；**没被裁时是 NoClipIndex**。
        public ushort ClipIndex;
    }

    public sealed class SpineGpuClipPolygon
    {
        public ushort SlotIndex;
        public ushort SkinId;
        public SpineVertexFormat VertexFormat;
        public ushort VertexCount;
        // 进入**所在顶点段**的顶点下标，不是字节偏移；段由 VertexFormat 决定。
        public uint VertexOffset;
    }

    // 骨骼 setup（chunk 10）。逐字段抄 spine-cpp `BoneData.h:110-115`。
    // ⚠️ 顺序不变量 **Parent < 自己的下标**：有它之后正向 for 一遍就是拓扑序，
    //    `updateWorldTransform` 不需要排序 / 递归 / 栈。
    public sealed class SpineGpuBone
    {
        public float X;
        public float Y;
        public float Rotation;      // 度
        public float ScaleX = 1.0f; // 默认 **1**（SkeletonJson.cpp:169-170）
        public float ScaleY = 1.0f;
        public float ShearX;
        public float ShearY;
        public float Length;        // IK / 编辑器挂点；不参与蒙皮
        public short Parent = -1;   // -1 = 根
        public SpineTransformMode TransformMode = SpineTransformMode.Normal;
    }

    public class SpineGpuContainerException : Exception
    {
        public SpineGpuContainerException(string message) : base(message) { }
    }
}
