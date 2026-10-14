"""
故障探测器：注入到故障样本里，用于在崩溃瞬间抓取现场。

**为什么需要它**

要拿到崩溃指令地址与调用链，有几种做法：
  1. 生成 minidump：需要额外工具或自己调 MiniDumpWriteDump，平台相关
  2. 解析 WER 弹框文本：那是 GUI 字符串，格式随版本变，且需 GUI 交互
  3. 在进程内挂向量化异常处理，崩溃瞬间自己抓 —— 本模块采用

选 3 的理由：零外部依赖、确定性强。代价是源码要多一段探测代码，
但这段是机械注入的，且 ground truth 会基于注入后的源码重算。

**五个必须守住的点（每一条都是实测踩出来的）**

1) 不能改变崩溃语义。处理器返回 EXCEPTION_CONTINUE_SEARCH，
   让进程仍然真的崩溃退出。吞掉异常会让样本失效 ——
   这跟「注入的缺陷根本没触发」是同一类错误，只是更隐蔽。

2) 必须用 RtlVirtualUnwind 手写展开，不能用 dbghelp。
   最初用 StackWalk64 + SymInitialize，结果两个模式直接失效：
   oob_write 超时（60 秒不返回），stack_overflow 无任何输出。
   根因是 SymInitialize 在崩溃上下文里加载符号会死锁 ——
   栈溢出时栈已耗尽，根本没有余量给符号加载用。
   改成 RtlLookupFunctionEntry + RtlVirtualUnwind（纯 ntdll，零初始化）后正常。
   教训：**崩溃 handler 里不能做任何可能分配内存或加载文件的操作**。

3) 不能用 RtlCaptureStackBackTrace。在 VEH handler 里它抓的是
   异常分发路径自己的栈，第一帧永远是 handler 自身，穿透不到崩溃点。

4) 栈回溯不可全信。越界写会污染栈：实测第 3 帧之后全是
   0xabababababababab，正是我们的填充字节。盲目 trust 会符号化出
   完全虚假的调用链。所以逐帧校验，任何一帧不通过就停止采信。

5) 检测点 ≠ 破坏点。越界写崩在 CRT 的 memset，栈溢出崩在
   chkstk.asm 的 _alloca_probe，堆破坏崩在 ntdll。
   这些帧都不在项目代码中 —— 只靠崩溃 IP 永远定位不到根因，
   必须往栈深处找第一个属于项目代码的帧。这是本模块存在的全部意义。
"""

from __future__ import annotations

import re

