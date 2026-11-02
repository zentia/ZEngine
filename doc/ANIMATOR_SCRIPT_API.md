# Animator 脚本 API（Unity `Animator` 的其余面）

`AnimationComponent`（`Runtime/Function/Framework/Component/Animation/AnimationComponent.h`）
+ `AnimatorInstance`（`Runtime/Function/Animation/AnimatorInstance.h`）。

这一片补的是 Unity `Animator` 上"除了参数与 Play/CrossFade 之外"的那一半：
播放速率、重新绑定、控制器形状查询、以及事件。

## 1. 为什么 `speed` 在实例上而不是控制器上

`AnimatorController` 是**共享资产**：十个角色共用一份，实例只拥有运行时状态
（参数值、每个状态的播放头、当前过渡）—— 这个分法是照 Unity 做的。

把 `speed` 放进控制器等于"让一个角色慢放改到所有人"。所以它在实例上，与参数值
同侧。连带的一条：`Initialize()`（以及 `Rebind()`）**不动** `m_Speed` —— 重新绑定
控制器不该把脚本设过的速率一起清掉。

## 2. `speed` 的语义与夹取

| Unity | 本引擎 |
|---|---|
| `animator.speed` | 属性 `speed`（`GetSpeed` / `SetSpeed`） |
| 0 = 冻结 | 同 |
| 负数 | **夹到 0** |

负数夹到 0 而不是原样接受：倒放需要 `AdvanceState` 支持反向推进，而今天
`PingPong` 的 `direction` 只覆盖"播到头再往回"，不是负 dt。接受 -1 会得到一个
"看起来在跑、其实卡在时间 0"的组件 —— 比直接拒绝难查得多。

`speed` 叠在 `delta_time` 上（`scaled_delta = delta_time * m_Speed`），状态的
`AnimatorState::speed` 再叠一层。两者是乘的关系，与 Unity 一致。

## 3. 控制器形状查询

| Unity | 本引擎 |
|---|---|
| `animator.parameterCount` | `GetParameterCount()` |
| `animator.GetParameter(i).name` | `GetParameterName(i)` |
| （无直接对应） | `GetStateCount()` / `GetStateName(i)` |
| `animator.GetCurrentAnimatorStateInfo(0).IsName(x)` | `IsName(x)` |

发这一组是为了让脚本能**枚举**而不是靠作者口述。"打错参数名静默失效"是这类
API 最难查的失败方式，`SetFloat("sped", 3)` 不会报错、不会崩。

`layerCount` 没有发：本引擎的状态机只有一层（Unity 的多层是为了叠加遮罩与权重，
这里没有对应物）。发一个恒为 1 的属性会让人以为有 `SetLayerWeight` 可用。

## 4. `Rebind`

`Rebind()` = `Reset()`：播放头归零、参数回到控制器默认值、跳回默认状态。

⚠️ 它会把 `SetStateClipLength()` 从片段资产解析出来的长度一起丢掉（重新按资产
值播种）。那是"从资产重新绑定"该有的行为 —— 调用方要的本来就是"回到初始状态"。

## 5. Unity 拼字与引擎拼字

`GetInteger` / `SetInteger` 与 `GetInt` / `SetInt` 指向**同一个槽位**。两组都留着，
因为照 Unity 文档抄过来的代码用的是 Integer，而引擎内部与旧脚本用的是 Int。

这类别名最容易在"改了原函数忘了改别名"之后悄悄分家，所以变异体 AM13/AM14 专门
钉住这一点。

## 6. 事件

| Unity | 本引擎 |
|---|---|
| `AnimationEvent` 回调到 MonoBehaviour 上的同名方法 | `GetFiredEventCount()` / `GetFiredEvent(i)`，由脚本轮询 |

⚠️ 形状与 Unity 不一样，写在最前面：Unity 是回调，这里是"上一次推进触发了哪些
名字"的列表。做成回调要靠字符串在脚本侧找方法，跨语言边界查函数比轮询更贵更脆
（而且找不到方法时是彻底的静默）。

列表在**每次推进的开头清空**，所以"这一帧触发了什么"读一次就没了 —— 不会同一个
事件被读到两次。

## 7. 刻意没做的 Unity 成员

- `applyRootMotion` / `bodyPosition` / `bodyRotation` / `rootPosition` /
  `deltaPosition` / `MatchTarget`：根位移没有被计算出来，没有后端。发一个恒为
  零的 `deltaPosition` 会让"用了根位移"的代码看起来在跑。
- `layerCount` / `GetLayerName` / `GetLayerIndex` / `SetLayerWeight`：单层。
- `GetBehaviour` / `GetBehaviours`：Unity 那个是"挂在状态上的 StateMachineBehaviour
  脚本"，本引擎的状态机没有"状态上挂脚本"这一层。
- `StringToHash`：Unity 用参数名哈希做查询优化。这里按名字查是 O(参数数)，而且
  发一个哈希函数会诱使脚本缓存哈希值 —— 那是只有 Unity 内部消费的东西。
- `cullingMode` / `updateMode` / `fireEvents` / `keepAnimatorControllerStateOnDisable`：
  没有后端。
- `StartPlayback` / `StopPlayback` / `StartRecording` / `StopRecording`：录制回放
  没有后端。
- `avatar` / `humanScale` / `GetBoneTransform`：人形骨架没有后端。

## 8. 测试

`UnityParitySmokeTest` 的 `BN0..BN37`（38 条）+ TS 链上的发布面门（ZA6）。

控制器是**在内存里现搭**的（`AnimatorInstance::Initialize` 收 `const` 引用，不碰
资产系统），所以这一组不依赖任何 .zasset。与 `Mesh` 那一组同样的理由：资产门面
的测试在本工程里一贯很弱（`loadAsset` 在测试进程里读不到东西），能不付这个代价
就不付。

## 9. 变异测试

`.mutate_anim.py`，18 个变异体，18/18 caught。

- **AM03（参数数报成状态数）与 AM04（反过来）一开始都存活**，原因是夹具里参数 2
  个、状态 2 个 —— 两个计数相等，"读错了另一个数组"这件事**没有任何可观测后果**。
  夹具改成 2 参数 / 3 状态（多一个没有过渡的 `Jump`）之后两条都被抓住。
  教训是通用的：**计数断言只有在数量不相等时才有信息量**，造夹具时故意让各类别的
  规模错开。
- **AM09（Rebind 变成空操作）由两条断言各抓一半**：`BN27`（回到默认状态）与
  `BN28`（参数回到默认值）。只留一条的话，"只重置播放头"的实现能活下来。
