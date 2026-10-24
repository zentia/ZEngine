#pragma once

// ============================================================================
// UafReporter — use-after-free 运行时检测（阶段一·夯地基 第一落点，#3 + #5）
// ----------------------------------------------------------------------------
// 目标：把"对象已销毁仍被访问"这类悬垂指针 bug 从"线上随机崩、栈里看不出
// 根因"变成"Debug/RelWithDebInfo 下当场断言、并打出明确的 UAF 报告"。
//
// 判据选型（关键）：
//   用 **instanceID 是否已从 ObjectManager 表注销** 作为"对象已死"的信号，
//   而不是"释放后填充内存（0xCD/0xDD）"。原因：
//     * Object::~Object() 必然调用 ObjectManager::UnregisterInstanceID
//       （见 Object.cpp），这是引擎里唯一"确定发生、跨平台一致"的死亡信号。
//     * Apple 上全局 operator new/delete 故意不接管 mimalloc（MemoryManager.cpp
//       的 `#if !defined(__APPLE__)`），`delete` 不一定走 MemoryManager::Free，
//       所以"在 Free 里填充"覆盖不了所有释放路径，反而会在 Apple 上引入新的
//       崩溃面。用 instanceID 注销做判据则完全绕开这个坑。
//
// 开销控制：
//   所有检测都 gate 在 ZENGINE_LIFETIME_CHECK 之后。该宏由 CMake 注入：
//   Debug / DebugV8 / RelWithDebInfo 下为 1，Release / MinSizeRel 下为 0。
//   Release 下 ImmediatePtr 的访问路径与改造前完全一致（零开销、直接返回裸指针）。
//
// 头文件保持轻量：不 include ObjectManager / Object，实现都在 .cpp。
// ImmediatePtr.h 被极广泛地 include（Component.h 等），这里只暴露两个自由函数。
// ============================================================================

#include <stdint.h>

namespace zengine::lifetime
{

// 该 instanceID 对应的对象是否仍然存活（仍在 ObjectManager 的 IDToPointer 表里）。
//
// 语义：
//   * instanceID == 0 → 返回 true（"无身份"的对象不参与校验，退化为不检测，
//     避免误报——例如通过 operator=(T*) 裸赋值、或尚未经 AllocateAndAssignInstanceID
//     的对象）。
//   * instanceID != 0 且表里查得到 → true（存活）。
//   * instanceID != 0 但表里查不到 → false（对象已销毁 → 后续访问即 UAF）。
//
// 注意：这依赖 ObjectManager 单例。若单例尚未初始化（早期启动 / 静态析构期），
// 视为"无法判定"，返回 true 不报错——检测是为运行期服务的，不该在启动/关闭期误伤。
bool IsInstanceAlive(int32_t instanceID);

// 报告一次 use-after-free。
//
//   * 累加全局计数（function-local static，避免 header-only 在每个 TU 各存一份）。
//   * LOG_ERROR 打出 instanceID + 访问点，明确指出"这是 UAF，不是普通空指针"。
//   * ZENGINE_LIFETIME_CHECK 下额外触发 ASSERT 中断，便于就地定位。
//
// access_site 建议传 __FUNCTION__（或 __FUNCDNAME__），编译期可见、零运行期开销。
void ReportUseAfterFree(int32_t instanceID, const char* access_site);

// 命中次数。测试/冒烟用它确认检测确实生效（而不是静默放过了）。
uint32_t GetUseAfterFreeCount();

}  // namespace zengine::lifetime