WINDOWS_PROBE = r'''
#include <windows.h>
#include <winternl.h>
#include <intrin.h>     // _ReturnAddress()
#include <cstdio>
#include <cstdint>
#include <cstring>

extern "C" NTSYSAPI PRUNTIME_FUNCTION NTAPI RtlLookupFunctionEntry(
    DWORD64 ControlPc, PDWORD64 ImageBase, PUNWIND_HISTORY_TABLE HistoryTable);
// RtlVirtualUnwind 已在 winnt.h 中声明（返回 PEXCEPTION_ROUTINE）。
// 不要自己再声明一遍：只按返回类型不同会被判为重载冲突（C2556）。

// ---------------------------------------------------------------- 堆追踪
//
// UAF 的根因信息（这块内存**何时**被分配、**何时**被释放）无法静态推断，
// 只能在运行时记录。这里用一张定长静态表记录分配/释放事件。
//
// 三条硬约束，每一条都是被 handler 的运行环境逼出来的：
//
//   1. 表必须是**静态数组**。崩溃 handler 里调用 malloc 会死锁或二次崩溃
//      —— 崩在堆管理器时堆可能已经损坏。定长表溢出时丢弃新记录，
//      宁可丢样本，也不能在崩溃路径上分配内存。
//
//   2. **回溯必须在分配/释放时做完**，不能留到崩溃时。
//      崩溃那一刻栈可能已被破坏（越界写会污染栈，实测第 3 帧之后
//      全是填充字节），此时展开出来的调用链是假的。
//      而分配/释放发生在正常执行流里，栈是完好的。
//
//   3. 只记录返回地址，不解析符号。解析要加载 PDB，
//      那在 handler 里会死锁（文档第 2 条）。地址交给外部符号化。

typedef struct {
    void*  base;        // 分配得到的指针
    size_t size;        // 块大小（用于页保护）
    void*  alloc_ra;    // 分配点的返回地址
    void*  free_ra;     // 释放点的返回地址（未释放则为 0）
    bool   freed;
    bool   in_use;
} ZLabBlock;

enum { ZLAB_MAX_BLOCKS = 4096 };

// 每块尾部有一页不可访问的隔离带。越界写会先撞上它，
// 所以"写穿块"的故障地址落在 [end, end+guard) 而不是块内。
// 用固定 64KB 兜住常见页大小（4KB / 64KB 都够）。
enum { ZLAB_GUARD_SPAN = 65536 };

// 静态存储：不占用堆，handler 里可安全读取
static ZLabBlock g_blocks[ZLAB_MAX_BLOCKS];
static int       g_block_count = 0;

// 正常执行流里做回溯，返回业务代码的调用点地址。
// 与 handler 里的展开用同一套 RtlVirtualUnwind，不引入 dbghelp。
//
// 必须定义在 ZLabAlloc / ZLabFree **之前**：C++ 要求先声明后使用，
// 写在后面会报 C3861 identifier not found。
// ---------------------------------------------------------------- 调用点
//
// **用 _ReturnAddress() 而不是手写栈展开。**
//
// 最初用 RtlCaptureContext + RtlVirtualUnwind 展开固定层数回到业务代码，
// 实测一路踩坑：展开 2 层落在 ZLabFree 内部，改 3 层仍落在探测代码里，
// 用哨兵函数围地址区间也没能稳定跳出。根因是**层数依赖编译器的内联、
// 帧指针与尾调用决策**，任何一个变化都会让结果静默错位。
//
// _ReturnAddress() 是编译器内建，语义就是"当前函数的返回地址"，
// 不依赖任何运行时展开 —— 调用它的函数必须 noinline，
// 保证"当前函数"就是 ZLabAlloc / ZLabFree 本身。
// 这样得到的地址直接就是业务代码里调用之后的那条指令。
//
// 代价：它只能拿到**直接调用者**，拿不到更上层。
// 这对定位足够 —— 我们要的就是"分配/释放发生在哪一行"。
// ---------------------------------------------------------------- 隔离分配
//
// **为什么 UAF 需要接管分配器，而不只是记录**
//
// 光 hook malloc/free 记录地址是不够的：释放后的内存通常**仍然可读**，
// 悬垂访问不会触发任何异常，UAF 因此永远是负样本（实测 uaf_01
// 跑出来 crashed=False）。记录在案却没人来问，等于没记录。
//
// 所以这里做 ASAN 的同款手法：**每次分配独占若干页，释放后整页置为
// PAGE_NOACCESS**。此后任何访问都必然触发访问违例，UAF 就从
// 「安静地错」变成「响亮地崩」—— 定位才有输入。
//
// 为什么必须独占页：VirtualProtect 是页粒度的，
// 若多个块共享一页，保护一页会误伤同页的其他活跃分配，产生假阳性。
// 样本程序分配次数很少，独占页的开销可以接受。
//
// 代价要说清楚：内存布局与真实 malloc 不同，
// 这改变了样本的物理表现，但不改变「哪块内存被释放后又访问了」这一事实。

__declspec(noinline) static void* ZLabAlloc(size_t n) {
    if (n == 0) n = 1;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    // 向上取整到页大小的整数倍，保证独占
    size_t page = (size_t)si.dwPageSize;
    size_t total = ((n + page - 1) / page) * page;
    // 多分配一页作为尾部的不可访问隔离带，捕获顺序越界
    void* p = VirtualAlloc(NULL, total + page, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    if (!p) return NULL;
    // 尾部一页设为不可访问：既捕获越界，也让每块天然独占
    DWORD oldProtect = 0;
    VirtualProtect((char*)p + total, page, PAGE_NOACCESS, &oldProtect);

    if (g_block_count < ZLAB_MAX_BLOCKS) {
        ZLabBlock& b = g_blocks[g_block_count++];
        b.base     = p;
        b.size     = total;
        b.alloc_ra = _ReturnAddress();
        b.free_ra  = NULL;
        b.freed    = false;
        b.in_use   = true;
    }
    return p;
}

__declspec(noinline) static void ZLabFree(void* p) {
    if (!p) return;
    for (int i = 0; i < g_block_count; i++) {
        if (!g_blocks[i].in_use || g_blocks[i].base != p) continue;

        if (g_blocks[i].freed) {
            // **重复释放：必须主动让它崩。**
            //
            // 接管分配器之后，free 不再走 CRT，于是 CRT 自带的重复释放
            // 校验就失效了 —— 实测 double_free_01 会静默退出（rc=0），
            // 从 HIT 退化成 MISS。校验的责任现在落在我们身上。
            //
            // 好处是比 CRT 更精确：块的完整历史都在表里，
            // 能直接指出"第二次释放"发生在业务代码的哪一行。
            //
            // 这里直接 printf 调用点而不依赖栈：崩溃会落在 ZLabFree 内部，
            // 栈符号化指向探测代码，对定位没有意义。
            void* ra = _ReturnAddress();
            std::printf("ZLAB_DOUBLEFREE_RA=%llx\n",
                        (unsigned long long)(uintptr_t)ra);
            std::fflush(stdout);
            // 该块已置 NOACCESS，写入必然触发访问违例
            *(volatile char*)p = 0;
            return;
        }

        g_blocks[i].freed   = true;
        g_blocks[i].free_ra = _ReturnAddress();
        // 整块置为不可访问：此后访问必然 AV，UAF 才观测得到
        DWORD oldProtect = 0;
        VirtualProtect(p, g_blocks[i].size, PAGE_NOACCESS, &oldProtect);
        return;
    }
}

// 崩溃时调用：故障地址落在哪个已释放块里？
// 只做数组遍历 + printf，不分配内存。
// ---------------------------------------------------------------- 泄漏报告
//
// **泄漏和崩溃不是一回事，检测时机也不同。**
//
// UAF / 重复释放是"崩了之后去查"，泄漏是"没崩但东西没还"。
// 程序正常走完 main、顺利退出，一个字节都没释放 —— 这正是它
// 最容易被漏掉的原因（"没有崩溃就没问题"）。
//
// 所以在**退出时**清点：main 结束后遍历记录表，
// 凡是 in_use 且未 freed 的块就是泄漏，且直接持有它的分配点。
// 这一步不需要任何静态分析 —— 块的分配点早在分配时就记下了。
//
// **为什么用全局对象析构，而不是 atexit**
//
// atexit 注册的回调由 CRT 在退出时调用，但它的执行顺序与
// C++ 静态对象析构的相对次序不在我们控制之内；
// 而静态对象的析构明确发生在 main 之后、堆被拆除之前，printf 此时仍可用。
//
// 另一个关键性质：**崩溃的进程不会执行静态析构**。
// 所以 UAF / 重复释放（异常终止）完全不受这段影响，
// 不会在它们的关键路径上多打印任何东西。
static void ZLabReportLeaks() {
    int leaked = 0;
    for (int i = 0; i < g_block_count && leaked < 16; i++) {
        const ZLabBlock& b = g_blocks[i];
        if (!b.in_use || b.freed) continue;
        leaked++;
        // 未崩溃的进程不会输出 ZLAB_BASE（那是在 VEH handler 里打的），
        // 而归一化地址必须知道模块基址。这里补上，
        // 让泄漏证据与崩溃证据走同一套归一化逻辑。
        std::printf("ZLAB_BASE=%llx\n",
                    (unsigned long long)(uintptr_t)GetModuleHandle(NULL));
        std::printf("ZLAB_LEAK_ALLOC_RA=%llx\n",
                    (unsigned long long)(uintptr_t)b.alloc_ra);
    }
    if (leaked > 0) {
        std::printf("ZLAB_LEAK_COUNT=%d\n", leaked);
        std::fflush(stdout);
    }
}

struct ZLabLeakReporter {
    ~ZLabLeakReporter() { ZLabReportLeaks(); }
};
// 静态存储期：main 结束后析构，届时清点未释放的块。
// 放在探测代码块内，且必须晚于 g_blocks 的定义。
static ZLabLeakReporter g_leak_reporter;

// ------------------------------------------------- 数据竞争检测（共享变量）
//
// **race 和泄漏一样不崩溃，但比泄漏更隐蔽。**
//
// 泄漏至少还留下"没还的块"这个实体，race 连这个都没有 ——
// 两个线程并发写同一个变量，谁都没做错什么事，
// 结果只是偶尔不对。实测 race_01 稳定返回 rc=1，
// 但栈是空的、没有异常、没有任何地址可供定位。
//
// **检测办法：让共享变量所在的页变成"会报警的页"。**
//
// 把变量放进单独一页并加上 PAGE_GUARD：
// 任何访问都会触发 STATUS_GUARD_PAGE_VIOLATION，
// 而异常信息里带着**触发访问的指令地址** ——
// 那正是 `g_counter++` 编译出来的那条读/写指令。
// 于是"哪个地址在被并发访问"和"在哪一行"同时拿到了。
//
// 判据（race 的定义，不是近似）：
//   两个**不同的线程**访问同一地址，至少一个是写，且中间无同步。
// 这里记录每个地址的访问线程集合与是否发生过写；
// 只要集合里出现第二个线程且涉及写，就是竞争。
//
// **PAGE_GUARD 的关键性质**：触发一次后系统**自动清除** guard 标志，
// 所以 handler 里必须重新加回去，否则只能捕获第一次访问。
// 也正因为会自动放行，不需要单步（Trap Flag）—— 比手工单步稳健得多。
#define ZLAB_MAX_WATCHED 64
#define ZLAB_MAX_WATCH_TIDS 16

struct ZLabWatch {
    void* page;                     // 变量所在页的基址
    unsigned long long tids[ZLAB_MAX_WATCH_TIDS];
    int tid_count;
    int saw_write;
    unsigned long long first_ra;    // 第一次访问的指令地址
    unsigned long long conflict_ra; // 首个"另一线程"访问的指令地址
};

static ZLabWatch g_watches[ZLAB_MAX_WATCHED];
static int g_watch_count = 0;

// 变量放进独立一页：页保护的最小粒度就是一页，
// 与其它全局数据同页会让无关的读写也触发报警。
static void* ZLabWatchVar() {
    if (g_watch_count >= ZLAB_MAX_WATCHED) return nullptr;
    void* page = VirtualAlloc(nullptr, 4096,
                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!page) return nullptr;
    std::memset(page, 0, 4096);
    std::memset(&g_watches[g_watch_count], 0, sizeof(ZLabWatch));
    g_watches[g_watch_count].page = page;
    g_watch_count++;
    return page;
}

static ZLabWatch* ZLabFindWatch(void* addr) {
    unsigned long long a = (unsigned long long)(uintptr_t)addr;
    for (int i = 0; i < g_watch_count; i++) {
        unsigned long long base =
            (unsigned long long)(uintptr_t)g_watches[i].page;
        if (a >= base && a < base + 4096) return &g_watches[i];
    }
    return nullptr;
}

static void ZLabRecordAccess(void* addr, int isWrite,
                             unsigned long long ra) {
    ZLabWatch* w = ZLabFindWatch(addr);
    if (!w) return;
    unsigned long long tid = (unsigned long long)GetCurrentThreadId();
    if (isWrite) w->saw_write = 1;
    if (w->tid_count == 0) w->first_ra = ra;
    int seen = 0;
    for (int i = 0; i < w->tid_count; i++) {
        if (w->tids[i] == tid) { seen = 1; break; }
    }
    if (!seen && w->tid_count < ZLAB_MAX_WATCH_TIDS) {
        w->tids[w->tid_count++] = tid;
        // 第二个线程进来，且涉及写 —— 这就是竞争
        if (w->tid_count >= 2 && w->saw_write && !w->conflict_ra) {
            w->conflict_ra = ra;
        }
    }
}

// 重新加上 guard。handler 返回 CONTINUE_EXECUTION 时
// 系统已把该页的 guard 标志清掉，不补回来就只能捕获第一次。
static void ZLabReArmGuard(void* page) {
    DWORD oldProtect = 0;
    VirtualProtect(page, 4096, PAGE_READWRITE | PAGE_GUARD, &oldProtect);
}

static void ZLabArmWatch(void* addr) {
    ZLabWatch* w = ZLabFindWatch(addr);
    if (w) ZLabReArmGuard(w->page);
}

// 退出时报告竞争。与泄漏同理用静态析构：
// 崩掉的进程不会执行它，因此不影响 UAF / 重复释放等样本。
static void ZLabReportRaces() {
    for (int i = 0; i < g_watch_count; i++) {
        const ZLabWatch& w = g_watches[i];
        if (w.tid_count >= 2 && w.saw_write && w.conflict_ra) {
            // 未崩溃的进程不会输出 ZLAB_BASE（那是在 VEH 崩溃分支里打的），
            // 而归一化地址必须知道模块基址。
            std::printf("ZLAB_BASE=%llx\n",
                        (unsigned long long)(uintptr_t)GetModuleHandle(NULL));
            std::printf("ZLAB_RACE_CONFLICT_RA=%llx\n",
                        (unsigned long long)w.conflict_ra);
            std::printf("ZLAB_RACE_FIRST_RA=%llx\n",
                        (unsigned long long)w.first_ra);
            std::printf("ZLAB_RACE_THREADS=%d\n", w.tid_count);
            std::fflush(stdout);
        }
    }
}

struct ZLabRaceReporter {
    ~ZLabRaceReporter() { ZLabReportRaces(); }
};
static ZLabRaceReporter g_race_reporter;


static void ZLabReportFreed(unsigned long long faultAddr) {
    if (!faultAddr) return;
    for (int i = 0; i < g_block_count; i++) {
        ZLabBlock& b = g_blocks[i];
        unsigned long long base = (unsigned long long)(uintptr_t)b.base;
        unsigned long long end = base + (unsigned long long)b.size;

        // **写穿活跃块 = 分配大小算错了。**
        //
        // 整数溢出的典型形态：count * sizeof(T) 在窄宽度上回绕，
        // 分配到的块远小于后续写入量，循环写穿块尾、撞上隔离页。
        //
        // 注意故障地址此时落在块**之后**的隔离页，不在 [base, end) 内 ——
        // 所以这里认的是"紧邻某个活跃块的尾部"，用一页窗口兜住。
        // ZLAB_HIT_ALLOC_RA 给出该块分配点的返回地址，
        // 符号化后就落在乘法那一行 —— 那才是根因，而不是崩溃的 memset 行。
        if (!b.freed && b.in_use && faultAddr >= base
            && faultAddr < end + (unsigned long long)ZLAB_GUARD_SPAN) {
            std::printf("ZLAB_HIT_ALLOC_RA=%llx\n",
                (unsigned long long)(uintptr_t)b.alloc_ra);
            std::printf("ZLAB_HIT_SIZE=%llu\n",
                (unsigned long long)b.size);
            std::printf("ZLAB_HIT_OFF=%llu\n",
                (unsigned long long)(faultAddr - base));
            return;
        }

        if (!b.freed) continue;
        if (faultAddr >= base && faultAddr < end) {
            std::printf("ZLAB_UAF_ALLOC_RA=%llx\n",
                (unsigned long long)(uintptr_t)b.alloc_ra);
            std::printf("ZLAB_UAF_FREE_RA=%llx\n",
                (unsigned long long)(uintptr_t)b.free_ra);
            std::printf("ZLAB_UAF_BASE=%llx\n", base);
            return;
        }
    }
}

LONG WINAPI ZLabVexHandler(PEXCEPTION_POINTERS p) {
    if (!p || !p->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;

    // 竞争观测：guard 页命中。
    //
    // 必须**先于**崩溃处理，且直接返回 CONTINUE_EXECUTION：
    // guard 命中是"我们来观测的"，不是程序出错，
    // 一旦掉进下面的崩溃分支，程序就被当成崩了。
    // 注意 ExceptionInformation[0]：0=读，1=写 —— 竞争要求至少一个是写。
    if (p->ExceptionRecord->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION) {
        void* addr = (void*)(uintptr_t)
            p->ExceptionRecord->ExceptionInformation[1];
        ZLabWatch* w = ZLabFindWatch(addr);
        if (w) {
            int isWrite = (p->ExceptionRecord->ExceptionInformation[0] == 1);
            ZLabRecordAccess(addr, isWrite,
                             (unsigned long long)p->ContextRecord->Rip);
            // guard 标志被系统自动清除了，想继续观测就必须补回来。
            //
            // **但抓到证据后必须停止观测。**
            // 每次 guard 命中都要进内核再回用户态，微秒级；
            // race_01 是 4 线程 × 20 万次累加 = 80 万次访问，
            // 一路武装下去要跑几十秒甚至更久。
            // 而竞争通常在前几百次访问内就暴露了 ——
            // 拿到 conflict_ra 之后继续观测没有额外信息，只有额外开销。
            // 于是不再武装，页面退回普通可读写，程序全速跑完。
            if (!w->conflict_ra) {
                ZLabReArmGuard(w->page);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    CONTEXT ctx = *(p->ContextRecord);

    std::printf("ZLAB_CRASHIP=%llx\n", (unsigned long long)ctx.Rip);
    std::printf("ZLAB_FAULTADDR=%llx\n",
        (unsigned long long)(uintptr_t)p->ExceptionRecord->ExceptionInformation[1]);
    std::printf("ZLAB_CODE=%lx\n",
        (unsigned long)p->ExceptionRecord->ExceptionCode);
    std::printf("ZLAB_BASE=%llx\n",
        (unsigned long long)(uintptr_t)GetModuleHandle(NULL));

    UNWIND_HISTORY_TABLE hist;
    std::memset(&hist, 0, sizeof(hist));

    int n = 0;
    for (int i = 0; i < 64; i++) {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, &hist);
        if (!fn) break;

        PVOID handlerData = NULL;
        DWORD64 frame = 0;
        KNONVOLATILE_CONTEXT_POINTERS nvcp;
        std::memset(&nvcp, 0, sizeof(nvcp));

        RtlVirtualUnwind(0, imageBase, ctx.Rip, fn, &ctx,
                         &handlerData, &frame, &nvcp);
        if (!ctx.Rip) break;
        std::printf("ZLAB_FRAME%d=%llx\n", n++, (unsigned long long)ctx.Rip);
    }
    std::printf("ZLAB_NFRAMES=%d\n", n);

    // UAF 现场：故障地址落在哪个已释放的块里。
    // 放在展开**之后**：先拿到崩溃现场，再补堆的上下文。
    // 只做数组遍历与 printf —— handler 里绝不能分配内存。
    ZLabReportFreed((unsigned long long)(uintptr_t)
        p->ExceptionRecord->ExceptionInformation[1]);

    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

'''

