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

class Mover extends Behaviour {
    speed: number = 2;
    origin: Vector3 = { x: 0, y: 0, z: 0 };
    ticks: number = 0;
    lastDt: number = 0;
    sawKeyDown: boolean = false;
    seenDeltaTime: number = 0;

    // 下面三个由 OnStart 填，ScriptTsChainSmokeTest 靠它们判断原型链是否接上。
    isBehaviour: boolean = false;
    inheritedHook: string = "";
    hasGameObject: boolean = true;

    lastSnapshot: string = "";

    // ---- 协程（Unity 的 StartCoroutine / yield）----------------------------
    //
    // 这一段是**端到端**的证明：C++ 侧的每个断言都能在完全没有 JS 的情况下
    // 通过（UnityParitySmokeTest 的 CoroutineProbe 就是这么做的），只有真的
    // 从 TypeScript 里 `*MyRoutine()` 出一个生成器、真的 `yield new
    // WaitForSeconds(...)`，才能证明 shim 的 `__zWait` 标记、ScriptEnv 的
    // 翻译、以及调度器的相位三者接得上。
    routineSteps: number = 0;
    routineKinds: string = "";
    routineDone: boolean = false;
    routineHandleOk: boolean = false;
    lastRoutineSnapshot: string = "";

    /// 测试通过 `InvokeMethod("StartMyRoutine")` 触发。
    StartMyRoutine(): void {
        this.routineSteps = 0;
        this.routineKinds = "";
        this.routineDone = false;
        const handle = this.StartCoroutine("MyRoutine");
        this.routineHandleOk = (handle != null);
    }

