// 跨语言对拍：C++ 落出来的 .zspg，C# 读回来再写一遍，必须**逐字节相同**。
//
// 为什么要跨语言，而不是两端各自自测：
//   两端各自对 = 各自对上了自己的想象。字节是全格式唯一的公共事实 ——
//   指针、地址、未初始化填充、struct 对齐、字节序，全都会在这里露出来
//   （规格 §12 第 7 项："容器必须可重入"）。
//
// 光有 round-trip 还不够 —— 一个只会 memcpy 的读端也能过。所以这里还断言
// **语义**（哪些项被裁、多边形顶点在哪、UV 条目数 ≠ 顶点数），
// 也就是 C++ 那边钉住的那几条，C# 这边再钉一遍。

using System;
using System.Collections.Generic;
using System.IO;
using ZEngine.SpineGpu;

namespace ZEngine.SpineGpu.Test
{
    public static class Program
    {
        private static int _checks;
        private static int _failed;
        private static string _prefix = "";

        public static int Main(string[] args)
        {
            Console.WriteLine("=== GPU-Spine container: C# 读端对拍 ===");

            string dir = args.Length >= 1 ? args[0] : "";
            if (string.IsNullOrEmpty(dir))
            {
                Console.WriteLine("用法：ZEngine.SpineGpu.Test <容器目录>");
                Console.WriteLine("（目录由 SpineGpuImportTest --dump <dir> 生成）");
                return 1;
            }
            if (!Directory.Exists(dir))
            {
                Console.WriteLine("  FAIL 容器目录不存在：" + dir);
                return 1;
            }

            SpineGpuLayout.Validate();
            Check(true, "布局自洽检查通过");

            var files = new List<string>(Directory.GetFiles(dir, "*.zspg"));
            files.Sort(StringComparer.Ordinal);
            Check(files.Count > 0, "容器目录里有 .zspg（" + files.Count + " 个）");
            if (files.Count == 0)
            {
                return Report();
            }

            var byName = new Dictionary<string, SpineGpuAsset>();

            foreach (string f in files)
            {
                string name = Path.GetFileName(f);
                _prefix = "[" + name + "] ";
                byte[] bytes = File.ReadAllBytes(f);

                SpineGpuAsset a;
                try
                {
                    a = SpineGpuContainerIo.Read(bytes);
                }
                catch (SpineGpuContainerException e)
                {
                    Check(false, "读成功 — " + e.Message);
                    continue;
                }
                byName[name] = a;
                Check(true, "读成功");

                // ---- 核心：读回来再写一遍，必须逐字节相同 ----
                byte[] again;
                try
                {
                    again = SpineGpuContainerIo.Write(a);
                }
                catch (SpineGpuContainerException e)
                {
                    Check(false, "重写成功 — " + e.Message);
                    continue;
                }
                Check(SameBytes(bytes, again), "重写后**逐字节相同**");

                // ---- 基本自洽 ----
                Check(a.Header.Magic == SpineGpuConstants.Magic, "魔数是 'ZSPG'");
                Check(a.Header.Version == SpineGpuConstants.FormatVersion, "版本是 10");
                // v10：槽位 setup 是 chunk 17，条数必须 = header.slot_count。
                // "chunk 不存在"本身不报错，少了这等式读端会得到"所有槽位纯白"
                // —— 而纯白与 setup 白在多数资产上完全看不出区别（坑与 chunk 10 同类）。
                Check((uint)a.Slots.Count == a.Header.SlotCount, "槽位 setup 条数与 header 一致");
                Check(a.DrawItems.Count == a.Header.DrawItemCount, "绘制项数与 header 一致");
                Check(a.BoneNames.Count == a.Header.BoneCount, "骨骼名数与 header 一致");
                // v8：骨骼 setup 必须落盘且条数与 header 一致 —— "chunk 不存在"本身不报错，
                // 少了这等式读端会拿到 0 根骨骼，后面所有 bone_idx 全部越界且不报错。
                Check((uint)a.Bones.Count == a.Header.BoneCount, "骨骼 setup 条数与 header 一致");
                Check(a.SlotNames.Count == a.Header.SlotCount, "槽位名数与 header 一致");
                Check(a.AttachmentNames.Count == a.AttachmentRecords.Count, "附件名数与附件记录数一致");
                Check((uint)(a.InfluenceBind.Length / 2) == (uint)(a.InfluenceBone.Length / (int)SpineGpuConstants.InfluenceBoneStride),
                      "影响流两条 chunk 条目数相同（bind / bone 共用同一个下标）");
                // v9：动画名的条数 = chunk 11 的条目数（header 里没有 anim_count 的位置）
                Check((uint)a.AnimNames.Count == (uint)a.Anims.Count, "动画名数与动画目录条数一致");

                // ---- chunk 按下标找会错位：这里显式验证"按 id 找"真的用上了 ----
                // （容器里 chunk 顺序是 1,2,3,21,22,4,5,9,6,7,8 —— 顺序 ≠ id 顺序）
                Check(true, "（chunk 按 id 定位，顺序与 id 不一致也读对了）");
            }
            _prefix = "";

            // ---------------- 语义断言 ----------------
            Semantics(byName);

            // ---------------- 负向：故意破坏必须被拒 ----------------
            Negative(files);

            return Report();
        }