WINDOWS_INSTALL = r'''
    AddVectoredExceptionHandler(0, ZLabVexHandler);
'''

MARKER_IP = "ZLAB_CRASHIP="
MARKER_FAULTADDR = "ZLAB_FAULTADDR="
MARKER_CODE = "ZLAB_CODE="
MARKER_BASE = "ZLAB_BASE="
MARKER_NFRAMES = "ZLAB_NFRAMES="
MARKER_UAF_ALLOC = "ZLAB_UAF_ALLOC_RA="
MARKER_UAF_FREE = "ZLAB_UAF_FREE_RA="
MARKER_UAF_BASE = "ZLAB_UAF_BASE="
MARKER_DOUBLEFREE = "ZLAB_DOUBLEFREE_RA="
MARKER_LEAK_ALLOC = "ZLAB_LEAK_ALLOC_RA="
MARKER_LEAK_COUNT = "ZLAB_LEAK_COUNT="
MARKER_RACE_CONFLICT = "ZLAB_RACE_CONFLICT_RA="
MARKER_HIT_ALLOC = "ZLAB_HIT_ALLOC_RA="
MARKER_HIT_SIZE = "ZLAB_HIT_SIZE="
MARKER_HIT_OFF = "ZLAB_HIT_OFF="
MARKER_RACE_FIRST = "ZLAB_RACE_FIRST_RA="
MARKER_RACE_THREADS = "ZLAB_RACE_THREADS="


