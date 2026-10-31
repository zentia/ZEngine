# 刚体脚本 API（`RigidBodyComponent`）

对应 Unity：`UnityEngine.Rigidbody`。

宿主：`Source/Runtime/Function/Physics/PhysicsScene.{h,cpp}`（逻辑在这里）
组件：`Source/Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.{h,cpp}`
门禁：`UnityParitySmokeTest` 的 **G 组**（行为）与 **O 组**（接线）。

---

## 1. 补了什么

原来只有 13 个入口（是否模拟 / 线速度 / 角速度 / AddForce / AddImpulse / 质量
只读 / 刚体位姿）。Unity 里日常用的那一半都缺：改质量、阻力、关重力、运动学
开关、力矩、**偏心力**、传送 vs 推动、睡眠、爆炸力。

```ts
// 属性（Unity 同拼写）
rb.mass            // 千克
rb.drag            // 线阻尼
rb.angularDrag
rb.useGravity
rb.isKinematic
rb.velocity        // = GetLinearVelocity / SetLinearVelocity
rb.angularVelocity
rb.position        // 刚体的（不是 Transform 的）
rb.rotation

// 方法
rb.AddTorque(t: Vector3): void
rb.AddAngularImpulse(t: Vector3): void
rb.AddForceAtPosition(force: Vector3, position: Vector3): void
rb.AddImpulseAtPosition(impulse: Vector3, position: Vector3): void
rb.AddExplosionForce(force: number, position: Vector3, radius: number): void
rb.MovePosition(p: Vector3): void
rb.MoveRotation(r: Quaternion): void
rb.Sleep(): void
rb.WakeUp(): void
rb.IsSleeping(): boolean
```

---

## 2. 三处容易做错的地方

### 2.1 改质量必须连惯性张量一起缩

质量不是"改一个倒数"。只写 `SetInverseMass(1/m)` 的话，一个 10kg 的箱子会带
着 1kg 的转动惯量 —— **推起来对了，转起来飞快**。用 `MassProperties::ScaleToMass`
一次把质量和惯性都缩对（O4 守着）。

质量 <= 0 一律夹到极小正数：0 质量的倒数质量是 inf，一步积分之后就是 NaN 位置，
而一个 NaN 物体会把整个 broadphase 拖垮（表现是"物理突然全不动了"，不报错）。

### 2.2 `AddForceAtPosition` 的价值就是它会带出力矩

`rb.AddForce(f)` 只平移，`rb.AddForceAtPosition(f, p)` 会一边推一边转。两者都
"推动了物体"，所以**只看线速度分不出来** —— G20 / G21 是一组互补断言：中心力
必须**不**产生角速度，偏心臂必须**产生**角速度。

### 2.3 `MovePosition` 和 `SetPosition` 不是一回事

| | 语义 | 会不会推路上的东西 |
|---|---|---|
| `rb.position = p` | 传送 | 不会 |
| `rb.MovePosition(p)` | 由物理步按 `delta_time` 反推速度走过去 | **会** |

移动平台、玩家推杆要用 `MovePosition`；只把物体摆过去用 `position`。
`MoveKinematic` 用位移 / `delta_time` 反推速度，`delta_time` 为 0 会得到 inf
速度，所以夹到极小正数（O6）。

`MovePosition` / `MoveRotation` 内部用 1/60 —— 与物理的固定步长一致；传一个和
真实步长不符的值会让运动学物体每帧走得或快或慢。

---

## 3. 顺手修掉的一个死锁

`PhysicsScene::ApplyForceOrImpulse`（`AddForce` / `AddImpulse` 的共用实现）原来
在**持有 `BodyLockWrite` 时**调用 `BodyInterface::ActivateBody`。后者自己会再拿
一次同优先级的写锁，Jolt 不允许（PhysicsLock 的 `inType > mutexes` 断言），随后
就是同一线程重入同一把 mutex —— **直接死锁**。

之前没有任何测试真正调过 `AddForce` / `AddImpulse`，所以它一直没暴露。表现是
"施一次力，物理整个停住"，而且不报错。现在改成：先用一个**读锁**探完"是不是动
力学刚体"，出锁之后唤醒，再拿写锁施力（O15 守着这个结构）。

---

## 4. `AddExplosionForce` 是**单刚体**的

Unity 的 `Rigidbody.AddExplosionForce(force, position, radius)` 只作用于**这一个**
刚体（不是"把周围都炸飞"—— 那是自己在 `Physics.OverlapSphere` 上循环）。衰减按
距离线性降到 0；半径外不受力，这是"手雷在墙那边"和"手雷在脚下"的分界。

两个除零都守住了：半径为 0 / 负（直接返回），炸点正好在质心上（方向没有定义，
给一个确定的方向而不是归一化零向量 —— 那会得到 NaN 并污染整个刚体的速度）。

---

## 5. 已知限制

* **组件层拿不到活的刚体做测试**。`RigidBodyComponent` 通过
  `WorldManager::GetCurrentActivePhysicsScene()` 找场景，而"活动关卡"只能由加载
  流程设置（`m_CurrentActiveLevel` 是私有的，没有 setter）。headless 加载不出真
  实关卡，所以 G 组里组件永远走"没有场景"那条早退 —— 真正的逻辑（质量 / 阻尼 /
  力矩 / 偏心力 / 传送 / 睡眠）是在 `PhysicsScene` 上**直接**测的，组件那些一行
  转发由 O12 的接线门守着。
* **脚本里的类名是 `RigidBodyComponent`，不是 Unity 的 `Rigidbody`**。ZCLASS 不
  支持改名（它只接 `Abstract` / `CustomTransfer` 之类的标记），而脚本类名直接取
  C++ 类名。所有组件都是这个命名（`AudioSourceComponent` / `LightComponent` /
  `CameraComponent`），要统一改是另一个独立的改动。
* `RigidBodyActorType` 只有 static / kinematic / dynamic 三档，所以
  `isKinematic = false` 得到的是 **dynamic**，不是 static（Unity 同）。