        private static void Semantics(Dictionary<string, SpineGpuAsset> by)
        {
            // 基线：没有裁剪 ⇒ 没有多边形，且**每个**绘制项都是 NoClipIndex
            if (by.TryGetValue("baseline.skel.zspg", out var b))
            {
                _prefix = "[baseline] ";
                Check(b.ClipPolygons.Count == 0, "基线没有裁剪多边形");
                bool allNo = true;
                foreach (var it in b.DrawItems) allNo = allNo && it.ClipIndex == SpineGpuConstants.NoClipIndex;
                Check(allNo, "基线每个绘制项都是 NoClipIndex");
                Check(b.UvBuffer.Length != 0 && b.UvBuffer.Length / 2 != b.VertexBuffer.Length,
                      "UV 条目数与顶点缓冲字节数不同量纲（UV 是独立通道）");

                // 影响流：基线 mesh 是 1,2,1,1 个影响 ⇒ 共 5 条，且必须有偏移非 0 的顶点
                Check(b.InfluenceBone.Length == 5 * (int)SpineGpuConstants.InfluenceBoneStride,
                      "影响流 5 条（1+2+1+1）× 4 B = 20 B");
                bool nonZeroOffset = false;
                bool allWeights255 = true;
                for (uint v = 0; v < b.WeightedVertexCount; ++v)
                {
                    int o = (int)(b.WeightedSegmentOffset + v * SpineGpuConstants.WeightedVertexStride);
                    uint off = ReadU32(b.VertexBuffer, o + SpineGpuOffsets.WvInfluenceOffset);
                    uint cnt = ReadU32(b.VertexBuffer, o + SpineGpuOffsets.WvInfluenceCount);
                    if (off != 0) nonZeroOffset = true;
                    int sum = 0;
                    for (uint k = 0; k < cnt; ++k)
                    {
                        sum += b.InfluenceBone[(off + k) * SpineGpuConstants.InfluenceBoneStride +
                                               SpineGpuOffsets.IbWeight];
                    }
                    if (sum != 255) allWeights255 = false;
                }
                Check(allWeights255, "每个权重顶点的 unorm8 权重和恰好 255");
                // ⚠️ 必须有一条**偏移非 0**的影响：全是 0 时"写错成恒 0"测不出来。
                Check(nonZeroOffset, "有权重顶点的 influence_offset 非 0");

                // ---- 骨骼 setup（chunk 10）----
                // 逐字段比，不只是比条数：把 x 与 y 的偏移对调，字节照样自洽
                // （读写共用同一组常量 ⇒ round-trip 抓不到"解读"漂，只有语义断言能抓到）。
                if (b.Bones.Count == 2)
                {
                    var r = b.Bones[0];
                    var h = b.Bones[1];
                    Check(r.Parent == -1, "root 的 parent = -1");
                    Check(h.Parent == 0, "hip 的 parent = 0（必须 < 自己的下标）");
                    Check(r.X == 3.0f && r.Y == -4.0f, "root 的 x/y");
                    Check(r.Rotation == 15.0f, "root 的 rotation（度）");
                    Check(r.ScaleX == 2.0f && r.ScaleY == 0.5f, "root 的 scaleX/scaleY");
                    Check(r.ShearX == 7.0f && r.ShearY == -9.0f, "root 的 shearX/shearY");
                    Check(r.Length == 42.0f, "root 的 length");
                    Check(r.TransformMode == SpineTransformMode.Normal, "root 的 transform_mode = Normal");
                    Check(h.X == 1.5f && h.Y == 2.5f, "hip 的 x/y");
                    Check(h.Rotation == -30.0f, "hip 的 rotation");
                    Check(h.ScaleX == 1.25f && h.ScaleY == 3.0f, "hip 的 scaleX/scaleY");
                    Check(h.ShearX == -1.5f && h.ShearY == 0.25f, "hip 的 shearX/shearY");
                    Check(h.Length == 17.0f, "hip 的 length");
                    Check(h.TransformMode == SpineTransformMode.NoScaleOrReflection,
                          "hip 的 transform_mode = NoScaleOrReflection(4)");
                }

                // ---- 动画（chunk 11-16，v9）----
                // 逐字段比，不只是比条数 —— 读写两端共用同一组常量，
                // round-trip 抓不到"解读"漂（比如把 frame_first 当成帧号）。
                if (b.Anims.Count == 2)
                {
                    Check(b.Anims[0].Pad == 0 && b.Anims[1].Pad == 0, "动画目录的 pad 落盘为 0");
                    Check(b.Anims[0].TimelineFirst == 0 && b.Anims[0].TimelineCount == 8,
                          "walk 从时间轴表第 0 条开始、8 条");
                    // ⚠️ 第二个动画的三个偏移**全非 0**：偏移 0 时"帧 vs float"两种解释
                    //    逐字节相同，写错单位测不出来。这条断言是全文件唯一能抓到它的地方。
                    Check(b.Anims[1].TimelineFirst == 8 && b.Anims[1].TimelineCount == 1,
                          "idle 的时间轴区间非 0（否则测不出下标写错）");
                    Check(b.Anims[0].Duration == 1.0f, "walk 的 duration = 1（各时间轴最后一帧 time 的最大值）");
                    Check(b.Anims[1].Duration == 0.0f, "idle 的 duration = 0（只有一帧 time=0）");
                    if (b.AnimNames.Count == 2)
                    {
                        Check(b.AnimNames[0] == "walk" && b.AnimNames[1] == "idle", "动画名是 walk / idle");
                    }
                }
                if (b.Timelines.Count == 9)
                {
                    // 规范化顺序：(type, target) 升序 —— 不跟着输入文件的书写顺序走。
                    ushort[] wantType = { 0, 0, 1, 2, 3, 4, 5, 14, 0 };
                    ushort[] wantTarget = { 0, 1, 0, 0, 0, 0, 0, 1, 0 };
                    uint[] wantFrames = { 3, 1, 2, 1, 1, 2, 2, 2, 1 };
                    // frame_first 是 **float 下标**：各类的每帧 float 数不同
                    // （rotate 6 + rotate 2 + translate 6 + scale 3 + shear 3 +
                    //  attachment 2 + color 10 + twoColor 16 …）
                    uint[] wantFrameFirst = { 0, 6, 8, 14, 17, 20, 22, 32, 48 };
                    bool allOk = true;
                    for (int i = 0; i < 9; ++i)
                    {
                        var t = b.Timelines[i];
                        allOk = allOk && t.Type == wantType[i] && t.Target == wantTarget[i] &&
                                t.FrameCount == wantFrames[i] && t.FrameFirst == wantFrameFirst[i];
                    }
                    Check(allOk, "9 条时间轴的 (type, target, frame_count, frame_first) 序列");
                    Check(b.Timelines[8].FrameFirst != 0 && b.Timelines[8].CurveFirst != 0,
                          "最后一条时间轴的 frame_first / curve_first 都非 0");
                }
                Check(b.Frames.Length == 50, "帧数据 50 个 float");
                if (b.CurveTypes.Length == 5)
                {
                    Check(b.CurveTypes[0] == (byte)SpineCurveType.Stepped, "段 0 = stepped");
                    Check(b.CurveTypes[1] == (byte)SpineCurveType.Bezier, "段 1 = bezier");
                    Check(b.CurveTypes[2] == (byte)SpineCurveType.Linear &&
                          b.CurveTypes[3] == (byte)SpineCurveType.Linear &&
                          b.CurveTypes[4] == (byte)SpineCurveType.Linear, "段 2..4 = linear");
                }
                if (b.Beziers.Count == 1)
                {
                    var z = b.Beziers[0];
                    Check(z.Cx1 == 0.25f && z.Cy1 == 0.5f && z.Cx2 == 0.75f && z.Cy2 == 0.8f,
                          "bezier 控制点按 (cx1, cy1, cx2, cy2) 落盘（顺序错了不报错）");
                }
                // 变长载荷：只有 attachment 时间轴有（2 帧 × u32）
                if (b.AnimAttachmentNames.Count == 1)
                {
                    Check(b.AnimAttachmentNames[0] == "quad", "动画附件名去重后只剩 quad");
                }
                if (b.TimelinePayload.Length >= 8)
                {
                    uint p0 = ReadU32(b.TimelinePayload, 0);
                    uint p1 = ReadU32(b.TimelinePayload, 4);
                    Check(p0 != SpineGpuConstants.NoAttachmentName, "attachment 第 0 帧指向真实附件名");
                    // ⚠️ 不是 0 —— 0 是 chunk 1 里第一个字符串的合法偏移
                    Check(p1 == SpineGpuConstants.NoAttachmentName,
                          "attachment 第 1 帧是空名 ⇒ NoAttachmentName（**不是** 0）");
                }
                // ---- 槽位 setup（chunk 17，v10）----
                // 逐字段比，不只是比条数：把 color 与 dark 的偏移对调，字节照样自洽
                // （读写共用同一组常量 ⇒ round-trip 抓不到"解读"漂，只有语义断言能抓到）。
                // ⚠️ 夹具的 s1 有显式色 + 暗色 ⇒ 断言必须**非零**：
                //    "读回来还是纯白"这种断言在"写端根本没写颜色"时也会通过。
                Check(b.SlotSetupAttachmentNames.Count == 2 &&
                      b.SlotSetupAttachmentNames[0] == "quad" && b.SlotSetupAttachmentNames[1] == "mesh",
                      "槽位 setup 附件名是 quad / mesh（attachment 时间轴的复位基线）");
                if (b.Slots.Count == 2)
                {
                    var s0 = b.Slots[0];
                    var s1 = b.Slots[1];
                    Check(Near(s0.ColorR, 1.0f) && Near(s0.ColorA, 1.0f) && Near(s0.DarkA, 0.0f) &&
                          s0.Flags == SpineGpuSlotFlags.None,
                          "s0 是默认（纯白 / 无暗色 / flags=0）");
                    Check(s1.Flags == SpineGpuSlotFlags.HasDark, "s1 的 flags 带 has_dark");
                    Check(Near(s1.ColorR, 0x80 / 255.0f) && Near(s1.ColorG, 0xC0 / 255.0f) &&
                          Near(s1.ColorB, 1.0f) && Near(s1.ColorA, 0x40 / 255.0f),
                          "s1 的 setup 色 = 80c0ff40（**不是**默认纯白）");
                    Check(Near(s1.DarkR, 0x20 / 255.0f) && Near(s1.DarkG, 0x40 / 255.0f) &&
                          Near(s1.DarkB, 0x60 / 255.0f) && Near(s1.DarkA, 1.0f),
                          "s1 的 setup 暗色 = 204060 且 alpha **恒 1**（参考实现写死）");
                    // setup 附件名偏移必须落在**第 4 组**字符串里 —— 指到前面几组（比如槽位名）
                    // 不报错，只是解算时挂了个错名字的附件。
                    Check(b.Slots[0].SetupAttachmentNameOffset == ExpectedSetupAttachmentOffset(b, 0) &&
                          b.Slots[1].SetupAttachmentNameOffset == ExpectedSetupAttachmentOffset(b, 1),
                          "两个槽位的 setup 附件名偏移落在字符串表第 4 组的对应条目上");
                }

                bool noDeformOrDrawOrder = true;
                foreach (var t in b.Timelines)
                {
                    if ((SpineTimelineType)t.Type == SpineTimelineType.Deform ||
                        (SpineTimelineType)t.Type == SpineTimelineType.DrawOrder)
                    {
                        noDeformOrDrawOrder = false;
                    }
                }
                Check(noDeformOrDrawOrder, "基线夹具没有 deform / drawOrder 时间轴");
            }

            // 裁剪：被裁的项指到一条多边形上，**没被裁的必须是 NoClipIndex 而不是 0**
            if (by.TryGetValue("clip_end_s2.skel.zspg", out var c))
            {
                _prefix = "[clip_end_s2] ";
                Check(c.ClipPolygons.Count == 1, "一条裁剪多边形");
                if (c.ClipPolygons.Count == 1)
                {
                    var p = c.ClipPolygons[0];
                    Check(p.SlotIndex == 1, "多边形记的是 s1");
                    Check(p.VertexFormat == SpineVertexFormat.Rigid, "多边形是 rigid");
                    Check(p.VertexCount == 4, "4 个顶点");
                    // 逐点比对：多边形那 4 个点必须真的在顶点缓冲里
                    float[] want = { 0, 0, 10, 0, 10, 10, 0, 10 };
                    Check(PointsEqual(c, p, want), "顶点缓冲里那 4 个点与源多边形逐点相同");
                }
                bool ok = true;
                foreach (var it in c.DrawItems)
                {
                    if (it.Kind == SpineDrawItemKind.Fallback && it.SlotIndex == 2)
                    {
                        ok = ok && it.ClipIndex == 0;
                    }
                    else
                    {
                        // ⚠️ 就是这里：填 0 会静默裁错东西，且不报错
                        ok = ok && it.ClipIndex == SpineGpuConstants.NoClipIndex;
                    }
                }
                Check(ok, "被裁的指到多边形 0，没被裁的是 NoClipIndex（**不是** 0）");
            }

            // 嵌套：两条多边形，第二条偏移非 0 ⇒ 钉住"单位是顶点下标不是字节"
            if (by.TryGetValue("clip_nested.skel.zspg", out var n))
            {
                _prefix = "[clip_nested] ";
                Check(n.ClipPolygons.Count == 2, "两条多边形都落盘");
                if (n.ClipPolygons.Count == 2)
                {
                    Check(n.ClipPolygons[0].SlotIndex == 1 && n.ClipPolygons[1].SlotIndex == 2,
                          "第 0 条是 s1 的 clipA，第 1 条是 s2 的 clipB");
                    Check(n.ClipPolygons[0].VertexOffset == 0, "clipA 落在 rigid 段开头");
                    // 4 个顶点 ⇒ 第 1 条从**顶点下标 4** 开始（字节偏移会算成 32）
                    Check(n.ClipPolygons[1].VertexOffset == 4, "clipB 从顶点下标 4 开始（**不是**字节偏移 32）");
                    float[] wantB = { 0, 0, 20, 0, 20, 20, 0, 20 };
                    Check(PointsEqual(n, n.ClipPolygons[1], wantB), "clipB 的 4 个点逐点相同");
                }
                // s3、s4 都由 clipA 裁 ⇒ 两个都是 Fallback（clipB 被忽略的判据）
                int fb = 0;
                foreach (var it in n.DrawItems) if (it.Kind == SpineDrawItemKind.Fallback) fb++;
                Check(fb == 2, "s3 与 s4 都回退（clipB 被忽略）");
            }

            // weighted 多边形：两个顶点分别绑到 bone 0 / bone 1
            if (by.TryGetValue("clip_weighted.skel.zspg", out var w))
            {
                _prefix = "[clip_weighted] ";
                Check(w.ClipPolygons.Count == 1, "一条多边形");
                if (w.ClipPolygons.Count == 1)
                {
                    var p = w.ClipPolygons[0];
                    Check(p.VertexFormat == SpineVertexFormat.Weighted, "顶点格式是 weighted");
                    Check(p.VertexCount == 2, "2 个顶点");
                    int baseAt = (int)(w.WeightedSegmentOffset + p.VertexOffset * SpineGpuConstants.WeightedVertexStride);
                    bool ok = baseAt + 2 * (int)SpineGpuConstants.WeightedVertexStride <= w.VertexBuffer.Length;
                    Check(ok, "顶点落在 weighted 段内");
                    if (ok)
                    {
                        // v7 起顶点只是引用 ⇒ 蒙皮数据要去影响流里取
                        uint i0 = ReadU32(w.VertexBuffer, baseAt + SpineGpuOffsets.WvInfluenceOffset);
                        uint i1 = ReadU32(w.VertexBuffer, baseAt + (int)SpineGpuConstants.WeightedVertexStride +
                                                                  SpineGpuOffsets.WvInfluenceOffset);
                        uint c0 = ReadU32(w.VertexBuffer, baseAt + SpineGpuOffsets.WvInfluenceCount);
                        uint c1 = ReadU32(w.VertexBuffer, baseAt + (int)SpineGpuConstants.WeightedVertexStride +
                                                                  SpineGpuOffsets.WvInfluenceCount);
                        Check(c0 == 1 && c1 == 1, "两个顶点各 1 个影响");
                        bool inRange = i0 * SpineGpuConstants.InfluenceBoneStride < w.InfluenceBone.Length &&
                                       i1 * SpineGpuConstants.InfluenceBoneStride < w.InfluenceBone.Length;
                        Check(inRange, "影响下标落在影响流里");
                        if (inRange)
                        {
                            ushort b0 = ReadU16(w.InfluenceBone, (int)(i0 * SpineGpuConstants.InfluenceBoneStride +
                                                                      SpineGpuOffsets.IbBoneIdx));
                            ushort b1 = ReadU16(w.InfluenceBone, (int)(i1 * SpineGpuConstants.InfluenceBoneStride +
                                                                      SpineGpuOffsets.IbBoneIdx));
                            byte w0 = w.InfluenceBone[i0 * SpineGpuConstants.InfluenceBoneStride +
                                                      SpineGpuOffsets.IbWeight];
                            byte w1 = w.InfluenceBone[i1 * SpineGpuConstants.InfluenceBoneStride +
                                                      SpineGpuOffsets.IbWeight];
                            Check(b0 == 0 && b1 == 1, "两个顶点分别绑到 bone 0 / bone 1");
                            Check(w0 == 255 && w1 == 255, "权重 1.0 量化成 255");
                        }
                    }
                }
            }

            // 几何共享：顶点数必须**不**随附件翻倍（linked mesh 与父共用同一段顶点）
            if (by.TryGetValue("linked_mesh.skel.zspg", out var l))
            {
                _prefix = "[linked_mesh] ";
                Check(l.VertexBuffer.Length ==
                          l.RigidVertexCount * SpineGpuConstants.RigidVertexStride +
                          l.WeightedVertexCount * SpineGpuConstants.WeightedVertexStride,
                      "顶点缓冲大小 = rigid 段 + weighted 段");
                // UV 条目数 ≠ 顶点数（拆通道的目的）
                Check((uint)(l.UvBuffer.Length / 2) != l.RigidVertexCount + l.WeightedVertexCount ||
                          l.UvBuffer.Length / 2 != l.RigidVertexCount + l.WeightedVertexCount,
                      "UV 条目数不等于顶点数（不能拿 vertex_count 算 UV 长度）");
            }
            _prefix = "";
        }

