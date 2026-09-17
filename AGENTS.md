# 开发规范（AGENTS.md）

本文件是本仓库的**强制开发规范**。任何自动化代理或开发者在本仓库工作都必须遵守。
进度与阶段状态见 [PLAN.md](PLAN.md)。

## 0. 项目定位

本仓库是 C++20 的机载总线监控后端（源码在 `avionics-backend/`）。当前目标包括：为一个
Python 研究项目提供**稳定的数据来源**，接口规范见 `avionics-backend/docs/interface-for-analysis.md`。

## 0.1 文件结构（入库）

仓库根 `avionics-backend/`；项目本体在其子目录 `avionics-backend/`（以下相对项目目录）：

```text
avionics-backend/
├─ CMakeLists.txt                 构建目标：bus_core、插件、程序、测试
├─ README.md                      项目说明（构建、运行、配置、接口入口）
├─ .gitignore                     忽略 build*/、runs/、snapshots/、*.log、本地演示件
├─ config/                        配置字典与规则（INI）
│   ├─ correlation-demo.ini       关联分析示例字典
│   ├─ flat-demo.ini              扁平导出示例（含申报周期 nominal_period_ns）
│   ├─ obd-can.ini                真实车辆 OBD-II 字典（公开标准编码，非厂家 ICD）
│   └─ simulated.example.txt      模拟插件配置示例
├─ docs/                          设计与接口文档
│   ├─ architecture.md            模块结构与生命周期
│   ├─ protocol-abstractions.md   协议抽象接口契约
│   ├─ protocol-runtime.md        协议识别/路由/解析实现
│   ├─ processing-v0.2.md         参数、时间质量、规则语义
│   ├─ interface-for-analysis.md  面向分析的数据接口（flat-1，字段/时间/状态）
│   ├─ plugin-contract.md         插件开发约定
│   └─ verification*.md|json      验证记录
├─ examples/
│   └─ read_flat.py               读取扁平导出（仅标准库）
├─ include/
│   ├─ core/                      核心接口与数据类型（见下）
│   └─ plugin_api/                C 插件 ABI（bus_plugin.h）与插件侧辅助
├─ plugins/                       采集插件（编译为动态库）
│   ├─ simulated/                 模拟数据源
│   ├─ random/                    随机/场景数据源
│   └─ replay/                    归档回放
├─ scripts/
│   ├─ build-mingw.ps1            一键构建 + CTest
│   └─ verify_live_roundtrip.py   在线/离线一致性验收脚本
├─ src/
│   ├─ main.cpp                   bus_backend 入口
│   ├─ analyze_main.cpp           bus_analyze 入口
│   ├─ bus/                       实例与生命周期（BusManager）
│   ├─ plugin/                    插件加载与 C ABI 适配
│   ├─ platform/                  动态库加载（Windows/POSIX）
│   ├─ pipeline/                  有界接收队列与处理线程（FramePipeline）
│   ├─ decoder/                   字典解析、位提取、解码（Dictionary/Simulated）
│   ├─ analysis/                  时间质量、一致性、响应、重排/去重/融合
│   ├─ protocol/                  协议识别/路由/解析、消息解码、bus_protocol 入口
│   ├─ export/                    扁平导出（flat_export）与 bus_export 入口
│   └─ storage/                   原始归档（AVBUS）与 JSONL 输出
└─ tests/                         CTest：integration / processing / protocol_* / flat_export
```

`include/core/` 关键头文件：

| 头文件 | 职责 |
|---|---|
| `types.hpp` | `RawFrame`、`ParameterSample`、`ParameterValue` 等基础类型 |
| `configuration.hpp` | 字典/时钟/规则/重排/去重/融合定义与 `BackendConfiguration::load` |
| `processing.hpp` | `TimeQualityProcessor`、`ProcessingService`、`IAnalysisSink` |
| `frame_pipeline.hpp` | `FramePipeline`、`IParameterDecoder`、`IParameterConsumer` |
| `archive.hpp` | `ArchiveWriter/Reader`（小端、逐记录 CRC32） |
| `decoder_registry.hpp` | 按协议编号分派解码器 |
| `protocol_types.hpp` / `protocol_interfaces.hpp` / `protocol_impl.hpp` | 协议类型、抽象接口、内置实现与工厂/路由器声明 |
| `flat_export.hpp` | 面向分析的扁平导出（`bus_export`） |
| `bitfield.hpp` / `field_value.hpp` | 位提取与字段值解码（供解码层复用） |
| `analysis.hpp` / `demo_fixture.hpp` | 示例分析器与确定性样例数据 |

