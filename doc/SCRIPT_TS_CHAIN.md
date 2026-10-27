# TS → tsc → .js → ScriptEnv → C++：用户真正走的那条路

> 配套：`doc/SCRIPT_VM_E2E.md`（pesapi 绑定链端到端）
> 测试：`engine/Source/Runtime/Scripting/ScriptVM/Test/ScriptTsChainSmokeTest.cpp`（31 项）
> 校验脚本：`scripts/check_ts_chain.py`（需要 node）
> 样例 TS：`engine/Source/Runtime/Scripting/ScriptVM/Test/Scripts/`（**仓里第一批引擎自己的 .ts**）

## 1. 之前缺的是哪一段

`ScriptVMSmokeTest` 证明了 pesapi 绑定链能跑，但它喂给 `ScriptEnv` 的是**手写**
的 CommonJS。用户真正走的那条路一次都没跑过：

```
写 .ts → tsc 按项目 tsconfig 编译 → 产物 .js
       → ScriptEnv::LoadModule → new 出来 → 每帧 OnUpdate
```

而 `bin/Debug/Scripts/`（项目里放 TS 源码的目录）**是空的**，全仓在 3rdparty 之外
没有一个 `.ts`。

这条路上真正会出问题的三件事，编译期一个都看不出来：

| # | 风险 | 现在的状态 |
|---|------|-----------|
| 1 | tsc 的 CommonJS 产物（`Object.defineProperty(exports, "__esModule", ...)` + ES2020 原生 class）能不能被 `LoadModuleInternal` 的 IIFE 包装正确执行 | 已验证：能 |
| 2 | `class Mover extends Behaviour` 在 `target: es2020` 下是**原生 class 继承**，原型链能不能被 `__zNewInstance` new 出来 | 已验证：`instanceof Behaviour` 为真 |
| 3 | 生成的 `zengine.d.ts` 到底能不能被真正的类型检查器接受 | 已验证：tsc 5.6.3 编译通过 |

第 3 条此前只有字符串形式的断言（`ReflectionCodegenSmokeTest` 检查 `AppendTypeScriptDeclarations`
的输出里有没有某几行），从来没交给 tsc。

## 2. 两件工具各管一半

| 工具 | 管什么 | 需要 node |
|------|--------|-----------|
| `ScriptTsChainSmokeTest` | 吃 tsc 的产物，验证"跑得起来 + 值对" | 不需要（`TscOutput/` 签进仓库了） |
| `scripts/check_ts_chain.py` | 重新生成 d.ts → tsc 编译 → 与 `TscOutput/` diff → 再跑测试 | 需要 |

分开的原因是"签进的产物"有个天然的失效方式：有人改了 `Mover.ts` / `tsconfig.json`，
或者引擎的 d.ts 变了，却忘了重新编译 —— 于是测试喂给 `ScriptEnv` 的其实是一份过期的
`.js`，而所有断言照样绿。脚本堵的就是这个洞。

跑法：

```bash
python3 scripts/check_ts_chain.py                 # 校验（漂移就非零退出）
python3 scripts/check_ts_chain.py --update        # 刷新 TscOutput/（确认过 diff 再刷）
python3 scripts/check_ts_chain.py --strict-dts    # 额外对 d.ts 本身做一次不开
                                                  # skipLibCheck 的检查（只报 warning）
```

## 3. d.ts 的单一来源

原来 `zengine.d.ts` 的模板（`kZEngineDtsTemplate`）埋在 `ProjectInfo.cpp` 的匿名
namespace 里，只有编辑器写文件的那条路径能拿到它 —— 想在编辑器之外复现同一份内容
就得复制一份模板，而复制的后果是**用户编译所依赖的 .d.ts 和运行时实际装的 global
悄悄漂移，两边各自的检查都还是绿的**。

现在挪到了绑定层：

```cpp
namespace ScriptBindingRegistry
{
    /// 完整的 zengine.d.ts = 手写前缀 + AppendTypeScriptDeclarations。
    std::string BuildTypeScriptDeclarationFile();
}
```

`ProjectInfo::EnsureScriptsScaffold` 用它写文件，`ScriptTsChainSmokeTest --dump-dts`
用它给 tsc 导出。两处必然字节一致。

