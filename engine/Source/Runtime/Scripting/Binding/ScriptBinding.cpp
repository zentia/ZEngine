#include "Runtime/Scripting/Binding/ScriptBinding.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    // Constant-initialized, so it is valid before any dynamic static init
    // (generated registrars run during dynamic init, in unspecified order).
    ZClassScriptBinding* g_Head = nullptr;
    ZScriptEnum* g_EnumHead = nullptr;
    ZSyntheticScriptClass* g_SyntheticHead = nullptr;
    bool g_Installed = false;

    // Placeholder `declare class <Super> {}` emitted for supers that have no
    // binding of their own. Kept outside AppendTypeScriptDeclarations so a
    // second call (one .d.ts per project, but the call may be exercised more
    // than once in tests) does not duplicate them.
    std::vector<const char*> g_EmittedPlaceholders;

    /// Whether `name` is emitted as a `declare class` in the generated file.
    ///
    /// A reserved name is not: InstallGlobals refuses to publish it as a global
    /// (it would shadow a JS built-in), and emitting the class anyway would
    /// collide with lib.d.ts. Both halves have to agree, so `extends` must treat
    /// a reserved super as absent too -- `extends Object` with no `declare class
    /// Object` in the file is a compile error in every user project.
    bool IsDeclared(const std::vector<const ZClassScriptBinding*>& classes, const char* name)
    {
        if (ScriptBindingRegistry::IsReservedScriptName(name))
            return false;
        for (const ZClassScriptBinding* cls : classes)
        {
            if (cls->scriptName != nullptr && std::strcmp(cls->scriptName, name) == 0)
                return true;
        }
        return false;
    }

    bool IsPlaceholderEmitted(const char* name)
    {
        for (const char* emitted : g_EmittedPlaceholders)
        {
            if (std::strcmp(emitted, name) == 0)
                return true;
        }
        return false;
    }

    bool IsTypeDeclared(const std::vector<const void*>& ids, const void* id)
    {
        for (const void* known : ids)
        {
            if (known == id)
                return true;
        }
        return false;
    }

    // Identifiers TypeScript's own lib.d.ts already declares at global scope.
    // The engine root class is literally called `Object`, so emitting
    // `declare class Object {}` would collide with the built-in `Object`
    // interface and break every user project, and publishing a global of that
    // name would shadow the JS built-in at runtime. Names in this list are
    // therefore dropped from both the .d.ts placeholder pass and the global
    // install; a class that would inherit from one is emitted without an
    // `extends` clause (it contributes no bound members anyway).
    const char* const kReservedScriptNames[] = {
        "Object", "Function", "String",  "Number",    "Boolean",   "Symbol",  "BigInt",  "Array",
        "Date",   "RegExp",   "Error",   "Promise",   "Map",       "Set",     "WeakMap", "WeakSet",
        "Math",   "JSON",     "Reflect", "Proxy",     "ArrayBuffer", "DataView", "console",
    };

    // --- Behaviour -------------------------------------------------------
    //
    // The Unity-compatible base class every user-authored TS behaviour extends.
    // It has no C++ binding table: the instance is a plain JS object produced by
    // `__zNewInstance`, and TypeScriptComponent injects these three properties
    // onto it once, right after construction and before OnAwake.
    const ZSyntheticProperty kBehaviourProperties[] = {
        {ZBehaviourScript::kGameObject, "GameObject", true},
        {ZBehaviourScript::kTransform, "Transform", true},
        {ZBehaviourScript::kComponent, "TypeScriptComponent", true},
    };

    // Lifecycle hooks. These live on the JS prototype (ScriptEnv's console
    // shim), so they are plain .d.ts lines with no C++ counterpart.
    const char* const kBehaviourMethodLines[] = {
        "    OnAwake(): void;",
        // Unity 的启用 / 停用回调。触发条件是 `enabled && activeInHierarchy`
        // 这个合取（isActiveAndEnabled），不是单独某一个 —— 声明在这里，实现在
        // TypeScriptComponent::RefreshEnabledState。
        "    OnEnable(): void;",
        "    OnDisable(): void;",
        "    OnStart(): void;",
        "    OnUpdate(delta_time: number): void;",
        // Unity 的 FixedUpdate。每**固定步**一次，一帧内可能 0 次（掉帧补步时
        // 也可能多次）。⚠️ 参数是**固定步长**（`Time.fixedDeltaTime`），不是
        // 帧间隔 —— 与 OnUpdate / OnLateUpdate 的那个 deltaTime 不是同一个数。
        //
        // 名字故意带 `fixed_` 前缀：写成 `delta_time` 的话，读签名的人会以为
        // 它和上面那条一样是帧间隔，于是拿它去做"每帧一次"的事。
        "    OnFixedUpdate(fixed_delta_time: number): void;",
        // Unity 的 LateUpdate。每帧一次，在所有 OnUpdate 之后。C++ 侧拿的
        // 是帧间隔（与 OnUpdate 同一个 deltaTime），不是固定步长。
        "    OnLateUpdate(delta_time: number): void;",
        // Unity 的应用层消息。与 OnUpdate 那一组**方向相反**：这四个由整个
        // 应用驱动（焦点 / 暂停 / 退出 / 改父级），派发时不判 enabled 与
        // 层级激活 —— 被停用的对象同样要收到"要退出了"。
        "    OnApplicationFocus(has_focus: boolean): void;",
        "    OnApplicationPause(pause_status: boolean): void;",
        "    OnApplicationQuit(): void;",
        "    OnTransformParentChanged(): void;",
        // 同一件事的另一半：这个是"我的孩子多了/少了一个"（发给**父级**），
        // 上一个是"我换父级了"（发给**子级**）。名字只差一个词，别合并。
        "    OnTransformChildrenChanged(): void;",
        // Unity 的 gizmo 消息。与上面那一组同类（推），与 OnUpdate 那一组不同
        // 类（拉）：派发只判 activeInHierarchy、**不判 enabled** —— 停用的脚本
        // 照样画辅助线，这是 Unity 明文写了的（见 Component::OnDrawGizmos）。
        "    OnDrawGizmos(): void;",
        "    OnDrawGizmosSelected(): void;",
        // Unity 的七条鼠标消息。与 gizmo 那两条同组：派发只判 activeInHierarchy，
        // 不判 enabled（Unity 的 enabled 只挡 Start/Awake/Update/FixedUpdate
        // /OnGUI）。
        "    OnMouseEnter(): void;",
        "    OnMouseOver(): void;",
        "    OnMouseExit(): void;",
        "    OnMouseDown(): void;",
        "    OnMouseDrag(): void;",
        "    OnMouseUp(): void;",
        "    OnMouseUpAsButton(): void;",
        // Unity 的两条可见性消息。判据与鼠标那一组相同（只判
        // activeInHierarchy、不判 enabled），派发方是 BaseRenderer。
        "    OnBecameVisible(): void;",
        "    OnBecameInvisible(): void;",
        // Unity 的 `OnValidate`：编辑器里序列化字段被改之后调用（编辑器专用，
        // Player 里不会来）。
        "    OnValidate(): void;",
        "    OnDestroy(): void;",
        // Unity 的 Invoke 家族。签名必须与 ScriptEnv 里
        // `g.Behaviour.prototype.*` 的转发一致；实现在 TypeScriptComponent 上
        // （Behaviour 不能作为绑定类发布，否则与合成类重名）。
        "    Invoke(methodName: string, time: number): void;",
        "    InvokeRepeating(methodName: string, time: number, repeatRate: number): void;",
        "    CancelInvoke(methodName: string): void;",
        "    IsInvoking(methodName: string): boolean;",
        // 协程。Unity 有两个重载：`StartCoroutine(IEnumerator)` 和
        // `StartCoroutine(string)`。生成器对象没法穿过 ZFUNCTION 边界
        // （绑定表只认引擎自己的类型），所以这里只提供字符串重载 —— 它本身
        // 就是 Unity 的公开 API，不是凑数的替代品。
        //
        // 返回 null 表示启动失败（没有 component，或那个方法没返回生成器）；
        // Unity 在同样的情形下会抛，这里按 Invoke 的既有风格记日志并返回空。
        "    StartCoroutine(methodName: string): Coroutine | null;",
        // 两个重载对应 Unity 的 `StopCoroutine(Coroutine)` 与
        // `StopCoroutine(string)`。C++ 侧只有一个 `StopCoroutine(string)`：
        // JS shim 从 Coroutine 句柄里取出方法名再转发。
        "    StopCoroutine(routine: Coroutine): void;",
        "    StopCoroutine(methodName: string): void;",
        "    StopAllCoroutines(): void;",
        // Unity 的碰撞 / 触发回调。由 PhysicsManager::DispatchContactEvents
        // 通过 SendMessageWithOther 调用（不是 ZFUNCTION，所以这里只是
        // 声明）—— 不写的话 tsc 会因为 `this.OnCollisionEnter` 不存在而报
        // 错，用户被迫写 `(this as any)` 绕过，类型检查也就没了。
        //
        // 两边都会收到：触发器自己和被触发的那个（只要有脚本）。
        // `other` 在自身碰撞（A 撞 A）时为 undefined —— Jolt 不会产生这种
        // pair，但声明成可选比让调用方不检查更诚实。
        "    OnCollisionEnter(other?: GameObject): void;",
        "    OnCollisionStay(other?: GameObject): void;",
        "    OnCollisionExit(other?: GameObject): void;",
        "    OnTriggerEnter(other?: GameObject): void;",
        "    OnTriggerStay(other?: GameObject): void;",
        "    OnTriggerExit(other?: GameObject): void;",
    };

    ZSyntheticScriptClass g_BehaviourClass {
        "Behaviour",
        nullptr,
        kBehaviourProperties,
        sizeof(kBehaviourProperties) / sizeof(kBehaviourProperties[0]),
        kBehaviourMethodLines,
        sizeof(kBehaviourMethodLines) / sizeof(kBehaviourMethodLines[0]),
        nullptr,
    };
    const ZSyntheticScriptClassRegistrar g_BehaviourRegistrar(g_BehaviourClass);

    // zengine.d.ts:
    //   * Lives under <project>/Intermediate/Typings/ which is gitignored - the
    //     engine OWNS this file and overwrites it on every project open. Users
    //     should not hand-edit it; if they do, their changes will be wiped on
    //     the next launch (this is the same contract Unity uses for the
    //     auto-generated UnityEngine.dll typings).
    //   * Mirrors the runtime shim installed by ScriptEnv::InstallConsoleAndDebugGlobals
    //     (see Wrapper/ScriptEnv.cpp) plus the __zNewInstance helper used by
    //     ScriptEnv::CreateInstance.
    //   * The engine-side class bindings (`declare class GameObject`, ...) are
    //     NOT hand-written here: ZHeaderTool emits a ZClassScriptBinding per
    //     ZFUNCTION-annotated class and ScriptBindingRegistry appends the
    //     matching `declare class` block (see AppendTypeScriptDeclarations).
    //     They are installed as globals of the same name by ScriptEnv, so the
    //     .d.ts and the runtime agree by construction.
    //   * `Behaviour` is NOT declared here: it is a synthetic script class, and
    //     AppendTypeScriptDeclarations emits its `declare class` from the same
    //     ZSyntheticProperty table TypeScriptComponent injects from (keeping the
    //     hand-written block would be a duplicate declaration). It is a class
    //     rather than an interface because `extends Behaviour` needs a
    //     constructor in TS's eyes.
    constexpr const char* kZEngineDtsTemplate =
        "// =========================================================================\n"
        "// AUTO-GENERATED by ZEngine. Do NOT edit -- regenerated on every project open.\n"
        "// Source of truth: ScriptBindingRegistry::BuildTypeScriptDeclarationFile\n"
        "// =========================================================================\n"
        "\n"
        "/** Base class for any script that wants to be attached to a GameObject. */\n"
        // Value types. These are not engine Objects: ZScript::Converter
        // marshals them as plain JS objects with these exact keys, so they are
        // declared as structural interfaces rather than classes.
        "/** Engine value types, marshalled as plain JS objects. */\n"
        "declare interface Vector3 {\n"
        "    x: number;\n"
        "    y: number;\n"
        "    z: number;\n"
        "}\n"
        "\n"
        // Vector4 是运行时材质实例带进来的：SetBaseColor / GetBaseColor 要走
        // ZScript::Converter<Vector4>，脚本侧得有个对应的结构类型。
        "declare interface Vector4 {\n"
        "    x: number;\n"
        "    y: number;\n"
        "    z: number;\n"
        "    w: number;\n"
        "}\n"
        "\n"
        "declare interface Vector2 {\n"
        "    x: number;\n"
        "    y: number;\n"
        "}\n"
        "\n"
        // Vector2Int / Vector3Int：与 Vector2 / Vector3 同名的整数版，键名一致
        // （`{x, y}` / `{x, y, z}`），但分量是整数。与
        // `declare class Vector2Int` / `Vector3Int`（ZHeaderTool 从
        // ScriptIntVector.h 生成，带 `zero` / `Distance` / `FloorToInt`
        // 那些静态成员）同名合并 —— 与 `Vector3` / `Color` 一个套路。
        //
        // ⚠️ 脚本字面量 `{x: 3.7, y: -2.1}` 传进来是**截断**成 (3, -2)，
        // 不是 floor。要 Unity 的 `FloorToInt` 语义（负数上不同）必须显式调
        // `Vector2Int.FloorToInt(v)`。
        "declare interface Vector2Int {\n"
        "    x: number;\n"
        "    y: number;\n"
        "}\n"
        "\n"
        "declare interface Vector3Int {\n"
        "    x: number;\n"
        "    y: number;\n"
        "    z: number;\n"
        "}\n"
        "\n"
        // SmoothDampResult / Vector3SmoothDampResult：Unity 的 SmoothDamp 用
        // `ref currentVelocity` 传跨帧状态，TS 没有 ref、ZEngine 的值类型跨边界
        // 又是快照，所以"新值 + 新速度"只能一起回来。Unity 没有这两个结构
        // （它不需要），名字是我们起的。
        //
        // ⚠️ 只接 `value` 不接 `velocity` 是**静默**的：看着在动，只是每帧从静止
        // 重新开始，永远慢一拍。
        "declare interface SmoothDampResult {\n"
        "    value: number;\n"
        "    velocity: number;\n"
        "}\n"
        "\n"
        "declare interface Vector3SmoothDampResult {\n"
        "    value: Vector3;\n"
        "    velocity: Vector3;\n"
        "}\n"
        "\n"
        // Plane：Unity 的 `UnityEngine.Plane` —— `{normal, distance}`，平面方程
        // `dot(normal, p) + distance == 0`。与 `declare class Plane`
        // （ZHeaderTool 从 ScriptPlane.h 生成，带 `FromPoints` / `Raycast`
        // 那些静态成员）同名合并 —— 与 `Vector3` / `Color` 一个套路。
        //
        // ⚠️ `normal` **不**在跨边界时归一化：脚本写 `{normal: {x:0,y:2,z:0}}`
        // 得到的平面，`GetDistanceToPoint` 会整体放大 2 倍。Unity 同样不归一化
        // （直接改 `plane.normal` 也不会归一化）。要"真正的距离"用
        // `Plane.Normalize(plane)`。
        "declare interface Plane {\n"
        "    normal: Vector3;\n"
        "    distance: number;\n"
        "}\n"
        "\n"
        // Rect / RectInt：Unity 的四个字段 `x`/`y`/`width`/`height`。
        // 与 `declare class Rect` / `RectInt`（ScriptRect.h 那批静态方法）
        // 同名合并。
        //
        // ⚠️ `width` / `height` **可以为负**（从右下往左上拖出来的矩形就是这个
        // 形状），`Rect.xMin` 那些派生 getter 才做归一化。直接读 `x` / `width`
        // 得不到"两条边"。
        "declare interface Rect {\n"
        "    x: number;\n"
        "    y: number;\n"
        "    width: number;\n"
        "    height: number;\n"
        "}\n"
        "\n"
        "declare interface RectInt {\n"
        "    x: number;\n"
        "    y: number;\n"
        "    width: number;\n"
        "    height: number;\n"
        "}\n"
        "\n"
        // Color：与 Vector3 同族的值类型（r/g/b/a 四个 0..1 的 float），键名与
        // ZScript::Converter<Color> 一致。它同时也是一个**绑定类**：`Color.Lerp`
        // / `Color.red` 那些静态成员由 ZHeaderTool 从 ScriptColor.h 生成
        // `declare class Color`，与这里的 interface 声明合并（TS 允许
        // class + interface 同名合并：interface 补实例成员，class 带静态成员）。
        // 这正是 Unity 的形状 —— `Color` 既是值类型，又挂着一堆静态常量与工具。
        "declare interface Color {\n"
        "    r: number;\n"
        "    g: number;\n"
        "    b: number;\n"
        "    a: number;\n"
        "}\n"
        "\n"
        // Color32：通道名与 Color 完全相同（Unity 里两者可隐式互转，脚本换用
        // 时不该改字段名），只是值域是 0..255 的整数。与 `declare class Color32`
        // （ZHeaderTool 从 ScriptColor.h 生成，带 Lerp / FromColor / ToColor）
        // 同名合并。
        "declare interface Color32 {\n"
        "    r: number;\n"
        "    g: number;\n"
        "    b: number;\n"
        "    a: number;\n"
        "}\n"
        "\n"
        // ColorParseResult：`ColorUtility.TryParseHtmlString` 的结果。Unity 的
        // 签名是 `bool TryParseHtmlString(string, out Color)` —— TS 没有 out
        // 参数，所以"成功与否"和"颜色"一起回来，与 `RaycastHit.valid` 同形。
        //
        // ⚠️ 解析失败时 `color` 是全 0（透明黑），**不是**保留调用方原值。不看
        // `valid` 直接用的话，配置表里一个拼错的色值会变成"这个 UI 不见了"。
        // Gradient：Unity 的 `UnityEngine.Gradient` —— 颜色随时间变化。
        //
        // ⚠️ **两条独立的键序列**（colorKeys / alphaKeys）不是冗余：Unity 允许
        // "颜色在 0.3 处变红、透明度在 0.7 处淡出"，两个维度的时间点不需要对齐。
        //
        // ⚠️ 门面（`declare class Gradient`，ZHeaderTool 生成）上的"改"全部**返回
        // 新值**：Gradient 跨边界是快照，原地改没有意义。所以这里刻意**没有**
        // colorKeys / alphaKeys 的访问器属性 —— setter 的返回值会被丢掉，装上
        // 就是个永久空操作。
        "declare interface GradientColorKey {\n"
        "    color: Color;\n"
        "    time: number;\n"
        "}\n"
        "\n"
        "declare interface GradientAlphaKey {\n"
        "    alpha: number;\n"
        "    time: number;\n"
        "}\n"
        "\n"
        "declare interface Gradient {\n"
        "    colorKeys: GradientColorKey[];\n"
        "    alphaKeys: GradientAlphaKey[];\n"
        "    mode: number;\n"
        "}\n"
        "\n"
        "declare interface ColorParseResult {\n"
        "    valid: boolean;\n"
        "    color: Color;\n"
        "}\n"
        "\n"
        // Touch：一次触摸在某一帧的快照（`Input.GetTouch(i)` / `Input.touches`）。
        //
        // ⚠️ 字段名与 `ZScript::Converter<Touch>` 写的键**逐字对应**，改一处就要
        // 改另一处 —— 少了字段 TS 会报"属性不存在"，多了字段运行时是 undefined。
        //
        // ⚠️ `pressure` / `maximumPossiblePressure` 在不支持压力的设备上是 **1**，
        // 不是 0（Unity 的规定，见 Touch.h）。脚本里拿压力做笔刷的话，判据是
        // `Input.touchPressureSupported`，不是 `pressure > 0`。
        "declare interface Touch {\n"
        "    fingerId: number;\n"
        "    position: Vector2;\n"
        "    rawPosition: Vector2;\n"
        "    deltaPosition: Vector2;\n"
        "    deltaTime: number;\n"
        "    tapCount: number;\n"
        "    phase: TouchPhase;\n"
        "    pressure: number;\n"
        "    maximumPossiblePressure: number;\n"
        "    type: number;\n"
        "    radius: number;\n"
        "    radiusVariance: number;\n"
        "    altitudeAngle: number;\n"
        "    azimuthAngle: number;\n"
        "}\n"
        "\n"
        // Unity 的 `Material`：引擎里那个类是 `MaterialInstance`
        // （ScriptMaterial.h 写了为什么 —— `Material` 这个名字被材质**资产**类占用，
        // ZHeaderTool 要求反射类名全局唯一）。类型别名把 Unity 的拼写还给脚本：
        // 标注、`renderer.material` 的返回类型都能写 `Material`。
        //
        // ⚠️ 只是**类型**别名，不是新类：`new Material()` 不行（Unity 那边要的
        // 是 `new Material(shader)`，本引擎没有"凭 shader 造一份材质"的入口），
        // 材质只能从 `renderer.material` 拿。
        "declare type Material = MaterialInstance;\n"
        "\n"
        // 同一件事的另一例：引擎里灯的类叫 `LightComponent`（与 CameraComponent /
        // RigidBodyComponent 等组件保持一致的命名），Unity 叫 `Light`。
        "declare type Light = LightComponent;\n"
        "\n"
        // 第三例：刚体。引擎里的组件叫 `RigidBodyComponent`（大写 B，与
        // CameraComponent / AudioSourceComponent 一族对齐），Unity 叫
        // `Rigidbody`（小写 b）。脚本里 `let body: Rigidbody` 是最高频的写法之一，
        // 所以把 Unity 的拼写也还给脚本。
        //
        // ⚠️ 同样只是类型别名：`GetComponent("Rigidbody")` 按**反射名**查，反射
        // 名仍然是 `RigidBodyComponent`（`AddComponent` / `GetComponent` 收的是
        // 字符串，与这里的类型名无关）。
        "declare type Rigidbody = RigidBodyComponent;\n"
        "\n"
        // 第四例：蒙皮网格渲染器。Unity 叫 `SkinnedMeshRenderer`（两个 n），引擎
        // 里叫 `SkinMeshRenderer`（与 `SkinMeshRenderer.cpp` / 资产侧的命名一致）。
        // 别名让脚本能照 Unity 的拼写写标注。
        //
        // ⚠️ 同样只是类型别名：`GetComponent("SkinnedMeshRenderer")` 查不到 ——
        // 反射名仍是 `SkinMeshRenderer`。
        "declare type SkinnedMeshRenderer = SkinMeshRenderer;\n"
        "\n"
        // Bounds：Unity 存的是 center + **extents（半尺寸）**，`size` / `min` /
        // `max` 都是派生量。键名与 `ZScript::Converter<Bounds>` 一致。
        // 与 `declare class Bounds`（ScriptBounds.h 那批静态方法）同名合并。
        //
        // ⚠️ 这里只有两个字段，所以 TS 里 `{ center: ..., extents: ... }` 字面量
        // 可以直接当 Bounds 用 —— 这正是 Unity 里 `new Bounds(c, s)` 的位置
        // （ZHeaderTool 绑不了构造函数，所以额外给了 `Bounds.Create(c, s)`）。
        "declare interface Bounds {\n"
        "    center: Vector3;\n"
        "    extents: Vector3;\n"
        "}\n"
        "\n"
        // AnimationCurve / Keyframe：Unity 的曲线与关键帧。键名与
        // `ZScript::Converter<AnimationCurve>` / `<Keyframe>` 一致。
        //
        // ⚠️ `keys` 过边界的是**快照**：改这个数组不影响曲线（见
        // `Converter<std::vector<T>>` 的说明）。要改曲线得用
        // `AnimationCurve.AddKey(...)` 并且**接住返回值** —— 值语义，忘了接的话
        // 曲线一直是空的，而空曲线求值是 0，不报错。
        "declare interface Keyframe {\n"
        "    time: number;\n"
        "    value: number;\n"
        "    /** 入切线，斜率 dValue/dTime（不是角度）。 */\n"
        "    inTangent: number;\n"
        "    /** 出切线，斜率 dValue/dTime。 */\n"
        "    outTangent: number;\n"
        "}\n"
        "\n"
        "declare interface AnimationCurve {\n"
        "    keys: Keyframe[];\n"
        "    preWrapMode: number;\n"
        "    postWrapMode: number;\n"
        "}\n"
        "\n"
        "declare interface Quaternion {\n"
        "    w: number;\n"
        "    x: number;\n"
        "    y: number;\n"
        "    z: number;\n"
        "}\n"
        "\n"
        // m<row><col>，与 ZScript::Converter<Matrix4x4> 的键名一致，也是 Unity 的
        // 拼写。不用扁平数组，否则下标含义会取决于源端是行主序还是列主序。
        "declare interface Matrix4x4 {\n"
        "    m00: number; m01: number; m02: number; m03: number;\n"
        "    m10: number; m11: number; m12: number; m13: number;\n"
        "    m20: number; m21: number; m22: number; m23: number;\n"
        "    m30: number; m31: number; m32: number; m33: number;\n"
        "}\n"
        "\n"
        // AI 的两个意图结构。键名与 ZScript::Converter<AIMoveIntent> /
        // <AIActionRequest> 一致。moveTarget / faceTarget 复用 Vector3。
        "declare interface AIMoveIntent {\n"
        "    hasMoveTarget: boolean;\n"
        "    moveTarget: Vector3;\n"
        "    speedRatio: number;\n"
        "    hasFaceTarget: boolean;\n"
        "    faceTarget: Vector3;\n"
        "}\n"
        "\n"
        "declare interface AIActionRequest {\n"
        "    actionName: string;\n"
        "    targetId: number;\n"
        "}\n"
        "\n"
        // Unity 的 RaycastHit。`valid` 是 Unity 用 Physics.Raycast 的 bool
        // 返回值表达的那一位——TS 侧没有 out 参数，所以由记录自己携带。
        // 键名与 ZScript::Converter<RaycastHit> 一致。
        // Unity 的关节值类型。键名与 ZScript::Converter<JointLimits> /
        // <JointMotor> / <JointSpring> 一致。
        //
        // ⚠️ 字段比 Unity 少，这是**故意**的（见 PhysicsJoint.h）：Unity 的
        // `JointLimits` 还有 bounciness / bounceMinVelocity / contactDistance，
        // `JointMotor` 还有 freeSpin —— Jolt 那侧没有对应位置。少声明，脚本
        // 用到这些字段时会**编译不过**，而编译不过优于"能跑、改了个没人读的
        // 字段"。
        "declare interface JointLimits {\n"
        "    /** 最小角度（铰链，弧度）/ 最小位置（滑轨、弹簧，米）。 */\n"
        "    min: number;\n"
        "    /** 最大角度（铰链，弧度）/ 最大位置（滑轨、弹簧，米）。 */\n"
        "    max: number;\n"
        "}\n"
        "\n"
        "declare interface JointMotor {\n"
        "    /** 目标角速度（铰链，弧度/秒）。 */\n"
        "    targetVelocity: number;\n"
        "    /** 电机最大力矩（铰链，牛·米）。 */\n"
        "    force: number;\n"
        "}\n"
        "\n"
        "declare interface JointSpring {\n"
        "    /** 刚度。Jolt 里是驱动弹簧的频率（Hz）。 */\n"
        "    spring: number;\n"
        "    /** 阻尼。 */\n"
        "    damper: number;\n"
        "    /** 弹簧要拉到的角度（铰链，弧度）。 */\n"
        "    targetPosition: number;\n"
        "}\n"
        "\n"
        // Unity 的 `SoftJointLimitSpring`（`CharacterJoint.twistLimitSpring` /
        // `.swingLimitSpring`）。与上面的 `JointSpring` 差一个 `targetPosition`：
        // 限位弹簧拉回的是限位面，目标位置由约束自己算，脚本给不了 —— 复用
        // `JointSpring` 会让那个字段变成"能写、没人读"。
        "declare interface SoftJointLimitSpring {\n"
        "    /** 刚度。Jolt 里是限位弹簧的频率（Hz），0 = 硬限位。 */\n"
        "    spring: number;\n"
        "    /** 阻尼。 */\n"
        "    damper: number;\n"
        "}\n"
        "\n"
        // Unity 的 `JointDrive`（`ConfigurableJoint.xDrive` / `.angularXDrive` /
        // ...）。与 `JointMotor` / `JointSpring` 三个并存：追的量不同（速度 /
        // 角度 / 位置+最大出力）。
        //
        // ⚠️ `positionSpring` <= 0 就是"这个轴没有驱动" —— 没有单独的开关，与
        // Unity 一致。
        "declare interface JointDrive {\n"
        "    /** 位置弹簧的刚度（牛/米，旋转轴是牛·米/弧度）。 */\n"
        "    positionSpring: number;\n"
        "    /** 阻尼。 */\n"
        "    positionDamper: number;\n"
        "    /** 最大出力（牛 / 牛·米）。默认 Number.MAX_VALUE，即不限。 */\n"
        "    maximumForce: number;\n"
        "}\n"
        "\n"
        // 关节族：引擎里的类叫 `HingeJointComponent` 等（与 RigidBodyComponent
        // / CameraComponent 一族对齐），Unity 叫 `HingeJoint` / `FixedJoint` /
        // `SpringJoint`。与 `Rigidbody` 同一条例子的第二、三、四、五例。
        //
        // ⚠️ 只是类型别名：`AddComponent("HingeJoint")` 拿不到东西 —— 反射名
        // 仍是 `HingeJointComponent`，AddComponent 收的是字符串、按反射名查。
        "declare type HingeJoint = HingeJointComponent;\n"
        "\n"
        "declare type FixedJoint = FixedJointComponent;\n"
        "\n"
        "declare type SpringJoint = SpringJointComponent;\n"
        "\n"
        "declare type SliderJoint = SliderJointComponent;\n"
        "\n"
        "declare type CharacterJoint = CharacterJointComponent;\n"
        "\n"
        "declare type ConfigurableJoint = ConfigurableJointComponent;\n"
        "\n"
        "declare interface RaycastHit {\n"
        "    valid: boolean;\n"
        "    point: Vector3;\n"
        "    normal: Vector3;\n"
        "    distance: number;\n"
        "    bodyId: number;\n"
        "    layer: number;\n"
        "    /** 打中的 body 属于哪个对象。纯几何体（没有 RigidBodyComponent 的）为 null。 */\n"
        "    gameObject: GameObject | null;\n"
        "}\n"
        "\n"
        // Unity 的 `YieldInstruction` 家族 + `StartCoroutine` 的返回句柄。
        //
        // 这四个在 C++ 侧没有对应类型：脚本 yield 出来的对象只带一个
        // `__zWait` 字符串标记（ScriptEnv 的 shim 打上，ScriptEnv::
        // GeneratorNext 读掉）。所以这里声明成"带一个私有标记字段的 class"，
        // 让用户**必须**走构造器 —— 手写 `{ __zWait: 'seconds' }` 仍能过
        // 运行期，但会丢掉类型检查。
        //
        // 声明成 class 而不是 interface：Unity 里 `new WaitForSeconds(1)` 是
        // 唯一的合法构造方式，interface 会让 `yield { seconds: 1 }` 也过编译，
        // 然后在运行期因为缺 `__zWait` 被当成"下一帧继续"——一个静默的语义
        // 降级，比编译期报错糟得多。
        "/** Unity's YieldInstruction family: the values a script coroutine yields. */\n"
        "declare class YieldInstruction {\n"
        "    private __zWait: string;\n"
        "}\n"
        "\n"
        "declare class WaitForSeconds extends YieldInstruction {\n"
        "    constructor(seconds: number);\n"
        "}\n"
        "\n"
        "declare class WaitForSecondsRealtime extends YieldInstruction {\n"
        "    constructor(seconds: number);\n"
        "}\n"
        "\n"
        "declare class WaitForFixedUpdate extends YieldInstruction {\n"
        "    constructor();\n"
        "}\n"
        "\n"
        "declare class WaitForLateUpdate extends YieldInstruction {\n"
        "    constructor();\n"
        "}\n"
        "\n"
        "declare class WaitForEndOfFrame extends YieldInstruction {\n"
        "    constructor();\n"
        "}\n"
        "\n"
        // WaitUntil / WaitWhile 的构造器吃一个**闭包**，所以它们的声明带参数
        // （其余几个是空构造器）。谓词每帧被轮询一次，直到它翻面为止 —— 不是
        // 只在 yield 的那一帧问一次。
        "/** Resumes the first Update where `predicate()` returns true. */\n"
        "declare class WaitUntil extends YieldInstruction {\n"
        "    constructor(predicate: () => boolean);\n"
        "}\n"
        "\n"
        "/** Resumes the first Update where `predicate()` returns false. */\n"
        "declare class WaitWhile extends YieldInstruction {\n"
        "    constructor(predicate: () => boolean);\n"
        "}\n"
        "\n"
        // Unity 里 `Coroutine` 是 `YieldInstruction` 的子类 —— 这是
        // `yield return StartCoroutine(other)` 能编译的唯一原因。这里的
        // `extends` 不是装饰：`YieldInstruction` 带私有字段 ⇒ TS 用名义类型
        // 判定，只有真子类才能塞进 `Generator<YieldInstruction | null>`。
        // 不 extends 的话用户必须写 `yield c as any`，等于把类型检查关掉。
        //
        // 运行期它不带 `__zWait`（Unity 也是直接 yield 句柄、没有第二个构造
        // 器），所以 GeneratorNext 按 `__zId` 的形状认，且必须在读 `__zWait`
        // 之前判。
        "/** What `StartCoroutine` hands back: feed it to `StopCoroutine`, or `yield` it. */\n"
        "declare class Coroutine extends YieldInstruction {\n"
        "    private __zId: number;\n"
        "    private __zMethod: string;\n"
        "}\n"
        "\n"
        "/** Engine-side logging facade. Routes to the [ZScripting] log category. */\n"
        // 注意：`Debug` 是 namespace 而不是 class —— 它是一个纯 JS 对象
        // （ScriptEnv 的 shim），支持 `Debug.Log("a", 1, obj)` 任意个参数，
        // 而 ZFUNCTION 的签名是固定的。改成绑定类会同时丢掉多参和覆盖掉
        // 那个 JS 对象。见 Runtime/Scripting/ScriptDebug/ScriptDebug.h。
        "declare namespace Debug {\n"
        "    function Log(...args: any[]): void;\n"
        "    function LogWarning(...args: any[]): void;\n"
        "    function LogError(...args: any[]): void;\n"
        "    /** 打异常的 stack（没有就退化为 String(e)）。 */\n"
        "    function LogException(e: any): void;\n"
        "    /** 条件为假时记一条 error。与 C++ assert 不同，不中断进程。 */\n"
        "    function Assert(condition: boolean, message?: string): void;\n"
        "    /** 当前二进制是不是开发构建（判据是 NDEBUG）。 */\n"
        "    function IsDebugBuild(): boolean;\n"
        "    /** 日志留痕并挂住调试器；没有调试器时只记日志，不中断。 */\n"
        "    function Break(message?: string): void;\n"
        "}\n"
        "\n"
        "/** Standard browser-style console, also routed into [ZScripting]. */\n"
        "declare const console: {\n"
        "    log(...args: any[]): void;\n"
        "    info(...args: any[]): void;\n"
        "    debug(...args: any[]): void;\n"
        "    warn(...args: any[]): void;\n"
        "    error(...args: any[]): void;\n"
        "};\n"
        "\n"
        "// Internal helper - keep declared so user code that accidentally references\n"
        "// it doesn't get a fresh `any` (which would mask typos).\n"
        "declare function __zNewInstance<T>(ctor: { new(): T }): T;\n"
        "\n"
        "// ---- P8 HMR ----\n"
        "// 运行时由 ScriptEnv 的 IIFE 包装注入（`module.hot` 只在这层里存在）。\n"
        "// 不声明的话用户写 `if (module.hot) module.hot.accept()` 会直接 TS2304，\n"
        "// HMR 在 .ts 侧等于不可用 —— 而 .ts 才是用户真正写的东西。\n"
        "// `hot` 声明成可选：非模块上下文（比如被 eval 的裸片段）里它可能不在。\n"
        "declare var module: {\n"
        "    exports: any;\n"
        "    hot?: {\n"
        "        /** 声明本模块可以热替换。给了 cb 就在换完 exports 之后以新 exports 调它。 */\n"
        "        accept(callback?: (newExports: any) => void): void;\n"
        "        /** 旧 exports 被丢弃之前调用（清定时器 / 事件监听）。 */\n"
        "        dispose(callback: () => void): void;\n"
        "        /** 永不热替换，一律走 P6 全量重建。 */\n"
        "        decline(): void;\n"
        "    };\n"
        "};\n";

}  // namespace