**新增代码放置规则**：
- 新协议：改 `src/protocol/protocols.cpp` 与 `include/core/protocol_impl.hpp`，加 `tests/protocol_runtime.cpp` 用例。
- 新分析规则：改 `src/analysis/processing.cpp`、`include/core/configuration.hpp`、`src/decoder/configuration.cpp`，加 `tests/processing.cpp` 用例。
- 新导出字段/状态：改 `src/export/flat_export.cpp` 与 `docs/interface-for-analysis.md`，加 `tests/flat_export.cpp` 用例；**只增不改**。
- 新插件：新增 `plugins/<name>/`，在 `CMakeLists.txt` 用 `add_bus_plugin` 注册，并加演示/测试。
- 新配置键：改 `include/core/configuration.hpp` 与 `src/decoder/configuration.cpp`，严格校验、缺省不猜测。
- 新命令行程序：新增 `src/<area>/*_main.cpp`，在 `CMakeLists.txt` 注册可执行目标与测试。

**不纳入版本库**：`build-gcc-debug/`、`runs/`、`snapshots/`、`*.log`，以及本地演示件
`presentation/`、`run-full-demo.bat`（见 `.gitignore`）。

## 1. 提交规范（强制）

- **每个阶段结束必须提交 git**，不允许把多个阶段的改动堆在一起未提交。
- 提交前：先 `git status`；只 stage 本次相关文件；**不提交**构建产物、`runs/`、临时文件、密钥。
- commit message 沿用仓库风格：英文、简短、祈使句；一条提交只做一件事。
- 提交后：在 `PLAN.md` 中更新该阶段状态（已完成 / 存在问题 / 进行中）与验证记录，并提交（可与阶段提交同批）。
- 禁止：force push、改写公共历史、提交密钥、跳过 hooks、交互式 `-i`。

## 2. 计划文件规范（PLAN.md）

- `PLAN.md` 是本仓库**唯一的进度来源**。
- 每阶段必须更新：该阶段状态、改动文件、验证命令与输出、遗留问题。
- 状态取值：`未开始` / `进行中` / `已完成` / `存在问题`。
- 需求或范围变化时，先更新计划，再动手。

## 3. 构建与测试（Windows / MinGW）

```powershell
$env:PATH = "C:\tools\mingw64\bin;C:\tools\cmake\bin;C:\tools\ninja;" + $env:PATH
cd C:\Users\31763\Desktop\avionics-backend\avionics-backend
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-mingw.ps1 -RunTests -MingwBin "C:\tools\mingw64\bin"
```

- 产物在 `build-gcc-debug/bin/`。
- **必须关闭 Windows“智能应用控制”**，否则 MinGW 的 `gcc.exe`/`cc1.exe` 会被系统拦截。
- 若机器路径带非 ASCII/空格，构建脚本会用临时盘符映射提供纯 ASCII 构建路径。

## 4. 代码规范

- 语言：C++20；**尽量不引入新依赖**（当前仅用 C++ 标准库、系统线程与动态库接口）。
- **只增不改**：优先新增字段 / 文件 / 命令；不改既有字段语义，不改既有输出格式。
- **不删除**现有采集、解析、回放、质量处理能力。
- 命名沿用现有风格：类型 `PascalCase`，成员变量结尾下划线，函数 `camelCase`。
- 头文件用 `#pragma once`；模块接口放 `include/core/`，实现放 `src/`。
- 新增能力必须同时给出单元/集成测试。
- 配置解析必须**严格**：未知键报错；缺省值不得被猜测（例如未申报的周期按 0/未知处理）。

## 5. 测试规范

- `scripts/build-mingw.ps1 -RunTests` **必须全绿**（当前 7 项）。
- **不得为通过测试而放宽断言**；行为变更须显式记录，并同步更新测试预期与文档。

## 6. 禁止项

- 不实现：统计特征、滞后特征、关联可信度、证据不足判定、风险-覆盖率、预测模型。
- 不实现：真实动态系统仿真（含隐含真值）。
- 不重构、不改既有行为语义、不加数据库 / HTTP / 可视化 / 多进程。
- 样例数据必须显式标注：**“仅用于贯通链路的样例数据，不得用于研究结论”**。

## 7. 接口稳定性

- 面向分析的接口以 `docs/interface-for-analysis.md` 为准（当前 `flat-1`）。
- 字段**只增不改**；不兼容变动必须提升版本号并在文档中更新承诺。

## 8. 文档规范

- 设计/接口变更同步更新 `avionics-backend/docs/` 下的对应文档与 `README.md`。
- 每阶段的验证记录写入 `PLAN.md`。
