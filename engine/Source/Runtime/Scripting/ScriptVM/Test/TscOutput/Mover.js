"use strict";
// ==========================================================================
// 引擎里第一个真正的 TypeScript 脚本。
//
// 为什么它必须存在
// ---------------
// 在它之前，脚本层的验证只到"手写 CommonJS 能被 ScriptEnv 加载"为止。用户真正走
// 的那条路一次都没跑过：
//
//     写 .ts -> tsc 按项目 tsconfig 编译 -> 产物 .js
//            -> ScriptEnv::LoadModule -> new 出来 -> 每帧 OnUpdate
//
// 这条路上真正会出问题的三件事，编译期一个都看不出来：
//
//   1. tsc 的 CommonJS 产物（`Object.defineProperty(exports, "__esModule", ...)`
//      + ES2020 原生 class）能不能被 LoadModuleInternal 的 IIFE 包装正确执行；
//   2. `class Mover extends Behaviour` 在 target=es2020 下是**原生 class 继承**，
//      原型链能不能被 `__zNewInstance` 正确 new 出来。`instanceof Behaviour` 为假
//      意味着原型链没接上，那 TypeScriptComponent 注入的 gameObject / transform
//      就会落在一个没有原型的对象上；
//   3. 生成的 zengine.d.ts 到底能不能被真正的类型检查器接受。此前对这个文件只有
//      字符串形式的断言，从没交给 tsc。
//
// 编译产物同目录 TscOutput/Mover.js 是签进仓库的（这样没有 node 的机器也能跑
// ScriptTsChainSmokeTest）。**不要手改它** —— 由 scripts/check_ts_chain.py 重新
// 生成并 diff。
//
// 这个文件本身也是"d.ts 好不好用"的样本：它故意把新加的几组 API 都用上一遍
// （VectorMath / Time / Input / Key / Debug / Behaviour 的注入属性）。
// ==========================================================================
Object.defineProperty(exports, "__esModule", { value: true });
exports.Mover = void 0;
class Mover extends Behaviour {
    constructor() {
        super(...arguments);
        this.speed = 2;
        this.origin = { x: 0, y: 0, z: 0 };
        this.ticks = 0;
        this.lastDt = 0;
        this.sawKeyDown = false;
        this.seenDeltaTime = 0;
        // 下面三个由 OnStart 填，ScriptTsChainSmokeTest 靠它们判断原型链是否接上。
        this.isBehaviour = false;
        this.inheritedHook = "";
        this.hasGameObject = true;
        this.lastSnapshot = "";
        // ---- 协程（Unity 的 StartCoroutine / yield）----------------------------
        //
        // 这一段是**端到端**的证明：C++ 侧的每个断言都能在完全没有 JS 的情况下
        // 通过（UnityParitySmokeTest 的 CoroutineProbe 就是这么做的），只有真的
        // 从 TypeScript 里 `*MyRoutine()` 出一个生成器、真的 `yield new
        // WaitForSeconds(...)`，才能证明 shim 的 `__zWait` 标记、ScriptEnv 的
        // 翻译、以及调度器的相位三者接得上。
        this.routineSteps = 0;
        this.routineKinds = "";
        this.routineDone = false;
        this.routineHandleOk = false;
        this.lastRoutineSnapshot = "";
        // ---- WaitUntil / WaitWhile -------------------------------------------
        //
        // 这两个带**闭包**，是 Unity 的 YieldInstruction 家族里唯一不能被"标记 +
        // 常量"表达完的两个：谓词必须每帧被问一次，直到它翻面。所以这里除了位
        // （1 / 2 / 4）之外还数了 `untilPolls` —— 只在 yield 那一帧问一次的实现，
        // 位最终也能走到 7，但 polls 会停在 1。
        this.gateReady = false;
        this.untilSteps = 0;
        this.untilPolls = 0;
        // ---- yield return StartCoroutine(other) ------------------------------
        //
        // Unity 的嵌套协程：外层不是"等一帧"，而是"等那个协程整个跑完"。
        //
        // ⚠️ 内层必须**多于一帧**（这里是两段 yield）：`yield this.StartCoroutine(...)`
        // 被当成 `yield null` 的实现，外层也只晚一帧就继续了，step 位最终照样能
        // 走到 7。只有"内层还在跑的时候外层必须停住"这一条能区分两者。
        this.nestedSteps = 0;
        this.nestedInner = 0;
        // ---- Unity 的应用层消息 ---------------------------------------------
        //
        // ⚠️ 两处 `=== true` 是**故意**的：它们能抓住"把 bool 当 1.0 / 0.0 传进
        // 来"的实现 —— 那种实现下 `if (hasFocus)` 是对的，但 `hasFocus === true`
        // 是假的。用隐式真值判断写这段，这个 bug 就永远测不出来。
        this.focusLog = "";
        this.pauseLog = "";
        this.quitCount = 0;
        this.parentChanged = 0;
        this.childrenChanged = 0;
        this.validateCount = 0;
        // 回调里读一次 `transform.parent`：派发点必须在层级**更新之后**。早一步
        // 派发的话这里读到的还是旧父级，而回调本身照样被调用一次 —— 只看调用
        // 次数完全分不出来。
        this.parentWasNullAtCallback = true;
        // 与上面那条是同一件事的**两半**，但收的人不同：上面是"我换父级了"（发给
        // 被搬的对象），这一个是"我的孩子多了/少了一个"（发给父级）。名字只差一个
        // 词，所以这边的判据也照抄一份结构：回调里读一次 `transform.childCount`，
        // 值必须是**新的**（1 / 0），否则"派发早了一步"照样计数对得上。
        this.childCountAtCallback = -1;
        // ---- Unity 的 gizmo 消息 ---------------------------------------------
        //
        // ⚠️ 判据与三趟 Tick **相反**：派发只判 `activeInHierarchy`，**不判 enabled**
        // —— 停用的脚本照样画辅助线。Unity 在 MonoBehaviour 参考页是明文写了的
        // （"The checkbox for disabling a MonoBehavior will only prevent Start,
        // Awake, Update, FixedUpdate, and OnGUI from executing"）。
        //
        // 这段计数的价值就在这一条：C++ 侧的 DG22 断言的是引擎自己的组件，这里断言
        // 的是**脚本**那条链路也照样不判 enabled。两边缺任何一边都测不到"TS 侧
        // 被某个中途加回来的判据挡住"。
        this.gizmoDraws = 0;
        this.gizmoSelectedDraws = 0;
        this.gizmoAtDraw = "";
        // ---- Unity 的七个鼠标消息 -------------------------------------------
        //
        // 与 gizmo 同一条判据：只判 activeInHierarchy，**不判 enabled**。停用的脚本
        // 照样收到 OnMouse*（Unity 的 enabled 只挡 Start/Awake/Update/FixedUpdate/
        // OnGUI）。C++ 侧的 ME 组断言的是引擎自己的组件，这一段断言的是**脚本**那条
        // 链路：`GameObject::BroadcastMouseEvent` → `TypeScriptComponent::OnMouse*` →
        // JS。
        //
        // ⚠️ 七条各用一个**不同的字母**，而不是共用一个计数器：共用计数器的话
        // "OnMouseUp 与 OnMouseUpAsButton 转发到了同一个 JS 名字"这种 bug 是绿的
        // （计数照样 +2）。同理 `mouseDownHadGameObject` 是"注入属性在回调里可读"
        // 的判据 —— 早一步/晚一步派发都会让 gameObject 为空。
        this.mouseLog = "";
        this.mouseDownHadGameObject = false;
        // ---- Unity 的两条可见性消息 ------------------------------------------
        //
        // 与鼠标那七条同一套判据（只判 activeInHierarchy，不判 enabled），但**只在
        // 变化那一帧来一次**。两个字母必须不同：共用一个计数的话"进和出转发到了
        // 同一个 JS 名字"是绿的（计数照样 +2）。
        this.visibilityLog = "";
        // ---- 值类型的静态门面：Vector2 / Vector4 / Matrix4x4 ------------------
        //
        // 这段的价值**在 tsc**：`Vector2.zero` 这种写法只有 d.ts 里同时存在
        // `declare interface Vector2`（值类型形状）和 `declare class Vector2`（静态
        // 面）时才编译得过 —— 两者同名合并。C++ 侧的 VT 组断言算术，ScriptVM 侧断言
        // 发布面存在，只有这里证明"合出来的那个类型真的能当值用"。
        //
        // ⚠️ 夹具一律不对称：(3,4) 而不是 (1,1)，`SignedAngle` 两个方向都取，Lerp 的
        // 夹与不夹各来一次。对称夹具分不出对错。
        this.vtSnapshot = "";
        this.vtAxis = "";
        // ---- Bounds / Color32 -------------------------------------------------
        //
        // 这两个是 Unity 里仅剩、而引擎里此前**零命中**的值类型。C++ 侧的 BC 组
        // 断言算术，ScriptVMSmokeTest 的 M 组断言"脚本看不看得见" —— 只有这里能
        // 证明 `Bounds.Create(...)` 在 **.ts 里也能通过 tsc 的类型检查**（d.ts 里
        // `declare interface Bounds` 与 `declare class Bounds` 的同名合并在真正的
        // 类型检查器下是成立的，不只是两段互不相干的字符串）。
        this.measured = "";
        // ---- Unity 的 `Quaternion.Euler` ---------------------------------------
        //
        // ⚠️ 这段的价值在 **tsc** 与"两处同源"：此前引擎里根本没有反向的
        // Euler(x, y, z)（"定错旋转顺序一样是静默算错"），所以脚本只能靠
        // `AngleAxis` 自己拼。拼出来的是不是和 `transform.eulerAngles` 同一套
        // 约定，编译期看不出来，运行期也看不出来 —— 只有"设进去再读回来"能看见。
        //
        // 判据是**往返**而不是"调用成功了"：`GetEulerAngles(Euler(v))` 必须等于 v。
        // 少任何一边（Euler 用了另一套顺序 / GetEulerAngles 换了轴）这里立刻不相等。
        this.quatRoundTrip = "";
        this.quatYToZ = "";
        // ---- Unity 的 `AnimationCurve` ----------------------------------------
        //
        // 与 `Bounds` 同一类理由：这段的价值在 **tsc** 与"端到端真的取值"。曲线是
        // 值类型（JS 端就是 `{keys, preWrapMode, postWrapMode}`），静态方法挂在同名
        // 的 `declare class` 上 —— 同名合并只在**真的类型检查器**跑过之后才算成立。
        //
        // 判据挑的是"写错了也不报错"的那几个数：
        //   * `Linear(0,0,10,20)` 在 **t=2.5** 处必须是 5（端点切线为 0 时 Hermite
        //     才退化成直线；只看端点或中点都分不出过冲的 S 形）；
        //   * `EaseInOut` 在 t=0.25 处是 0.15625 而不是 0.25；
        //   * `WrapMode.Loop` 是枚举成员（不是裸数字）—— 写 `WrapMode.Loop` 过不了
        //     tsc 的话，说明那条 ZENUM 链断了，而 C++ 测试看不见。
        this.curveProbe = "";
        // ---------------------------------------------------------------------
        // Unity 的 Vector2Int / Vector3Int / Rect / RectInt（X 段）
        // ---------------------------------------------------------------------
        //
        // 与曲线同一类理由：C++ 的算术门证明不了 **tsc 认这四个名字**。这四个类型
        // 都是"值类型 interface + 同名静态 class"的合并，任何一个环节断掉（少了
        // `declare interface`、少了 `Converter`、ZCLASS 名字对不上）都只在**真正的
        // 类型检查器**下才报错。
        //
        // 判据挑的是"写错了也不报错"的那几个数：
        //   * `FloorToInt(-0.5)` 必须是 **-1**（截断的实现给 0，而正数上两者相同）；
        //   * `RoundToInt(2.5)` 必须是 **2**（平局取偶，不是"远离零"的 3）；
        //   * `Vector3Int.forward` 是 (0,0,1) —— 常量名走的是 ZSCRIPTPROPERTY 那条
        //     链，与 ZFUNCTION 不是同一条。
        this.intVecProbe = "";
        // 判据挑的是那三处"反直觉"：
        //   * `Contains` 是**半开**区间 —— 右边界 (10, 5) 必须 false，而 (0, 0) 是 true；
        //   * 负 width 的矩形 `xMin` 是 0 而不是 10（两条边归一化）；
        //   * `RectInt.ClampToBounds` 把伸出去的矩形拉回 `xMax - width` = 7；
        //   * 只共享一条边的两个矩形**不**相交（与 `Bounds.Intersects` 相反）。
        this.rectProbe = "";
        // ---------------------------------------------------------------------
        // Unity 的 Plane / GeometryUtility（Y 段）
        // ---------------------------------------------------------------------
        //
        // 与 X 段同一类理由：C++ 的 `Plane.h` / `Frustum.h` 一直都在，但脚本侧
        // **一个都没有** —— `Converter<Plane>`、`declare interface Plane`、门面全是
        // 零命中。这里端到端走一遍，证明 `Plane` 既是 `{normal, distance}` 值又挂着
        // 静态方法（同名合并），且 `GeometryUtility` 这个名字在 tsc 下存在。
        //
        // 判据挑的是"写错了也不报错"的那几个数：
        //   * `distance` 必须是 **-3**（`dot(n,p) + d = 0`），写成 +3 的话整个视锥
        //     内外反过来，而 `TestPlanesAABB` 照样返回一个看着合理的 bool；
        //   * 从**背面**射的射线必须返回 **-1**（只打正面）；
        //   * 六个面都要判，所以"只看第一个面"的实现在末位 false 上会露馅。
        this.planeProbe = "";
        /// `Mathf.SmoothDamp` / `Vector3.SmoothDamp` 的端到端探针。
        ///
        /// ⚠️ 这一条必须走 **TS**，C++ 侧的 `TestSmoothDamp` 测不到它：值类型跨边界
        /// 是**快照**，而 SmoothDamp 的返回值是两个 float（或六个）的**结构体**。
        /// 少一个 `Converter<SmoothDampResult>`，TS 侧拿到的是 `undefined`，`.value`
        /// 读出来是 `undefined` —— 而 `String(undefined)` 照样能拼进字符串，不报错。
        ///
        /// 判据挑的是"写错了也不报错"的那几个数：
        ///   * 第一帧 **0.002061**：换成 `std::exp(-x)` 会是 0.002126。两条曲线都平滑、
        ///     都收敛，只有这个数能分清；
        ///   * 限速 **0.001031** vs 不限速 **2.061**：差三个数量级。只比大小的话任何
        ///     限速都过；
        ///   * `SmoothDampAngle(350, 10)` 必须 **> 350**（跨 0 走 +20 的短弧），往下走
        ///     说明少了 `DeltaAngle`；
        ///   * `Vector3` 版的 z 分量（已经在目标上）必须是 **0**。
        this.smoothDampProbe = "";
        /// `LayerMask` 的端到端探针。
        ///
        /// ⚠️ 这一条必须走 **TS**：C++ 那边 `TagLayerManager` 一直都在，`Physics.LayerToName`
        /// 也早就有了，缺的正是 **tsc 认 `LayerMask` 这个名字**。而"方法不存在"在
        /// 运行时是静默无事发生（InvokeInstanceMethodNumber 对找不到的方法不报错）。
        ///
        /// 判据挑的是"写错了也不报错"的那几个：
        ///   * `-1`（拼错的层名），不是 0 —— 0 是 Default，射线照样有结果；
        ///   * `32`（未知层名**跳过**，不是 -1 = 所有层）；
        ///   * `33` = (1<<0)|(1<<5)。
        this.layerMaskProbe = "";
        /// `Vector3` 静态面补齐（`Slerp` / `RotateTowards` / `ProjectOnPlane` /
        /// `SignedAngle`）与 `Mathf.PerlinNoise1D` 的端到端探针。
        ///
        /// ⚠️ 与 LayerMask 同一类理由：这四个算法在 `VectorMath` 上早就有了，缺的是
        /// Unity 的拼写。判据挑的是"替代实现也满足"之外的那几个：
        ///   * `Slerp` 的**长度 1.0000** vs `Lerp` 的 **0.7071** —— 只看方向分不开；
        ///   * `ProjectOnPlane` 用**非单位**法线 `(0,0,2)`；
        ///   * `SignedAngle` 的 **-90**（换顺序必须换符号，只测 90 的话 `Angle` 也过）。
        this.vecStaticsProbe = "";
        /// `Transform` 补齐面的端到端探针：`hasChanged` / `hierarchyCount` /
        /// `hierarchyCapacity` / `localToWorldMatrix` / `worldToLocalMatrix` /
        /// `SetPositionAndRotation` / `SetLocalPositionAndRotation`。
        ///
        /// 这一段真正要抓的**不是**算法（C++ 的 TE / MA 段已经钉死了），而是
        /// 「属性拼写真的进了 d.ts」：ZSCRIPTPROPERTY 少写一条，tsc 直接报
        /// `Property 'hasChanged' does not exist on type 'Transform'` —— 比任何
        /// 运行时断言都早，而且 C++ 侧一个测试都发现不了。
        ///
        /// 运行时这部分只验两件 tsc 验不了的事：
        ///   1. 属性和方法**是同一个东西**（`t.hierarchyCount` vs
        ///      `t.GetHierarchyCount()`）—— getter 挂错方法时类型是对的、值是错的；
        ///   2. 矩阵属性和 `TransformPoint` 是同一套（getter 挂到 worldToLocal 上
        ///      的话仍然是个 Matrix4x4，类型检查一样过）。
        this.transformExtrasProbe = "";
        /// `Color` 补齐面 + `ColorUtility` 的端到端探针。
        ///
        /// 判据挑的是"替代实现也满足"之外的那几个：
        ///   * `GetGrayscale` 的**三个纯色互不相同**（1/3 平均值版三条都是 1/3）；
        ///   * HTML 串 0.5 -> **80**，而 `Color32` 截断是 127；
        ///   * 缩写串 `#FFF` 必须是**纯白**（每位 `*17`，不是 `*16` 的 F0）；
        ///   * 解析失败时 `valid === false` 且 `color` 全 0（含 a）。
        this.colorExtrasProbe = "";
        /// `SystemInfo` / `QualitySettings` / `Cursor` / `Time` 补项 / `Application` 补项
        /// 的**脚本侧**探针。
        ///
        /// C++ 侧的 UnityParitySmokeTest 只钉住"类本身对不对"；这里钉的是"这些名字
        /// 在 TS 里真的存在且类型对" —— 少了 `.d.ts` 里的 `declare class`、或者
        /// 静态属性没装到构造器上，C++ 那一侧是全绿的。
        ///
        /// ⚠️ 断言的是**哨兵值与夹取规则**，不是"真机上的真值"：这个进程没有
        /// RenderSystem，图形相关的一律走"拿不到"那条路 —— 那条路才是没人测过的。
        this.platformProbe = "";
        this.inputProbe = "";
        this.materialLightProbe = "";
        this.meshProbe = "";
        this.animatorProbe = "";
        // ---- Unity 的 FixedUpdate ---------------------------------------------
        //
        // 这一段存在的理由不只是"多一个钩子"：在此之前 `OnFixedUpdate` 在 d.ts 里
        // **根本没有声明**（C++ 侧每个固定步都在调，脚本侧却看不见） —— 于是 TS 里
        // 写 `OnFixedUpdate(...)` 过不了 tsc，而这个洞靠任何 C++ 测试都发现不了
        // （InvokeInstanceMethodNumber 对"方法不存在"是静默无事发生的）。
        //
        // ⚠️ 判据是**步长**，不是"调用了几次"：`OnUpdate` 拿帧间隔、`OnFixedUpdate`
        // 拿固定步长。传错的话计数照样 +1，只有 `lastFixedDt` 能看出来。
        this.fixedTicks = 0;
        this.lastFixedDt = 0;
        this.fixedDtMatches = false;
    }
    /// 测试通过 `InvokeMethod("StartMyRoutine")` 触发。
    StartMyRoutine() {
        this.routineSteps = 0;
        this.routineKinds = "";
        this.routineDone = false;
        const handle = this.StartCoroutine("MyRoutine");
        this.routineHandleOk = (handle != null);
    }
    /// 一个 Unity 式的协程体。三次 yield，三种等待，每步写一个不同的位
    /// （1 / 2 / 4）—— 这样"跑到了第几步"是一个数字就能读出来的，而不是
    /// 要靠数字符串。
    *MyRoutine() {
        this.routineSteps = this.routineSteps + 1;
        this.routineKinds = "a";
        yield new WaitForSeconds(0.2);
        this.routineSteps = this.routineSteps + 2;
        this.routineKinds = this.routineKinds + "b";
        yield new WaitForEndOfFrame();
        this.routineSteps = this.routineSteps + 4;
        this.routineKinds = this.routineKinds + "c";
        this.routineDone = true;
    }
    StartUntilRoutine() {
        this.untilSteps = 0;
        this.untilPolls = 0;
        this.gateReady = false;
        this.StartCoroutine("WaitUntilRoutine");
    }
    *WaitUntilRoutine() {
        this.untilSteps = this.untilSteps + 1;
        // 箭头函数：this 是词法捕获的，C++ 侧拿到的只是一个可调用值。
        yield new WaitUntil(() => {
            this.untilPolls = this.untilPolls + 1;
            return this.gateReady;
        });
        this.untilSteps = this.untilSteps + 2;
        // 反过来的一半：gateReady 现在是 true，所以 WaitWhile 必须**挡住**。
        // 只测 WaitUntil 的话，"把 WaitWhile 当 WaitUntil 转发"这种实现是绿的。
        //
        // 这个闭包也要数 `untilPolls`：轮询计数是"每帧都在问"的判据，而它必须
        // 覆盖**两个**等待 —— 只数 WaitUntil 那个，WaitWhile 这一段的轮询
        // 频率就完全没被观测到。
        yield new WaitWhile(() => {
            this.untilPolls = this.untilPolls + 1;
            return this.gateReady;
        });
        this.untilSteps = this.untilSteps + 4;
    }
    StartNestedRoutine() {
        this.nestedSteps = 0;
        this.nestedInner = 0;
        this.StartCoroutine("NestedOuterRoutine");
    }
    *NestedOuterRoutine() {
        this.nestedSteps = this.nestedSteps + 1; // 1，同步段
        yield this.StartCoroutine("NestedInnerRoutine");
        this.nestedSteps = this.nestedSteps + 2; // 3，内层结束后才到
        yield null;
        this.nestedSteps = this.nestedSteps + 4; // 7
    }
    *NestedInnerRoutine() {
        this.nestedInner = this.nestedInner + 1; // 1
        yield null;
        this.nestedInner = this.nestedInner + 2; // 3
        yield null;
        this.nestedInner = this.nestedInner + 4; // 7，跑完
    }
    OnApplicationFocus(hasFocus) {
        this.focusLog = this.focusLog + (hasFocus === true ? "T" : "F");
    }
    OnApplicationPause(pauseStatus) {
        this.pauseLog = this.pauseLog + (pauseStatus === true ? "T" : "F");
    }
    OnApplicationQuit() {
        this.quitCount = this.quitCount + 1;
    }
    OnTransformParentChanged() {
        this.parentChanged = this.parentChanged + 1;
        this.parentWasNullAtCallback = this.transform.parent === null;
    }
    OnTransformChildrenChanged() {
        this.childrenChanged = this.childrenChanged + 1;
        this.childCountAtCallback = this.transform.childCount;
    }
    OnValidate() {
        this.validateCount = this.validateCount + 1;
    }
    OnDrawGizmos() {
        this.gizmoDraws = this.gizmoDraws + 1;
        // 回调里真的画一次，并且读回 `Gizmos.color` —— 证明这次派发发生在
        // "gizmo 后端可用"的上下文里（后端缺失时是 no-op，不抛）。
        Gizmos.color = { r: 0, g: 1, b: 0, a: 1 };
        Gizmos.DrawWireSphere({ x: 0, y: 0, z: 0 }, 1);
        this.gizmoAtDraw = String(Gizmos.color.g);
        Gizmos.color = { r: 1, g: 1, b: 1, a: 1 };
    }
    OnDrawGizmosSelected() {
        this.gizmoSelectedDraws = this.gizmoSelectedDraws + 1;
    }
    OnMouseEnter() { this.mouseLog = this.mouseLog + "E"; }
    OnMouseOver() { this.mouseLog = this.mouseLog + "O"; }
    OnMouseExit() { this.mouseLog = this.mouseLog + "X"; }
    OnMouseDown() {
        this.mouseLog = this.mouseLog + "D";
        this.mouseDownHadGameObject = (this.gameObject != null);
    }
    OnMouseDrag() { this.mouseLog = this.mouseLog + "G"; }
    OnMouseUp() { this.mouseLog = this.mouseLog + "U"; }
    OnMouseUpAsButton() { this.mouseLog = this.mouseLog + "B"; }
    OnBecameVisible() { this.visibilityLog = this.visibilityLog + "+"; }
    OnBecameInvisible() { this.visibilityLog = this.visibilityLog + "-"; }
    ProbeValueTypes() {
        const a = { x: 3, y: 4 };
        const parts = [];
        // Vector2：常量 + 长度 + 有符号夹角（负号那个方向才是真判据）。
        parts.push(Vector2.zero.x.toFixed(0) + Vector2.zero.y.toFixed(0));
        parts.push(Vector2.one.x.toFixed(0) + Vector2.one.y.toFixed(0));
        parts.push(Vector2.right.x.toFixed(0) + Vector2.right.y.toFixed(0));
        parts.push(Vector2.up.x.toFixed(0) + Vector2.up.y.toFixed(0));
        parts.push(Vector2.Magnitude(a).toFixed(3));
        parts.push(Vector2.SignedAngle({ x: 1, y: 0 }, { x: 0, y: 1 }).toFixed(1));
        parts.push(Vector2.SignedAngle({ x: 0, y: 1 }, { x: 1, y: 0 }).toFixed(1));
        // 夹 vs 不夹：t=2 时两者差一整个 b，是这个区别唯一能被看见的地方。
        parts.push(Vector2.Lerp(Vector2.zero, { x: 10, y: 10 }, 2).x.toFixed(1));
        parts.push(Vector2.LerpUnclamped(Vector2.zero, { x: 10, y: 10 }, 2).x.toFixed(1));
        // Vector4：四维长度（(1,2,2,4) 的三维 hypot 是 3，四维才是 5）。
        const v4 = { x: 1, y: 2, z: 2, w: 4 };
        parts.push(Vector4.Magnitude(v4).toFixed(3));
        parts.push(Vector4.Normalize(v4).w.toFixed(3));
        // Matrix4x4：平移影响点、不影响方向；TRS 是 T*R*S 不是 S*R*T。
        const t = Matrix4x4.Translate({ x: 1, y: 2, z: 3 });
        const p = Matrix4x4.MultiplyPoint(t, { x: 1, y: 1, z: 1 });
        parts.push(p.x.toFixed(0) + p.y.toFixed(0) + p.z.toFixed(0));
        const d = Matrix4x4.MultiplyVector(t, { x: 1, y: 1, z: 1 });
        parts.push(d.x.toFixed(0) + d.y.toFixed(0) + d.z.toFixed(0));
        parts.push(String(Matrix4x4.IsIdentity(Matrix4x4.identity)));
        parts.push(String(Matrix4x4.IsIdentity(Matrix4x4.zero)));
        const trs = Matrix4x4.TRS({ x: 1, y: 2, z: 3 }, Quaternion.identity, { x: 2, y: 2, z: 2 });
        const q = Matrix4x4.MultiplyPoint(trs, { x: 1, y: 0, z: 0 });
        parts.push(q.x.toFixed(0) + q.y.toFixed(0) + q.z.toFixed(0));
        this.vtSnapshot = parts.join("|");
        // 跨平面那条：Vector2.right 是 +X，Vector3.right 是 -X。两边**故意**不同，
        // 被"统一"成一个符号就是错的（各自的平面里才是右）。
        this.vtAxis = Vector2.right.x.toFixed(0) + "/" + Vector3.right.x.toFixed(0);
    }
    /// 从 TypeScript 里画 gizmo。C++ 侧的 GZ 组断言的是"画出了什么"，
    /// ScriptVMSmokeTest 的 L 组断言的是"脚本看不看得见 Gizmos" —— 只有这里
    /// 证明 `Gizmos.DrawLine(...)` 在 **.ts 里也能通过类型检查**（d.ts 里那
    /// 个 `declare class Gizmos` 是真的能用的，不只是一串字符串）。
    DrawGizmo() {
        Gizmos.color = { r: 1, g: 0, b: 0, a: 1 };
        Gizmos.DrawLine({ x: 0, y: 0, z: 0 }, { x: 1, y: 0, z: 0 });
        Gizmos.DrawWireSphere({ x: 0, y: 0, z: 0 }, 1);
        // 视锥 12 条线 + 上面那条 = 13。用 12 而不是 1，是为了让"DrawFrustum 只
        // 画了近平面那 4 条"这种实现在脚本这一层也红 —— 光有一条线的话，
        // 4 条和 12 条都只表现为"有东西"。
        Gizmos.DrawFrustum({ x: 0, y: 0, z: 0 }, 90, 4, 2, 1);
        Gizmos.color = { r: 1, g: 1, b: 1, a: 1 };
    }
    MeasureBounds() {
        const box = Bounds.Create({ x: 1, y: 2, z: 3 }, { x: 4, y: 6, z: 8 });
        const grown = Bounds.Expand(box, 2);
        const size = Bounds.GetSize(grown);
        const bytes = Color32.FromColor(Color.red);
        this.measured = [
            size.x + "," + size.y + "," + size.z,
            bytes.r + "," + bytes.g + "," + bytes.b + "," + bytes.a,
            String(Bounds.Contains(box, { x: 1, y: 2, z: 3 })),
            String(Bounds.IntersectRay(box, null))
        ].join("|");
    }
    ProbeQuaternionEuler() {
        const q = Quaternion.Euler(20, 30, 40);
        const e = Quaternion.GetEulerAngles(q);
        this.quatRoundTrip = e.x.toFixed(1) + "," + e.y.toFixed(1) + "," + e.z.toFixed(1);
        // 90° 绕 X 必须把 +Y 抬到 +Z（右手系的定义，不是抄实现）。
        const r = Quaternion.Rotate(Quaternion.Euler(90, 0, 0), Vector3.up);
        // ⚠️ `Math.round` 不是装饰：浮点残差让本来该是 0 的分量是 -4e-8，
        // 直接 `toFixed(0)` 会得到 "-0"，和想要的 "0" 不相等。
        this.quatYToZ = Math.round(r.x) + "," + Math.round(r.y) + "," + Math.round(r.z);
    }
    ProbeAnimationCurve() {
        const line = AnimationCurve.Linear(0, 0, 10, 20);
        const ease = AnimationCurve.EaseInOut(0, 0, 1, 1);
        // ⚠️ `AddKey` 是**值语义**：必须接住返回值。忘了接的话曲线一直是空的，
        // 而空曲线求值是 0 —— 不报错，所以这里故意把两条路都走一遍。
        const built = AnimationCurve.AddKey(AnimationCurve.AddKey(AnimationCurve.Create(), 0, 0), 2, 4);
        const looped = AnimationCurve.SetPostWrapMode(line, WrapMode.Loop);
        this.curveProbe = [
            AnimationCurve.Evaluate(line, 2.5).toFixed(3),
            AnimationCurve.Evaluate(ease, 0.25).toFixed(5),
            String(AnimationCurve.GetKeyCount(built)),
            AnimationCurve.Evaluate(built, 1).toFixed(3),
            AnimationCurve.Evaluate(looped, 12.5).toFixed(3),
            String(AnimationCurve.GetPostWrapMode(looped) === WrapMode.Loop)
        ].join("|");
    }
    ProbeIntVectors() {
        const floored = Vector2Int.FloorToInt({ x: -0.5, y: 1.7 });
        const rounded = Vector2Int.RoundToInt({ x: 2.5, y: 3.5 });
        const stepped = Vector3Int.Add(Vector3Int.forward, Vector3Int.up);
        const dist = Vector3Int.Distance(Vector3Int.zero, { x: 1, y: 1, z: 1 });
        this.intVecProbe = [
            floored.x + "," + floored.y,
            rounded.x + "," + rounded.y,
            stepped.x + "," + stepped.y + "," + stepped.z,
            dist.toFixed(5),
            String(Vector2Int.one.x),
            String(Vector3Int.GetSqrMagnitude({ x: 1, y: 2, z: 3 }))
        ].join("|");
    }
    ProbeRects() {
        const r = Rect.Create(0, 0, 10, 20);
        const flipped = Rect.Create(10, 0, -10, 10);
        const uv = Rect.PointToNormalized(r, { x: 2.5, y: 5 });
        const clamped = RectInt.ClampToBounds(RectInt.Create(9, 2, 3, 3), RectInt.Create(0, 0, 10, 10));
        const centre = RectInt.GetCenter(RectInt.Create(0, 0, 3, 5));
        this.rectProbe = [
            String(Rect.Contains(r, { x: 10, y: 5 })),
            String(Rect.Contains(r, { x: 0, y: 0 })),
            String(Rect.GetXMin(flipped)),
            uv.x.toFixed(2) + "," + uv.y.toFixed(2),
            String(clamped.x),
            centre.x + "," + centre.y,
            String(Rect.Overlaps(Rect.Create(0, 0, 10, 10), Rect.Create(10, 0, 10, 10)))
        ].join("|");
    }
    ProbePlanes() {
        const ground = Plane.FromNormalAndPosition({ x: 0, y: 0, z: 1 }, { x: 0, y: 0, z: 3 });
        const wall = Plane.Create({ x: 1, y: 0, z: 0 }, -5); // x = 5，正面是 +x
        const t = Plane.Raycast(wall, { x: 9, y: 0, z: 0 }, { x: -1, y: 0, z: 0 });
        const fromBehind = Plane.Raycast(wall, { x: 0, y: 0, z: 0 }, { x: 1, y: 0, z: 0 });
        // 六个面围成的盒子 [-10,10]^3，法线朝内（与视锥同一约定）。
        const boxPlanes = [
            Plane.Create({ x: 1, y: 0, z: 0 }, 10),
            Plane.Create({ x: -1, y: 0, z: 0 }, 10),
            Plane.Create({ x: 0, y: 1, z: 0 }, 10),
            Plane.Create({ x: 0, y: -1, z: 0 }, 10),
            Plane.Create({ x: 0, y: 0, z: 1 }, 10),
            Plane.Create({ x: 0, y: 0, z: -1 }, 10)
        ];
        const inside = GeometryUtility.TestPlanesAABB(boxPlanes, { center: { x: 0, y: 0, z: 0 }, extents: { x: 2, y: 2, z: 2 } });
        const outside = GeometryUtility.TestPlanesAABB(boxPlanes, { center: { x: 0, y: 50, z: 0 }, extents: { x: 2, y: 2, z: 2 } });
        this.planeProbe = [
            String(ground.distance),
            String(Plane.GetDistanceToPoint(ground, { x: 0, y: 0, z: 5 })),
            String(t),
            String(fromBehind),
            String(Plane.GetFlipped(wall).distance),
            String(inside),
            String(outside)
        ].join("|");
    }
    ProbeSmoothDamp() {
        const dt = 1.0 / 60.0;
        const first = Mathf.SmoothDamp(0, 1, 0, 0.5, 0, dt);
        const limited = Mathf.SmoothDamp(0, 1000, 0, 0.5, 1, dt);
        const free = Mathf.SmoothDamp(0, 1000, 0, 0.5, 0, dt);
        const angle = Mathf.SmoothDampAngle(350, 10, 0, 0.5, 0, dt);
        const v3 = Vector3.SmoothDamp({ x: 0, y: 0, z: 0 }, { x: 100, y: 100, z: 0 }, { x: 0, y: 0, z: 0 }, 0.5, 1, dt);
        this.smoothDampProbe = [
            first.value.toFixed(6),
            first.velocity.toFixed(6),
            limited.value.toFixed(6),
            free.value.toFixed(3),
            angle.value.toFixed(3),
            v3.value.x.toFixed(6),
            v3.value.y.toFixed(6),
            String(v3.value.z)
        ].join("|");
    }
    ProbeLayerMask() {
        const mask = LayerMask.GetMask(["Default", "UI"]);
        this.layerMaskProbe = [
            String(LayerMask.NameToLayer("Default")),
            String(LayerMask.NameToLayer("UI")),
            String(LayerMask.NameToLayer("NoSuchLayer")),
            LayerMask.LayerToName(5),
            String(mask),
            String(LayerMask.GetMask(["NoSuchLayer", "UI"])),
            String(LayerMask.MaskContainsLayer(mask, 0)),
            String(LayerMask.MaskContainsLayer(mask, 3))
        ].join("|");
    }
    ProbeVector3Statics() {
        const ex = { x: 1, y: 0, z: 0 };
        const ey = { x: 0, y: 1, z: 0 };
        const slerp = Vector3.Slerp(ex, ey, 0.5);
        const lerp = Vector3.Lerp(ex, ey, 0.5);
        const rotated = Vector3.RotateTowards(ex, ey, 0.7853982, 0);
        const flattened = Vector3.ProjectOnPlane({ x: 1, y: 2, z: 3 }, { x: 0, y: 0, z: 2 });
        this.vecStaticsProbe = [
            slerp.x.toFixed(4),
            Vector3.Magnitude(slerp).toFixed(4),
            Vector3.Magnitude(lerp).toFixed(4),
            rotated.x.toFixed(4),
            flattened.z.toFixed(4),
            Vector3.SignedAngle(ex, ey, { x: 0, y: 0, z: 1 }).toFixed(3),
            Vector3.SignedAngle(ey, ex, { x: 0, y: 0, z: 1 }).toFixed(3),
            Mathf.PerlinNoise1D(3.3).toFixed(5)
        ].join("|");
    }
    ProbeTransformExtras() {
        const t = this.transform;
        // hasChanged：写 false -> 读 -> 改 -> 读 -> 写 false -> 读。
        // 「读不清零」这一条是轮询式用法的命根子，省略了就变成只读一次。
        t.SetHasChanged(false);
        const cleared = t.hasChanged;
        t.SetLocalPosition({ x: 7, y: 0, z: 0 });
        const afterSet = t.hasChanged;
        t.SetHasChanged(false);
        const clearedAgain = t.hasChanged;
        const countByProperty = t.hierarchyCount;
        const countByMethod = t.GetHierarchyCount();
        const capacity = t.hierarchyCapacity;
        // 矩阵属性必须和 TransformPoint / InverseTransformPoint 一致。
        // 用 (1,2,3) 这种**非零点**当样本：只看平移列的话，矩阵里的旋转部分
        // 写错（转置 / 少乘一项）完全看不出来。
        const p = { x: 1, y: 2, z: 3 };
        const m = t.localToWorldMatrix;
        const im = t.worldToLocalMatrix;
        const viaMatrix = {
            x: m.m00 * p.x + m.m01 * p.y + m.m02 * p.z + m.m03,
            y: m.m10 * p.x + m.m11 * p.y + m.m12 * p.z + m.m13,
            z: m.m20 * p.x + m.m21 * p.y + m.m22 * p.z + m.m23
        };
        const viaInverseMatrix = {
            x: im.m00 * p.x + im.m01 * p.y + im.m02 * p.z + im.m03,
            y: im.m10 * p.x + im.m11 * p.y + im.m12 * p.z + im.m13,
            z: im.m20 * p.x + im.m21 * p.y + im.m22 * p.z + im.m23
        };
        const forward = t.TransformPoint(p);
        const backward = t.InverseTransformPoint(p);
        t.SetPositionAndRotation({ x: 11, y: 12, z: 13 }, { w: 1, x: 0, y: 0, z: 0 });
        const worldPos = t.position;
        this.transformExtrasProbe = [
            String(cleared),
            String(afterSet),
            String(clearedAgain),
            String(countByProperty === countByMethod),
            String(capacity >= countByProperty),
            String(countByProperty),
            String(Math.abs(viaMatrix.x - forward.x) < 0.001 &&
                Math.abs(viaMatrix.y - forward.y) < 0.001 &&
                Math.abs(viaMatrix.z - forward.z) < 0.001),
            String(Math.abs(viaInverseMatrix.x - backward.x) < 0.001 &&
                Math.abs(viaInverseMatrix.y - backward.y) < 0.001 &&
                Math.abs(viaInverseMatrix.z - backward.z) < 0.001),
            worldPos.x.toFixed(2) + "," + worldPos.y.toFixed(2) + "," + worldPos.z.toFixed(2)
        ].join("|");
    }
    ProbeColorExtras() {
        const red = { r: 1, g: 0, b: 0, a: 1 };
        const grey = { r: 0.5, g: 0.5, b: 0.5, a: 1 };
        const htmlGrey = ColorUtility.ToHtmlStringRGB(grey);
        const alphaKept = ColorUtility.ToHtmlStringRGBA({ r: 1, g: 0, b: 0, a: 0.25 });
        const alphaDropped = ColorUtility.ToHtmlStringRGB({ r: 1, g: 0, b: 0, a: 0.25 });
        const white = ColorUtility.TryParseHtmlString("#FFF");
        const longForm = ColorUtility.TryParseHtmlString("#FF0000");
        const bad = ColorUtility.TryParseHtmlString("#ZZZZZZ");
        const linear = Color.ToLinear(grey);
        const warm = Color.CorrelatedColorTemperatureToRGB(1500);
        this.colorExtrasProbe = [
            Color.GetGrayscale(red).toFixed(3),
            Color.GetGrayscale({ r: 0, g: 1, b: 0, a: 1 }).toFixed(3),
            Color.GetGrayscale({ r: 0, g: 0, b: 1, a: 1 }).toFixed(3),
            Color.GetMaxColorComponent({ r: 0.2, g: 0.9, b: 0.4, a: 1 }).toFixed(1),
            htmlGrey,
            alphaKept,
            alphaDropped,
            Color.ToString(red),
            linear.r.toFixed(3),
            Color.ToGamma(linear).r.toFixed(3),
            String(white.valid),
            String(white.color.r === 1 && white.color.g === 1 && white.color.b === 1),
            String(longForm.valid),
            ColorUtility.ToHtmlStringRGBA(longForm.color),
            String(bad.valid),
            String(bad.color.r === 0 && bad.color.g === 0 && bad.color.b === 0 && bad.color.a === 0),
            warm.b.toFixed(3),
            String(warm.a === 1)
        ].join("|");
    }
    ProbePlatformFacades() {
        // ---- SystemInfo：headless 下必须是哨兵值 ----
        const sysDeviceType = SystemInfo.deviceType;
        const sysFamily = SystemInfo.operatingSystemFamily;
        const sysProcessor = SystemInfo.processorCount;
        const sysDeviceName = SystemInfo.deviceName;
        const sysGraphicsMemory = SystemInfo.graphicsMemorySize;
        const sysShaderLevel = SystemInfo.graphicsShaderLevel;
        const sysBattery = SystemInfo.batteryLevel;
        const sysBatteryStatus = SystemInfo.batteryStatus;
        const sysInstancing = SystemInfo.supportsInstancing;
        // ---- QualitySettings：档位 / 夹取 / 越界 ----
        QualitySettings.ResetToDefaults();
        const qualityDefault = QualitySettings.qualityLevel;
        const qualityNameDefault = QualitySettings.name;
        const qualityCount = QualitySettings.GetQualityLevelCount();
        const qualityNames = QualitySettings.names;
        // ⚠️ 走**属性**而不是 SetQualityLevel：setter 被 codegen 从方法表摘掉了
        // （静态访问器只能挂一处，见 ScriptTime.h 的 GetRealtimeSinceStartup）。
        QualitySettings.qualityLevel = 4;
        const qualityAfterSet = QualitySettings.qualityLevel;
        const aaFromPreset = QualitySettings.antiAliasing;
        const shadowFromPreset = QualitySettings.shadowDistance;
        // 越界不动（不是夹紧到最高档）。
        QualitySettings.qualityLevel = 99;
        const qualityAfterOutOfRange = QualitySettings.qualityLevel;
        // 离散合法集向下取。
        QualitySettings.antiAliasing = 3;
        const aaSnapped = QualitySettings.antiAliasing;
        QualitySettings.masterTextureLimit = 7;
        const mipClamped = QualitySettings.masterTextureLimit;
        QualitySettings.ResetToDefaults();
        const qualityAfterReset = QualitySettings.qualityLevel;
        // ---- Cursor：设什么读回什么 ----
        Cursor.ResetToDefaults();
        const cursorVisibleDefault = Cursor.visible;
        const cursorLockDefault = Cursor.lockState;
        Cursor.SetCursorState(false, CursorLockMode.Locked);
        const cursorVisibleAfter = Cursor.visible;
        const cursorLockAfter = Cursor.lockState;
        Cursor.ResetToDefaults();
        const cursorVisibleReset = Cursor.visible;
        // ---- Time 补项 ----
        const maxDelta = Time.maximumDeltaTime;
        const captureRate = Time.captureFramerate;
        const sinceLevelLoad = Time.timeSinceLevelLoad;
        // ---- Application 补项 ----
        const appIdentifier = Application.identifier;
        const appMobile = Application.isMobilePlatform;
        const appConsole = Application.isConsolePlatform;
        const appFocused = Application.isFocused;
        const appLanguage = Application.systemLanguage;
        // 白名单：没有合法 scheme 的串必须被拒（防 shell 注入）。
        const appOpenBogus = Application.OpenURL("bogus");
        const appOpenInjected = Application.OpenURL("http://x/a; rm -rf /");
        Application.backgroundLoadingPriority = ThreadPriority.High;
        const appPriority = Application.backgroundLoadingPriority;
        // ---- Unity 的 Material / Light（slice 52）----
        //
        // ⚠️ 这一段只能验证**发布面**，做不了真的往返。原因是一条引擎当前的限制：
        // 跨边界返回的指针按**声明的静态类型**包装（`Converter<T*>` 用
        // `ZScriptTypeId<T>::Get()`，T 是签名的返回类型），所以
        // `AddComponent("LightComponent")` 在 JS 侧拿到的是 `Component`，
        // `as Light` 只是编译期断言、运行时拿不到 `GetFlux()`；`new LightComponent()`
        // 也没有被绑定（ZHeaderTool 不导构造函数）。
        //
        // 于是 color × intensity 的可逆性、spotAngle 的单位换算这些真正的行为规则
        // **只在 C++ 侧测**（UnityParitySmokeTest 的 LI0..LI16）。这里留下的是
        // "这两个类确实进了绑定表"这道接线门 —— 它挡的是"实现对了但没发布"。
        const materialClassPublished = (typeof MaterialInstance === "function");
        const lightClassPublished = (typeof LightComponent === "function");
        const rendererClassPublished = (typeof BaseRenderer === "function");
        const materialHasSetFloat = (typeof MaterialInstance.prototype.SetFloat === "function");
        const materialHasKeyword = (typeof MaterialInstance.prototype.IsKeywordEnabled === "function");
        const lightHasSpotAngle = (typeof LightComponent.prototype.GetSpotAngle === "function");
        this.materialLightProbe = [
            String(materialClassPublished),
            String(lightClassPublished),
            String(rendererClassPublished),
            String(materialHasSetFloat),
            String(materialHasKeyword),
            String(lightHasSpotAngle)
        ].join("|");
        // ---- Unity 的 Mesh / MeshFilter（slice 53）----
        //
        // 与上面同一条限制：`new Mesh()` 没有被绑定（ZHeaderTool 不导构造函数），
        // 所以做不了"造一个网格、填顶点、读回来"这种往返。这里钉住的是发布面：
        // 两个类进了绑定表、以及 `Mesh` 的数据 API 里那几个**形状特殊**的成员
        // （返回数组、返回 bool 的 SetUVs）确实挂上了。
        //
        // 数组返回类型是这一段真正要挡的东西：`std::vector<Vector3>` ->
        // `Vector3[]` 的映射在 ZHeaderTool 里是一条独立的分支，挂不上的话这些
        // 方法会从 d.ts 里**消失**（不是变成 any），而 TS 侧毫无察觉。
        const meshClassPublished = (typeof Mesh === "function");
        const filterClassPublished = (typeof MeshFilter === "function");
        const meshHasVertices = (typeof Mesh.prototype.GetVertices === "function");
        const meshHasBounds = (typeof Mesh.prototype.RecalculateBounds === "function");
        const meshHasUvChannel = (typeof Mesh.prototype.GetUVs === "function");
        const filterHasShared = (typeof MeshFilter.prototype.SetSharedMeshPath === "function");
        const filterHasRenderer = (typeof MeshFilter.prototype.HasRenderer === "function");
        this.meshProbe = [
            String(meshClassPublished),
            String(filterClassPublished),
            String(meshHasVertices),
            String(meshHasBounds),
            String(meshHasUvChannel),
            String(filterHasShared),
            String(filterHasRenderer)
        ].join("|");
        // ---- Unity 的 Animator 其余面（slice 54）----
        //
        // 还是发布面门，理由同上面两段（拿不到构造函数、AddComponent 按声明类型
        // 包装）。这里额外钉住一件 tsc 抓不到的事：`speed` 是**属性**而不是一对
        // 方法 —— ZSCRIPTPROPERTY 漏掉的话 `Animator.speed = 2` 在运行时是给一个
        // 普通 JS 对象加了个字段，静默失效。
        const animatorClassPublished = (typeof AnimationComponent === "function");
        const animatorHasSpeed = (typeof AnimationComponent.prototype.GetSpeed === "function") &&
            (typeof AnimationComponent.prototype.SetSpeed === "function");
        const animatorHasRebind = (typeof AnimationComponent.prototype.Rebind === "function");
        const animatorHasShape = (typeof AnimationComponent.prototype.GetParameterCount === "function") &&
            (typeof AnimationComponent.prototype.GetStateName === "function");
        const animatorHasIsName = (typeof AnimationComponent.prototype.IsName === "function");
        const animatorHasEvents = (typeof AnimationComponent.prototype.GetFiredEventCount === "function");
        this.animatorProbe = [
            String(animatorClassPublished),
            String(animatorHasSpeed),
            String(animatorHasRebind),
            String(animatorHasShape),
            String(animatorHasIsName),
            String(animatorHasEvents)
        ].join("|");
        // ---- Unity 的 Input 那一层（slice 51）----
        // 这个进程没有 InputSystem，所以下面断言的是**安全默认值**与**可写的
        // 进程级设置** —— 跨边界真正要验证的是 `Touch` 值类型有没有被正确地
        // 打包成对象（字段对不上时 TS 侧读到的是 undefined，而不是报错）。
        // （AdvanceAxis 那条纯函数规则只在 C++ 侧测：它不是 Unity 的 API，不该
        //  为了可测而把它发布进脚本面。）
        const anyKey = Input.anyKey;
        const anyKeyDown = Input.anyKeyDown;
        const inputString = Input.inputString;
        const axisHorizontal = Input.GetAxis("Horizontal");
        const axisBogus = Input.GetAxisRaw("NoSuchAxis");
        const buttonFire = Input.GetButtonDown("Fire1");
        const touchCount = Input.touchCount;
        const touches = Input.touches;
        const missingTouch = Input.GetTouch(0);
        const joysticks = Input.GetJoystickNames();
        const orientation = Input.deviceOrientation;
        const mousePresent = Input.mousePresent;
        const touchSupported = Input.touchSupported;
        const pressureSupported = Input.touchPressureSupported;
        const imeSelected = Input.imeIsSelected;
        const composition = Input.compositionString;
        const mousePos = Input.mousePosition;
        const scrollDelta = Input.mouseScrollDelta;
        const acceleration = Input.acceleration;
        // 设置回环
        Input.multiTouchEnabled = false;
        const multiOff = Input.multiTouchEnabled;
        Input.multiTouchEnabled = true;
        const multiOn = Input.multiTouchEnabled;
        Input.backButtonLeavesApp = true;
        const backLeaves = Input.backButtonLeavesApp;
        Input.backButtonLeavesApp = false;
        Input.simulateMouseWithTouches = false;
        const simOff = Input.simulateMouseWithTouches;
        Input.simulateMouseWithTouches = true;
        Input.imeCompositionMode = IMECompositionMode.On;
        const imeModeOn = Input.imeCompositionMode === IMECompositionMode.On;
        Input.imeCompositionMode = IMECompositionMode.Auto;
        this.inputProbe = [
            String(anyKey === false),
            String(anyKeyDown === false),
            String(inputString.length === 0),
            String(axisHorizontal === 0),
            String(axisBogus === 0),
            String(buttonFire === false),
            String(touchCount === 0),
            String(touches.length === 0),
            String(missingTouch.fingerId === -1),
            String(missingTouch.phase === TouchPhase.Canceled),
            String(missingTouch.pressure === 1),
            String(missingTouch.maximumPossiblePressure === 1),
            // ⚠️ Touch.type 是 number，不是枚举：TS 的 lib.dom 里已经有全局的
            // `type TouchType`，再导出一个同名 enum 会让脚本侧只能当类型用。
            String(missingTouch.type === 0),
            String(joysticks.length === 0),
            String(orientation === DeviceOrientation.Unknown),
            String(mousePresent === true),
            String(touchSupported === false),
            String(pressureSupported === false),
            String(imeSelected === false),
            String(composition.length === 0),
            String(mousePos.x === 0 && mousePos.y === 0),
            String(scrollDelta.x === 0 && scrollDelta.y === 0),
            String(acceleration.x === 0 && acceleration.y === 0),
            String(multiOff === false),
            String(multiOn === true),
            String(backLeaves === true),
            String(simOff === false),
            String(imeModeOn === true)
        ].join("|");
        this.platformProbe = [
            String(sysDeviceType),
            String(sysFamily),
            String(sysProcessor > 0),
            String(sysDeviceName.length > 0),
            String(sysGraphicsMemory === 0),
            String(sysShaderLevel === 0),
            String(sysBattery === -1),
            String(sysBatteryStatus === 0),
            String(sysInstancing === false),
            String(qualityDefault),
            qualityNameDefault,
            String(qualityCount),
            String(qualityNames.length),
            qualityNames[0],
            String(qualityAfterSet),
            String(aaFromPreset),
            shadowFromPreset.toFixed(1),
            String(qualityAfterOutOfRange),
            String(aaSnapped),
            String(mipClamped),
            String(qualityAfterReset),
            String(cursorVisibleDefault),
            String(cursorLockDefault),
            String(cursorVisibleAfter),
            String(cursorLockAfter),
            String(cursorVisibleReset),
            maxDelta.toFixed(4),
            String(captureRate),
            String(sinceLevelLoad >= 0),
            String(appIdentifier.indexOf("com.") === 0),
            String(appMobile === false),
            String(appConsole === false),
            String(appFocused),
            String(appLanguage >= 0),
            String(appOpenBogus === false),
            String(appOpenInjected === false),
            String(appPriority)
        ].join("|");
    }
    /// 走 **JS shim** 的那条停止路径（`g.Behaviour.prototype.StopAllCoroutines`）。
    /// C++ 直接调 comp.StopAllCoroutines() 是测不到 shim 的 —— 少一行转发
    /// 照样全绿（变异 SC16 就是这么漏的）。
    StopMyRoutine() {
        this.StopAllCoroutines();
    }
    routineSnapshot() {
        this.lastRoutineSnapshot = [
            String(this.routineSteps),
            this.routineKinds,
            String(this.routineDone),
            String(this.routineHandleOk)
        ].join("|");
        return this.lastRoutineSnapshot;
    }
    OnAwake() {
        Debug.Log("Mover.OnAwake");
    }
    OnStart() {
        Debug.Log("Mover.OnStart");
        // instanceof 为假 == 原型链没接上。
        this.isBehaviour = (this instanceof Behaviour);
        // OnDestroy 只定义在 Behaviour.prototype 上，Mover 自己没有。
        this.inheritedHook = (typeof this.OnDestroy);
        // 未注入时是 undefined；用 != null 同时覆盖 null 与 undefined。
        this.hasGameObject = (this.gameObject != null);
    }
    OnFixedUpdate(fixed_delta_time) {
        this.fixedTicks = this.fixedTicks + 1;
        this.lastFixedDt = fixed_delta_time;
        // 固定步长 vs 帧间隔：用 `!==` 而不是 `<` —— 前者能抓住"把帧 delta
        // 传下来"这种实现，后者在帧率恰好等于固定步率时是绿的。
        this.fixedDtMatches = (fixed_delta_time === Time.fixedDeltaTime);
    }
    OnUpdate(delta_time) {
        this.ticks = this.ticks + 1;
        this.lastDt = delta_time;
        this.origin = VectorMath.Add(this.origin, VectorMath.Scale(VectorMath.UnitX(), this.speed * delta_time));
        this.sawKeyDown = Input.GetKeyDown(Key.Space);
        this.seenDeltaTime = Time.deltaTime;
    }
    /// 把当前状态打成一行字符串。测试的读法：调 snapshot() 再用
    /// ScriptEnv::EnumerateInstanceFields 取 lastSnapshot。
    snapshot() {
        this.lastSnapshot = [
            this.origin.x.toFixed(4),
            String(this.ticks),
            String(this.lastDt),
            String(this.sawKeyDown),
            this.seenDeltaTime.toFixed(4)
        ].join(",");
        return this.lastSnapshot;
    }
}
exports.Mover = Mover;
