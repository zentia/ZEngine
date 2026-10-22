// 容器在内存里的形态。与 C++ `SpineGpuAsset`（SpineGpuImporter.h）逐字段对应。
//
// 与 C++ 一样：**顶点缓冲是一整块字节流**，不是"顶点数组"。
// 因为两种顶点格式步长不同（8 / 8），且同一段顶点会被多个绘制项共用 ——
// 把它摊成对象数组既浪费也丢掉了"共用"这个事实。
//
// v7 起 weighted 顶点只剩 8 B 的**引用**（influence_offset / influence_count），
// 真正的蒙皮数据在下面的影响流里。

using System.Collections.Generic;

namespace ZEngine.SpineGpu
{
    public sealed class SpineGpuAsset
    {
        public SpineGpuHeader Header = new SpineGpuHeader();

        // 顶点缓冲按格式分段：先所有 rigid 段、再所有 weighted 段（段首 8 B 对齐）。
        // **不含 UV** —— 顶点只存几何 + 蒙皮。
        public byte[] VertexBuffer = System.Array.Empty<byte>();
        public uint RigidVertexCount;
        public uint WeightedVertexCount;
        public uint RigidSegmentOffset;
        public uint WeightedSegmentOffset;

        // UV 通道（chunk 4）：每条目 2 float。**条目数 ≠ 顶点数**。
        public float[] UvBuffer = System.Array.Empty<float>();

        // ---- 影响流（chunk 21 / 22）----
        // 两条流**共用同一个条目下标**，就是权重顶点的 influence_offset。
        // 条目数 = InfluenceBind.Length / 2 = InfluenceBone.Length。
        public float[] InfluenceBind = System.Array.Empty<float>();      // chunk 21：float2 × 条目数

        // chunk 22：(boneIdx u16, weight u8, pad u8) × 条目数，按 4 B 一条**平铺**存
        // （不是结构数组）—— 读写两端都按偏移取，这样和 C++ 的字节流完全一致。
        public byte[] InfluenceBone = System.Array.Empty<byte>();

        public ushort[] IndexBuffer = System.Array.Empty<ushort>();
        public List<SpineDrawItem> DrawItems = new List<SpineDrawItem>();
        public ushort[] SlotBone = System.Array.Empty<ushort>();

        // 骨骼 setup（chunk 10，36 B/骨）。下标 = header.bone_count 的序，
        // 也 = 骨骼名的序号，也 = 影响流 bone_idx 引用的同一个下标 —— 三处必须同一个空间。
        public List<SpineGpuBone> Bones = new List<SpineGpuBone>();

        // 槽位 setup（chunk 17，40 B/槽）。下标 = header.slot_count 的序
        // = 槽位名的序号 = 时间轴 Target（attachment / color / twoColor）的同一个空间。
        // ⚠️ 颜色**必须**在容器里：时间轴写的是绝对颜色，"没被命中"的槽位停在 setup 色，
        //    缺了它就只能默认纯白 —— 而纯白与 setup 白在多数资产上完全看不出区别。
        public List<SpineGpuSlot> Slots = new List<SpineGpuSlot>();

        // 字符串表（chunk 1）。落盘顺序固定：
        // 骨骼名 → 皮肤名 → 槽位名 → **槽位 setup 附件名** → 附件名 → 图集页名 →
        // **动画名** → **动画附件名**，每项 '\0' 结尾。
        // 读回时按 bone_count / skin_count / slot_count / slot_count / 附件记录数 /
        // 动画数切分；最后一组（动画附件名）没有计数，只按偏移访问。
        public List<string> BoneNames = new List<string>();
        public List<string> SkinNames = new List<string>();
        public List<string> SlotNames = new List<string>();
        public List<string> SlotSetupAttachmentNames = new List<string>();
        public List<string> AttachmentNames = new List<string>();
        public List<string> AtlasPageNames = new List<string>();
        public List<string> AnimNames = new List<string>();
        public List<string> AnimAttachmentNames = new List<string>();

        // ---- 动画（v9，chunk 11-16）----
        // 五块之间全是**下标**引用，越界在 C# 里不抛、只取到别人的数据
        // ⇒ 读端必须逐条查区间（见 SpineGpuContainerIo.Read）。
        public List<SpineGpuAnim> Anims = new List<SpineGpuAnim>();          // chunk 11
        public List<SpineGpuTimeline> Timelines = new List<SpineGpuTimeline>(); // chunk 12
        public float[] Frames = System.Array.Empty<float>();                 // chunk 13
        public byte[] CurveTypes = System.Array.Empty<byte>();               // chunk 14
        public List<SpineGpuBezier> Beziers = new List<SpineGpuBezier>();    // chunk 15
        public byte[] TimelinePayload = System.Array.Empty<byte>();          // chunk 16

        // 裁剪多边形记录（chunk 9，12 B/条）。下标就是绘制项 ClipIndex 引用的行号。
        // **顶点本身在 VertexBuffer 里**。
        public List<SpineGpuClipPolygon> ClipPolygons = new List<SpineGpuClipPolygon>();

        public List<SpineGpuAttachmentRecord> AttachmentRecords = new List<SpineGpuAttachmentRecord>();

        // ---- 约束（chunk 18 / 19 / 20，v11）----
        // 下标空间统一：Iks[i] / Transforms[i] / Paths[i] 的 i 就是 chunk 19 里
        // SpineGpuUpdateStep.Index（配 Kind 用）与时间轴里约束时间轴 Target
        // 引用的同一个下标。
        public List<SpineGpuIkConstraint> Iks = new List<SpineGpuIkConstraint>();
        public List<SpineGpuTransformConstraint> Transforms = new List<SpineGpuTransformConstraint>();
        public List<SpineGpuPathConstraint> Paths = new List<SpineGpuPathConstraint>();
        // 三类约束的 `bones[]` **共用**一段 u16 索引池（长度可变 ⇒ 不能内联）。
        public ushort[] ConstraintBonePool = System.Array.Empty<ushort>();

        // `Skeleton::updateCache()` 的结果**原样落盘**，运行时照着遍历。
        public List<SpineGpuUpdateStep> UpdateSteps = new List<SpineGpuUpdateStep>();
        // `_updateCacheReset`：遍历之前要把哪些骨骼复位到 setup
        // （Skeleton.cpp:200-211）。少复位 ⇒ 上一帧的应用值残留（"动作拖影"），不报错。
        public ushort[] UpdateReset = System.Array.Empty<ushort>();

        // PathAttachment 几何。**不产生绘制项**，只给 path 约束用。
        public List<SpineGpuPathAttachment> PathAttachments = new List<SpineGpuPathAttachment>();
        public ushort[] PathBonePool = System.Array.Empty<ushort>();
        public float[] PathVertices = System.Array.Empty<float>();
        public float[] PathLengths = System.Array.Empty<float>();
        // 一条 path 附件一个名字，**与 PathAttachments 一一对应**（不去重）。
        public List<string> PathAttachmentNames = new List<string>();
    }
}
