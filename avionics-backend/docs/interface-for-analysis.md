# 面向分析的数据接口（flat-1）

本文档定义后端作为“下游 Python 研究项目稳定数据来源”的扁平导出接口。导出由独立命令 `bus_export` 产生，**不改变**原有 `parameters.jsonl` / `events.jsonl` 的行为与格式。

- 格式版本：`flat-1`（见导出目录的 `export_metadata.json` 的 `format`）
- 入口：`bus_export CONFIG_INI INPUT_ARCHIVE OUTPUT_DIRECTORY [--session-id ID] [--mode offline|online]`
- 产物：`parameters_flat.csv`、`parameters_flat.jsonl`、`export_metadata.json`、`configuration.ini`（配置副本）

## 稳定性承诺

- 字段名和含义在 `flat-1` 内**保持向后兼容**：只增字段，不改既有字段含义。
- 新增字段会提升 `format` 版本（例如 `flat-2`），并在此文档更新。
- 不导出任何研究派生量（滞后/统计特征、可信度、风险等），也不导出隐含真值。

## 字段字典

一条**观测记录**一行；参数声明了申报周期时，另外为**每个到期槽位**导出一行（值可空）。字段顺序如下：

| 字段 | 类型 | 含义 |
|---|---|---|
| `run_id` | string | 本次导出运行的标识（每次 `bus_export` 生成） |
| `session_id` | string | 采集/归档会话标识（默认取归档文件名，可用 `--session-id` 覆盖） |
| `source_id` | string | 来源标识（主机侧实例/来源名） |
| `channel` | int | 通道号 |
| `protocol_id` | int | 协议编号（程序内部标识） |
| `parameter_id` | string | 参数标识 |
| `unit` | string | 单位 |
| `value_type` | string | `int64` / `uint64` / `double` / `bool` / `enum`；槽位行为空 |
| `value_num` | number | 数值（`bool` 为 0/1，`enum` 为码值）；空值表示无观测 |
| `value_str` | string | 文本值（`enum` 标签、`bool` 的 true/false）；其余为空 |
| `validity` | bool | 该条载荷解析是否有效；槽位行为空 |
| `status` | string | 观测/槽位状态，见下节 |
| `observation_time_ns` | int | **采集/观测时刻**。槽位行为该槽位的名义时刻 |
| `available_time_ns` | int | 该数据**首次可被下游使用**的时刻，是包含处理延迟的保守上界 |
| `ingest_time_ns` | int | 宿主**接收**该数据的时刻 |
| `clock_group` | string | 时钟组（用于时间对齐与水位） |
| `offset_ns` | int | 该来源声明的时钟偏移 |
| `uncertainty_ns` | int | 该时钟组的时间不确定度 |
| `nominal_period_ns` | int | 申报周期；`0` 表示未知 |
| `sequence` | int | 来源序号 |
| `sequence_step` | int | 期望序号步长 |
| `raw_record_index` | int | 原始归档内记录编号（跨文件见下节） |
| `origin_source` | string | 回放时保留的原始来源；非回放为空 |
| `origin_generation` | int | 回放时保留的原始代次 |
| `decoder_version` | string | 解码器/字典版本（由配置驱动时与 `config_version` 相同） |
| `config_version` | string | 配置版本；配置内容变化时必须变化 |
| `flags` | string | 附加标记，逗号分隔；目前只有 `off_schedule` |

> `flags` 是为满足“额外观测标为 ok 并标记 off_schedule”而新增的一个**附加列**（不在原始 A1 清单内），置于末尾，不影响既有列顺序。

## 时间语义

```
observation_time_ns ≤ ingest_time_ns ≤ available_time_ns
```

- `observation_time_ns`：采集/观测时刻（`RawFrame.capture_time_ns`）。
- `ingest_time_ns`：宿主接收时刻（`RawFrame.ingest_time_ns`）。
- `available_time_ns`：
  - `--mode online`：处理/写出行时刻（含处理延迟的上界）。
  - `--mode offline`（默认）：以归档中的 `ingest` 作为**代理值**。
  - 回放模拟在线时，用回放产生的**新接收时刻**作为 `ingest`，并保留原 `observation_time`；该行以 `mode` 与 `session_id` 标明属于“回放在线模式”。`export_metadata.json` 的 `mode` 字段说明本次为 `offline` 还是 `online`。

导出保证上述不等式成立；若上游时间倒退，导出取三者最大值以保证单调。

## 状态判定规则

对声明了申报周期 `nominal_period_ns = P > 0` 的参数：

- 锚点 `t0 = epoch_ns`（若声明），否则取该参数**首次观测**的 `observation_time`。
- 槽位 `E_k = t0 + phase_offset_ns + k · P`，`k = 0,1,2,…`。
- 容差 `Tol = jitter_tolerance_ns + uncertainty_ns + arrival_delay_tolerance_ns`。
- 处理水位 `W` = 该时钟组内已见 `observation_time` 的最大值。

对每个槽位 `E_k ≤ W`：

- `E_k + Tol < W`（已到期）：
  - 有有效观测且 `|observation_time − E_k| ≤ P/2` → `ok`
  - 有观测但无效位/解析失败 → `invalid`
  - 否则 → `expected_absent`
- `E_k + Tol ≥ W` → `not_due`（容差窗口尚未关闭，不提前判定缺失）

观测行状态：

- 周期未知（`P = 0`）→ `unknown_schedule`，**不**产生 `expected_absent`/`not_due`。
- 周期已知且无效 → `invalid`。
- 周期已知且有效 → `ok`；若该观测不在任何槽位的 `P/2` 范围内（即早于 `t0`）→ 追加 `flags=off_schedule`。

说明：

1. “没有申报周期不得输出 `not_due`/`expected_absent`”，只能 `unknown_schedule`。
2. `expected_absent` 必须等水位越过 `E_k + Tol` 才判定，不因导出时刻未到而提前判缺失。
3. 到期槽位**每槽一行**（值可空），原始观测行照常导出；因此一个被满足的槽位会有两行（一条观测 + 一条 `ok` 槽位）。
4. 槽位行不携带值：`value_*` 与 `validity` 为空，时间列取该槽位名义时刻 `E_k`。

## 假设与限制

- 本规则**假设申报周期确实是周期性的**；不适用于变间隔、事件触发或半批次过程。
- `t0` 缺省时以首次观测为锚，因此 `P` 必须真实反映设备更新周期，否则会产生虚假的 `expected_absent`。
- 每参数只使用一个申明周期；不支持周期变化分段。
- 时间水位按**时钟组**推进，跨时钟组不混合判定。

## 跨文件 / 会话引用

`raw_record_index` 只在**单个归档内**唯一。跨文件或跨会话定位原始记录，请使用组合键：

```
(session_id, raw_record_index)
```

回放记录还需带上 `origin_source` / `origin_generation`。回放参数来源为 `回放实例/原来源`，`origin_generation` 为原采集代次。

## 示例

```powershell
bus_analyze --fixture runs\demo.avbus
bus_export config\correlation-demo.ini runs\demo.avbus runs\demo_export
python examples\read_flat.py runs\demo_export
```

`examples/read_flat.py` 仅用标准库读取 CSV/JSONL，打印字段清单与状态计数，可作为下游解析起点。

## 已知边界（不属于本接口）

- 不提供统计特征、滞后特征、关联可信度、证据不足判定、风险-覆盖率、预测模型。
- 不提供真实动态过程仿真或隐含真值。
- 不提供数据库、HTTP、可视化、多进程。