顺带发现：**仓库里 `bin/Debug/Intermediate/Typings/zengine.d.ts` 是 2026-10-26 的旧
产物**，`declare class` 里没有 `Time` / `Input` / `VectorMath` / `QuaternionMath` /
`Mathf`，`declare enum` 里没有 `Key`。也就是说那份产物已经漂移了三轮提交，而没人
发现（它是 gitignore 的构建产物，编辑器每次开项目会重写，所以不影响用户——但"没人
发现"本身就是问题）。

## 4. 顺手修的一个真 bug：抛异常的模块会被留在缓存里

给新测试做负向验证时（把运行时的 `Behaviour` shim 从 `function(){}` 改成 `{}`）
撞出来的：那时 `LoadModule('Mover')` 返回 false，但 `IsModuleLoaded('Mover')` **竟然
是真**。

原因是 `LoadModuleInternal` 有个 exports 兜底分支：

```cpp
if (!resolved_exports)
    resolved_exports = pesapi_get_property(m_Papis, env, module_obj, "exports");
```

模块执行到一半抛了（`class X extends NotAConstructor` 在**模块求值阶段**就炸），
但这个兜底仍然拿得到一个非空的 `module.exports`，于是照样把引用塞进 `m_Modules`。
后果：

* `IsModuleLoaded()` 说"已加载"；
* 之后每次 `LoadModule` 直接命中缓存返回 true；
* 这段模块**永远不会再被执行一次** —— 改好源码再热重载，拿到的还是那次抛异常
  留下的空壳。

修复：`ScriptEnv::LoadModule` 在捕获到异常时 `UnloadModule(module_id)`。
`Scripts/Broken.ts`（模块顶层就 throw）+ T26/T27/T28 把这三条钉住了。

## 5. 测试覆盖了什么（31 项）

| 组 | 内容 |
|----|------|
| A | tsc 产物能被 IIFE 包装加载、`CreateInstance` 能 new 出 ES2020 原生 class |
| B | 原型链：`instanceof Behaviour` 为真、`Behaviour.prototype.OnDestroy` 被继承到了、未注入时 `this.gameObject` 是 falsy |
| C | 每帧 `OnUpdate` 穿过 C++ 的 `VectorMath.Add/Scale`；脚本读 `Time.GetDeltaTime()` 拿到的是 `TimeState` 推的值 |
| D | C++ 用 `ApplyInstanceField` 改 `speed`，脚本下一帧按新值走 |
| E | `EnumerateInstanceFields` 不把原型方法当字段；字段初始值都还在 |
| F | 抛异常的模块不留缓存（见第 4 节） |
| G | 卸载后 `CreateInstance` 返回 nullptr |

## 6. 负向验证（三处均已还原，复跑 31/0）

| 注入 | 结果 |
|------|------|
| 改 `Mover.ts`（`*` 改 `+`）但**不**重新编译 | `check_ts_chain.py` 报"漂移：Mover.js"并非零退出 |
| d.ts 里 ZENUM 的名字改成 `ZzKey` | `error TS2304: Cannot find name 'Key'`，脚本在第 2 步就失败 |
| 运行时 `g.Behaviour` 从 `function(){}` 改成 `{}` | T1/T3 失败（`class Mover extends Behaviour` 在模块求值阶段抛）—— 并由此发现第 4 节那个 bug |

## 7. 已知取舍

* **`skipLibCheck: true`**。测试用的可执行文件只编了 6 个类的 `.gen.cpp`，所以生成的
  d.ts 里没有 `GameObject` / `Transform`（它们只被 `Behaviour` 的声明引用到）。开着
  `skipLibCheck` 就没问题 —— 真实工程的 tsconfig 也是这么配的。想看 d.ts 本身的完整
  类型检查就用 `--strict-dts`，它会把这些缺失当 warning 打出来。
  试过把 `GameObject.gen.cpp` / `Transform.gen.cpp` 也编进来，但 `GameObject::
  AddComponentByTypeName` 不在旧 `libZRuntime.a` 里，运行期 dyld 直接挂，所以放弃了。
* **只签了 `.js`，没签 `.js.map`**（`sourceMap: false`）。
* **没有 es5 / `__extends` 变体**。项目 tsconfig 钉的是 `target: es2020`，tsc 发的是
  原生 class，不走 `__extends` helper。哪天要支持 es5 目标，加一份 tsconfig + 产物即可。
* `Typings/` 下的 `zengine.d.ts` 是脚本导出的，已加进 `.gitignore`。
