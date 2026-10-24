# 阶段二 · 建能力（Capability Building）规划草案

> 本文档是 ZEngine "阶段二·建能力" 的**单一源**（对标阶段一 `PHASE1_FOUNDATION_PLAN.md`）。
> 阶段一（"夯地基"，稳定性/正确性审计补齐，#1–#12 全部落地）已完成，阶段二从**现有设计蓝图
> 与代码现状**里识别主线，不凭空造需求。
>
> 结论等级：沿用阶段一的 **Blocker / High / Medium / Low**（此处表示"作为 Phase-2 主线的优先级
> 与风险"，而非审计严重度）。
> 环境事实：阶段一已把四个 RHI 后端（Vulkan/D3D12/Metal/WebGL2）在各自目标上编译链接打通；
> 阶段二**每一步仍以"可验证、风险可控、能力可见"为纲**，但允许包含较大的渲染/内容管线能力。
>
> ⚠️ 本文档为**草案**（2026-10-25 只读盘点产出，尚未动手）。动手前每个子项必须回读对应文件
> 确认现状未变，并在此追加"落地记录"。

---

## 0. 盘点方法与环境事实（重要）

规划前做了一次全量只读盘点（`doc/` 87 篇设计文档 × `engine/Source` 代码 grep 交叉验证），
三条必须先记录在案的事实，否则会基于错误前提排期：

1. **多篇"系统设计文档"是过时的概念稿，代码远比文档先进**。`doc/nanite/NANITE_SYSTEM_DESIGN.md`、
   `doc/lumen/LUMEN_SYSTEM_DESIGN.md` 是纯散文、无状态标记，但代码各约 4000+ LOC，已有 CVar
   （`r.Nanite.*`/`r.Lumen.*`）、管线挂钩（`RenderPipelineBase::UpdateNanite/UpdateLumen`）、
   真 GPU dispatch（`nanite_cluster_cull.comp` + `.spv`）与 smoke test。**规划要看代码，不要看这两篇
   文档**，否则工作量会估错一个数量级。同理 `doc/asset_management/` 两篇 40KB/34KB 大设计稿自标
   "**早期设计草稿（pre-Route-B）**"。

2. **大量子系统"已实现"**：World Partition + LWC、序列化（YAML 对象图）、Prefab、Undo/Redo、
   DDC/LMDB 缓存、MegaLights、Preload/纹理流送、EASTL、Bindless 纹理、Apple/Metal 验收、
   Package Manager、GPU Spine（活跃主线）—— 这些**不需进 Phase 2**（个别有收尾 follow-up，见 §2）。

3. **可行动 TODO 高度集中**：`engine/Source` 约 152 处 TODO/FIXME（剔除 vendored `zstb_truetype.h`
   13 处后约 139 处），**~80% 集中在 Nanite + Lumen + VirtualTexture + RHI 后端**——且都是各系统
   最硬的那一半（GPU 化/流送/HWRT），这正是它们此前没做完的原因。

---

## 1. Phase-2 候选总表（按推荐优先级）

> 评级 = 作为 Phase-2 主线的价值/风险。★★★ 首选，★★ 主力，★ 填缝/收尾，✗ 不建议进。

| # | 候选 | 主题 | 成熟度 | 依赖 | 评级 |
|---|------|------|--------|------|------|
| C0 | **Vulkan `MainCameraPass` 崩溃根因修复** | 跨切面阻塞：解除 Vulkan 侧验证能力 | **✅ 已被 `7e5d2521` 修掉（2026-10-25 复核关闭）** | — | ★★★ 已关闭 |
| C1 | **RenderGraph 接管流水线** | 帧图编译器：自动屏障、瞬态 RT 别名、剔除 | **已实现·默认关闭**（165 编译+92 执行断言；`ZENGINE_RG_RP2=1` 门控，默认 off） | 阶段一 RHI 注册表 | ★★★ 高价值/低风险 |
| C2 | **RenderGraph → RP1**（`render_pass_refactor`） | RP1 三子pass 拆成 3 独立 pass，纳入图管理 | **仅提案**（doc 无落地标记） | **必须在 C1 之后** | ★★ 高价值/中风险 |
| C3 | **Nanite 收尾** | 流送 → 几何压缩 → HiZ 遮挡 | **部分实现**（真 GPU dispatch 已有；4 处 TODO=最硬的一半） | C1/C2 给其 RG 管理的目标 | ★★★ 高价值/中风险 |
| C4 | **Virtual Texture 接线** | 分页纹理虚拟化，从"孤儿"到可用 | **已实现但零消费者**（6 文件无外部引用、无 CVar、3 处构建排除） | C1/C2 的 RG 资源注册 | ★★ 高价值/中高风险 |
| C5 | ~~**Vulkan `MainCameraPass` 崩溃根因修复**~~ | 跨切面阻塞：解除 Vulkan 侧验证能力 | **✅ 已被 `7e5d2521` 修掉（2026-10-25 复核关闭）** | — | ★★★ 已关闭（见 §2 step 2.0） |
| C6 | **Lumen 收尾（SSR → GPU 化）** | 动态 GI + 反射；取 SSR 一半 + 首个 Lumen shader | **部分实现**（CPU 探针网格已通；**无任何 Lumen shader**） | C2 的 GBuffer/depth 声明 | ★★ 中价值/高风险（放最后） |
| C7 | **编辑器/内容管线填充**（低风险稳态轨） | 平台窗口化收尾 / Timeline 四片段 / Prefab §9.4 | 各为**部分实现·机械收尾** | — | ★★ 低风险填缝 |
| — | **RHI 胖接口拆分**（阶段一 #9 遗留） | 124 纯虚 `RHI.h` 拆成职责子接口 | **未动手**（阶段一明确后置） | 触碰 4 后端 + 所有调用点 | ✗ **不建议进 Phase 2**（见 §3） |

