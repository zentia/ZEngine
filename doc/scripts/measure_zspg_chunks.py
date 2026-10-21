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


def Mb(n):
    return "%.2f MB" % (n / 1048576.0)


def main():
    if len(sys.argv) < 2:
        print("用法：python measure_zspg_chunks.py <容器目录>")
        return 1

    root = sys.argv[1]
    sizes = collections.Counter()
    files = 0
    rigid_vertices = 0
    weighted_vertices = 0

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
            nchunk = struct.unpack_from("<I", d, kOffChunkCount)[0]
            for i in range(nchunk):
                cid, _, size = struct.unpack_from("<III", d, kHeaderSize + i * kChunkEntrySize)
                sizes[cid] += size

    if files == 0:
        print("目录里没有 .zspg：" + root)
        return 1

    influences = sizes[22] // 4  # chunk 22 是 4 B/条
    print("容器数        : %d" % files)
    print("rigid 顶点    : %d" % rigid_vertices)
    print("weighted 顶点 : %d" % weighted_vertices)
    print("影响条目      : %d  (= chunk22 / 4)" % influences)
    if weighted_vertices:
        print("均值影响数    : %.3f" % (influences / float(weighted_vertices)))
    print()

    print("chunk 3  顶点      : %s  (rigid 8B + weighted 8B 引用)" % Mb(sizes[3]))
    print("chunk 21 bindPos   : %s" % Mb(sizes[21]))
    print("chunk 22 bone/w    : %s" % Mb(sizes[22]))
    print("  => 蒙皮合计      : %s" % Mb(sizes[3] + sizes[21] + sizes[22]))
    print("chunk 4  UV        : %s" % Mb(sizes[4]))
    print("chunk 5  索引      : %s" % Mb(sizes[5]))
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
