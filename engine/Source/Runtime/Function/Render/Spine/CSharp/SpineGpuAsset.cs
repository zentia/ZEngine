// 容器在内存里的形态。与 C++ `SpineGpuAsset`（SpineGpuImporter.h）逐字段对应。
//
// 与 C++ 一样：**顶点缓冲是一整块字节流**，不是"顶点数组"。
// 因为两种顶点格式步长不同（8 / 48），且同一段顶点会被多个绘制项共用 ——
// 把它摊成对象数组既浪费也丢掉了"共用"这个事实。

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

        public ushort[] IndexBuffer = System.Array.Empty<ushort>();
        public List<SpineDrawItem> DrawItems = new List<SpineDrawItem>();
        public ushort[] SlotBone = System.Array.Empty<ushort>();

        // 字符串表（chunk 1）。落盘顺序固定：
        // 骨骼名 → 皮肤名 → 槽位名 → 附件名 → 图集页名，每项 '\0' 结尾。
        // 读回时按 bone_count / skin_count / slot_count / 附件记录数切分。
        public List<string> BoneNames = new List<string>();
        public List<string> SkinNames = new List<string>();
        public List<string> SlotNames = new List<string>();
        public List<string> AttachmentNames = new List<string>();
        public List<string> AtlasPageNames = new List<string>();

        // 裁剪多边形记录（chunk 9，12 B/条）。下标就是绘制项 ClipIndex 引用的行号。
        // **顶点本身在 VertexBuffer 里**。
        public List<SpineGpuClipPolygon> ClipPolygons = new List<SpineGpuClipPolygon>();

        public List<SpineGpuAttachmentRecord> AttachmentRecords = new List<SpineGpuAttachmentRecord>();
    }
}
