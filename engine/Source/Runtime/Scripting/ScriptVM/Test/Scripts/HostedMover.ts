// ==========================================================================
// 挂在 GameObject 上的 Behaviour —— TypeScriptComponent 真身测试用的脚本。
//
// 和 Mover.ts 的分工
// ------------------
// Mover.ts 钉的是"tsc 产物能不能被 ScriptEnv 加载"；这个文件钉的是
// **TypeScriptComponent 那条路**：
//
//   ScriptRegistry 扫到 .ts -> guid -> ScriptAsset
//     -> TypeScriptComponent::ResolveScript 解出 module id + 类名
//     -> LoadModule -> CreateInstance -> OnAwake/OnStart
//     -> 每帧 Tick 转发 OnUpdate -> 析构/重载时 OnDestroy
//
// 所以它必须把生命周期的**每一次**都记下来（计数而不是布尔），并把引擎注入
// 的三个宿主对象（gameObject / transform / component）在 OnAwake 里就验一遍 ——
// 那是 TypeScriptComponent::BindAndAwake 唯一一次推宿主的机会，晚一帧都拿不到。
//
// 两个刻意的设计
// --------------
// 1. `export class ... extends Behaviour`（而不是 `class X ...; export { X }`）
//    ScriptRegistry::ParseDefaultClassName 的正则只认 `export class` /
//    `export default class`，写 `export { X }` 会让 m_DefaultClassName 为空，
//    组件就只能在 Inspector 里手填 m_ClassName 才绑得上。两种写法都被测试覆盖
//    （HostedMover 走自动，Mover 走显式）。
// 2. OnDestroy 写的是 **globalThis** 而不是实例字段：热重载会把整实例丢掉，
//    写在实例上的计数随实例一起消失，测试就没法证明"OnDestroy 真的被调用了"。
//    模块级变量也不行 —— ReloadModule 会重新执行模块，变量归零。只有 globalThis
//    跨得过重编译。
// ==========================================================================

export class HostedMover extends Behaviour {
    speed: number = 2;

    awakeCount: number = 0;
    startCount: number = 0;
    updateCount: number = 0;
    lastDt: number = 0;
    originX: number = 0;

    // OnAwake 里一次性验完的三个宿主对象。false 说明注入没生效。
    gotGameObject: boolean = false;
    gotTransform: boolean = false;
    gotComponent: boolean = false;
    hostName: string = "";

    // 每帧打成一行，测试的读法：Tick 完之后用 EnumerateInstanceFields 取它。
    lastSnapshot: string = "";

    OnAwake(): void {
        this.awakeCount = this.awakeCount + 1;
        this.gotGameObject = (this.gameObject != null);
        this.gotTransform = (this.transform != null);
        this.gotComponent = (this.component != null);
        if (this.gameObject != null) {
            this.hostName = this.gameObject.GetName();
        }
    }

    OnStart(): void {
        this.startCount = this.startCount + 1;
    }

    OnUpdate(delta_time: number): void {
        this.updateCount = this.updateCount + 1;
        this.lastDt = delta_time;
        this.originX = this.originX + this.speed * delta_time;
        this.lastSnapshot = [
            this.originX.toFixed(4),
            String(this.updateCount),
            String(this.lastDt)
        ].join(",");
    }

    OnDestroy(): void {
        const g: any = globalThis as any;
        const prev: number = (g.__hostedDestroyCount == null) ? 0 : g.__hostedDestroyCount;
        g.__hostedDestroyCount = prev + 1;
    }
}
