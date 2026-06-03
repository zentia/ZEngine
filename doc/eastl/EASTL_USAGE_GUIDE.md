# EASTL 使用指南

## 📖 概述

EASTL (Electronic Arts Standard Template Library) 是 EA 开发的 STL 替代库，专为游戏开发优化，提供更好的性能和内存管理。本项目已集成 EASTL 并配置为使用 mimalloc 作为内存分配器。

## 🚀 快速开始

### 基本包含方式

```cpp
// 容器类
#include <EASTL/vector.h>
#include <EASTL/string.h>
#include <EASTL/hash_map.h>
#include <EASTL/map.h>
#include <EASTL/set.h>

// 智能指针
#include <EASTL/shared_ptr.h>
#include <EASTL/unique_ptr.h>
#include <EASTL/weak_ptr.h>

// 功能类
#include <EASTL/functional.h>
#include <EASTL/algorithm.h>
#include <EASTL/memory.h>
```

### 命名空间

EASTL 使用 `eastl` 命名空间：

```cpp
eastl::vector<int> vec;
eastl::string str;
eastl::shared_ptr<MyClass> ptr;
```

## 📦 常用容器使用

### 1. vector（动态数组）

```cpp
#include <EASTL/vector.h>

// 创建和使用
eastl::vector<int> vec;
vec.push_back(1);
vec.push_back(2);
vec.push_back(3);

// 访问元素
int first = vec[0];
int size = vec.size();
bool empty = vec.empty();

// 迭代器遍历
for (auto it = vec.begin(); it != vec.end(); ++it) {
    int value = *it;
}

// 范围for循环（C++11）
for (int value : vec) {
    // 处理 value
}

// 删除元素
// 1. 删除最后一个元素
vec.pop_back();

// 2. 删除指定位置的元素（通过迭代器）
auto it = vec.begin() + 1;  // 指向第二个元素
vec.erase(it);  // 删除后，后面的元素会前移

// 3. 删除指定范围的元素
vec.erase(vec.begin() + 1, vec.begin() + 3);  // 删除索引1到2的元素

// 4. EASTL 特有的 erase_unsorted（性能更好，但会改变顺序）
// 删除指定位置的元素，但用最后一个元素替换，不移动其他元素
vec.erase_unsorted(it);

// 5. 删除所有元素
vec.clear();

// 6. 删除特定值的元素（需要配合算法）
#include <EASTL/algorithm.h>
vec.erase(
    eastl::remove(vec.begin(), vec.end(), 2),  // 移除所有值为2的元素
    vec.end()
);

// 7. 删除满足条件的元素（使用 remove_if）
vec.erase(
    eastl::remove_if(vec.begin(), vec.end(), 
        [](int x) { return x > 10; }),  // 删除大于10的元素
    vec.end()
);

// 8. 在遍历时删除元素（注意迭代器失效）
for (auto it = vec.begin(); it != vec.end(); ) {
    if (*it == 2) {
        it = vec.erase(it);  // erase 返回下一个有效迭代器
    } else {
        ++it;
    }
}

// 项目中的实际使用示例
typedef eastl::vector<GUIView*> GUIViews;  // 来自 gui_view.h
```

### 2. string（字符串）

```cpp
#include <EASTL/string.h>

// 创建字符串
eastl::string str = "Hello";
eastl::string str2("World");

// 字符串操作
str += " EASTL";
str.append("!");
size_t len = str.length();
const char* cstr = str.c_str();

// 查找和替换
size_t pos = str.find("EASTL");
str.replace(pos, 5, "STL");

// 比较
bool equal = (str == "Hello EASTL!");
```

### 3. hash_map / unordered_map（哈希表）

```cpp
#include <EASTL/hash_map.h>
// 或者
#include <EASTL/unordered_map.h>

// 创建哈希表
eastl::hash_map<eastl::string, int> map;
// 或者使用 unordered_map（C++11风格）
eastl::unordered_map<eastl::string, int> map;

// 插入元素
map["key1"] = 100;
map.insert(eastl::make_pair("key2", 200));

// 查找元素
auto it = map.find("key1");
if (it != map.end()) {
    int value = it->second;
}

// 检查是否存在
if (map.count("key1") > 0) {
    // 键存在
}

// 删除元素
map.erase("key1");
```

### 4. map（有序映射）

```cpp
#include <EASTL/map.h>

eastl::map<eastl::string, int> sortedMap;
sortedMap["zebra"] = 1;
sortedMap["apple"] = 2;
sortedMap["banana"] = 3;

// map 会按 key 自动排序
// 遍历时会得到: apple -> banana -> zebra
```