def _wrap_calls(source: str, callee: str, wrapper: str,
                returns_value: bool = True, start: int = 0,
                replace_with: str = "") -> str:
    """
    把 `std::malloc(...)` / `std::free(...)` 包进追踪函数，保留原调用。

    **为什么必须做括号配平**

    朴素的字符串替换 `std::malloc(` -> `ZLabAlloc(std::malloc(`
    会在遇到嵌套调用时生成括号不匹配的源码：
        std::malloc(sizeof(GameObject))
    变成
        ZLabAlloc(std::malloc(sizeof(GameObject))
    少一个右括号，直接编译失败。
    所以要从调用点开始扫到**配平的右括号**再替换。

    **为什么还要区分"包装"和"接管"**

    malloc 被 ZLabAlloc **接管**（换成自己的分配器，收大小参数）；
    若写成 ZLabAlloc(std::malloc(n))，等于把 void* 传给 size_t，
    报 C2664。free 同理：ZLabFree 自己完成"释放 + 置不可访问"，
    不需要再调 std::free。

    **为什么必须跳过注入的探测代码**

    探测代码自身也有 std::memset 等调用，
    若整篇替换会把自己包进去，形成无限递归。

    **失败时原样返回**：宁可不追踪，也不能产出编译不过的源码。
    """
    needle = "std::" + callee + "("

    # start 之前是注入的探测代码，绝不参与替换 ——
    # 探测代码里也有 std::malloc / std::memset 调用，
    # 被包装会让 ZLabTrackAlloc 递归调用自己。
    head, tail = source[:start], source[start:]

    result = []
    i = 0
    while True:
        j = tail.find(needle, i)
        if j < 0:
            result.append(tail[i:])
            break
        result.append(tail[i:j])
        # 从 '(' 开始做括号配平
        open_idx = j + len(needle) - 1
        depth = 0
        k = open_idx
        while k < len(tail):
            if tail[k] == "(":
                depth += 1
            elif tail[k] == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        if k >= len(tail):
            # 括号没配平（不该发生）：放弃本次替换，保证源码可用
            result.append(tail[j:])
            break
        inner = tail[j:k + 1]          # 完整的 std::malloc(...) / std::free(...)
        call_args = inner[inner.find("(") + 1:-1]

        if replace_with:
            # 接管调用：直接换成新函数，不保留原调用。
            # ZLabAlloc 收的是**大小**，不是指针 ——
            # 包成 ZLabAlloc(std::malloc(n)) 会类型不匹配（void* -> size_t）。
            result.append(f"{replace_with}({call_args})")
        elif returns_value:
            # malloc：先分配，返回值再交给追踪函数
            result.append(f"{wrapper}({inner})")
        else:
            # free 返回 void，不能当实参。
            # 逗号表达式达成「先释放，再登记」，且整体仍是表达式。
            result.append(f"({wrapper}({call_args}), {inner})")
        i = k + 1

    return head + "".join(result)