namespace ScriptBindingRegistry
{
    bool IsReservedScriptName(const char* name)
    {
        if (name == nullptr)
            return false;
        for (const char* reserved : kReservedScriptNames)
        {
            if (std::strcmp(reserved, name) == 0)
                return true;
        }
        return false;
    }

    void Register(ZClassScriptBinding& binding)
    {
        binding.next = g_Head;
        g_Head = &binding;
    }

    const ZClassScriptBinding* Classes() { return g_Head; }

    void RegisterEnum(ZScriptEnum& script_enum)
    {
        script_enum.next = g_EnumHead;
        g_EnumHead = &script_enum;
    }

    const ZScriptEnum* Enums() { return g_EnumHead; }

    void RegisterSynthetic(ZSyntheticScriptClass& cls)
    {
        cls.next = g_SyntheticHead;
        g_SyntheticHead = &cls;
    }

    const ZSyntheticScriptClass* SyntheticClasses() { return g_SyntheticHead; }

    void InstallAll(pesapi_registry_api* apis, pesapi_registry registry)
    {
        if (apis == nullptr || registry == nullptr || g_Installed)
            return;
        g_Installed = true;

        std::vector<const ZClassScriptBinding*> classes;
        for (const ZClassScriptBinding* cls = g_Head; cls != nullptr; cls = cls->next)
            classes.push_back(cls);

        // Pass 1 -- a bound class may derive from a class that has no ZFUNCTION
        // of its own (GameObject -> Object). pesapi resolves the super chain
        // through the registry, so an unknown super type id would leave the
        // derived class with a broken prototype. Declare those supers first as
        // empty classes; the .d.ts emits matching `declare class <Super> {}`.
        std::vector<const void*> declared;
        for (const ZClassScriptBinding* cls : classes)
            declared.push_back(cls->typeId);
        for (const ZClassScriptBinding* cls : classes)
        {
            if (cls->superTypeId == nullptr || cls->superName == nullptr || cls->superName[0] == '\0')
                continue;
            if (IsTypeDeclared(declared, cls->superTypeId))
                continue;
            apis->define_class(registry, cls->superTypeId, nullptr, nullptr, cls->superName, nullptr, nullptr, nullptr, 1);
            apis->set_property_info_size(registry, cls->superTypeId, 0, 0, 0, 0);
            declared.push_back(cls->superTypeId);
        }

        // Pass 2 -- the real classes. Registration order does not matter here:
        // pesapi only needs the type ids to be unique, and pass 1 already made
        // sure every referenced super exists.
        for (const ZClassScriptBinding* cls : classes)
        {
            apis->define_class(registry, cls->typeId, cls->superTypeId, nullptr, cls->scriptName, nullptr, nullptr, nullptr, 1);
            // pesapi 把实例方法与静态方法放在**两张**表里（ScriptClassDefinition::Methods / Functions，各按
            // set_property_info_size 给的个数 malloc，名字为空的那一项当结束符）⇒ 两种要分开计数、各自从 0 编号。
            // 以前静态方法个数传 0、下标用合表里的位置：第一个静态 ZFUNCTION（SpineRenderer::Of）一注册就写穿
            // Functions[] 的堆块（启动即崩），而且 Methods[] 在它的位置留下一个空洞 ⇒ 排在后面的实例方法全部"消失"。
            int instance_count = 0;
            int static_count = 0;
            for (std::size_t i = 0; i < cls->methodCount; ++i)
            {
                if (cls->methods[i].callback != nullptr)
                    ++(cls->methods[i].isStatic ? static_count : instance_count);
            }
            // 属性同理也是两张表：Properties（实例，装到 prototype）与
            // Variables（静态，装到构造器）。两者各自 malloc、各自从 0 编号，
            // 所以个数必须分开传，下标也必须分开设。混在一起会写穿堆块 —— 和
            // 上面方法表那次是同一个坑。
            int instance_prop_count = 0;
            int static_prop_count = 0;
            for (std::size_t i = 0; i < cls->propertyCount; ++i)
            {
                const ZScriptPropertyEntry& prop = cls->properties[i];
                if (prop.name == nullptr || prop.getter == nullptr)
                    continue;
                ++(prop.isStatic ? static_prop_count : instance_prop_count);
            }
            apis->set_property_info_size(registry, cls->typeId, instance_count, static_count,
                                         instance_prop_count, static_prop_count);
            int instance_index = 0;
            int static_index = 0;
            for (std::size_t i = 0; i < cls->methodCount; ++i)
            {
                const ZScriptMethodEntry& method = cls->methods[i];
                if (method.callback == nullptr)
                    continue;
                const int index = method.isStatic ? static_index++ : instance_index++;
                apis->set_method_info(registry, cls->typeId, index, method.name, method.isStatic ? 1 : 0, method.callback,
                                      nullptr, 1);
            }
            // Accessor properties. `is_static` routes the entry into pesapi's
            // `Variables` (installed on the constructor -- `Color.red`) or
            // `Properties` (installed on the prototype -- `transform.position`).
            // Each table is indexed from 0 independently, so the two counters
            // here are not the same as `i`.
            int instance_prop_index = 0;
            int static_prop_index = 0;
            for (std::size_t i = 0; i < cls->propertyCount; ++i)
            {
                const ZScriptPropertyEntry& prop = cls->properties[i];
                if (prop.name == nullptr || prop.getter == nullptr)
                    continue;
                const int index = prop.isStatic ? static_prop_index++ : instance_prop_index++;
                apis->set_property_info(registry, cls->typeId, index, prop.name, prop.isStatic ? 1 : 0,
                                        prop.getter, prop.readonly ? nullptr : prop.setter,
                                        nullptr, nullptr, 1);
            }
        }
    }

