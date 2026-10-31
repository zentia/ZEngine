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
        // ---- Bounds / Color32 -------------------------------------------------
        //
        // 这两个是 Unity 里仅剩、而引擎里此前**零命中**的值类型。C++ 侧的 BC 组
        // 断言算术，ScriptVMSmokeTest 的 M 组断言"脚本看不看得见" —— 只有这里能
        // 证明 `Bounds.Create(...)` 在 **.ts 里也能通过 tsc 的类型检查**（d.ts 里
        // `declare interface Bounds` 与 `declare class Bounds` 的同名合并在真正的
        // 类型检查器下是成立的，不只是两段互不相干的字符串）。
        this.measured = "";
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
    /// 从 TypeScript 里画 gizmo。C++ 侧的 GZ 组断言的是"画出了什么"，
    /// ScriptVMSmokeTest 的 L 组断言的是"脚本看不看得见 Gizmos" —— 只有这里
    /// 证明 `Gizmos.DrawLine(...)` 在 **.ts 里也能通过类型检查**（d.ts 里那
    /// 个 `declare class Gizmos` 是真的能用的，不只是一串字符串）。
    DrawGizmo() {
        Gizmos.color = { r: 1, g: 0, b: 0, a: 1 };
        Gizmos.DrawLine({ x: 0, y: 0, z: 0 }, { x: 1, y: 0, z: 0 });
        Gizmos.DrawWireSphere({ x: 0, y: 0, z: 0 }, 1);
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