def inject_heap_tracking(source: str, start: int = 0) -> str:
    """
    给源码装上堆追踪：分配与释放各记一笔，附带调用点返回地址。

    这一步把 UAF 从「静态推不出来」变成「有运行时证据」。
    没有它，UAF 只能靠猜 —— 因为要知道的是
    「这块内存何时被分配、何时被释放」，代码里看不出来。

    start 之前的内容（注入的探测代码）不参与替换，
    否则追踪函数会把自己包进去，形成无限递归。
    """
    # std::malloc 被 ZLabAlloc **接管**（不是包装）：
    # ZLabAlloc 收大小参数，包住 malloc 会变成 void* 传给 size_t。
    src = _wrap_calls(source, "malloc", "ZLabAlloc",
                      returns_value=True, start=start, replace_with="ZLabAlloc")
    # std::free 被 ZLabFree 接管：先登记再真正释放
    return _wrap_calls(src, "free", "ZLabFree",
                       returns_value=False, start=start, replace_with="ZLabFree")


RACE_WATCH_RE = re.compile(
    r"^\s*(?:int|long|long\s+long|short|unsigned(?:\s+\w+)?)\s+"
    r"(\w+)\s*=\s*\d+\s*;\s*(?://.*)?$")


def inject_race_watch(source: str, start: int = 0) -> tuple[str, list[str]]:
    """
    把全局共享计数器搬到受保护的页上，并武装观测。

    **为什么只在"有线程"时才做**

    把全局变量放进 guard 页会让**每次访问都进一次异常**。
    单线程程序这么做毫无意义 —— 没有第二个线程就不可能有竞争，
    只会白白拖慢。所以只在源码真的用了 std::thread 时才启用。

    **为什么只挑全局整型变量**

    竞争样本的形态是"多个线程无保护地读写同一个共享计数器"。
    这里认的是**文件作用域的整型初始化声明**（`long g_counter = 0;`），
    不包括局部变量（栈上变量跨不了线程，除非被共享出去）。

    返回 (新源码, 被监视的变量名列表)。
    拿不准就一个都不改 —— 宁可漏检，也不要产出编译不过的源码。
    """
    head, tail = source[:start], source[start:]

    # 只在真的起了线程时才观测
    if "std::thread" not in tail:
        return source, []

    watched: list[str] = []
    out: list[str] = []

    # 只认**文件作用域**的声明。
    # 栈上的局部变量跨不了线程，把它放进 guard 页只会让每次访问都进异常，
    # 白白拖慢且毫无意义。用大括号深度区分：depth==0 才是文件作用域。
    depth = 0
    for line in tail.splitlines():
        m = RACE_WATCH_RE.match(line) if depth == 0 else None
        if m:
            name = m.group(1)
            indent = line[:len(line) - len(line.lstrip())]
            # 全局引用：指向受保护页。初值 0 由 ZLabWatchVar 的 memset 保证。
            out.append(f"{indent}long& {name} = *(long*)ZLabWatchVar();")
            watched.append(name)
        else:
            out.append(line)
        depth += line.count("{") - line.count("}")

    if not watched:
        return source, []

    return head + "\n".join(out), watched