    void InstallGlobals(pesapi_ffi* apis, pesapi_env env, bool include_editor_domain)
    {
        if (apis == nullptr || env == nullptr)
            return;

        pesapi_value global = apis->global(env);
        if (global == nullptr)
            return;

        for (const ZClassScriptBinding* cls = g_Head; cls != nullptr; cls = cls->next)
        {
            if (cls->domain == ZScriptDomain::Editor && !include_editor_domain)
                continue;
            if (cls->scriptName == nullptr || cls->scriptName[0] == '\0')
                continue;
            // Never shadow a JS built-in (see kReservedScriptNames).
            if (ScriptBindingRegistry::IsReservedScriptName(cls->scriptName))
                continue;
            pesapi_value ctor = apis->create_class(env, cls->typeId);
            if (ctor == nullptr)
                continue;
            apis->set_property(env, global, cls->scriptName, ctor);
        }

        // ZENUM: a plain object of `{ Name: value }` pairs, matching the
        // `declare enum` the .d.ts emits from the same table. Without it the
        // ambient declaration would type-check and then evaluate to undefined.
        for (const ZScriptEnum* script_enum = g_EnumHead; script_enum != nullptr; script_enum = script_enum->next)
        {
            if (script_enum->domain == ZScriptDomain::Editor && !include_editor_domain)
                continue;
            if (script_enum->scriptName == nullptr || script_enum->scriptName[0] == '\0')
                continue;
            if (ScriptBindingRegistry::IsReservedScriptName(script_enum->scriptName))
                continue;
            pesapi_value object = apis->create_object(env);
            if (object == nullptr)
                continue;
            for (std::size_t i = 0; i < script_enum->valueCount; ++i)
            {
                const ZScriptEnumEntry& entry = script_enum->values[i];
                if (entry.name == nullptr || entry.name[0] == '\0')
                    continue;
                apis->set_property(env, object, entry.name, apis->create_int32(env, entry.value));
            }
            apis->set_property(env, global, script_enum->scriptName, object);
        }
    }