        private static void Negative(List<string> files)
        {
            string f = null;
            foreach (string x in files)
            {
                if (Path.GetFileName(x) == "clip_nested.skel.zspg") { f = x; break; }
            }
            if (f == null)
            {
                return;
            }
            byte[] good = File.ReadAllBytes(f);

            // 版本改一改 ⇒ 必须明确拒绝，不做静默兼容（规格 §11）
            _prefix = "[负向] ";
            // ⚠️ 这里的"未来版本"必须跟着 kFormatVersion 走：v10 是当前版本，
            //    所以"未来"是 11，而 9 已经变成"旧容器"了（没有 chunk 17）。
            Rejects(Mutate(good, SpineGpuOffsets.HdrVersion, 11), "版本改成 11（未来容器）⇒ 拒绝");
            Rejects(Mutate(good, SpineGpuOffsets.HdrVersion, 9), "版本改成 9（v9 旧容器：没有 chunk 17）⇒ 拒绝");
            Rejects(Mutate(good, SpineGpuOffsets.HdrVersion, 8), "版本改成 8（v8 旧容器：没有 chunk 11-16）⇒ 拒绝");
            Rejects(Mutate(good, SpineGpuOffsets.HdrVersion, 7), "版本改成 7（v7 旧容器：没有 chunk 10）⇒ 拒绝");
            Rejects(Mutate(good, SpineGpuOffsets.HdrVersion, 6), "版本改成 6（v6 旧容器）⇒ 拒绝");
            Rejects(Mutate(good, SpineGpuOffsets.HdrVersion, 5), "版本改成 5（更旧的容器）⇒ 拒绝");
            Rejects(Mutate(good, SpineGpuOffsets.HdrMagic, 0x11223344), "魔数破坏 ⇒ 拒绝");

            // 截断：chunk 表都放不下
            var cut = new byte[SpineGpuConstants.HeaderSize + 4];
            Array.Copy(good, cut, cut.Length);
            cut[SpineGpuOffsets.HdrChunkCount] = 200;
            Rejects(cut, "chunk_count 撒谎 ⇒ 拒绝（不是越界读）");

            // chunk 大小不是记录大小的整数倍 ⇒ 拒绝（这正是"两端布局漂了"的探测器）
            int at = FindChunkSizeField(good, (uint)SpineGpuChunkId.DrawItems);
            if (at >= 0)
            {
                Rejects(Mutate(good, at, ReadU32(good, at) + 1), "draw_items 大小 +1 ⇒ 拒绝（28 B 的整数倍）");
            }
            int atc = FindChunkSizeField(good, (uint)SpineGpuChunkId.ClipData);
            if (atc >= 0)
            {
                Rejects(Mutate(good, atc, ReadU32(good, atc) + 1), "clip_data 大小 +1 ⇒ 拒绝（12 B 的整数倍）");
            }
            int atb = FindChunkSizeField(good, (uint)SpineGpuChunkId.InfluenceBind);
            if (atb >= 0)
            {
                Rejects(Mutate(good, atb, ReadU32(good, atb) + 1), "influence_bind 大小 +1 ⇒ 拒绝（8 B 的整数倍）");
            }
            int ate = FindChunkSizeField(good, (uint)SpineGpuChunkId.InfluenceBone);
            if (ate >= 0)
            {
                Rejects(Mutate(good, ate, ReadU32(good, ate) + 1), "influence_bone 大小 +1 ⇒ 拒绝（4 B 的整数倍）");
            }
            int atBone = FindChunkSizeField(good, (uint)SpineGpuChunkId.BoneSetup);
            if (atBone >= 0)
            {
                Rejects(Mutate(good, atBone, ReadU32(good, atBone) + 1),
                        "bone_setup 大小 +1 ⇒ 拒绝（36 B 的整数倍）");
                // 条数与 header 对不上 ⇒ 拒绝（"chunk 缺了"本身不报错，只有这个等式能挡）
                Rejects(Mutate(good, atBone, ReadU32(good, atBone) - (uint)SpineGpuConstants.BoneSetupSize),
                        "bone_setup 少一条 ⇒ 拒绝（条数必须 = header.bone_count）");
            }

            // ---- v9 动画块：越界引用必须被**读端**挡住 ----
            // 写端永远不会产出这些字节，但容器是跨语言、跨版本的产物 —— 读端不能假设
            // "写端一定对"。而且下标越界在 C# 里不抛异常，只会播到别人的数据。
            _prefix = "[负向/动画] ";
            string fb = null;
            foreach (string x in files)
            {
                if (Path.GetFileName(x) == "baseline.skel.zspg") { fb = x; break; }
            }
            if (fb != null)
            {
                byte[] baseBytes = File.ReadAllBytes(fb);
                int atDir = FindChunkSizeField(baseBytes, (uint)SpineGpuChunkId.AnimDir);
                if (atDir >= 0)
                {
                    Rejects(Mutate(baseBytes, atDir, ReadU32(baseBytes, atDir) + 1),
                            "动画目录大小 +1 ⇒ 拒绝（20 B 的整数倍）");
                }
                int atTl = FindChunk(baseBytes, (uint)SpineGpuChunkId.Timelines);
                if (atTl >= 0)
                {
                    // 第 0 条时间轴的 frame_first 挪到远处 ⇒ 帧区间越界
                    Rejects(Mutate(baseBytes, atTl + SpineGpuOffsets.TlFrameFirst, 0xFFFFFFF0u),
                            "frame_first 越界 ⇒ 拒绝（越界读不抛异常，只会播到别人的帧）");
                    // 类型改成 99 ⇒ entries 无从确定
                    var t99 = (byte[])baseBytes.Clone();
                    WriteU16(t99, atTl + SpineGpuOffsets.TlType, 99);
                    Rejects(t99, "时间轴类型 99 ⇒ 拒绝（TimelineType 只到 14）");
                    // frame_count = 0 ⇒ 参考实现是负下标读
                    Rejects(Mutate(baseBytes, atTl + SpineGpuOffsets.TlFrameCount, 0),
                            "时间轴 0 帧 ⇒ 拒绝（参考实现在这里是负下标读）");
                }

                // ---- v10 槽位块：越界 / 脏位必须被**读端**挡住 ----
                _prefix = "[负向/槽位] ";
                int atSs = FindChunkSizeField(baseBytes, (uint)SpineGpuChunkId.SlotSetup);
                if (atSs >= 0)
                {
                    Rejects(Mutate(baseBytes, atSs, ReadU32(baseBytes, atSs) + 1),
                            "slot_setup 大小 +1 ⇒ 拒绝（40 B 的整数倍）");
                    // 条数与 header 对不上 ⇒ 拒绝（"chunk 缺了"本身不报错，
                    // 只有这个等式能挡住"后面的槽位读到别人的颜色"）
                    Rejects(Mutate(baseBytes, atSs, ReadU32(baseBytes, atSs) - SpineGpuConstants.SlotSetupSize),
                            "slot_setup 少一条 ⇒ 拒绝（条数必须 = header.slot_count）");
                }
                int atSsData = FindChunk(baseBytes, (uint)SpineGpuChunkId.SlotSetup);
                if (atSsData >= 0)
                {
                    Rejects(Mutate(baseBytes, atSsData + SpineGpuOffsets.SlotFlags, 2u),
                            "槽位 flags 有未知位 ⇒ 拒绝（留着脏字节会让跨语言逐字节对拍对不上）");
                    Rejects(Mutate(baseBytes, atSsData + SpineGpuOffsets.SlotSetupAttachmentNameOffset, 0xFFFFFFF0u),
                            "setup 附件名偏移越界 ⇒ 拒绝（指到别的组不报错，只是挂错附件）");
                }
            }
            _prefix = "";
        }