    /// 一个 Unity 式的协程体。三次 yield，三种等待，每步写一个不同的位
    /// （1 / 2 / 4）—— 这样"跑到了第几步"是一个数字就能读出来的，而不是
    /// 要靠数字符串。
    *MyRoutine(): Generator<YieldInstruction, void, unknown> {
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

    // ---- WaitUntil / WaitWhile -------------------------------------------
    //
    // 这两个带**闭包**，是 Unity 的 YieldInstruction 家族里唯一不能被"标记 +
    // 常量"表达完的两个：谓词必须每帧被问一次，直到它翻面。所以这里除了位
    // （1 / 2 / 4）之外还数了 `untilPolls` —— 只在 yield 那一帧问一次的实现，
    // 位最终也能走到 7，但 polls 会停在 1。
    gateReady: boolean = false;
    untilSteps: number = 0;
    untilPolls: number = 0;

    StartUntilRoutine(): void {
        this.untilSteps = 0;
        this.untilPolls = 0;
        this.gateReady = false;
        this.StartCoroutine("WaitUntilRoutine");
    }

    *WaitUntilRoutine(): Generator<YieldInstruction, void, unknown> {
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

    // ---- yield return StartCoroutine(other) ------------------------------
    //
    // Unity 的嵌套协程：外层不是"等一帧"，而是"等那个协程整个跑完"。
    //
    // ⚠️ 内层必须**多于一帧**（这里是两段 yield）：`yield this.StartCoroutine(...)`
    // 被当成 `yield null` 的实现，外层也只晚一帧就继续了，step 位最终照样能
    // 走到 7。只有"内层还在跑的时候外层必须停住"这一条能区分两者。
    nestedSteps: number = 0;
    nestedInner: number = 0;

    StartNestedRoutine(): void {
        this.nestedSteps = 0;
        this.nestedInner = 0;
        this.StartCoroutine("NestedOuterRoutine");
    }

    *NestedOuterRoutine(): Generator<YieldInstruction | null, void, unknown> {
        this.nestedSteps = this.nestedSteps + 1;   // 1，同步段
        yield this.StartCoroutine("NestedInnerRoutine");
        this.nestedSteps = this.nestedSteps + 2;   // 3，内层结束后才到
        yield null;
        this.nestedSteps = this.nestedSteps + 4;   // 7
    }

    *NestedInnerRoutine(): Generator<YieldInstruction | null, void, unknown> {
        this.nestedInner = this.nestedInner + 1;   // 1
        yield null;
        this.nestedInner = this.nestedInner + 2;   // 3
        yield null;
        this.nestedInner = this.nestedInner + 4;   // 7，跑完
    }

    // ---- Unity 的应用层消息 ---------------------------------------------
    //
    // ⚠️ 两处 `=== true` 是**故意**的：它们能抓住"把 bool 当 1.0 / 0.0 传进
    // 来"的实现 —— 那种实现下 `if (hasFocus)` 是对的，但 `hasFocus === true`
    // 是假的。用隐式真值判断写这段，这个 bug 就永远测不出来。
    focusLog: string = "";
    pauseLog: string = "";
    quitCount: number = 0;
    parentChanged: number = 0;
    childrenChanged: number = 0;
    validateCount: number = 0;

    OnApplicationFocus(hasFocus: boolean): void {
        this.focusLog = this.focusLog + (hasFocus === true ? "T" : "F");
    }

    OnApplicationPause(pauseStatus: boolean): void {
        this.pauseLog = this.pauseLog + (pauseStatus === true ? "T" : "F");
    }

    OnApplicationQuit(): void {
        this.quitCount = this.quitCount + 1;
    }

    // 回调里读一次 `transform.parent`：派发点必须在层级**更新之后**。早一步
    // 派发的话这里读到的还是旧父级，而回调本身照样被调用一次 —— 只看调用
    // 次数完全分不出来。
    parentWasNullAtCallback: boolean = true;

    OnTransformParentChanged(): void {
        this.parentChanged = this.parentChanged + 1;
        this.parentWasNullAtCallback = this.transform.parent === null;
    }

    // 与上面那条是同一件事的**两半**，但收的人不同：上面是"我换父级了"（发给
    // 被搬的对象），这一个是"我的孩子多了/少了一个"（发给父级）。名字只差一个
    // 词，所以这边的判据也照抄一份结构：回调里读一次 `transform.childCount`，
    // 值必须是**新的**（1 / 0），否则"派发早了一步"照样计数对得上。
    childCountAtCallback: number = -1;

    OnTransformChildrenChanged(): void {
        this.childrenChanged = this.childrenChanged + 1;
        this.childCountAtCallback = this.transform.childCount;
    }

    OnValidate(): void {
        this.validateCount = this.validateCount + 1;
    }

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
    gizmoDraws: number = 0;
    gizmoSelectedDraws: number = 0;
    gizmoAtDraw: string = "";

    OnDrawGizmos(): void {
        this.gizmoDraws = this.gizmoDraws + 1;
        // 回调里真的画一次，并且读回 `Gizmos.color` —— 证明这次派发发生在
        // "gizmo 后端可用"的上下文里（后端缺失时是 no-op，不抛）。
        Gizmos.color = { r: 0, g: 1, b: 0, a: 1 };
        Gizmos.DrawWireSphere({ x: 0, y: 0, z: 0 }, 1);
        this.gizmoAtDraw = String(Gizmos.color.g);
        Gizmos.color = { r: 1, g: 1, b: 1, a: 1 };
    }

    OnDrawGizmosSelected(): void {
        this.gizmoSelectedDraws = this.gizmoSelectedDraws + 1;
    }

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
    mouseLog: string = "";
    mouseDownHadGameObject: boolean = false;

    OnMouseEnter(): void { this.mouseLog = this.mouseLog + "E"; }
    OnMouseOver(): void { this.mouseLog = this.mouseLog + "O"; }
    OnMouseExit(): void { this.mouseLog = this.mouseLog + "X"; }

    OnMouseDown(): void {
        this.mouseLog = this.mouseLog + "D";
        this.mouseDownHadGameObject = (this.gameObject != null);
    }

    OnMouseDrag(): void { this.mouseLog = this.mouseLog + "G"; }
    OnMouseUp(): void { this.mouseLog = this.mouseLog + "U"; }
    OnMouseUpAsButton(): void { this.mouseLog = this.mouseLog + "B"; }

    // ---- 值类型的静态门面：Vector2 / Vector4 / Matrix4x4 ------------------
    //
    // 这段的价值**在 tsc**：`Vector2.zero` 这种写法只有 d.ts 里同时存在
    // `declare interface Vector2`（值类型形状）和 `declare class Vector2`（静态
    // 面）时才编译得过 —— 两者同名合并。C++ 侧的 VT 组断言算术，ScriptVM 侧断言
    // 发布面存在，只有这里证明"合出来的那个类型真的能当值用"。
    //
    // ⚠️ 夹具一律不对称：(3,4) 而不是 (1,1)，`SignedAngle` 两个方向都取，Lerp 的
    // 夹与不夹各来一次。对称夹具分不出对错。
    vtSnapshot: string = "";
    vtAxis: string = "";

    ProbeValueTypes(): void {
        const a: Vector2 = { x: 3, y: 4 };
        const parts: string[] = [];

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
        const v4: Vector4 = { x: 1, y: 2, z: 2, w: 4 };
        parts.push(Vector4.Magnitude(v4).toFixed(3));
        parts.push(Vector4.Normalize(v4).w.toFixed(3));

        // Matrix4x4：平移影响点、不影响方向；TRS 是 T*R*S 不是 S*R*T。
        const t: Matrix4x4 = Matrix4x4.Translate({ x: 1, y: 2, z: 3 });
        const p: Vector3 = Matrix4x4.MultiplyPoint(t, { x: 1, y: 1, z: 1 });
        parts.push(p.x.toFixed(0) + p.y.toFixed(0) + p.z.toFixed(0));
        const d: Vector3 = Matrix4x4.MultiplyVector(t, { x: 1, y: 1, z: 1 });
        parts.push(d.x.toFixed(0) + d.y.toFixed(0) + d.z.toFixed(0));
        parts.push(String(Matrix4x4.IsIdentity(Matrix4x4.identity)));
        parts.push(String(Matrix4x4.IsIdentity(Matrix4x4.zero)));
        const trs: Matrix4x4 = Matrix4x4.TRS(
            { x: 1, y: 2, z: 3 }, Quaternion.identity, { x: 2, y: 2, z: 2 });
        const q: Vector3 = Matrix4x4.MultiplyPoint(trs, { x: 1, y: 0, z: 0 });
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
    DrawGizmo(): void {
        Gizmos.color = { r: 1, g: 0, b: 0, a: 1 };
        Gizmos.DrawLine({ x: 0, y: 0, z: 0 }, { x: 1, y: 0, z: 0 });
        Gizmos.DrawWireSphere({ x: 0, y: 0, z: 0 }, 1);
        // 视锥 12 条线 + 上面那条 = 13。用 12 而不是 1，是为了让"DrawFrustum 只
        // 画了近平面那 4 条"这种实现在脚本这一层也红 —— 光有一条线的话，
        // 4 条和 12 条都只表现为"有东西"。
        Gizmos.DrawFrustum({ x: 0, y: 0, z: 0 }, 90, 4, 2, 1);
        Gizmos.color = { r: 1, g: 1, b: 1, a: 1 };
    }

    // ---- Bounds / Color32 -------------------------------------------------
    //
    // 这两个是 Unity 里仅剩、而引擎里此前**零命中**的值类型。C++ 侧的 BC 组
    // 断言算术，ScriptVMSmokeTest 的 M 组断言"脚本看不看得见" —— 只有这里能
    // 证明 `Bounds.Create(...)` 在 **.ts 里也能通过 tsc 的类型检查**（d.ts 里
    // `declare interface Bounds` 与 `declare class Bounds` 的同名合并在真正的
    // 类型检查器下是成立的，不只是两段互不相干的字符串）。
    measured: string = "";

    MeasureBounds(): void {
        const box: Bounds = Bounds.Create({ x: 1, y: 2, z: 3 }, { x: 4, y: 6, z: 8 });
        const grown: Bounds = Bounds.Expand(box, 2);
        const size: Vector3 = Bounds.GetSize(grown);
        const bytes: Color32 = Color32.FromColor(Color.red);
        this.measured = [
            size.x + "," + size.y + "," + size.z,
            bytes.r + "," + bytes.g + "," + bytes.b + "," + bytes.a,
            String(Bounds.Contains(box, { x: 1, y: 2, z: 3 })),
            String(Bounds.IntersectRay(box, null as unknown as Ray))
        ].join("|");
    }

    /// 走 **JS shim** 的那条停止路径（`g.Behaviour.prototype.StopAllCoroutines`）。
    /// C++ 直接调 comp.StopAllCoroutines() 是测不到 shim 的 —— 少一行转发
    /// 照样全绿（变异 SC16 就是这么漏的）。
    StopMyRoutine(): void {
        this.StopAllCoroutines();
    }

    routineSnapshot(): string {
        this.lastRoutineSnapshot = [
            String(this.routineSteps),
            this.routineKinds,
            String(this.routineDone),
            String(this.routineHandleOk)
        ].join("|");
        return this.lastRoutineSnapshot;
    }

    OnAwake(): void {
        Debug.Log("Mover.OnAwake");
    }

    OnStart(): void {
        Debug.Log("Mover.OnStart");
        // instanceof 为假 == 原型链没接上。
        this.isBehaviour = (this instanceof Behaviour);
        // OnDestroy 只定义在 Behaviour.prototype 上，Mover 自己没有。
        this.inheritedHook = (typeof this.OnDestroy);
        // 未注入时是 undefined；用 != null 同时覆盖 null 与 undefined。
        this.hasGameObject = (this.gameObject != null);
    }

    OnUpdate(delta_time: number): void {
        this.ticks = this.ticks + 1;
        this.lastDt = delta_time;
        this.origin = VectorMath.Add(
            this.origin,
            VectorMath.Scale(VectorMath.UnitX(), this.speed * delta_time));
        this.sawKeyDown = Input.GetKeyDown(Key.Space);
        this.seenDeltaTime = Time.deltaTime;
    }

    /// 把当前状态打成一行字符串。测试的读法：调 snapshot() 再用
    /// ScriptEnv::EnumerateInstanceFields 取 lastSnapshot。
    snapshot(): string {
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

export { Mover };
