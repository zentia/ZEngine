# glTF / GLB 网格导入

> 门禁：`GltfMeshImportSmokeTest`（`-DZENGINE_BUILD_GLTF_MESH_IMPORT_SMOKE_TEST=ON`）

## 1. 原来是"假支持"

2026-10-30 之前：

```cpp
bool IsSupportedMeshExtension(const std::string& ext_lower)
{
    return ext_lower == ".fbx" || ext_lower == ".obj" || ext_lower == ".gltf" || ext_lower == ".glb";
}
// ...
else { error = "glTF/GLB import is not implemented yet"; }
```

`CanImport` 无条件列出 `.gltf` / `.glb`，但导入分支只有一句报错。于是
ContentBrowser 里**能选中、能点导入、然后必然失败** —— 用户分不清"这引擎不支持"
和"我的文件坏了"。

**FBX 有同样的毛病**：`ZENGINE_HAS_UFBX` 没定义时扩展名照样列着，导入时报
"requires ufbx"。

## 2. 现在

| 格式 | 状态 |
| --- | --- |
| `.obj` | ✅ tinyobjloader |
| `.fbx` | ✅ ufbx（有 `ZENGINE_HAS_UFBX`） |
| `.gltf` / `.glb` | ✅ cgltf（有 `ZENGINE_HAS_CGLTF`；外置 `.bin` 与 `data:` URI 都支持） |

**扩展名清单与"解析器真的在"严格绑定**：解析器缺失 ⇒ 扩展名根本不出现在
`CanImport` / `GetSupportedExtensions` 里 ⇒ 文件被当作不支持的类型，走"不支持"
而不是"导入失败"。FBX 与 glTF 都按这个规则改了。

## 3. 支持范围（刻意收窄）

✅ 三角形图元、`POSITION` / `NORMAL` / `TEXCOORD_0`、索引（含无索引的隐式顺序）、
节点层级世界变换（含非均匀缩放下的法线逆转置）、sparse accessor（cgltf 本身处理）。

❌ **以下一律显式报错，绝不静默产出错误几何**：

| 情况 | 报错 |
| --- | --- |
| 非 TRIANGLES 图元（LINES / TRIANGLE_STRIP / …） | `unsupported primitive mode N` |
| 节点引用 skin | `skinned meshes are not supported yet` |
| Draco 压缩 | `Draco-compressed primitives are not supported` |
| 无 POSITION / 无几何 / 坏文件 / 索引越界 | 各自点名 |

理由：glTF 是个大格式，"部分支持"最容易退化成**"导入成功但模型是错的"**。
一个摆着 T-pose 不动的角色、一圈光照全错的模型，比一条"不支持"的报错难查得多。

morph target 属于中间地带：仍然导入基础几何，但通过 `out_warnings` 报出来，
`MeshImporter::Import` 会 `LOG_WARNING`。

## 4. 为什么单独成 `GltfMeshReader.{h,cpp}`

`MeshImporter::Import` 要 `GET_SYSTEM(ObjectManager)` / `AssetManager`，拉不起
这些系统就没法测。几何读取本身不需要任何系统，所以拆出来（同 `FbxAnimation.h`
的拆法）—— 冒烟测试只要编这一个 `.cpp` + 链 `ZRuntime`（拿 `MeshData`）和
`cgltf`，不必拖进整个编辑器。

## 5. 依赖：cgltf

`engine/3rdparty/cgltf/`：`cgltf.h`（单头，MIT，1.15）+ `cgltf_impl.c`
（`CGLTF_IMPLEMENTATION`，以 C 编译，免掉 C99 在 C++ 下的告警；同 `ufbx` 的套路）。

接入见 `engine/Source/Editor/CMakeLists.txt` 的 cgltf 块（定义 `ZENGINE_HAS_CGLTF`）。

⚠️ 用 `cgltf_parse`（内存字节）而不是 `cgltf_parse_file`：后者走
`fopen(const char*)`，在 Windows 上遇到非 ASCII 路径会直接失败，而中文路径是常态。

## 6. 测试

`GltfMeshImportSmokeTest`，37 项：

| 组 | 钉什么 |
| --- | --- |
| A | `.gltf`+外置 `.bin` / `.gltf`+`data:` URI / `.glb` 三种容器**结果逐字节一致**（同一份顶点数据） |
| B | 位置受 `scale`、法线、UV、索引正确；`scale` 只作用于位置 |
| C | 节点世界变换生效，**法线走逆转置** |
| D | LINES / 蒙皮 / 空场景 / 坏文件 / 缺文件都显式报错且**报错点名原因** |
| E | 源码级：`MeshImporter.cpp` 里没有占位报错分支，两个格式都由宏门控 |

### C 组的设计

节点 `scale = (1,2,1)`、法线 `(1,1,0)/√2`：

* 逆转置（正确）：`diag(1, 0.5, 1) · n ∝ (0.7071, 0.3536, 0)` → **(0.8944, 0.4472, 0)**
* 原矩阵（错误）：`diag(1, 2, 1) · n ∝ (0.7071, 1.4142, 0)` → **(0.4472, 0.8944, 0)**

两个结果几乎是"互换"，1e-3 容差就能区分 ⇒ 用错矩阵一定红。这一组专门针对
"非均匀缩放下忘了逆转置"——那是静默错误：模型看着导入成功了，光照整片发暗/发亮。

### 变异验证（都按预期红后还原）

| 变异 | 结果 |
| --- | --- |
| 法线改用原矩阵 | C3/C4 红，`got=0.447214 want=0.894427`（正是上面两个候选互换） |
| 忽略节点世界变换 | C2 红（`got=1 want=2`） |
| 静默接受 LINES 图元 | D1 红 |

### 夹具踩的坑（记下来，别再栽）

写测试夹具时用固定 `char head[256]` + `snprintf` 拼 JSON，`data:` URI 的 base64
有几百字节 ⇒ **被静默截断**，症状是 `cgltf parse failed: invalid JSON`，看起来像
cgltf 的 bug，其实是夹具自己的。改用 `std::string` 拼接。

更早的一个：JSON 里 `buffers` 数组忘了写 `]`，三个夹具一起报 `invalid JSON`。

⇒ **"解析失败"类的报错要先怀疑自己的夹具，再怀疑解析器。**

## 7. 遗留

| 项 | 说明 |
| --- | --- |
| 蒙皮 / 材质 / 贴图 / 动画 | glTF 里这些不导入。蒙皮已显式报错，材质/贴图目前**静默忽略**（不产生错误几何，但也不生成材质槽）—— 真要支持得先定材质槽的映射规则 |
| 多图元 → 子网格 | 现在所有 primitive 拼进同一个 `vertex_buffer` / `index_buffer`，没有子网格信息（与 OBJ 导入一致）。需要多材质时要改 `MeshData` 的表结构 |
| 顶点去重 | 不做（与 OBJ 导入一致，一角一顶点） |