def inject_probe(source: str) -> str:
    """
    把探测器注入到故障模式源码中。

    注入点：所有 #include 之后。安装点：main 函数体第一行。

    注入失败（找不到 main）时原样返回 —— 此时样本仍能崩，
    只是拿不到地址，走降级路径。宁可降级，也不要产出编译不过的源码。
    """
    lines = source.splitlines()

    last_include = -1
    for i, line in enumerate(lines):
        if line.strip().startswith("#include"):
            last_include = i
    if last_include < 0:
        return source
    lines.insert(last_include + 1, WINDOWS_PROBE)

    src = "\n".join(lines)

    # 堆追踪必须跳过**注入的探测代码**本身 ——
    # 探测代码里也有 memset 之类调用，被包装会出问题。
    # 分界点 = 探测代码结束的位置（用它的末行做锚点）。
    probe_end = src.rfind("ZLabReportFreed")
    if probe_end >= 0:
        close = src.find("\n}\n", probe_end)
        cut = close + 3 if close >= 0 else len(WINDOWS_PROBE) + 1
    else:
        cut = 0

    src = inject_heap_tracking(src, start=cut)
    src, watched = inject_race_watch(src, start=cut)
    lines = src.splitlines()

    # 武装观测必须放在 main 开头：线程的创建紧随其后，
    # 晚一步就漏掉最早的那批并发访问。
    if watched:
        arm = "\n".join(f"    ZLabArmWatch(&{n});" for n in watched)
        for i, line in enumerate(lines):
            if line.strip().startswith("int main"):
                if "{" in line:
                    lines.insert(i + 1, arm)
                    break
                if i + 1 < len(lines) and lines[i + 1].strip() == "{":
                    lines.insert(i + 2, arm)
                    break

    for i, line in enumerate(lines):
        if line.strip().startswith("int main"):
            if "{" in line:
                lines.insert(i + 1, WINDOWS_INSTALL)
                break
            if i + 1 < len(lines) and lines[i + 1].strip() == "{":
                lines.insert(i + 2, WINDOWS_INSTALL)
                break

    return "\n".join(lines) + "\n"