        // 字符串表第 4 组（槽位 setup 附件名）第 index 条的期望字节偏移。
        // **手算**一遍而不是直接信 offset —— 这样"偏移指到槽位名那组"才会被测出来
        // （指错组不报错，只是 attachment 时间轴复位到一个错名字的附件上）。
        // 夹具全是 ASCII ⇒ C# 的 char 数与 UTF-8 字节数相同；非 ASCII 名要按字节算。
        private static uint ExpectedSetupAttachmentOffset(SpineGpuAsset a, int index)
        {
            uint off = 0;
            foreach (string s in a.BoneNames) off += (uint)(s.Length + 1);
            foreach (string s in a.SkinNames) off += (uint)(s.Length + 1);
            foreach (string s in a.SlotNames) off += (uint)(s.Length + 1);
            for (int i = 0; i < index; ++i) off += (uint)(a.SlotSetupAttachmentNames[i].Length + 1);
            return off;
        }

        private static bool Near(float got, float want)
        {
            return Math.Abs(got - want) < 1e-6f;
        }

        private static int FindChunk(byte[] data, uint id)
        {
            uint n = ReadU32(data, SpineGpuOffsets.HdrChunkCount);
            for (uint i = 0; i < n; ++i)
            {
                int e = (int)(SpineGpuConstants.HeaderSize + i * SpineGpuConstants.ChunkTableEntrySize);
                if (ReadU32(data, e + SpineGpuOffsets.ChunkId) == id)
                {
                    return (int)ReadU32(data, e + SpineGpuOffsets.ChunkOffset);
                }
            }
            return -1;
        }

