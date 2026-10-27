# 脚本 VM 端到端：pesapi 绑定链第一次被真的跑起来

> 配套测试：`engine/Source/Runtime/Scripting/ScriptVM/Test/ScriptVMSmokeTest.cpp`
> 开关：`-DZENGINE_BUILD_SCRIPT_VM_SMOKE_TEST=ON`（且 `PAPI_TYPE=quickjs`）
>
> 本测试喂给 `ScriptEnv` 的是**手写**的 CommonJS。用户真正走的那条路
> （`.ts` → tsc → `.js` → ScriptEnv）见 `doc/SCRIPT_TS_CHAIN.md`。

## 1. 为什么单独写这个测试

到 2026-10-27 为止，脚本层已经攒了 72 个类、293 个方法，但**没有任何一行走过
真正的脚本链路**。前面三个冒烟测试（ScriptMath / ScriptTime / ScriptInput）都是
"C++ 直接调那些静态方法，看返回值对不对"——它们验证的是实现，不是绑定。

于是下面这五件事此前只是"编译过了"和"生成过了"，一次都没执行过：

| # | 环节 | 位置 |
|---|------|------|
| 1 | 生成的 `ZClassScriptBinding` -> pesapi 类 | `ScriptBinding.cpp::InstallAll` |
| 2 | `create_class` -> 挂成 `globalThis` 上的名字 | `ScriptBinding.cpp::InstallGlobals` |
| 3 | ZENUM -> `globalThis` 上的同名普通对象（`Key.Space`） | `InstallGlobals` 第二段 |
| 4 | `console` / `Debug` / `__zNewInstance` / `__zApplyField` shim | `ScriptEnv::InstallConsoleAndDebugGlobals` |
| 5 | 模块加载（IIFE + `module.exports`）+ 实例生命周期 | `ScriptEnv::LoadModuleInternal` 等 |

"编译得过"对这些是完全无效的证明，因为失败模式全是运行期的空值：
注册表是空的 → `create_class` 返回 undefined → global 不存在 → 脚本一调就
`ReferenceError`。静态检查一个都发现不了。

## 2. 这个测试到底跑了什么

真起一个 QuickJS VM（`BackendQuickJS` + `ScriptEnv`），往里 `pesapi_eval` 真的 JS，
把结果写进 `globalThis.__zResult` 再读回来断言。55 项检查，分 8 组：

| 组 | 内容 |
|----|------|
| V | VM 起来了（papis / env_ref / 注册表非空 / Eval 可用 / 回读可用） |
| A | `VectorMath` / `QuaternionMath` / `Mathf` / `Time` / `Input` / `Key` 被发布成 global；`console` / `Debug` / `print` / `Behaviour` shim 已注入 |
| B | VectorMath 往返：`Sub` / `Add` / `Scale` / `Magnitude` / `Distance` / `Dot` / `Cross` / `Normalize` / `Lerp` / `ClampMagnitude` / `MoveTowards` / `IsZero` / `Min` / `Max` / `Angle` |
| C | `QuaternionMath.ToEuler` 的分量->轴映射；`Mathf` 标量一族 |
| D | Time：C++ 推帧 -> 脚本读到；**脚本 `SetTimeScale` -> C++ 下一帧按新 scale 算**（双向） |
| E | ZENUM：`Key.Space` 与 C++ `KeyCodes::Key::Space` 同值；无 InputSystem 时的安全降级 |
| F | `Debug.Log` / `console.log` / `print` 可调用且不抛，非字符串参数走 `String()` 兜底 |
| G | 模块加载 + 实例生命周期：`LoadModule` / `InvokeExportedFunction` / `CreateInstance` / `OnUpdate` / `ApplyInstanceField` / `EnumerateInstanceFields` / `UnloadModule` |
| H | 异常能被捕获、捕获后 VM 仍可用、参数类型不对是 NaN 不是崩 |

### G 组是重点

`Mover.prototype.OnUpdate` 里穿过的是 C++ 的 `VectorMath.Add` / `Scale`：

```js
Mover.prototype.OnUpdate = function (dt) {
    this.ticks += 1;
    this.origin = VectorMath.Add(this.origin,
        VectorMath.Scale(VectorMath.UnitX(), this.speed * dt));
};
```

`speed=2`、`dt=0.5` 连跑两帧后 `origin.x` 必须是 2；再由 C++ 用
`ApplyInstanceField(speed, "10")` 改成 10，再跑一帧必须是 7。这条链路正是
`TypeScriptComponent` 每帧走的路：**JS 实例 -> C++ 绑定 -> JS**。

## 3. 一个必须知道的坑：静态注册器不会被拉进链接