### 5. set（集合）

```cpp
#include <EASTL/set.h>

eastl::set<int> uniqueSet;
uniqueSet.insert(3);
uniqueSet.insert(1);
uniqueSet.insert(3);  // 重复，不会被插入
uniqueSet.insert(2);

// 查找
if (uniqueSet.find(2) != uniqueSet.end()) {
    // 元素存在
}
```

## 🔧 智能指针

### shared_ptr（共享指针）

```cpp
#include <EASTL/shared_ptr.h>

// 创建 shared_ptr
eastl::shared_ptr<MyClass> ptr = eastl::make_shared<MyClass>(arg1, arg2);
eastl::shared_ptr<MyClass> ptr2(new MyClass());

// 使用
ptr->doSomething();
(*ptr).doSomething();

// 获取引用计数
long count = ptr.use_count();

// 项目中的实际使用示例
eastl::shared_ptr<Event> m_event;  // 来自 global_context.h
```

### unique_ptr（独占指针）

```cpp
#include <EASTL/unique_ptr.h>

// 创建 unique_ptr
eastl::unique_ptr<MyClass> ptr = eastl::make_unique<MyClass>();
eastl::unique_ptr<MyClass> ptr2(new MyClass());

// 移动语义（转移所有权）
eastl::unique_ptr<MyClass> ptr3 = eastl::move(ptr2);

// 释放所有权
MyClass* rawPtr = ptr.release();

// 重置
ptr.reset(new MyClass());
```

### weak_ptr（弱指针）

```cpp
#include <EASTL/weak_ptr.h>

eastl::shared_ptr<MyClass> shared = eastl::make_shared<MyClass>();
eastl::weak_ptr<MyClass> weak = shared;

// 检查对象是否仍然存在
if (!weak.expired()) {
    eastl::shared_ptr<MyClass> locked = weak.lock();
    if (locked) {
        // 使用 locked
    }
}
```

## 🎯 函数对象 (functional)

```cpp
#include <EASTL/functional.h>

// 函数指针
void myFunction(int x) { }
eastl::function<void(int)> func = myFunction;
func(42);

// Lambda 表达式
eastl::function<int(int, int)> add = [](int a, int b) {
    return a + b;
};
int result = add(3, 4);

// 成员函数
class MyClass {
public:
    void method(int x) { }
};

MyClass obj;
eastl::function<void(MyClass*, int)> memFunc = &MyClass::method;
memFunc(&obj, 42);

// 绑定
auto bound = eastl::bind(&MyClass::method, &obj, eastl::placeholders::_1);
bound(42);
```

## 🎨 EASTL 特有容器

### fixed_vector（固定大小vector）

```cpp
#include <EASTL/fixed_vector.h>

// 最多32个元素在栈上分配，超过后自动切换到堆
eastl::fixed_vector<int, 32> fixedVec;
fixedVec.push_back(1);
fixedVec.push_back(2);
// 如果超过32个元素，会自动使用堆内存
```

### fixed_string（固定大小string）

```cpp
#include <EASTL/fixed_string.h>

// 最多31个字符在栈上（SSO优化）
eastl::fixed_string<char, 32> fixedStr;
fixedStr = "Hello";  // 栈分配
fixedStr = "Very long string that exceeds stack capacity...";  // 堆分配
```

### vector_map / vector_set（基于vector的map/set）

```cpp
#include <EASTL/vector_map.h>
#include <EASTL/vector_set.h>

// 适用于小数据集，查找性能略低于map，但内存占用更小
eastl::vector_map<int, int> vmap;
eastl::vector_set<int> vset;
```

## 🔄 STL 到 EASTL 迁移对照表

| STL | EASTL | 说明 |
|-----|-------|------|
| `std::vector` | `eastl::vector` | 动态数组 |
| `std::string` | `eastl::string` | 字符串 |
| `std::unordered_map` | `eastl::hash_map` 或 `eastl::unordered_map` | 哈希表 |
| `std::unordered_set` | `eastl::hash_set` 或 `eastl::unordered_set` | 哈希集合 |
| `std::map` | `eastl::map` | 有序映射 |
| `std::set` | `eastl::set` | 有序集合 |
| `std::shared_ptr` | `eastl::shared_ptr` | 共享指针 |
| `std::unique_ptr` | `eastl::unique_ptr` | 独占指针 |
| `std::weak_ptr` | `eastl::weak_ptr` | 弱指针 |
| `std::function` | `eastl::function` | 函数对象 |
| `std::make_shared` | `eastl::make_shared` | 创建shared_ptr |
| `std::make_unique` | `eastl::make_unique` | 创建unique_ptr |