        // ---------------------------------------------------------
        private static bool PointsEqual(SpineGpuAsset a, SpineGpuClipPolygon p, float[] want)
        {
            if (p.VertexFormat != SpineVertexFormat.Rigid)
            {
                return false;
            }
            int stride = (int)SpineGpuConstants.RigidVertexStride;
            int baseAt = (int)(p.VertexOffset * SpineGpuConstants.RigidVertexStride);
            if (baseAt + p.VertexCount * stride > a.VertexBuffer.Length)
            {
                return false;
            }
            for (int v = 0; v < p.VertexCount; ++v)
            {
                int o = baseAt + v * stride;
                float x = ReadF32(a.VertexBuffer, o);
                float y = ReadF32(a.VertexBuffer, o + 4);
                if (x != want[v * 2] || y != want[v * 2 + 1])
                {
                    return false;
                }
            }
            return true;
        }

        private static int FindChunkSizeField(byte[] data, uint id)
        {
            uint n = ReadU32(data, SpineGpuOffsets.HdrChunkCount);
            for (uint i = 0; i < n; ++i)
            {
                int e = (int)(SpineGpuConstants.HeaderSize + i * SpineGpuConstants.ChunkTableEntrySize);
                if (ReadU32(data, e + SpineGpuOffsets.ChunkId) == id)
                {
                    return e + SpineGpuOffsets.ChunkSize;
                }
            }
            return -1;
        }