`ZClassScriptBindingRegistrar` 住在
`build/Generated/ZHT/ZRuntime/Src/<Name>.gen.cpp`，靠静态初始化把自己挂进
`ScriptBindingRegistry` 的侵入式链表。

链接器从 `libZRuntime.a` 里**只拉被引用的成员**。这些注册器没有任何外部引用，
所以静态链接时它们根本不会被拉进来 —— `InstallAll` 看到一张空表，
`InstallGlobals` 于是一个 global 都发不出来。

所以这个测试必须把用到的 `.gen.cpp` 显式编进来（CMake 里用 `target_sources`
加 4 个；本机脚本用 `--extra-source` 传同样的 4 个）。

## 4. 怎么跑

有 cmake：

```bash
cmake -B build -DZENGINE_BUILD_SCRIPT_VM_SMOKE_TEST=ON
cmake --build build --target ScriptVMSmokeTest
ctest --test-dir build -R ScriptVMSmokeTest
```

没 cmake（本机现状）：

```bash
python3 scripts/run_smoke_test.py \
  engine/Source/Runtime/Scripting/ScriptVM/Test/ScriptVMSmokeTest.cpp \
  --template Function/Audio/AudioSystem.cpp \
  --extra-source engine/Source/Runtime/Scripting/Backends/QuickJS/BackendQuickJS.cpp \
  --extra-source build/Generated/ZHT/ZRuntime/Src/ScriptMath.gen.cpp \
  --extra-source build/Generated/ZHT/ZRuntime/Src/ScriptTime.gen.cpp \
  --extra-source build/Generated/ZHT/ZRuntime/Src/ScriptInput.gen.cpp \
  --extra-source build/Generated/ZHT/ZRuntime/Src/KeyCodes.gen.cpp \
  --extra-source engine/Source/Runtime/Core/Time/TimeState.cpp
```

（`BackendQuickJS.cpp` 是因为它只有 `GetModuleExecutor` / `OnEnter` 两个非 inline
成员；`TimeState.cpp` 是新文件，旧的 `libZRuntime.a` 里没有。）

不需要起引擎、不需要窗口、不需要 GPU：`ScriptEnv` 的构造只用到 `LOG_*`，
`LoadModule` 里那次 `GET_SYSTEM(AssetBundleManager)` 是 nullptr 容忍的。

## 5. 负向验证（证明这些断言真的会红）

| 注入 | 结果 |
|------|------|
| `VectorMath::Sub` 从 `a - b` 改成 `a + b` | B1 失败（1 项） |
| 注释掉 `ScriptBindingRegistry::InstallGlobals(m_Papis, env)` | 29 项失败：A1/A2/B 全组/C 全组/D 全组/E 全组/G6/G7/G9/G10/G12/G15/G17/H4，报 `ReferenceError: VectorMath is not defined` |
| ZENUM 安装时 `entry.value + 1` | 只有 E1 失败（`got='33' want='32'`） |

第二行值得单独说：A3（`console` / `Debug` shim）在那一跑里**仍然是 ok** 的——
因为 shim 不来自 `InstallGlobals`。这说明"类没发布"和"shim 没注入"是两件独立
的事，测试把它们分开了。

三处注入都已还原，复跑 55/0。

## 6. 顺带钉住的一件事：`QuaternionMath.ToEuler` 的分量->轴映射

`ScriptMath.h` 里原来的注释写着"三个分量在 Z-up 下各自对应哪根轴读不出来，
等真机验证确定了轴向再补"。这次真跑一遍就确定了（C1）：

| 绕哪根轴转 90° | 落在哪个分量 |
|----------------|--------------|
| X | x（pitch） |
| Y | z（roll） |
| Z | y（yaw） |

也就是 **x = pitch = 绕 X，y = yaw = 绕 Z，z = roll = 绕 Y**。yaw 绕 Z 正是
Z-up 该有的样子（竖直轴是 Z）。`ScriptMath.h` 的注释已按此更新。

反向的 `Euler(x, y, z)` **仍然没有**：反向要先定死旋转顺序（YXZ 还是 ZYX），
定错一样是静默算错，等真有需求再定。

## 7. 刻意没做

* **V8 后端**：`BackendQuickJS.h` 在非 QuickJS 构建下是空的，测试直接 `#error`。
  等哪天要在 V8 上跑，加一个 `BackendV8` 分支即可，断言不用改（都是
  `pesapi` 层面的）。
* **TypeScriptComponent 真身**：测试用的是手写的 CommonJS 模块（等价于 tsc 在
  `"module": "commonjs"` 下的产物），没把 tsc 拖进测试依赖。
* **多 env / 多线程**：pesapi 明确不是线程安全的，注释也写了"必须从主线程调"。
  这里只有一个 env。
* **远端调试器**：`OpenRemoteDebugger` 在 QuickJS 后端是空实现，没得测。
