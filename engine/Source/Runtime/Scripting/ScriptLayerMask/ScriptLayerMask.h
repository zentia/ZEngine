#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <EASTL/string.h>

#include <cstdint>
#include <vector>

#include "ScriptLayerMask.generated.h"

// =============================================================================
// `LayerMask` —— Unity 的 `UnityEngine.LayerMask`
// =============================================================================
//
// Unity 的 `LayerMask` 是个包着一个 `int` 的 struct，还带 `int <-> LayerMask`
// 的隐式转换，所以 C# 里写得像整数：
//
//     int mask = LayerMask.GetMask("Enemy", "Player");
//     Physics.Raycast(origin, dir, out hit, 100f, mask);
//
// ⚠️ 这里**不做** struct，静态方法直接返回 `number`
// ------------------------------------------------
// 理由有三条，都压过"形状上更像 Unity"：
//
//   1. TS 没有隐式转换。做成 `{value: number}` 之后每个调用点都要写
//      `LayerMask.GetMask([...]).value`，而 Unity 那边是不写的 —— 搬代码时
//      多出来的那半个调用点比"少一个 struct"更难发现。
//   2. `Physics.Raycast` 那批查询的 `layerMask` 参数已经是 `number`（Unity 同）。
//      两边都是 number 才对得上。
//   3. 只有静态方法的 `declare class LayerMask` 与 `Mathf` / `Time` / `Physics`
//      同一形态（ZCLASS 必须派生 Object，它们都是）—— 不引入新概念。
//
// 层号与 Unity 一致：0..5 是内置（Default / TransparentFX / Ignore Raycast /
// (空) / Water / UI），6、7 保留，8..31 是用户层。`NameToLayer` 找不到时给
// **-1**（Unity 同），不是 0 —— 0 是 Default，是合法且最常用的层。
//
// ⚠️ 第 31 位是**负数**，别在脚本里手写 `1 << 31`
// -------------------------------------------------
// `<<` 在 JS 里是 32 位**有符号**运算，`1 << 31 === -2147483648`。层 31 的掩码
// 因此是负数 —— 这不是错，`Physics` 那边的 `layerMask` 也是 `int32_t`，位模式
// 一致。但"掩码是正数"这个直觉会让人写出 `mask > 0` 之类的判断，而它在只有
// 第 31 位置位时是假的。
//
// 所以判某层在不在掩码里**用 `MaskContainsLayer`**，不要自己 `&`：那个函数
// 两端的类型（uint32 的位、int32 的传参）在 C++ 里对齐好了。
//
// ⚠️ `GetMask` 对**未知层名**的处理：跳过（不是返回 -1）
// ------------------------------------------------------
// Unity 那边遇到不认识的层名会返回 -1（整张掩码作废）。这里选择**跳过**：
// 名字拼错时你会得到"少了一个层"的掩码，而不是"所有层"。
//
// 这两者的失败方向相反，而方向决定了排查成本：
//   * 返回 -1（全部层）：射线打到**所有**东西，包括 UI 和触发器 —— 看起来
//     像"物理有点吵"，很难联想到一个拼错的层名；
//   * 跳过：掩码里少了那一层，射线**打不到**本该打到的东西 —— 表现为"这个
//     物体不响应点击"，定位到"层名没对上"要直接得多。
//
// 与 `TagLayerManager::GetMask` 保持一致（那才是数据所在处），测试 LM5 钉住。
// =============================================================================

namespace Runtime
{
    /// Unity 的 `UnityEngine.LayerMask` 静态面。不该被实例化（同 `Mathf`）。
    ZCLASS()
    class LayerMask : public Object
    {
        ZGENERATED_BODY(LayerMask)

    public:
        /// 层名 -> 层号（0..31）。找不到给 **-1**（不是 0 —— 0 是 Default）。
        /// 空串同样是 -1：Unity 那边空串也匹配不到任何层。
        ZFUNCTION()
        static int32_t NameToLayer(const eastl::string& name);

        /// 层号 -> 层名。越界（< 0 或 >= 32）给**空串**，不抛异常。
        ZFUNCTION()
        static eastl::string LayerToName(int32_t layer);

        /// 若干层名 -> 合并后的掩码（Unity 的 `LayerMask.GetMask`）。
        ///
        /// Unity 是 `params string[]`（可变实参），TS 里传**数组**：
        /// `LayerMask.GetMask(["Enemy", "Player"])`。
        ///
        /// ⚠️ 未知层名**跳过**（见文件头），空数组给 0（"什么都不打"）。
        ZFUNCTION()
        static int32_t GetMask(const std::vector<eastl::string>& layer_names);

        /// `mask` 里有没有 `layer` 这一位。
        ///
        /// ⚠️ 请用它，不要用 `(mask & (1 << layer)) !== 0`：第 31 位在 JS 里是
        /// 负数，手写表达式在 layer == 31 时给出 `-2147483648 !== 0`（**真**），
        /// 恰好也对，但 `mask > 0` 那类判断就错了。这个函数两端类型对齐好了。
        ZFUNCTION()
        static bool MaskContainsLayer(int32_t mask, int32_t layer);
    };
}  // namespace Runtime