---

## 2. 建议落地顺序（每步可验证、风险可控、能力可见）

原则继承自阶段一（`PHASE1_FOUNDATION_PLAN.md:49-53`）：先低风险高杠杆、每步能本地跑通；
阶段二额外要求"能力可见"—— 每个里程碑都应带来可演示的渲染/内容能力。

| 步骤 | 对应候选 | 子项 | 为什么排这里 |
|------|----------|------|--------------|
| **2.0**（✅ 已关闭） | C0 | Vulkan `MainCameraPass::SetupModelGlobalDescriptorSet` 崩溃根因修复 | **已被 `7e5d2521`（2026-10-22）修掉**，2026-10-25 复核确认关闭。根因：MegaLights 默认关闭时 binding 8..12 提交空资源 → `UpdateDescriptorSets` 解引用空指针；修复加判空早退 + RHI 跳过空 write。两篇文档（`CROSS_PLATFORM_PIPELINE.md` §5、`TEXTURE_COOK_PIPELINE.md` §8）写于 2026-06-02、早于修复，已加"已修复"标注。**Vulkan 验证通路已恢复，无需再动手**。 |
| **2.1** | C1 | RenderGraph RP2 目视验证 → 默认打开 | 图与执行层已有 257 项断言；失败模式是"后处理不对"而非黑屏（`RENDER_GRAPH.md:136`）；一个环境变量可回滚。是所有后续渲染候选的地基。 |
| **2.2** | C1 | RenderGraph 扩面（ParticlePass / 后处理链） | 复用已验证机制；消除 `RENDER_GRAPH.md:15` 列举的手写屏障。 |
| **2.3** | C2 | `render_pass_refactor`：RP1 三分 | **必须在 2.1 之后**——此时屏障/别名机制已在真机验证过，单独做它是高风险。 |
| **2.4** | C2 | RenderGraph 接管 RP1 | 2.3 的直接收益兑现；解锁 Nanite/Lumen/VT 的图管理资源。 |
| **2.5** | C3 | Nanite：streaming → 几何压缩 → HiZ 遮挡 | 三个子项风险递增、各自独立可验（streaming/压缩是纯 CPU 数据、可 headless 验；HiZ 是 GPU 侧需真机）；已有真 GPU dispatch 作基线。 |
| **2.6** | C4 | VirtualTexture 接线（CVar 门控） | 孤岛子系统，不会回归现有路径；需要 2.4 的 RG 资源注册。9 处真 TODO（页上传/LRU 淘汰/页表更新）。 |
| **2.7** | C6 | Lumen：SSR + 首个 Lumen shader | 依赖 2.4 的 GBuffer/depth 声明；放最后，**只取 SSR 一半**。**HWRT/BLAS-TLAS 明确移出 Phase 2**（需独立专项）。 |
| **穿插** | C7 | 平台窗口化收尾 / Timeline 四类片段 / Prefab §9.4 follow-ups | 渲染项卡在真机验证时用来保持节奏；每项都可独立编译演示，低风险。 |

---

## 3. 关于阶段一 #9（RHI 胖接口拆分）—— 建议**不**进 Phase 2

阶段一已正确论证并后置（`PHASE1_FOUNDATION_PLAN.md:688-692, :822-824`）：`RHI.h` = **124 个纯虚、
689 行**。三条理由维持后置：