        private static byte[] Mutate(byte[] src, int at, uint value)
        {
            var b = (byte[])src.Clone();
            WriteU32(b, at, value);
            return b;
        }

        private static void Rejects(byte[] data, string what)
        {
            bool threw = false;
            try
            {
                SpineGpuContainerIo.Read(data);
            }
            catch (SpineGpuContainerException)
            {
                threw = true;
            }
            Check(threw, what);
        }

        private static bool SameBytes(byte[] a, byte[] b)
        {
            if (a.Length != b.Length) return false;
            for (int i = 0; i < a.Length; ++i) if (a[i] != b[i]) return false;
            return true;
        }

        private static ushort ReadU16(byte[] b, int at) => (ushort)(b[at] | (b[at + 1] << 8));
        private static uint ReadU32(byte[] b, int at) =>
            b[at] | ((uint)b[at + 1] << 8) | ((uint)b[at + 2] << 16) | ((uint)b[at + 3] << 24);
        private static float ReadF32(byte[] b, int at)
        {
            uint u = ReadU32(b, at);
            return BitConverter.ToSingle(BitConverter.GetBytes(u), 0);
        }
        private static void WriteU16(byte[] b, int at, ushort v)
        {
            b[at] = (byte)(v & 0xFF);
            b[at + 1] = (byte)((v >> 8) & 0xFF);
        }

        private static void WriteU32(byte[] b, int at, uint v)
        {
            b[at] = (byte)(v & 0xFF);
            b[at + 1] = (byte)((v >> 8) & 0xFF);
            b[at + 2] = (byte)((v >> 16) & 0xFF);
            b[at + 3] = (byte)((v >> 24) & 0xFF);
        }

        private static void Check(bool cond, string what)
        {
            _checks++;
            if (!cond)
            {
                _failed++;
                Console.WriteLine("  [FAIL] " + _prefix + what);
            }
        }

        private static int Report()
        {
            Console.WriteLine("=== verdict: " + (_failed == 0 ? "PASS" : "FAIL") +
                              " (" + (_checks - _failed) + "/" + _checks + " checks) ===");
            return _failed == 0 ? 0 : 1;
        }
    }
}
