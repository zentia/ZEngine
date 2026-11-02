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
