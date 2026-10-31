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
