# 变异 harness 的基线欠账（决策：B，先不动）

> 2026-11-04 登记。**结论：暂不修改其余 harness，等基线恢复干净。**
> 本文只登记事实与恢复条件，不包含任何已执行的构建/验证结果 —— 编译与验证由用户执行。

## 1. 结论与清单

| 项 | 结论 |
| --- | --- |
| 决策 | **B**：不改其余 47 个 harness，也不碰 `AX14/AX15/AX16/AW1` |
| 当前影响面 | **47 / 48** 个 `.mutate*.py` 的结论不可信（基线脏时） |
| 已修 | 仅 `.mutate_ec.py`（slice 59 专用） |
| 恢复条件 | 那 4 条 `AX/AW` 失败转绿 ⇒ `UnityParitySmokeTest.exe` rc = 0 ⇒ 47 个自动恢复有效 |
| 恢复后仍欠 | d.ts 快照保护、Windows 超时护栏（见 §5） |
| 分工 | 代码/文档由 AI 写；**编译、测试、变异运行一律由用户执行** |

⚠️ **更正上一轮的数字**：此前口头报的「35/48 是 rc-based」是粗筛低估（条件写成了
`run(<EXE>)` 或 `rc.*== 0` 的字面匹配，漏掉了 `run(A) | run(B) == 0` 这类写法）。
按「含 `NOT CAUGHT` 且**不含** `FAIL_RE`」重新精确统计：**47 个 rc-based，1 个已修**。

## 2. 环境事实

`UnityParitySmokeTest.exe` 当前基线：`3517 checks, 4 failures`，4 条恒为：

| FAIL id | 门 |
| --- | --- |
| `AX14` | the engine authors the probe asset |
| `AX15` | and it is on disk |
| `AX16` | Resources.Load reads the asset back |
| `AW1` | a synthetic wav clip exists on disk |

来源在 `UnityParitySmokeTest.cpp` 约 8771 / 8773 / 8777 / 8404 行，属另一个会话的
Resources probe / 合成 wav 切片，与 Joint 无关。**48 个 harness 全部跑这个 exe。**

## 3. 为什么 rc-based 在脏基线上必然假绿

```
基线 rc = 1（4 条已知失败）
判据：if rc_parity == 0 and rc_comp == 0: "NOT CAUGHT"  else: "caught"
  ⇒ rc 恒为 1 ⇒ 任何变异体（含 no-op、含编译后行为完全没变的）都被报成 caught
```

这是**逻辑上的必然**，不是概率问题。所以脏基线期间的任何「N/N killed」都不构成覆盖率证据。
正确判据是「有没有冒出基线集合里没有的 FAIL id」，参考实现见 §6。

## 4. 恢复条件与验收判据（由用户执行）

| 步骤 | 命令 / 观察点 | 通过条件 |
| --- | --- | --- |
| 1 | 跑 `UnityParitySmokeTest.exe` | 汇总行 `0 failures`，且**没有** `AX14/AX15/AX16/AW1` |
| 2 | 跑 `ScriptComponentSmokeTest.exe` | 失败 0 项 |
| 3 | 抽查任一旧 harness 的**基线**（未变异源码） | 输出里出现 `>>> NOT CAUGHT <<<` 或等价的"基线通过"信号 |
| 4 | 再跑该 harness 的一轮变异 | 至少有一个变异体被判 `NOT CAUGHT`（等价/未被覆盖），或全 killed 但每条都能对上专属 FAIL id |

⚠️ 第 3 步是关键：**基线自身必须被判"没被抓住"**。若基线仍被报成 caught，说明 exe 还是脏的，
后面所有结论继续作废。

## 5. 基线修绿之后仍然欠的两项保护

这两项与基线干净与否无关，属于 harness 自身的可变状态管理，B 方案暂不补：

| 欠账 | 症状 | 后果 |
| --- | --- | --- |
| **d.ts 是第二份可变状态** | 改脚本发布面的变异体 `regen_dts()` 把**变异后**的 `zengine.d.ts` 留在盘上；后续 `NEED_DTS=False` 的变异体不刷新 | `EC40a/EC41/EC42/EC46` 成片恒红 ⇒ **存活**的变异体被伪装成 caught |
| **Windows `subprocess.run(timeout=N)` 超时不生效** | 超时后 kill 直接子进程，再 `communicate()` 一次；孙进程继承 stdout 句柄时这次调用永不返回 | 单个变异体可挂十几分钟（实测 ECM20 卡 19 分钟），且超时不算失败 |