def parse_probe_output(stdout: str) -> dict:
    """
    解析探测器输出。缺失字段返回 None，不填默认值。

    这条纪律贯穿整个项目：拿不到就说拿不到，不要用 0 或空串假装拿到了。
    """
    out: dict = {"ip": None, "fault_addr": None, "code": None,
                 "base": None, "frames": [], "nframes": 0,
                 "uaf_alloc_ra": None, "uaf_free_ra": None,
                 "uaf_base": None, "doublefree_ra": None,
                 "leak_alloc_ras": [], "leak_count": 0,
                 "race_conflict_ra": None, "race_first_ra": None,
                 "race_threads": 0, "hit_alloc_ra": None,
                 "hit_size": None, "hit_off": None}

    for line in (stdout or "").splitlines():
        line = line.strip()

        # 竞争：conflict_ra 是"第二个线程"访问共享变量的那条指令。
        # 它就是竞争现场，比 first_ra 更能代表"冲突发生在哪"。
        if line.startswith(MARKER_RACE_CONFLICT):
            try:
                v = int(line[len(MARKER_RACE_CONFLICT):], 16)
                if v:
                    out["race_conflict_ra"] = v
            except ValueError:
                pass
            continue

        if line.startswith(MARKER_RACE_FIRST):
            try:
                v = int(line[len(MARKER_RACE_FIRST):], 16)
                if v:
                    out["race_first_ra"] = v
            except ValueError:
                pass
            continue

        if line.startswith(MARKER_RACE_THREADS):
            try:
                out["race_threads"] = int(line[len(MARKER_RACE_THREADS):])
            except ValueError:
                pass
            continue

        # 泄漏可能有多个块，逐个收下。
        # 第一个泄漏块的分配点通常是主要的定位依据，
        # 但全部保留，让上层可以自己挑。
        if line.startswith(MARKER_LEAK_ALLOC):
            try:
                v = int(line[len(MARKER_LEAK_ALLOC):], 16)
                if v:
                    out["leak_alloc_ras"].append(v)
            except ValueError:
                pass
            continue

        if line.startswith(MARKER_LEAK_COUNT):
            try:
                out["leak_count"] = int(line[len(MARKER_LEAK_COUNT):])
            except ValueError:
                pass
            continue

        if line.startswith(MARKER_HIT_ALLOC):
            try:
                v = int(line[len(MARKER_HIT_ALLOC):], 16)
                out["hit_alloc_ra"] = v if v else None
            except ValueError:
                pass
            continue

        if line.startswith(MARKER_HIT_SIZE):
            try:
                out["hit_size"] = int(line[len(MARKER_HIT_SIZE):])
            except ValueError:
                pass
            continue

        if line.startswith(MARKER_HIT_OFF):
            try:
                out["hit_off"] = int(line[len(MARKER_HIT_OFF):])
            except ValueError:
                pass
            continue

        if line.startswith(MARKER_DOUBLEFREE):
            try:
                v = int(line[len(MARKER_DOUBLEFREE):], 16)
                out["doublefree_ra"] = v if v else None
            except ValueError:
                pass
            continue

        # UAF 堆追踪字段。拿不到就保持 None ——
        # 这条纪律贯穿整个项目：不要用 0 或空串假装拿到了。
        for key, marker in (("uaf_alloc_ra", MARKER_UAF_ALLOC),
                            ("uaf_free_ra", MARKER_UAF_FREE),
                            ("uaf_base", MARKER_UAF_BASE),
                            ("doublefree_ra", MARKER_DOUBLEFREE)):
            if line.startswith(marker):
                try:
                    v = int(line[len(marker):], 16)
                    out[key] = v if v else None
                except ValueError:
                    pass
                continue
        if line.startswith(MARKER_NFRAMES):
            try:
                out["nframes"] = int(line[len(MARKER_NFRAMES):])
            except ValueError:
                pass
            continue

        m = re.match(r"^ZLAB_FRAME(\d+)=([0-9a-fA-F]+)$", line)
        if m:
            try:
                out["frames"].append((int(m.group(1)), int(m.group(2), 16)))
            except ValueError:
                pass
            continue

        for key, marker in (("ip", MARKER_IP), ("fault_addr", MARKER_FAULTADDR),
                            ("code", MARKER_CODE), ("base", MARKER_BASE)):
            if line.startswith(marker):
                try:
                    out[key] = int(line[len(marker):], 16)
                except ValueError:
                    pass

    out["frames"].sort(key=lambda x: x[0])
    return out