    void AppendTypeScriptDeclarations(std::string& out, ZScriptDomain domain)
    {
        // `all` answers "does this name exist anywhere in the program" (an
        // Editor class may extend a Runtime one); `classes` is what this call
        // actually emits.
        std::vector<const ZClassScriptBinding*> all;
        std::vector<const ZClassScriptBinding*> classes;
        for (const ZClassScriptBinding* cls = g_Head; cls != nullptr; cls = cls->next)
        {
            all.push_back(cls);
            if (cls->domain == domain)
                classes.push_back(cls);
        }

        // ZENUM first: class members declare their types with these names, and
        // a reader meeting `mode: CameraMode` before seeing what CameraMode is
        // has to scroll up. The values are printed as literal numbers so the
        // ambient enum and the runtime object cannot drift -- both are fed
        // from the same ZScriptEnumEntry table.
        for (const ZScriptEnum* script_enum = g_EnumHead; script_enum != nullptr; script_enum = script_enum->next)
        {
            if (script_enum->domain != domain)
                continue;
            if (script_enum->scriptName == nullptr || script_enum->scriptName[0] == '\0')
                continue;
            if (ScriptBindingRegistry::IsReservedScriptName(script_enum->scriptName))
                continue;
            out += "declare enum ";
            out += script_enum->scriptName;
            out += " {\n";
            for (std::size_t i = 0; i < script_enum->valueCount; ++i)
            {
                const ZScriptEnumEntry& entry = script_enum->values[i];
                if (entry.name == nullptr || entry.name[0] == '\0')
                    continue;
                out += "    ";
                out += entry.name;
                out += " = ";
                out += std::to_string(entry.value);
                out += ",\n";
            }
            out += "}\n\n";
        }

        if (classes.empty())
            return;

        // Names an `extends` clause may legally refer to. A Runtime file only
        // knows itself; an Editor file is compiled together with the Runtime
        // one, so it may extend a Runtime class too.
        const std::vector<const ZClassScriptBinding*>& known = (domain == ZScriptDomain::Runtime) ? classes : all;

        // A super class with no ZFUNCTION of its own still has to exist in the
        // .d.ts, otherwise `extends <Super>` does not compile. Two derived
        // classes sharing the same unbound super must only emit it once.
        // Editor files never emit placeholders: that is the Runtime file's job.
        for (const ZClassScriptBinding* cls : classes)
        {
            if (domain != ZScriptDomain::Runtime)
                break;
            if (cls->superName == nullptr || cls->superName[0] == '\0')
                continue;
            if (IsDeclared(known, cls->superName))
                continue;
            if (IsPlaceholderEmitted(cls->superName))
                continue;
            // `declare class Object {}` would collide with lib.d.ts.
            if (IsReservedScriptName(cls->superName))
                continue;
            out += "declare class ";
            out += cls->superName;
            out += " {}\n\n";
            g_EmittedPlaceholders.push_back(cls->superName);
        }

        for (const ZClassScriptBinding* cls : classes)
        {
            // A reserved name is skipped by InstallGlobals too, so declaring it
            // here would be a `declare class` with no runtime counterpart (and
            // a collision with lib.d.ts). --check-script-bindings reports it.
            if (ScriptBindingRegistry::IsReservedScriptName(cls->scriptName))
                continue;
            out += "declare class ";
            out += cls->scriptName != nullptr ? cls->scriptName : "?";
            // Only write `extends <Super>` when that name actually exists in
            // the emitted file: either the super has its own binding, or a
            // placeholder was emitted for it above. A reserved name (Object)
            // gets neither, so the class is emitted standalone.
            const bool super_is_bound = cls->superName != nullptr && IsDeclared(known, cls->superName);
            const bool super_has_placeholder = cls->superName != nullptr && IsPlaceholderEmitted(cls->superName);
            if (cls->superName != nullptr && cls->superName[0] != '\0' && (super_is_bound || super_has_placeholder))
            {
                out += " extends ";
                out += cls->superName;
            }
            out += " {\n";
            // Properties first: they are what a Unity-authored script reaches
            // for, and reading the shape of a class before its methods is the
            // order the Inspector shows too.
            for (std::size_t i = 0; i < cls->propertyCount; ++i)
            {
                const ZScriptPropertyEntry& prop = cls->properties[i];
                if (prop.name == nullptr || prop.tsType == nullptr)
                    continue;
                if (prop.docComment != nullptr && prop.docComment[0] != '\0')
                {
                    out += "    /** ";
                    out += prop.docComment;
                    out += " */\n";
                }
                out += "    ";
                // A static accessor is a class-level property (`Color.red`),
                // installed on the constructor rather than the prototype -- see
                // InstallAll. Order matters: `static readonly`, never the
                // reverse (TS rejects `readonly static`).
                if (prop.isStatic)
                    out += "static ";
                out += prop.readonly ? "readonly " : "";
                out += prop.name;
                out += ": ";
                out += prop.tsType;
                out += ";\n";
            }
            for (std::size_t i = 0; i < cls->methodCount; ++i)
            {
                const ZScriptMethodEntry& method = cls->methods[i];
                if (method.tsSignature == nullptr || method.tsSignature[0] == '\0')
                    continue;
                if (method.docComment != nullptr && method.docComment[0] != '\0')
                {
                    out += "    /** ";
                    out += method.docComment;
                    out += " */\n";
                }
                out += method.isStatic ? "    static " : "    ";
                out += method.tsSignature;
                out += ";\n";
            }
            out += "}\n\n";
        }

        // Script-side classes last, so the reader sees the engine types their
        // members refer to first. They belong to the Runtime file only.
        for (const ZSyntheticScriptClass* cls = g_SyntheticHead; domain == ZScriptDomain::Runtime && cls != nullptr; cls = cls->next)
        {
            if (cls->scriptName == nullptr || cls->scriptName[0] == '\0')
                continue;
            out += "declare class ";
            out += cls->scriptName;
            if (cls->superName != nullptr && cls->superName[0] != '\0')
            {
                out += " extends ";
                out += cls->superName;
            }
            out += " {\n";
            for (std::size_t i = 0; i < cls->propertyCount; ++i)
            {
                const ZSyntheticProperty& prop = cls->properties[i];
                if (prop.name == nullptr || prop.typeName == nullptr)
                    continue;
                out += "    ";
                if (prop.readonly)
                    out += "readonly ";
                out += prop.name;
                out += ": ";
                out += prop.typeName;
                out += ";\n";
            }
            for (std::size_t i = 0; i < cls->methodCount; ++i)
            {
                if (cls->methodLines[i] == nullptr)
                    continue;
                out += cls->methodLines[i];
                out += "\n";
            }
            out += "}\n\n";
        }
    }

    std::string BuildTypeScriptDeclarationFile()
    {
        std::string dts = kZEngineDtsTemplate;
        dts += "\n// ---- engine bindings (generated from ZFUNCTION / ZPROPERTY / ZENUM) ----\n\n";
        AppendTypeScriptDeclarations(dts);
        return dts;
    }

    std::string BuildEditorTypeScriptDeclarationFile()
    {
        std::string dts =
            "// =========================================================================\n"
            "// AUTO-GENERATED by ZEngine. Do NOT edit -- regenerated on every editor start.\n"
            "// Source of truth: ScriptBindingRegistry::BuildEditorTypeScriptDeclarationFile\n"
            "// Editor-only API (the UnityEditor.dll equivalent). Compile together with\n"
            "// zengine.d.ts; game scripts must never see this file.\n"
            "// =========================================================================\n"
            "\n";
        AppendTypeScriptDeclarations(dts, ZScriptDomain::Editor);
        return dts;
    }
}  // namespace ScriptBindingRegistry