1. **痛点已被局部化解**：胖接口伤测试的根因，已被 `IRGResourceDevice`（10 方法，`RENDER_GRAPH.md:192-212`）
   局部解决——"RHI 签名漂移只会撞到 adapter 一个文件"。窄接口模式可按子系统复制，不必动 `RHI.h`。
2. **成本/收益此刻倒挂**：它触碰全部 4 后端 + 每个调用点，而阶段一刚加完第 4 个后端（WebGL2）——
   刚加后端就拆分 = 最大化返工。
3. **不产生用户可见能力**：Phase 2 应买渲染/内容能力；此拆只买可维护性，且 churn 最大。

**更优做法：让它"涌现"**。每个需要窄后端切片的 Phase-2 候选（Nanite/Lumen/VT）各自定义自己的
`IRGNaniteDevice` 式接口；等积累 2–3 个后，自然接缝集是"证据驱动"而非"拍脑袋"，届时拆分机械化。
**Phase 2 末再评估**。

---

## 4. 注意事项（排期前必须知道）

1. **别信过时设计稿估工作量**：`doc/nanite/NANITE_SYSTEM_DESIGN.md`、`doc/lumen/LUMEN_SYSTEM_DESIGN.md`
   是纯概念稿、无状态标记，代码却各约 4k LOC 且已接线。**从代码规划**。
2. **Schema 演进可能已完成**：`doc/asset_management/SCHEMA_EVOLUTION_AND_TYPETREE.md:360,391` 标
   Stage 3/4 待做，但 `Core/Serialize/Test/SchemaEvolutionSmokeTest.cpp` 已存在、`TEXTURE_COOK_PIPELINE.md:83-84`
   引用其 T1/T2 覆盖 Texture2D 迁移。**排期前先核对**，别重复劳动。
3. **Vulkan `MainCameraPass` 崩溃——已确认修复，无需再动手**：两篇 doc 断言其"pre-existing"，
   复核（2026-10-25）确认已被 commit `7e5d2521`（2026-10-22，"fix(vulkan): 空描述符资源与动态
   视口不再访问违例"）修掉——正是 §5 描述的同一故障点（`SetupModelGlobalDescriptorSet` 提交
   MegaLights 空资源）。commit 实测调试器下连续跑 240s 无 AV。**step 2.0 直接关闭**，Vulkan 验证
   通路随之恢复。两篇文档已加"已修复"标注。
4. **vendored stb 污染 TODO 指标**：`Runtime/UI/Render/zstb_truetype.h` 贡献 152 处 TODO 里的 13 处，
   任何"TODO 消化率"统计应排除它。
5. **WebGL2 残留仅 2 处真标记**：`WebGL2RHI.cpp:136`（挂深度/模板 rb）、`:885`（释放全部）——
   可在 C1/C2 之后顺手清，不必单列。

---

## 5. 明确移出 Phase 2 的项

- **RHI 胖接口拆分**（见 §3）—— 让它涌现，Phase 2 末再评估。
- **Lumen HWRT / BLAS-TLAS / BVH** —— 需独立专项（Vulkan RT 扩展 + 加速结构）。
- **Mobile 真前向管线**（`CROSS_PLATFORM_PIPELINE.md` §6 首项）—— 依赖尚未存在的 lightmap baker 与
  `LightComponent` 类型枚举。
- **Material editor → 真编译** —— 需与 ShaderLab codegen 绑定，是另一个专项。
- **LLM 内存追踪窗口** —— 仅概念提案（`LlmTracker.{h,cpp}` 存在但无窗口）。
- **GAS 编辑器 UI** —— 除非 gameplay 是 Phase-2 目标，否则价值低（★）。

---

## 6. 与阶段一的衔接状态

阶段一（`PHASE1_FOUNDATION_PLAN.md`）#1–#12 全部落地，其中两项"待真机"验证已铺路（2026-10-25）：
- **#2 桌面 POSIX fork 路径 + #1/#4 Linux RHI 运行期**：本机已装 WSL（组件 3.0.3 + Ubuntu 24.04，
  待重启注册 distro）。重启后跑 `scripts/wsl_verify_linux.sh` 一次性闭环，见 §3.10 补记。
- **#12 WebGL2 浏览器运行期**：待真实浏览器跑通 `.wasm`（沙箱拦截 headless Edge）。

这两项是阶段一的收尾尾巴，与阶段二主线（C1–C7）**相互独立**——阶段二从 C5（Vulkan 崩溃复核）
与 C1（RenderGraph）起步，不依赖它们完成。