修法要点（照抄 `.mutate_ec.py` 即可，**不要"重跑生成器"还原 d.ts** —— 那个 exe 是用变异后
头文件编出来的，非 DTS 变异体不会重建它，越刷越脏）：

- 基线时快照三份 d.ts 的**字节**，每个变异体 `finally`（含 SIGTERM 处理器）逐字节写回。
- 用 `Popen` + `communicate(timeout)`；超时后 `kill() + wait()`，**不再碰管道**；超时判
  `harness error (hung)` 且**不计入 killed**。

## 6. 参考实现索引（`.mutate_ec.py`）

行号会随编辑漂移，**以函数名为准**。

| 函数 / 常量 | 当前行 | 作用 |
| --- | --- | --- |
| `BASELINE` / `FAIL_RE` | 74 / 75 | 基线 FAIL-id 集合与提取正则 |
| `DTS_CLEAN` | 81 | 三份 d.ts 的干净字节 |
| `snapshot_dts_clean()` / `restore_dts_clean()` | 167 / 175 | 快照与写回 |
| `_capture(argv, cwd, timeout)` | 228 | 不会挂死的带超时执行 |
| `run(exe, baseline=None)` | 248 | 返回 `(rc, fail_ids, has_summary, new_ids)` |
| `build_and_run()` | 353 | 只认**新增** FAIL id 的判据 |
| `lock_is_stale()` | 571 | 陈旧锁检测（PID 存在性 + 6 小时年龄兜底） |
| `main()` 里 `=== baseline (clean sources) ===` | 641 | 基线采集 + 连跑两次的稳定性自检（顺带当 no-op 自检） |

## 7. 受影响文件清单（47 个，rc-based）

```
.mutate_ac.py    .mutate_am.py    .mutate_anim.py  .mutate_au.py    .mutate_bc.py
.mutate_cc.py    .mutate_cjoint.py .mutate_cl.py   .mutate_cm.py    .mutate_co.py
.mutate_col.py   .mutate_config.py .mutate_cp.py   .mutate_dg.py    .mutate_fr.py
.mutate_fx.py    .mutate_gz.py    .mutate_ip.py   .mutate_iv.py    .mutate_joints.py
.mutate_lm.py    .mutate_lu.py    .mutate_me.py   .mutate_mesh.py  .mutate_mf.py
.mutate_ml.py    .mutate_na.py    .mutate_nc.py   .mutate_ol.py    .mutate_pf.py
.mutate_pl.py    .mutate_pm.py    .mutate_pp.py   .mutate_qe.py    .mutate_rb.py
.mutate_rbody.py .mutate_re.py    .mutate_rv.py   .mutate_sc.py    .mutate_sd.py
.mutate_sf.py    .mutate_tc.py    .mutate_te.py   .mutate_vec.py   .mutate_vq.py
.mutate_vt.py    .mutate_ww.py
```

复算命令（只读，不构建）：

```bash
cd /e/Engine/ZEngine
for f in .mutate*.py; do
  if grep -q "NOT CAUGHT" "$f" && ! grep -q "FAIL_RE" "$f"; then echo "$f"; fi
done | wc -l
```

## 8. 待验证项 / 未完成

- `AX14/AX15/AX16/AW1` 何时转绿，取决于另一个会话；本文**不假设**时间。
- 基线转绿后，§4 的四步验收**尚未执行**（按分工由用户执行）。
- §5 的两项保护在本决策下**明确不做**；若将来给旧 harness 补，需单独登记为一次改动。
- 本文只登记状态，未修改任何 harness 代码。

## 9. 本轮产出 / 未完成 / 建议下一步

| 段 | 内容 |
| --- | --- |
| 本轮产出 | 本文（B 决策、精确影响面 47/48、恢复条件、验收判据、参考实现索引、受影响清单） |
| 未完成 | 4 条 `AX/AW` 失败仍红；47 个 harness 结论仍不可信；§4 验收未跑 |
| 建议下一步 | 等基线转绿后按 §4 验收；若届时旧切片要重跑变异，再决定是否把 §5 两项保护一并补上 |
