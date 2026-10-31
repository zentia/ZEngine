# Component 脚本 API（Unity 门面）

切片 13。补的是脚本层最日常、此前却几乎全空的一层。

## 缺口在哪

`GameObject` 上早就有完整的查询家族（`GetComponent` / `GetComponentInChildren` /
`GetComponentInParent` / `SendMessage` / `BroadcastMessage` / `SendMessageUpwards`），
但 **`Component` 上只有 3 个 `ZFUNCTION`**。于是 TypeScript 里没有 `this.GetComponent(...)`，
每个 behaviour 想拿兄弟组件都得先绕一圈宿主：

```ts
// 补之前：Unity 里根本不需要这一行
const rb = this.gameObject.GetComponent("RigidBodyComponent");
// 补之后
const rb = this.GetComponent("RigidBodyComponent");
```

在 Unity 里这两者从来就是一句话，本引擎凭空多出来的差异只会让人写错。

补进来的 15 个方法**没有一个是新能力** —— 全部转发到宿主 `GameObject`。补的是
"脚本能不能直接用"，不是"引擎会不会做"。

## API

### 属性

```ts
comp.name          // 宿主 GameObject 的名字（可读可写）
comp.tag           // 宿主的标签
comp.transform     // 只读
comp.gameObject    // 只读
comp.enabled       // 沿用 m_Enabled（ZPROPERTY）
```

`name` 值得单独说：Unity 的 `Component.name` **与游戏对象及其所有组件共享同一个
名字**。所以这里返回的是宿主的名字，**不是** 组件自己的 `Object::name`（那个字段通常
是空的，因为序列化的是 GameObject 的名字）。写也穿透 —— `this.name = "x"` 真的把
对象改名了。

### 查询

```ts
comp.GetComponent(type)              // 宿主身上第一个
comp.GetComponents(type)             // 宿主身上全部（不含子对象）
comp.GetComponentInChildren(type)    // 宿主 + 后代，先序，第一个
comp.GetComponentsInChildren(type)   // 宿主 + 后代，先序，全部
comp.GetComponentInParent(type)      // 宿主 + 祖先，由近及远，第一个
comp.GetComponentsInParent(type)     // 宿主 + 祖先，由近及远，全部
```

三条规则与 `GameObject` 上同名方法完全一致（同一份实现，不是两套）：

| 规则 | 说明 |
| --- | --- |
| `InChildren` / `InParent` **都含自己** | Unity 的 `GetComponentInChildren` 先查自己。不含自己的话，"碰撞体就在本对象上"这个最常见的情况会返回 null，而调用方拿 null 当"没有碰撞体"。 |
| 先序、返回**第一个**命中的 | 不是"最近的一层"，Unity 是先序遍历。 |
| 基类也算匹配 | `GetComponents("Component")` 捞到所有组件；判定走 `Type::IsBaseOf`，不是名字相等。 |

对应的 `GameObject` 侧补了两个复数查询：`GetComponents(type)`（只查自己）和
`GetComponentsInParent(type)`。`GetComponentsInParent` 走的是 `GetComponents`
那条路（自己 → 父 → 祖父），所以"第一个"一定是"全部里的第一个"。

### 消息

```ts
comp.SendMessage(name)         // 同 GameObject.SendMessage
comp.BroadcastMessage(name)    // 向下
comp.SendMessageUpwards(name)  // 向上
```

返回**实际收到消息的组件数**（Unity 返回 void，这里返回 int 是有意的超集）：
0 表示"没人收到"，而"没人收到"绝大多数时候是方法名拼错了 —— 一个静默的 void 会让
这种拼写错误活到上线。

## 没有宿主的组件

组件的生命周期里真实存在一段 `m_ParentObject == nullptr` 的窗口：`MemoryManager`
造出来之后、`PostLoadResource` 挂上宿主之前。脚本没理由知道这个窗口，但撞上时必须
拿到**合法空值**而不是崩。全部 15 个入口都是这个形状：

| 方法 | 无宿主时 |
| --- | --- |
| `transform` | `null` |
| `name` | 自己的 `Object::name`（空串） |
| `tag` | `"Untagged"` —— **不是空串**，空串会让 `CompareTag("")` 恒真 |
| `CompareTag` | 与 `tag` 自洽：`CompareTag("Untagged")` 为 true |
| `SetTag` | `false` —— 不承诺一个保不住的标签 |
| 六个查询 | `null` / 空数组 |
| 三个消息 | `0` |

`tag` 与 `CompareTag` 的自洽是刻意的：一边返回 `"Untagged"` 一边说不匹配，是自相
矛盾的 API，只能靠读源码才能用对。

## 实现位置

转发需要 `GameObject` / `Transform` 的完整定义，而两者都反向 include 了
`Component.h`，所以定义落在**新建的 `Component.cpp`**，头文件里只能前向声明：

```cpp
class Transform;   // 头文件里只有这个
```

`Component.h` 里的 `#include "Component.generated.h"` 必须保持在**最后一条** include
（ZHeaderTool 会报错），新增的 `<vector>` / `<EASTL/string.h>` 排在它前面。

## TS 侧

`TypeScriptComponent` 通过继承拿到这些方法，所以 behaviour 里直接 `this.GetComponent(...)`
就可用。合成脚本类 `Behaviour` 的 `gameObject` / `transform` / `component` 三个属性
走的是另一条路（`ScriptBinding.cpp` 的 `g_BehaviourClass`），与本切片不冲突 ——
`Component` 本来就是绑定类，只是之前几乎没挂东西。

`enabled` 不要再加 `ZSCRIPTPROPERTY`：`m_Enabled` 本身就是 `ZPROPERTY()`，
ZHeaderTool 已经发布成 `enabled` 了，再挂一条会撞名（工具直接报错，不是静默）。
