# Tweeny 使用指南

Tweeny 已成功集成到 ZEngine 中。这是一个现代化的 C++ 缓动动画库，提供了类似 Unity DOTween 和 UE Tween 插件的功能。

## 基本用法

### 包含头文件

```cpp
#include <tweeny.h>
```

### 简单的数值缓动

```cpp
// 创建一个从 0 到 100 的缓动，持续 1 秒，使用弹性缓动
auto tween = tweeny::from(0.0f).to(100.0f).during(1000).via(tweeny::easing::elasticOut);

// 在游戏循环中更新
void tick(float delta_time) {
    // step 方法接受毫秒数
    tween.step(static_cast<int>(delta_time * 1000));
    
    // 获取当前值
    float current_value = tween.peek();
    
    // 检查是否完成
    if (tween.progress() >= 1.0f) {
        // 动画完成
    }
}
```

### 在组件中使用

```cpp
#include "runtime/function/framework/component/component.h"
#include <tweeny.h>

namespace Z {
    class ExampleComponent : public Component {
    private:
        tweeny::tween<float> m_position_tween;
        float m_current_position = 0.0f;
        
    public:
        void postLoadResource(GameObject* parent_object) override {
            Component::postLoadResource(parent_object);
            
            // 初始化缓动：从当前位置移动到 100，持续 2 秒
            m_position_tween = tweeny::from(0.0f)
                .to(100.0f)
                .during(2000)
                .via(tweeny::easing::easeInOutCubic);
        }
        
        void tick(float delta_time) override {
            if (!m_enabled) return;
            
            // 更新缓动
            m_position_tween.step(static_cast<int>(delta_time * 1000));
            m_current_position = m_position_tween.peek();
            
            // 使用缓动值更新对象位置
            // TransformComponent* transform = m_parent_object->tryGetComponent(TransformComponent);
            // if (transform) {
            //     transform->setPosition(Vector3(m_current_position, 0, 0));
            // }
        }
    };
}
```

### 链式调用

```cpp
// 创建复杂的动画序列
auto tween = tweeny::from(0.0f)
    .to(100.0f).during(1000).via(tweeny::easing::easeOutQuad)
    .to(50.0f).during(500).via(tweeny::easing::easeInQuad)
    .to(200.0f).during(1500).via(tweeny::easing::elasticOut);

// 更新
tween.step(static_cast<int>(delta_time * 1000));
float value = tween.peek();
```

### 向量缓动

```cpp
#include "runtime/core/math/vector3.h"
#include <tweeny.h>

// 注意：tweeny 支持多种类型，但需要确保类型支持必要的操作
// 对于 Vector3，可能需要适配或使用单独的 x, y, z 分量

// 方法1：分别缓动每个分量
auto x_tween = tweeny::from(0.0f).to(10.0f).during(1000);
auto y_tween = tweeny::from(0.0f).to(20.0f).during(1000);
auto z_tween = tweeny::from(0.0f).to(5.0f).during(1000);

void tick(float delta_time) {
    x_tween.step(static_cast<int>(delta_time * 1000));
    y_tween.step(static_cast<int>(delta_time * 1000));
    z_tween.step(static_cast<int>(delta_time * 1000));
    
    Vector3 position(
        x_tween.peek(),
        y_tween.peek(),
        z_tween.peek()
    );
}
```

### 可用的缓动函数

Tweeny 提供了丰富的缓动函数：

- `tweeny::easing::linear` - 线性
- `tweeny::easing::easeInQuad` - 二次缓入
- `tweeny::easing::easeOutQuad` - 二次缓出
- `tweeny::easing::easeInOutQuad` - 二次缓入缓出
- `tweeny::easing::easeInCubic` - 三次缓入
- `tweeny::easing::easeOutCubic` - 三次缓出
- `tweeny::easing::easeInOutCubic` - 三次缓入缓出
- `tweeny::easing::elasticIn` - 弹性缓入
- `tweeny::easing::elasticOut` - 弹性缓出
- `tweeny::easing::elasticInOut` - 弹性缓入缓出
- `tweeny::easing::bounceIn` - 弹跳缓入
- `tweeny::easing::bounceOut` - 弹跳缓出
- `tweeny::easing::bounceInOut` - 弹跳缓入缓出
- 更多...

### 控制动画

```cpp
// 暂停/恢复
tween.pause();
tween.resume();

// 重置
tween.seek(0);  // 回到开始
tween.seek(500); // 跳转到 500 毫秒

// 设置进度（0.0 到 1.0）
tween.progress(0.5f); // 50% 进度

// 检查状态
bool is_paused = tween.isPaused();
float progress = tween.progress(); // 0.0 到 1.0
bool is_finished = (tween.progress() >= 1.0f);
```

### 回调函数

```cpp
auto tween = tweeny::from(0.0f)
    .to(100.0f)
    .during(1000)
    .via(tweeny::easing::easeOutQuad)
    .onStep([](float value) {
        // 每步更新时调用
        // 可以在这里更新 UI 或其他状态
    })
    .onFinish([]() {
        // 动画完成时调用
    });
```

## 最佳实践

1. **性能考虑**：Tweeny 是 header-only 库，编译时优化良好。对于大量动画，考虑使用对象池管理 tween 实例。

2. **时间单位**：注意 `step()` 方法接受毫秒数，而游戏循环通常使用秒。记得转换：
   ```cpp
   tween.step(static_cast<int>(delta_time * 1000));
   ```

3. **组件集成**：建议在组件的 `tick()` 方法中更新缓动，这样可以自动处理暂停、禁用等状态。

4. **内存管理**：Tweeny 对象可以安全地作为成员变量存储，无需特殊的内存管理。

## 更多资源

- [Tweeny GitHub](https://github.com/mobius3/tweeny)
- [Tweeny 文档](https://mobius3.github.io/tweeny/)