## 📝 完整示例

```cpp
#include <EASTL/vector.h>
#include <EASTL/hash_map.h>
#include <EASTL/string.h>
#include <EASTL/shared_ptr.h>
#include <EASTL/algorithm.h>

class GameObject {
public:
    GameObject(const eastl::string& name) : m_name(name) {}
    eastl::string getName() const { return m_name; }
private:
    eastl::string m_name;
};

void exampleGameSystem() {
    // 使用 vector 存储游戏对象
    eastl::vector<eastl::shared_ptr<GameObject>> objects;
    objects.push_back(eastl::make_shared<GameObject>("Player"));
    objects.push_back(eastl::make_shared<GameObject>("Enemy"));
    
    // 使用 hash_map 存储对象ID到对象的映射
    eastl::hash_map<uint32_t, eastl::shared_ptr<GameObject>> objectMap;
    objectMap[1] = objects[0];
    objectMap[2] = objects[1];
    
    // 查找对象
    auto it = objectMap.find(1);
    if (it != objectMap.end()) {
        eastl::string name = it->second->getName();
    }
    
    // 使用算法
    eastl::sort(objects.begin(), objects.end(), 
        [](const auto& a, const auto& b) {
            return a->getName() < b->getName();
        });
}
```

## ⚠️ 注意事项

### 1. 不能混用 STL 和 EASTL

```cpp
// ❌ 错误：不能混用
std::vector<int> stdVec;
eastl::vector<int> eastlVec;
stdVec = std::vector<int>(eastlVec.begin(), eastlVec.end());  // 需要显式转换

// ✅ 正确：统一使用 EASTL
eastl::vector<int> vec1;
eastl::vector<int> vec2 = vec1;  // 可以直接复制
```

### 2. 迭代器兼容性

EASTL 迭代器可以与 STL 算法一起使用：

```cpp
#include <EASTL/vector.h>
#include <algorithm>  // STL 算法

eastl::vector<int> vec = {3, 1, 4, 1, 5};
std::sort(vec.begin(), vec.end());  // ✅ 可以使用 STL 算法
```

### 3. 第三方库兼容性

某些第三方库可能需要 STL 类型，此时需要保留 STL 使用：

```cpp
// 第三方库函数需要 std::vector
void thirdPartyFunction(const std::vector<int>& data);

// 需要转换
eastl::vector<int> eastlData;
std::vector<int> stdData(eastlData.begin(), eastlData.end());
thirdPartyFunction(stdData);
```

### 4. 内存分配器

项目已配置 EASTL 使用 mimalloc 作为内存分配器，配置在：
- `engine/3rdparty/EASTL/EASTLUserConfig.h`

所有 EASTL 容器会自动使用 mimalloc，无需额外配置。

## 🎯 性能优势

1. **更快的执行速度**：通常比 STL 快 10-20%
2. **更好的缓存局部性**：优化的内存布局
3. **固定大小容器**：减少堆分配，提高性能
4. **自定义分配器**：已集成 mimalloc，性能更优

## 📚 项目中的实际使用

查看以下文件了解项目中的实际使用案例：

1. **global_context.h** - 使用 `eastl::shared_ptr`
   ```cpp
   eastl::shared_ptr<Event> m_event;
   ```

2. **gui_view.h** - 使用 `eastl::vector`
   ```cpp
   typedef eastl::vector<GUIView*> GUIViews;
   ```

3. **host_view.h** - 使用 `eastl::function`（已注释）
   ```cpp
   //eastl::function m_on_gui;
   ```

## 🔍 更多资源

- **EASTL GitHub**: https://github.com/electronicarts/EASTL
- **EASTL Wiki**: https://github.com/electronicarts/EASTL/wiki
- **项目集成指南**: `EASTL_INTEGRATION_GUIDE.md`
- **性能对比**: `GAME_CONTAINER_LIBRARIES.md`

## 💡 最佳实践

1. **新代码优先使用 EASTL**：所有新代码应使用 EASTL 容器
2. **性能关键路径优先迁移**：优先迁移性能敏感的代码
3. **固定大小容器**：对于小数据集，使用 `fixed_vector` 等固定容器
4. **统一命名空间**：在同一模块中统一使用 `eastl` 命名空间
5. **逐步迁移**：不要一次性替换所有代码，逐步迁移更安全

