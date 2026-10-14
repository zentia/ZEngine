"""
故障模式库。

这是整个故障实验室的核心资产：**每一条模式都是一个已知根因**。
正因为根因已知且精确，用它生成样本时 ground truth 是免费且可靠的。

分类原则（对应崩溃根因定位里最常见的几类误判）：

  null_deref      空指针解引用。检测点常不等于根因点。
  oob_write       写越界。破坏发生点与被检测到点通常相距很远。
  use_after_free  释放后使用。
  double_free     重复释放。
  race            数据竞争。非确定性，同一输入多次运行结果不同。
  overflow        整数溢出导致长度计算错误（如 size_t 下溢）。
  stack_overflow  栈溢出。
  leak            内存泄漏（不崩溃，用于负样本）。

关键设计：**每条模式带 `detect_hint`**，说明「agent 应该在哪里找到答案，
以及哪里是陷阱」。这是把领域知识形式化的地方，也是 eval 能判定
「是真懂还是蒙对」的依据。
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class FaultPattern:
    id: str
    category: str
    title: str
    source: str                    # 完整可编译的 C++ 源
    root_cause_line_marker: str    # 源码中标记根因行的注释锚点
    expected_category: str         # 期望 agent 判定的类别
    detect_hint: str               # 给 eval 的判定提示
    root_cause_anchor: str = ""    # 标记行必须额外包含的子串（源码里有多个标记时区分）
    deterministic: bool = True     # 是否每次必崩
    platforms: tuple[str, ...] = ("windows", "linux", "macos")
    notes: str = ""

    def _marker_match(self, line: str) -> bool:
        """
        判断某行是否为根因标记行。

        root_cause_anchor 用于源码里存在多个 <FAULT> 的情形。
        典型例子 oob_write：CopyRegion（有检查）和 CopyRegionUnchecked
        （无检查）里各有一个同样的下溢标记，而 main 只调用后者。
        若取第一个匹配，ground truth 就落在**从不执行的死代码**里 ——
        agent 就算完美定位到真正的下溢点，eval 也会判它错。
        这种标签失真会让好算法看起来不行，比算法缺陷更难发现。
        """
        if self.root_cause_line_marker not in line:
            return False
        if self.root_cause_anchor and self.root_cause_anchor not in line:
            return False
        return True

    def root_cause_line(self) -> int:
        """从源码里定位根因行（1-based）。找不到返回 -1，不猜。"""
        for i, line in enumerate(self.source.splitlines(), 1):
            if self._marker_match(line):
                return i
        return -1

    def root_cause_line_in(self, text: str) -> int:
        """
        在任意文本（如注入后的源码）里定位根因行。

        与 root_cause_line 共用同一判据，避免两处逻辑漂移 ——
        上轮「行号未重算」就是因为注入后行号变了而标签没跟着变。
        """
        for i, line in enumerate(text.splitlines(), 1):
            if self._marker_match(line):
                return i
        return -1


# ---------------------------------------------------------------- 模式定义
#
# 源码刻意保持「看起来像正常业务代码」，而不是一眼就知道有 bug 的玩具。
# 理由：agent 要面对的是真实代码库，注入太明显会让 eval 失去区分度。

NULL_DEREF = FaultPattern(
    id="null_deref_01",
    category="null_deref",
    title="工厂函数返回空指针，调用方未校验",
    root_cause_line_marker="// <FAULT>",
    # 源码里有两个同样的下溢标记：CopyRegion（有检查）和 CopyRegionUnchecked
    # （无检查）。main 只调用后者 —— 不指定 anchor 的话 ground truth 会
    # 落在从不执行的死代码里，agent 定位对了也会被判 MISS。

    source=r'''
// 场景：资源加载失败返回空，调用方直接解引用。
// 陷阱：崩溃点在 WriteMetadata，但真正的根因在调用方没做空判断。
#include <cstdio>
#include <cstring>
#include <string>

struct AssetMetadata {
    int id;
    char name[64];
};

// 模拟资源查找：找不到就返回 nullptr
AssetMetadata* FindMetadata(int id) {
    static AssetMetadata cache[4];
    if (id < 0 || id >= 4) {
        return nullptr;
    }
    return &cache[id];
}

void WriteMetadata(AssetMetadata* meta, const char* name) {
    meta->id = 1;  // <FAULT> 崩溃点：meta 可能为空
    std::strncpy(meta->name, name, sizeof(meta->name) - 1);
}

int LoadAndApply(int id, const char* name) {
    // 调用方没有校验返回值
    AssetMetadata* meta = FindMetadata(id);
    WriteMetadata(meta, name);
    return meta->id;
}

int main(int argc, char**) {
    return LoadAndApply(argc > 1 ? 100 : 99, "default");
}
''',
    expected_category="null_deref",
    detect_hint=(
        "崩溃栈顶点是 WriteMetadata，但该函数本身对入参没有约定。"
        "真正的根因在 LoadAndApply —— 它没有校验 FindMetadata 的返回值。"
        "如果 agent 只报 WriteMetadata，属于定位到检测点而非根因点（部分正确）。"
    ),
    deterministic=True,
)

OOB_WRITE = FaultPattern(
    id="oob_write_01",
    category="oob_write",
    title="长度计算下溢导致写越界",
    root_cause_line_marker="// <FAULT>",
    # 源码里有两个同样的下溢标记：CopyRegion（有检查）和 CopyRegionUnchecked
    # （无检查）。main 只调用后者 —— 不指定 anchor 的话 ground truth 会
    # 落在从不执行的死代码里，agent 定位对了也会被判 MISS。
    root_cause_anchor="同样的下溢",
    source=r'''
// 场景：从数据档读出 end/start，算长度时 end < start 导致 size_t 下溢。
// 陷阱：崩溃点（memset / 后续 free）离破坏发生点很远，且不是每次必崩。
//
// 注意这里刻意保留了一个「看起来有防御」的检查：
//   if (len > dstCap) return;
// 下溢后 len 变成接近 SIZE_MAX 的巨大正数，这个检查确实会触发 return，
// 于是不崩 —— 这正是真实工程里这类 bug 能潜伏很久的原因。
// 所以本样本用 dstCap 传 0 的方式让下溢路径真正执行写入，保证可复现。
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstddef>

struct Region { size_t start; size_t end; };

// 模拟从数据档读出的区间（真实场景里这里可能读到异常值）
Region ReadRegion(int idx) {
    Region r;
    // idx 为奇数时制造 end < start 的非法区间
    r.start = 1000;
    r.end = (idx % 2 == 0) ? 1200 : 400;
    return r;
}

void CopyRegion(char* dst, size_t dstCap, int idx) {
    Region r = ReadRegion(idx);
    size_t len = r.end - r.start;   // <FAULT> 下溢：400-1000 回绕成巨大值
    if (len > dstCap) {
        // 看上去是防御，实际下溢后反而依赖它才不崩
        return;
    }
    std::memset(dst, 0xAB, len);    // 越界写
}

// 绕过检查的路径：调用方用自己的长度，不再比对
void CopyRegionUnchecked(char* dst, int idx) {
    Region r = ReadRegion(idx);
    size_t len = r.end - r.start;   // <FAULT> 同样的下溢，但无检查
    std::memset(dst, 0xAB, len);
}

int main(int argc, char**) {
    char* buf = (char*)std::malloc(64);
    CopyRegionUnchecked(buf, 1);    // 必崩路径
    std::free(buf);
    return 0;
}
''',
    expected_category="oob_write",
    detect_hint=(
        "崩溃点在 memset，根因在 r.end - r.start 的 size_t 下溢。"
        "关键洞察：CopyRegion 里 len > dstCap 的检查看似有防御，"
        "但下溢后 len 极大，检查会直接 return 掩盖问题；"
        "CopyRegionUnchecked 是同一缺陷没有检查的版本。"
        "如果 agent 说『有边界检查所以没问题』，属于误判 —— "
        "它没意识到检查本身依赖了被污染的 len。"
    ),
    deterministic=True,
)

USE_AFTER_FREE = FaultPattern(
    id="uaf_01",
    category="use_after_free",
    title="对象释放后仍通过缓存指针访问",
    root_cause_line_marker="// <FAULT>",
    # 源码里有两个同样的下溢标记：CopyRegion（有检查）和 CopyRegionUnchecked
    # （无检查）。main 只调用后者 —— 不指定 anchor 的话 ground truth 会
    # 落在从不执行的死代码里，agent 定位对了也会被判 MISS。

    source=r'''
// 场景：对象被销毁，但全局缓存里的裸指针没有清掉。
// 陷阱：崩溃点在使用处，根因在生命周期管理（销毁时未清理引用者）。
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct GameObject {
    int hp;
    char tag[32];
};

GameObject* g_cached = nullptr;

void DestroyObject(GameObject* obj) {
    std::free(obj);
    // 注意：这里没有把 g_cached 置空
}

void SpawnAndDestroy() {
    GameObject* obj = (GameObject*)std::malloc(sizeof(GameObject));
    obj->hp = 100;
    std::strncpy(obj->tag, "player", sizeof(obj->tag) - 1);
    g_cached = obj;
    DestroyObject(obj);
}

int TouchCached() {
    return g_cached->hp;  // <FAULT> 悬垂指针访问
}

int main() {
    SpawnAndDestroy();
    return TouchCached();
}
''',
    expected_category="use_after_free",
    detect_hint=(
        "崩溃点是 TouchCached，但根因是 DestroyObject 未清理 g_cached。"
        "良好的分析应该指出『修复点在销毁侧而非使用侧』，"
        "只在 TouchCached 加空判断是治标。"
        "注意：本样本在小对象 + 立即复用的情况下可能不崩（返回 100），"
        "因为 free 后的内存尚未被回收复用 —— 这正是 UAF 难以复现的原因。"
    ),
    deterministic=False,
    notes="不保证崩溃：free 后内存可能未被复用，读到旧值。",
)

DOUBLE_FREE = FaultPattern(
    id="double_free_01",
    category="double_free",
    title="异常路径与正常路径重复释放",
    root_cause_line_marker="// <FAULT>",
    # 源码里有两个同样的下溢标记：CopyRegion（有检查）和 CopyRegionUnchecked
    # （无检查）。main 只调用后者 —— 不指定 anchor 的话 ground truth 会
    # 落在从不执行的死代码里，agent 定位对了也会被判 MISS。

    source=r'''
// 场景：正常路径释放一次，错误分支又释放一次。
// 陷阱：两条路径在不同分支上，静态看代码容易只看到其中一条。
#include <cstdio>
#include <cstdlib>
#include <cstring>

char* LoadBuffer(int mode) {
    char* buf = (char*)std::malloc(128);
    if (!buf) return nullptr;
    std::strncpy(buf, "data", 127);
    if (mode == 1) {
        std::free(buf);   // 错误分支提前释放
        return nullptr;
    }
    return buf;
}

int Process(int mode) {
    char* buf = LoadBuffer(mode);
    if (!buf) {
        return -1;
    }
    std::printf("%s\n", buf);
    std::free(buf);
    if (mode == 2) {
        std::free(buf);   // <FAULT> 重复释放
    }
    return 0;
}

int main(int argc, char**) {
    // 默认走 mode=2，确保重复释放路径被执行
    return Process(argc > 2 ? 3 : 2);
}
''',
    expected_category="double_free",
    detect_hint=(
        "需要从 mode 的取值反推哪些分支会走到。 "
        "agent 若只看 Process 会漏掉 LoadBuffer 里的提前释放。"
    ),
    deterministic=True,
)

INT_OVERFLOW = FaultPattern(
    id="overflow_01",
    category="overflow",
    title="元素数量与大小相乘溢出",
    root_cause_line_marker="// <FAULT>",
    # 源码里有两个同样的下溢标记：CopyRegion（有检查）和 CopyRegionUnchecked
    # （无检查）。main 只调用后者 —— 不指定 anchor 的话 ground truth 会
    # 落在从不执行的死代码里，agent 定位对了也会被判 MISS。

    source=r'''
// 场景：count * sizeof(T) 溢出，分配到的内存远小于后续写入量。
// 陷阱：malloc 成功返回，崩溃发生在后面的循环写入里。
//
// **乘法必须发生在 32 位宽度上，才有可能溢出。**
// 曾经写成 `(size_t)count * sizeof(BigEntry)`：
// count 是 int（上限 2^31），先提升到 64 位 size_t 再乘，
// 乘积上限约 2^39 —— 在 64 位下**永远不可能溢出**。
// 实测结果是申请 2GB 失败、malloc 返回 NULL，程序走 return -1 退出，
// 连循环都没进：一个"应该崩"的样本安静地返回了 -1。
//
// 真实的整数溢出漏洞形态是**窄宽度乘法先溢出，再赋给宽类型** ——
// 先提升再乘恰好绕开了它。所以这里用 unsigned int 承载乘积，
// 溢出后再交给 malloc 提升为 size_t。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

struct BigEntry { char payload[256]; };

BigEntry* AllocateTable(int count) {
    // 32 位乘法溢出：16777220 * 256 回绕成 1024（仅 4 个条目）
    unsigned int bytes32 = (unsigned int)count * (unsigned int)sizeof(BigEntry);  // <FAULT>
    return (BigEntry*)std::malloc(bytes32);
}

int FillTable(int count) {
    BigEntry* table = AllocateTable(count);
    if (!table) return -1;
    for (int i = 0; i < count; ++i) {   // 写远超实际分配的大小
        std::memset(&table[i], 0xCD, sizeof(BigEntry));
    }
    std::free(table);
    return 0;
}

int main(int argc, char**) {
    // 16777220 * 256 = 4294967680，在 32 位下回绕成 1024
    int count = 16777220;
    return FillTable(count);
}
''',
    expected_category="overflow",
    detect_hint=(
        "崩溃在循环写入，根因在 AllocateTable 的乘法。"
        "关键点：malloc 并不会失败（乘法溢出后 bytes 很小），"
        "所以『检查 malloc 返回值』这个常见建议在这里无效。"
    ),
    deterministic=False,
)

STACK_OVERFLOW = FaultPattern(
    id="stack_overflow_01",
    category="stack_overflow",
    title="无终止条件的递归",
    root_cause_line_marker="// <FAULT>",
    # 源码里有两个同样的下溢标记：CopyRegion（有检查）和 CopyRegionUnchecked
    # （无检查）。main 只调用后者 —— 不指定 anchor 的话 ground truth 会
    # 落在从不执行的死代码里，agent 定位对了也会被判 MISS。

    source=r'''
// 场景：递归缺少终止条件（或终止条件在某些输入下永不满足）。
// 陷阱：栈溢出常被误判成堆问题，要看栈帧是否重复出现同一函数。
#include <cstdio>

// 用 volatile 累加，阻止编译器把递归优化成循环。
// 这一条很重要：早期版本用 `return Evaluate(depth+1, limit) + 1;`，
// MSVC 在 /O2 下直接把尾递归转成了循环，程序正常退出 —— 故障模式形同虚设。
volatile long g_sink = 0;

int Evaluate(int depth, int limit) {
    // 每次递归消耗 32KB 栈，默认 1MB 栈约 32 层就溢出 —— 快且确定。
    char local[32768];
    local[0] = (char)depth;
    local[32767] = (char)(depth >> 8);
    g_sink += local[0] + local[32767];

    // 终止条件只对合法的 limit 生效；limit 为负时永不满足，于是无限递归。
    //
    // 这里踩过一个逻辑坑：最初写成 `if (depth > limit) return 0;`
    // 配合 main 里 limit=-1，本意是「条件永不满足」，
    // 实际 0 > -1 恒真，函数第一次调用就直接返回，根本没有递归，
    // 程序正常退出，故障模式名存实亡。验证时它 1 秒 rc=0 就是这个原因。
    if (limit >= 0 && depth >= limit) {
        return 0;
    }
    return Evaluate(depth + 1, limit) + 1;  // <FAULT> 无限递归
}

int main(int argc, char**) {
    return Evaluate(0, -1);
}
''',
    expected_category="stack_overflow",
    detect_hint=(
        "判据是栈上同一函数重复出现成百上千次。"
        "若 agent 报成堆问题或空指针，属于类别误判。"
    ),
    deterministic=True,
)

RACE = FaultPattern(
    id="race_01",
    category="race",
    title="两个线程无保护地累加同一计数器",
    root_cause_line_marker="// <FAULT>",
    # 源码里有两个同样的下溢标记：CopyRegion（有检查）和 CopyRegionUnchecked
    # （无检查）。main 只调用后者 —— 不指定 anchor 的话 ground truth 会
    # 落在从不执行的死代码里，agent 定位对了也会被判 MISS。

    source=r'''
// 场景：多线程无锁累加，结果不确定。
// 陷阱：这类问题同输入多次运行结果不同，是稳定性测试的好样本。
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>

long g_counter = 0;   // 非原子

void Worker(int iterations) {
    for (int i = 0; i < iterations; ++i) {
        g_counter++;   // <FAULT> 无同步
    }
}

int main() {
    const int kThreads = 4;
    const int kIter = 200000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back(Worker, kIter);
    }
    for (auto& th : ts) th.join();

    long expected = (long)kThreads * kIter;
    std::printf("counter=%ld expected=%ld\n", g_counter, expected);
    return g_counter == expected ? 0 : 1;
}
''',
    expected_category="race",
    detect_hint=(
        "这个样本不崩，只是结果不对（退出码非 0）。"
        "它检验 agent 能否识别『非崩溃类的并发缺陷』，"
        "以及是否会因为『没有崩溃』就断言代码没问题。"
    ),
    deterministic=False,
    notes="不崩溃，靠退出码判定。适合做负样本或稳定性样本。",
)

LEAK = FaultPattern(
    id="leak_01",
    category="leak",
    title="早退分支未释放已分配资源",
    root_cause_line_marker="// <FAULT>",
    # 源码里有两个同样的下溢标记：CopyRegion（有检查）和 CopyRegionUnchecked
    # （无检查）。main 只调用后者 —— 不指定 anchor 的话 ground truth 会
    # 落在从不执行的死代码里，agent 定位对了也会被判 MISS。

    source=r'''
// 场景：中间步骤失败时提前返回，前面分配的资源泄漏。
// 陷阱：不崩溃、不报错，最容易被『没有崩溃就没问题』的说法漏掉。
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct Context { char* a; char* b; };

int BuildContext(Context* ctx, int failAt) {
    ctx->a = (char*)std::malloc(64);
    if (failAt == 1) {
        return -1;   // <FAULT> 泄漏 ctx->a
    }
    ctx->b = (char*)std::malloc(64);
    if (failAt == 2) {
        return -2;   // <FAULT> 泄漏 ctx->a 和 ctx->b
    }
    return 0;
}

void FreeContext(Context* ctx) {
    std::free(ctx->a);
    std::free(ctx->b);
}

int main(int argc, char**) {
    Context ctx {};
    int rc = BuildContext(&ctx, argc > 1 ? 1 : 0);
    if (rc == 0) FreeContext(&ctx);
    return rc;
}
''',
    expected_category="leak",
    detect_hint=(
        "同样不崩溃。用于检验 agent 是否会误报『无问题』。"
        "若样本集里全是崩溃类，agent 学会的只是『找崩溃』而不是『找缺陷』。"
    ),
    deterministic=True,
    notes="不崩溃，作负样本使用。",
)


# **为什么需要一个「没有缺陷」的样本**
#
# overflow 被修好之后，8 个样本全部成了正样本 ——
# 负样本归零，honesty（负样本上的克制率）这个最关键的指标
# 因为除零直接变成 0%，失去意义。
#
# 而没有负样本的评估是危险的：它只测"能不能找到问题"，
# 不测"会不会在没问题的代码上编造问题"。一个对所有输入都报
# "找到根因"的 agent，在这样的评估里能拿满分。
#
# 所以这里加一段**确实没有缺陷**的代码。它刻意带上
# 乘法与分配（和 overflow 同形），但宽度正确、无溢出 ——
# 若规则把宽度判据漏掉，就会在这段干净代码上误报。
# 规则能被干净代码推翻，才算真的在识别，而不是在猜。
CLEAN = FaultPattern(
    id="clean_01",
    category="clean",
    title="正确分配且正确释放（无缺陷）",
    root_cause_line_marker="// <NOFAULT>",
    source=r'''
// 场景：一段**没有缺陷**的分配与释放。
// 它刻意带上乘法与分配（与 overflow 同形），但宽度正确、无溢出。
// 用来检验规则是否会在干净代码上误报。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

struct Entry { char payload[256]; };

Entry* MakeTable(int count) {
    // size_t 是 64 位，先提升再乘，count 为 int 时不会溢出
    size_t bytes = (size_t)count * sizeof(Entry);  // <NOFAULT> 宽度正确
    return (Entry*)std::malloc(bytes);
}

int UseTable(int count) {
    Entry* table = MakeTable(count);
    if (!table) return -1;
    for (int i = 0; i < count; ++i) {
        std::memset(&table[i], 0xCD, sizeof(Entry));
    }
    std::free(table);
    return 0;
}

int main(int argc, char**) {
    int count = 64;   // 小到不可能溢出
    return UseTable(count);
}
''',
    expected_category="clean",
    detect_hint=(
        "这段代码没有问题。正确行为是**不给结论**或置信度很低。"
        "若 agent 在这里也报出根因，说明规则在猜，不是在识别。"
    ),
    deterministic=True,
    notes="负样本：无缺陷。测的是会不会误报。",
)


# ---------------------------------------------------------------- 注册表

PATTERNS: dict[str, FaultPattern] = {
    p.id: p for p in (
        NULL_DEREF, OOB_WRITE, USE_AFTER_FREE, DOUBLE_FREE,
        INT_OVERFLOW, STACK_OVERFLOW, RACE, LEAK, CLEAN,
    )
}


def all_patterns() -> list[FaultPattern]:
    return list(PATTERNS.values())


def by_category(cat: str) -> list[FaultPattern]:
    return [p for p in PATTERNS.values() if p.category == cat]


def get(pid: str) -> FaultPattern | None:
    return PATTERNS.get(pid)
