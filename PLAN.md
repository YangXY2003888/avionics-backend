# 开发计划与进度（PLAN.md）

> 规范见 [AGENTS.md](AGENTS.md)。**每个阶段结束必须提交 git，并在本文件更新状态**。
> 状态取值：`未开始` / `进行中` / `已完成` / `存在问题`。

## 项目目标

C++20 机载总线监控后端。当前阶段目标（贴合性改造）：**成为下游 Python 研究项目的稳定数据来源**——
只做加法，不扩展研究性功能，不改既有行为语义。

## 阶段总览

| 阶段 | 内容 | 状态 |
|---|---|---|
| 0 | 基础后端：插件/热替换、原始归档与回放、配置化解码、时间质量、关联分析 | 已完成（上游） |
| 1 | 多协议运行时：抽象接口落地、自动识别、路由、解析、管线解码 | 已完成 |
| 2 | 具体总线：ARINC 429、MIL-STD-1553B、CAN 2.0、抓包 CAN(canlog) | 已完成 |
| 3 | 数据质量：重排缓冲、冗余去重、多源统计融合 | 已完成 |
| 4 | 真实数据贯通：实车 CAN(OBD-II) 日志导入 → 识别 → 解码 → 分析 | 已完成 |
| 5 | **贴合性改造：面向分析的扁平导出（A1–A9）** | **已完成（含 2 处待确认）** |
| 6 | 下游对接与按需扩展（DBC 信号、更多样例场景等） | 未开始 |

---

## 阶段 5 详细（本轮 · A 必做项）

交付入口：新增命令 `bus_export`；产物 `parameters_flat.csv/jsonl` + `export_metadata.json` + `configuration.ini`。

| 项 | 内容 | 状态 | 关键文件 |
|---|---|---|---|
| A1 | 扁平导出字段（26 列，同名复用） | 已完成 | `include/core/flat_export.hpp`、`src/export/flat_export.cpp`、`include/core/types.hpp` |
| A2 | 状态判定 `ok/expected_absent/invalid/not_due`（+`unknown_schedule`、`off_schedule`） | 已完成 | `src/export/flat_export.cpp`、`tests/flat_export.cpp` |
| A3 | 字典新增申报周期键 | 已完成 | `include/core/configuration.hpp`、`src/decoder/configuration.cpp` |
| A4 | 新增导出命令 `bus_export` | 已完成 | `src/export/export_main.cpp`、`CMakeLists.txt` |
| A5 | 接口文档 | 已完成 | `avionics-backend/docs/interface-for-analysis.md` |
| A6 | 样例与 `read_flat.py` | 已完成 | `avionics-backend/config/flat-demo.ini`、`avionics-backend/examples/read_flat.py` |
| A7 | 运行元数据 | 已完成 | `export_metadata.json`（run_id/session_id/config_version/counts/时间范围） |
| A8 | 测试 | 已完成 | `tests/flat_export.cpp`（覆盖 5 种状态 + off_schedule + 时间单调） |
| A9 | 样例插件键（可选） | 已完成 | `plugins/simulated/*`、`plugins/random/*` |

### A3 新增字典键
`nominal_period_ns`、`phase_offset_ns`、`epoch_ns`、`jitter_tolerance_ns`、`arrival_delay_tolerance_ns`
（均为纳秒；缺省 0 表示未知，不猜测）。

### A1 字段与时间语义
- 26 列：`run_id, session_id, source_id, channel, protocol_id, parameter_id, unit, value_type, value_num,
  value_str, validity, status, observation_time_ns, available_time_ns, ingest_time_ns, clock_group, offset_ns,
  uncertainty_ns, nominal_period_ns, sequence, sequence_step, raw_record_index, origin_source, origin_generation,
  decoder_version, config_version`。
- 额外第 27 列 `flags`（承载 `off_schedule`，见“遗留问题 1”）。
- 时间语义：`observation_time_ns ≤ ingest_time_ns ≤ available_time_ns`。
  - `available_time_ns`：在线=处理/写出行时刻；离线=以 `ingest` 为代理值（按确认②）。

### A2 状态规则（摘要）
`E_k = t0 + phase + k·P`，`Tol = jitter + uncertainty + arrival`，水位 `W` 按时钟组推进。
`E_k+Tol<W` 到期：匹配有效观测→`ok`；匹配无效观测→`invalid`；无→`expected_absent`。
`E_k+Tol≥W`→`not_due`。`P=0`→只输出 `unknown_schedule`。到期槽位每槽一行（值空），观测行照常导出；
观测早于 `t0` 且 `ok` 时追加 `flags=off_schedule`。

### A9 新增插件键（默认不配置时行为不变）
`period_ms, phase_ms, jitter_ms, loss_prob, valid_prob, arrival_delay_ms`，
并显式标注“仅用于贯通链路的样例数据，不得用于研究结论”。

---

## 验证记录

构建/测试（7/7）：
```
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-mingw.ps1 -RunTests -MingwBin "C:\tools\mingw64\bin"
=> 100% tests passed, 0 failed out of 7
```
最小闭环（记录 → 导出 → 读取）：
```
bus_analyze --fixture runs\export_demo.avbus
bus_export config\correlation-demo.ini runs\export_demo.avbus runs\export_demo_out
python examples\read_flat.py runs\export_demo_out
=> rows=17; status: unknown_schedule=17（该配置未申报周期，符合规则 1）
```
带周期 + 丢帧样例（A9 生成）：
```
bus_backend --interactive runs\flat_demo.avbus config\flat-demo.ini   # add src ... period_ms=10;loss_prob=20
bus_export config\flat-demo.ini runs\flat_demo.avbus runs\flat_demo_out
=> records=70 samples=70 ok=139 invalid=0 expected_absent=76 not_due=0
read_flat.py: rows=215; status: expected_absent=76, ok=139
```
桌面演示（快捷方式“机载总线后端演示”）：
```
选项1 设计样例 => records=17 results=5
选项2 随机实时 => records=768 usable=768 rejected=0 results=198
```
旧格式：`parameters.jsonl` / `events.jsonl` 未改动。

---

## 遗留问题 / 待确认

1. **`flags` 列超出 A1**：规则 3 要求给“额外观测”加 `off_schedule` 标记，但 A1 26 列无处承载。
   已作为**第 27 列**追加在末尾，不影响原列顺序；**待确认**是否采用该方式。
2. **行为变化**：未申报周期的参数，其观测行状态由最小闭环时的 `ok` 变为 `unknown_schedule`
   （规则 1 的必然结果）。`parameters.jsonl`/`events.jsonl` 不受影响。
3. **未做（可选）**：`bus_analyze --export-flat` 便捷别名与“逐字节等价”测试断言。
4. **`available_time_ns` 离线取代理值**（= `ingest`），已在文档注明为保守上界。
5. **本地演示件未入库**：`avionics-backend/presentation/*` 与 `run-full-demo.bat` 属本地演示，
   未纳入版本库（如需入库请提出）。
6. **环境依赖**：构建需关闭 Windows“智能应用控制”；该开关**不可逆**（关闭后需重装系统才能再开）。
7. A2 到期槽位与被满足观测会各出一行（可能同值双行），已按规则 4 处理并写入文档。

## 阶段 6（未开始）

- 下游 Python 侧对接：按 `interface-for-analysis.md` 读取并校验字段/状态。
- 按需扩展真实信号（DBC）、更多样例场景；仍遵守 AGENTS.md 的“只增不改”与禁止项。
