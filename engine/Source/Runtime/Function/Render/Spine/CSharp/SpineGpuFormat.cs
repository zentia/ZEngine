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
        // v9 = 动画进容器（新增 chunk 11-16）。header 与既有 chunk **全都没动**
        //      （动画数量由 chunk 11 的大小反推：定长块的字节数本身就是计数），
        //      flags 多一位 HasEventTimeline。
        //      ⇒ 不兼容：v8 容器被新读端读会得到 **0 个动画**（chunk 11 不存在），
        //        而"没有动画"本身不报错 —— 与 chunk 10 是同一类坑。
        // v10 = 槽位 setup 进容器（新增 chunk 17，40 B/槽）。之前容器里只有一个
        //       UsesTintBlack 布尔、**没有颜色值**；而 color / twoColor 时间轴是
        //       相对 setup 颜色混合的（时间轴写绝对颜色，"没被命中"的槽位停在 setup 色）
        //       ⇒ 没有 setup 色就无从解算。这是一个既有缺口，不是新增需求。
        //       ⇒ 不兼容：v9 容器被新读端读会得到"所有槽位纯白 + 无暗色"，
        //         而"纯白"本身不报错 —— 与 chunk 10 / 11 是同一类坑。
        // v11 = 约束进容器（新增 chunk 18 约束 setup / 19 执行顺序 / 20 path 几何）。
        //       约束**每帧都跑**，与时间轴无关（实测 transform 48.3% / IK 22.5% /
        //       path 11.1% 的资产带约束），容器里没有它们就无从解算。
        //       ⇒ 不兼容：v10 容器被新读端读会得到「0 条约束」，而"没有约束"本身
        //         不报错 —— 与 chunk 10 / 11 / 17 是同一类坑。
        public const uint FormatVersion = 11;

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
        public const uint AnimDirSize = 20;        // 动画目录项（v9，chunk 11）
        public const uint TimelineEntrySize = 32;  // 时间轴表项（v9，chunk 12）
        // 一条 bezier 段的控制点（v9，chunk 15）。
        // ⚠️ 存**控制点**不存参考实现那 18 个 float 的展开采样表
        //    （`CurveTimeline::BEZIER_VALUES = 10*2-2`）：实测 2 567 358 段里 bezier 占
        //    36.6%，定长 20 B/段要 49.0 MB，紧凑写法 16.8 MB（差 65%）。
        public const uint BezierSize = 16;

        // 槽位 setup 记录（v10，chunk 17）：
        //   color rgba f32×4 @0 + dark rgba f32×4 @16 +
        //   setup_attachment_name_offset u32 @32 + flags u32 @36。
        // 32 B 不够：还要一格放 setup 附件名偏移（attachment 时间轴的复位基线，
        // `AttachmentTimeline.cpp:65-68`）和一格放逐槽位 flags（has_dark ——
        // 它**不能**由 dark 是否全 0 推断：3.7 的判据是 `读到的 int32 != -1`，
        // 0x00000000（纯黑）照样是"有暗色"）。40 = 5 × 8 ⇒ 仍是 8 B 对齐。
        //
        // ⚠️ dark 的 alpha **恒为 1**（只要这个槽位有暗色）—— 参考实现写死的：
        //   二进制 `SkeletonBinary.cpp:188` `set(r/255, g/255, b/255, 1)`；
        //   json    `SkeletonJson.cpp:230`  `darkColor.a = 1`。
        //   二进制里读进来的第 4 个字节只用来判"有没有暗色"，**从不作为 alpha 落地**。
        public const uint SlotSetupSize = 40;

        // attachment 时间轴里"这一帧没有附件"的哨兵。**不是 0** —— 0 是 chunk 1 里
        // 第一个字符串的合法偏移，填 0 会让空帧变成"挂上某个真实附件"且不报错。
        public const uint NoAttachmentName = 0xFFFFFFFFu;

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

        // ---- v11 约束 ----
        public const uint IkConstraintSize = 20;
        public const uint TransformConstraintSize = 56;
        public const uint PathConstraintSize = 40;
        public const uint UpdateStepSize = 4;
        public const uint PathAttachmentSize = 40;
        // 三个块头。**必需**：没有它们，"chunk 不存在"和"chunk 被截断"在字节上
        // 分不开（v11 之前 chunk 10 / 11 / 17 都栽过同一个坑）。
        public const uint ConstraintHeaderSize = 16;   // ik_count, tf_count, path_count, bone_pool_count
        public const uint UpdateOrderHeaderSize = 8;   // update_count, reset_count
        public const uint PathAttHeaderSize = 16;      // path_count, bone_count, vertex_float_count, length_count

        // "这个槽位上没有 path 附件"。**不是 0** —— 0 是 chunk 20 里第一条
        // 合法记录，填 0 会让约束沿一条别人的曲线摆骨骼，且不报错。
        public const uint NoPathAttachment = 0xFFFFFFFFu;
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

    // 时间轴类型：逐值抄 spine-cpp `TimelineType.h:35-49`。
    // ⚠️ **不自造枚举值**：换一套值会让"参考实现的 switch 分支"对不上号，
    //    而这只是静默地解错，不会报错。
    public enum SpineTimelineType : ushort
    {
        Rotate = 0,
        Translate = 1,
        Scale = 2,
        Shear = 3,
        Attachment = 4,
        Color = 5,
        Deform = 6,
        Event = 7,
        DrawOrder = 8,
        IkConstraint = 9,
        TransformConstraint = 10,
        PathConstraintPosition = 11,
        PathConstraintSpacing = 12,
        PathConstraintMix = 13,
        TwoColor = 14,
    }

    // 曲线段类型：逐值抄 `CurveTimeline.cpp:42-44`（LINEAR=0 / STEPPED=1 / BEZIER=2）。
    public enum SpineCurveType : byte
    {
        Linear = 0,
        Stepped = 1,
        Bezier = 2,
    }

    // 每类时间轴的"每帧几个 float"与"要不要曲线"。与 C++ `SpineTimelineEntries()`
    // / `SpineTimelineHasCurve()`（SpineGpuFormat.h）逐值对应 —— 这两张表错了不报错，
    // 只会让 chunk 13 / 14 的下标整个错位。
    public static class SpineTimelineInfo
    {
        // 每帧占几个 float（进 chunk 13 的步长单位）。逐类抄各 Timeline 的 ENTRIES：
        //   Rotate 2（`RotateTimeline.h:44`）/ Translate·Scale·Shear 3（`TranslateTimeline.cpp:48`）
        //   Color 5（`ColorTimeline.cpp:48`）/ TwoColor 8（`TwoColorTimeline.cpp:48`）
        //   IkConstraint 5（`IkConstraintTimeline.cpp:50`）/ TransformConstraint 5（`:50`）
        //   PathConstraintPosition·Spacing 2（`PathConstraintPositionTimeline.cpp:50`）
        //   PathConstraintMix 3（`PathConstraintMixTimeline.cpp:50`）
        // 变长的三类（attachment / deform / drawOrder）只有 **time** 进 chunk 13（1 个），
        // 其余部分在 chunk 16 —— 这样"取第 n 帧的时间"对所有类型都是同一个公式。
        //
        // ⚠️ 单位陷阱：chunk 13 的 `frame_first` 是 **float 下标**不是"第几帧"
        //    （各类的每帧 float 数不同，没有统一的帧单位）。
        public static uint Entries(SpineTimelineType type)
        {
            switch (type)
            {
                case SpineTimelineType.Rotate:                  return 2;
                case SpineTimelineType.Translate:               return 3;
                case SpineTimelineType.Scale:                   return 3;
                case SpineTimelineType.Shear:                   return 3;
                case SpineTimelineType.Attachment:              return 1;
                case SpineTimelineType.Color:                   return 5;
                case SpineTimelineType.Deform:                  return 1;
                case SpineTimelineType.Event:                   return 1;
                case SpineTimelineType.DrawOrder:               return 1;
                case SpineTimelineType.IkConstraint:            return 5;
                case SpineTimelineType.TransformConstraint:     return 5;
                case SpineTimelineType.PathConstraintPosition:  return 2;
                case SpineTimelineType.PathConstraintSpacing:   return 2;
                case SpineTimelineType.PathConstraintMix:       return 3;
                case SpineTimelineType.TwoColor:                return 8;
            }
            return 0;   // 不认识 ⇒ 读端必须报错（entries 无从确定）
        }

        // 这一类的帧**是否需要曲线**。attachment / drawOrder / event **不是** CurveTimeline
        // （`AttachmentTimeline` / `DrawOrderTimeline` / `EventTimeline` 直接继承 Timeline），
        // 参考实现根本不给它们读 curve —— 给它们写段会让 chunk 13/14 错位且不报错。
        public static bool HasCurve(SpineTimelineType type)
        {
            return type != SpineTimelineType.Attachment && type != SpineTimelineType.DrawOrder &&
                   type != SpineTimelineType.Event;
        }
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
        // 有事件（event）时间轴。事件**不烘焙**（它不改任何渲染状态，且本工程实测 0 个
        // 资产用到），但"有却不记"等于静默丢弃 ⇒ 用一位显式说出来。
        HasEventTimeline = 1u << 5,
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

        // v9：动画。id 11..16 就是当初留给"v7 动画阶段"的。
        AnimDir = 11,          // SpineGpuAnim[]（20 B/条）
        Timelines = 12,        // SpineGpuTimeline[]（32 B/条，全局一张表，动画按区间引用）
        Frames = 13,           // float[]（帧数据平面数组）
        CurveTypes = 14,       // uint8[]（每段一个：0 linear / 1 stepped / 2 bezier）
        Beziers = 15,          // SpineGpuBezier[]（16 B/条，按段序）
        TimelinePayload = 16,  // 字节（attachment 名 / deform 稀疏帧 / drawOrder 偏移）

        SlotSetup = 17,     // v10：槽位 setup（40 B/槽），下标 = header.slot_count 的序

        // v11：约束。约束**必须**烘焙 —— 实测有约束的资产占
        //   transform 48.3% / IK 22.5% / path 11.1%（v7 文档 §1.3），
        //   而且约束是**每帧都跑**的，与时间轴无关。
        ConstraintSetup = 18,   // 约束 setup：IK / transform / path + 共用的 u16 骨骼池
        UpdateOrder = 19,       // `Skeleton::updateCache()` 的结果（**直接落结果**）
        PathAttachments = 20,   // PathAttachment 几何（path 约束要沿它摆骨骼）

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

        // 动画目录一条（chunk 11，20 B）
        public const int AnimNameOffset = 0;
        public const int AnimTimelineFirst = 4;
        public const int AnimTimelineCount = 8;
        public const int AnimDuration = 12;
        public const int AnimPad = 16;             // 落盘必须写 0

        // 时间轴表一条（chunk 12，32 B）
        public const int TlType = 0;               // SpineTimelineType
        public const int TlTarget = 2;             // 骨骼 / 槽位 / 附件记录 / 约束 下标
        public const int TlFrameCount = 4;
        public const int TlFrameFirst = 8;         // ⚠️ float 下标，不是帧号
        public const int TlCurveFirst = 12;
        public const int TlBezierFirst = 16;
        public const int TlBezierCount = 20;
        public const int TlPayloadOffset = 24;     // 字节
        public const int TlPayloadCount = 28;      // 条目数

        // bezier 控制点一条（chunk 15，16 B）
        public const int BezCx1 = 0;
        public const int BezCy1 = 4;
        public const int BezCx2 = 8;
        public const int BezCy2 = 12;

        // 槽位 setup 一条（chunk 17，40 B）
        public const int SlotColorR = 0;
        public const int SlotColorG = 4;
        public const int SlotColorB = 8;
        public const int SlotColorA = 12;
        public const int SlotDarkR = 16;
        public const int SlotDarkG = 20;
        public const int SlotDarkB = 24;
        public const int SlotDarkA = 28;      // 有暗色时恒 1（参考实现写死）
        public const int SlotSetupAttachmentNameOffset = 32;
        public const int SlotFlags = 36;

        // ---- 约束 setup（chunk 18，v11）----
        // IK 一条（20 B）
        public const int IkOrder = 0;         // u16，三类约束**共用**一个排名空间
        public const int IkBoneCount = 2;     // u16
        public const int IkBoneFirst = 4;     // u32，进 chunk 18 的 u16 骨骼池
        public const int IkTarget = 8;        // u32，目标**骨骼**下标
        public const int IkMix = 12;          // f32，json 默认 1
        public const int IkBendDir = 16;      // i16，1 或 -1
        public const int IkFlags = 18;        // u16，kIkFlag*

        // transform 一条（56 B）
        public const int TfOrder = 0;
        public const int TfBoneCount = 2;
        public const int TfBoneFirst = 4;
        public const int TfTarget = 8;        // 目标**骨骼**下标
        public const int TfOffsetRotation = 12;
        public const int TfOffsetX = 16;
        public const int TfOffsetY = 20;
        public const int TfOffsetScaleX = 24;
        public const int TfOffsetScaleY = 28;
        public const int TfOffsetShearY = 32;
        public const int TfRotateMix = 36;
        public const int TfTranslateMix = 40;
        public const int TfScaleMix = 44;
        public const int TfShearMix = 48;
        public const int TfFlags = 52;        // u16，kTfFlag*
        public const int TfPad = 54;          // 落盘必须写 0

        // path 一条（40 B）
        public const int PathOrder = 0;
        public const int PathBoneCount = 2;
        public const int PathBoneFirst = 4;
        public const int PathTargetSlot = 8;  // ⚠️ **槽位**下标不是骨骼
        public const int PathPositionMode = 12;
        public const int PathSpacingMode = 13;
        public const int PathRotateMode = 14;
        public const int PathPad = 15;        // 落盘必须写 0
        public const int PathOffsetRotation = 16;
        public const int PathPosition = 20;
        public const int PathSpacing = 24;
        public const int PathRotateMix = 28;
        public const int PathTranslateMix = 32;
        public const int PathIndex = 36;      // default 皮肤下该槽位的 path 附件下标

        // 执行顺序一条（chunk 19，4 B）
        public const int StepKind = 0;        // u8，SpineGpuUpdateKind
        public const int StepPad = 1;         // 落盘必须写 0
        public const int StepIndex = 2;       // u16，该类内的下标

        // PathAttachment 一条（chunk 20，36 B）
        public const int PathAttSlotIndex = 0;
        public const int PathAttSkinIndex = 2;   // ⚠️ 必须落：曲线取决于激活的皮肤
        public const int PathAttFlags = 4;
        public const int PathAttPad = 6;         // 落盘必须写 0
        public const int PathAttVertexCount = 8;
        public const int PathAttBoneOffset = 12;
        public const int PathAttBoneCount = 16;
        public const int PathAttVertexOffset = 20;
        public const int PathAttVertexFloatCount = 24;
        public const int PathAttLengthOffset = 28;
        public const int PathAttLengthCount = 32;
        public const int PathAttNameOffset = 36;   // u32，进字符串表第 8 组
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

            // ---- v9 动画 ----
            Check(SpineGpuConstants.AnimDirSize == 20, "anim dir entry is 20 B (v9)");
            Check(SpineGpuConstants.TimelineEntrySize == 32, "timeline entry is 32 B (v9)");
            Check(SpineGpuConstants.BezierSize == 16, "bezier record is 16 B (v9)");
            Check(SpineGpuConstants.NoAttachmentName == 0xFFFFFFFFu,
                  "NoAttachmentName must be 0xFFFFFFFF, not 0");
            CheckOffset(SpineGpuOffsets.AnimDuration, 4, SpineGpuConstants.AnimDirSize, "anim duration");
            CheckOffset(SpineGpuOffsets.AnimPad, 4, SpineGpuConstants.AnimDirSize, "anim pad");
            CheckOffset(SpineGpuOffsets.TlFrameCount, 4, SpineGpuConstants.TimelineEntrySize, "timeline frame_count");
            CheckOffset(SpineGpuOffsets.TlPayloadCount, 4, SpineGpuConstants.TimelineEntrySize,
                        "timeline payload_count");
            CheckOffset(SpineGpuOffsets.BezCy2, 4, SpineGpuConstants.BezierSize, "bezier cy2");

            // 每类时间轴都必须有"每帧 float 数"，且**定长类**都不许是 0 ——
            // 少一类不报错，只是那条时间轴的下标整个错位。
            Check(SpineTimelineInfo.Entries(SpineTimelineType.TwoColor) == 8, "twoColor is 8 floats/frame");
            Check(SpineTimelineInfo.Entries(SpineTimelineType.Rotate) == 2, "rotate is 2 floats/frame");
            Check(SpineTimelineInfo.Entries(SpineTimelineType.Color) == 5, "color is 5 floats/frame");
            Check(SpineTimelineInfo.Entries(SpineTimelineType.IkConstraint) == 5, "ik is 5 floats/frame");
            Check(SpineTimelineInfo.Entries(SpineTimelineType.Attachment) == 1, "attachment is 1 float/frame");
            Check(SpineTimelineInfo.Entries((SpineTimelineType)99) == 0, "unknown timeline type has no entries");
            Check(!SpineTimelineInfo.HasCurve(SpineTimelineType.Attachment), "attachment has no curve");
            Check(!SpineTimelineInfo.HasCurve(SpineTimelineType.DrawOrder), "drawOrder has no curve");
            Check(!SpineTimelineInfo.HasCurve(SpineTimelineType.Event), "event has no curve");
            Check(SpineTimelineInfo.HasCurve(SpineTimelineType.Rotate), "rotate has curve");

            // ---- v10 槽位 setup ----
            Check(SpineGpuConstants.SlotSetupSize == 40, "slot setup record is 40 B (v10)");
            Check(SpineGpuConstants.SlotSetupSize % 8 == 0, "slot setup stride is 8-byte aligned (40 = 5 x 8)");
            CheckOffset(SpineGpuOffsets.SlotColorA, 4, SpineGpuConstants.SlotSetupSize, "slot color_a");
            CheckOffset(SpineGpuOffsets.SlotDarkA, 4, SpineGpuConstants.SlotSetupSize, "slot dark_a");
            CheckOffset(SpineGpuOffsets.SlotSetupAttachmentNameOffset, 4, SpineGpuConstants.SlotSetupSize,
                        "slot setup_attachment_name_offset");
            CheckOffset(SpineGpuOffsets.SlotFlags, 4, SpineGpuConstants.SlotSetupSize, "slot flags");
            Check((uint)SpineGpuSlotFlags.HasDark == 1u, "slot flag has_dark is bit0");

            // ---- v11 约束 ----
            Check(SpineGpuConstants.IkConstraintSize == 20, "ik constraint is 20 B (v11)");
            Check(SpineGpuConstants.TransformConstraintSize == 56, "transform constraint is 56 B (v11)");
            Check(SpineGpuConstants.PathConstraintSize == 40, "path constraint is 40 B (v11)");
            Check(SpineGpuConstants.UpdateStepSize == 4, "update step is 4 B (v11)");
            Check(SpineGpuConstants.PathAttachmentSize == 40, "path attachment record is 40 B (v11)");
            Check(SpineGpuConstants.ConstraintHeaderSize == 16, "constraint chunk header is 16 B (v11)");
            Check(SpineGpuConstants.UpdateOrderHeaderSize == 8, "update order chunk header is 8 B (v11)");
            Check(SpineGpuConstants.PathAttHeaderSize == 16, "path attachment chunk header is 16 B (v11)");
            Check(SpineGpuConstants.NoPathAttachment == 0xFFFFFFFFu, "NoPathAttachment must be 0xFFFFFFFF, not 0");

            // 56 B 的 transform 必须 4 B 对齐（里面有 11 个 float + 2 个 u16 + 2 个 u16）
            Check(SpineGpuConstants.TransformConstraintSize % 4 == 0, "transform constraint stride is 4-byte aligned");
            Check(SpineGpuConstants.PathAttachmentSize % 4 == 0, "path attachment stride is 4-byte aligned");

            CheckOffset(SpineGpuOffsets.IkTarget, 4, SpineGpuConstants.IkConstraintSize, "ik target");
            CheckOffset(SpineGpuOffsets.IkFlags, 2, SpineGpuConstants.IkConstraintSize, "ik flags");
            CheckOffset(SpineGpuOffsets.TfShearMix, 4, SpineGpuConstants.TransformConstraintSize, "tf shear_mix");
            CheckOffset(SpineGpuOffsets.TfFlags, 2, SpineGpuConstants.TransformConstraintSize, "tf flags");
            CheckOffset(SpineGpuOffsets.TfPad, 2, SpineGpuConstants.TransformConstraintSize, "tf pad");
            CheckOffset(SpineGpuOffsets.PathTargetSlot, 4, SpineGpuConstants.PathConstraintSize, "path target_slot");
            CheckOffset(SpineGpuOffsets.PathTranslateMix, 4, SpineGpuConstants.PathConstraintSize,
                        "path translate_mix");
            CheckOffset(SpineGpuOffsets.PathIndex, 4, SpineGpuConstants.PathConstraintSize, "path path_index");
            CheckOffset(SpineGpuOffsets.StepIndex, 2, SpineGpuConstants.UpdateStepSize, "step index");
            CheckOffset(SpineGpuOffsets.PathAttSkinIndex, 2, SpineGpuConstants.PathAttachmentSize,
                        "path attachment skin_index");
            CheckOffset(SpineGpuOffsets.PathAttLengthCount, 4, SpineGpuConstants.PathAttachmentSize,
                        "path attachment length_count");
            CheckOffset(SpineGpuOffsets.PathAttNameOffset, 4, SpineGpuConstants.PathAttachmentSize,
                        "path attachment name_offset");

            Check((uint)SpineGpuIkFlags.Compress == 1u, "ik flag compress is bit0");
            Check((uint)SpineGpuIkFlags.Stretch == 2u, "ik flag stretch is bit1");
            Check((uint)SpineGpuIkFlags.Uniform == 4u, "ik flag uniform is bit2");
            Check((uint)SpineGpuTransformFlags.Local == 1u, "tf flag local is bit0");
            Check((uint)SpineGpuTransformFlags.Relative == 2u, "tf flag relative is bit1");
            Check((uint)SpineGpuPathAttachmentFlags.ConstantSpeed == 2u, "path att flag constant_speed is bit1");
            Check((uint)SpineGpuPathAttachmentFlags.Weighted == 4u, "path att flag weighted is bit2");

            // ⚠️ path 的 target 是槽位、IK/transform 的 target 是骨骼 —— 两个下标的
            //    **取值域**不同，但字段名在两边都叫"target"。把这条钉在启动路径上，
            //    免得以后有人把 path 的 target_slot 当成骨骼下标用（不报错，只是
            //    约束一直作用在错误的对象上）。
            Check(SpineGpuOffsets.IkTarget == SpineGpuOffsets.TfTarget, "ik/tf target share the same slot");
            Check(SpineGpuOffsets.IkTarget == SpineGpuOffsets.PathTargetSlot,
                  "path target_slot sits where ik/tf target sits (different index space!)");
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

    // 动画目录一条（chunk 11，20 B）。
    // ⚠️ 动画**数量不进 header**：64 B header 的 16 个 u32 已经排满，而 chunk 11 的条目
    //    是定长 20 B ⇒ 数量 = chunk 11 大小 / 20，读端照此反推并校验整除。
    public sealed class SpineGpuAnim
    {
        public uint NameOffset;     // chunk 1 里的**字节**偏移
        public uint TimelineFirst;  // 进 chunk 12 的**条**下标
        public uint TimelineCount;
        public float Duration;      // = max(各时间轴最后一帧的 time)
        public uint Pad;            // 落盘必须写 0
    }

    // 时间轴表一条（chunk 12，32 B，全局一张表）。
    // `Target` 的含义随 Type 变，这也是它叫 Target 不叫 BoneIndex 的原因：
    //   * rotate / translate / scale / shear → **骨骼**下标
    //   * attachment / color / twoColor      → **槽位**下标
    //   * deform                             → **附件记录**（chunk 7 行号）下标
    //   * ik / transform / path*             → 对应**约束**下标
    //   * drawOrder                          → 恒 0（它作用于整个骨架的槽位顺序）
    //
    // deform 用"附件记录下标"是有意的：一条记录 = (皮肤, 槽位, 附件) 三元组，
    // 正好是 deform 时间轴的寻址键，不需要再给 32 B 塞三个字段。
    public sealed class SpineGpuTimeline
    {
        public ushort Type;               // SpineTimelineType
        public ushort Target;
        public uint FrameCount;
        // ⚠️ 进 chunk 13 的 **float 下标**（字节 / 4），不是"第几帧"。
        //    每类时间轴每帧的 float 数不同，不存在全局统一的"帧"单位 ——
        //    用"帧"去算下标，第一条（偏移 0）看不出错，第二条起就读到别人的帧数据，且不报错。
        public uint FrameFirst;
        public uint CurveFirst;           // 进 chunk 14 的**段**下标
        public uint BezierFirst;          // 进 chunk 15 的**条**下标
        public uint BezierCount;
        public uint PayloadOffset;        // 进 chunk 16 的**字节**偏移
        public uint PayloadCount;         // 载荷里的**条目**数
    }

    // bezier 控制点（chunk 15，16 B/条）。
    // ⚠️ 存**控制点**而不是参考实现那张展开表。`CurveTimeline::setCurve()`（:78-83）
    //    把 4 个控制点展开成 `BEZIER_VALUES = 18` 个 float 的采样表；展开式里的 4 个常数
    //    `0.03f / 0.006f / 0.3f / 0.16666667f`（:80-83）由**读端照抄**，
    //    ⚠️ 化简即错且不报错（那是 bezier 的前向差分系数，不是可以重排的算术式）。
    public sealed class SpineGpuBezier
    {
        public float Cx1;
        public float Cy1;
        public float Cx2;
        public float Cy2;
    }

    // 逐槽位 flags（chunk 17 @36）。
    [Flags]
    public enum SpineGpuSlotFlags : uint
    {
        None = 0,
        // 这个槽位**有暗色**（tint black）。渲染要不要走双色分支看的是它，
        // 不是"dark 是否全 0" —— 3.7 的判据是 `读到的 int32 != -1`，
        // 0x00000000（纯黑）照样是"有暗色"。
        HasDark = 1u << 0,
    }

    // 槽位 setup（chunk 17，40 B/条）。下标 = header.slot_count 的序
    // = chunk 1 里槽位名的序号 = 时间轴 Target（attachment / color / twoColor）
    // 引用的同一个下标空间。三处必须同一个下标空间 —— 分叉了不报错，
    // 只是颜色 / 附件挂到别的槽位上。
    public sealed class SpineGpuSlot
    {
        public float ColorR = 1.0f;
        public float ColorG = 1.0f;
        public float ColorB = 1.0f;
        public float ColorA = 1.0f;      // 默认 (1,1,1,1)（SlotData.cpp:39）
        public float DarkR;
        public float DarkG;
        public float DarkB;
        public float DarkA;              // 默认 (0,0,0,0)（SlotData.cpp:40）
        // setup 附件名在 chunk 1 里的字节偏移；NoAttachmentName = 空名（默认不挂附件）。
        // attachment 时间轴在 time < frames[0] 时复位到它（AttachmentTimeline.cpp:65-68）。
        public uint SetupAttachmentNameOffset = SpineGpuConstants.NoAttachmentName;
        public SpineGpuSlotFlags Flags = SpineGpuSlotFlags.None;
    }

    // -----------------------------------------------------------------
    // 约束（chunk 18 / 19 / 20，v11）
    // -----------------------------------------------------------------
    // 三条约束的 `target` **不是同一个下标空间**：
    //   IK / transform 的 target 是**骨骼**下标，path 的 target 是**槽位**下标
    //   （`PathConstraintData::_target` 是 `SlotData*`）。写反了不报错，
    //   只是约束一直作用在错误的对象上。

    [Flags]
    public enum SpineGpuIkFlags : ushort
    {
        None = 0,
        Compress = 1 << 0,
        Stretch = 1 << 1,
        Uniform = 1 << 2,
    }

    [Flags]
    public enum SpineGpuTransformFlags : ushort
    {
        None = 0,
        Local = 1 << 0,
        Relative = 1 << 1,
    }

    // 逐值抄 3.7 `PathConstraintData.h:39-43`（PositionMode / SpacingMode / RotateMode）
    public enum SpineGpuPathPositionMode : byte { Fixed = 0, Percent = 1 }
    public enum SpineGpuPathSpacingMode : byte { Length = 0, Fixed = 1, Percent = 2 }
    public enum SpineGpuPathRotateMode : byte { Tangent = 0, Chain = 1, ChainScale = 2 }

    // IK 一条（chunk 18，20 B）
    public sealed class SpineGpuIkConstraint
    {
        public ushort Order;          // 三类约束**共用**一个排名空间
        public ushort BoneCount;
        public uint BoneFirst;        // 进 chunk 18 的 u16 骨骼池
        public uint Target;           // 目标**骨骼**下标
        public float Mix = 1.0f;      // json 默认 1（SkeletonJson.cpp:290）
        public short BendDir = 1;     // 1 或 -1；json 是 `bendPositive` 布尔
        public SpineGpuIkFlags Flags = SpineGpuIkFlags.None;
    }

    // transform 一条（chunk 18，56 B）
    public sealed class SpineGpuTransformConstraint
    {
        public ushort Order;
        public ushort BoneCount;
        public uint BoneFirst;
        public uint Target;           // 目标**骨骼**下标
        public float OffsetRotation;
        public float OffsetX;
        public float OffsetY;
        public float OffsetScaleX;
        public float OffsetScaleY;
        public float OffsetShearY;
        public float RotateMix = 1.0f;
        public float TranslateMix = 1.0f;
        public float ScaleMix = 1.0f;
        public float ShearMix = 1.0f;
        public SpineGpuTransformFlags Flags = SpineGpuTransformFlags.None;
        public ushort Pad;            // 落盘必须写 0
    }

    // path 一条（chunk 18，40 B）
    public sealed class SpineGpuPathConstraint
    {
        public ushort Order;
        public ushort BoneCount;
        public uint BoneFirst;
        // ⚠️ 是**槽位**下标不是骨骼（SkeletonBinary.cpp:249 取的是 `_slots[...]`）
        public uint TargetSlot;
        public SpineGpuPathPositionMode PositionMode = SpineGpuPathPositionMode.Percent;
        public SpineGpuPathSpacingMode SpacingMode = SpineGpuPathSpacingMode.Length;
        public SpineGpuPathRotateMode RotateMode = SpineGpuPathRotateMode.Tangent;
        public byte Pad;              // 落盘必须写 0
        public float OffsetRotation;
        // ⚠️ `position` / `spacing` 是否乘 scale 取决于 mode（Fixed / Length）——
        //    那是**解析期**的事（SkeletonBinary.cpp:252-255），运行时不要再乘一次。
        public float Position;
        public float Spacing;
        public float RotateMix = 1.0f;
        public float TranslateMix = 1.0f;
        // default 皮肤下该槽位的 PathAttachment 在 chunk 20 里的下标；
        // NoPathAttachment = "这个槽位上没有 path 附件"。
        // ⚠️ 换装后必须按 (激活皮肤, 槽位) 重查 —— `PathConstraint::update()`
        //    用的是槽位**当前**挂的附件（PathConstraint.cpp:77）。
        public uint PathIndex = SpineGpuConstants.NoPathAttachment;
    }

    // 执行顺序一步（chunk 19，4 B）
    public enum SpineGpuUpdateKind : byte
    {
        Bone = 0,
        Ik = 1,
        Transform = 2,
        Path = 3,
    }

    public sealed class SpineGpuUpdateStep
    {
        public SpineGpuUpdateKind Kind = SpineGpuUpdateKind.Bone;
        public byte Pad;              // 落盘必须写 0
        // 约束下标是**该类内的**下标（ik / transform / path 各自从 0 数），
        // 与 chunk 18 三段数组的下标一致。
        public ushort Index;
    }

    // PathAttachment 几何（chunk 20，36 B）
    [Flags]
    public enum SpineGpuPathAttachmentFlags : ushort
    {
        None = 0,
        Closed = 1 << 0,
        ConstantSpeed = 1 << 1,
        Weighted = 1 << 2,
    }

    public sealed class SpineGpuPathAttachment
    {
        public ushort SlotIndex;
        // ⚠️ 皮肤**必须**落盘：`PathConstraint::update()` 用的是
        //    `_target->getAttachment()`（PathConstraint.cpp:77）—— 槽位**当前**挂的附件，
        //    而"当前"取决于激活的皮肤。只按槽位找会在换装后拿到别的皮肤的曲线。
        public ushort SkinIndex;
        public SpineGpuPathAttachmentFlags Flags = SpineGpuPathAttachmentFlags.None;
        public ushort Pad;            // 落盘必须写 0
        public uint VertexCount;
        public uint BoneOffset;       // 进 chunk 20 的 u16 池（**下标**不是字节）
        public uint BoneCount;
        public uint VertexOffset;     // 进 chunk 20 的 f32 池（**下标**不是字节）
        public uint VertexFloatCount;
        public uint LengthOffset;
        public uint LengthCount;      // = VertexCount / 3（SkeletonBinary.cpp:550）
        // ⚠️ 附件名**必须**落盘：`PathConstraint::update()` 取的是槽位**当前**挂的附件
        //    （PathConstraint.cpp:76-80），而"当前"会被 attachment 时间轴改掉。
        //    一个槽位在同一皮肤下挂两条 path 附件是**合法的**（实测 782 个资产里
        //    有 6 个）⇒ 静态烘焙只能给出"setup 时用的是哪条"（PathIndex），
        //    运行时换装 / 换附件后必须**按名字重新查**。
        public uint NameOffset;       // 进字符串表第 8 组
    }

    public class SpineGpuContainerException : Exception
    {
        public SpineGpuContainerException(string message) : base(message) { }
    }
}
