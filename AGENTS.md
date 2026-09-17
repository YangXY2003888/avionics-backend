# 开发规范（AGENTS.md）

本文件是本仓库的**强制开发规范**。任何自动化代理或开发者在本仓库工作都必须遵守。
进度与阶段状态见 [PLAN.md](PLAN.md)。

## 0. 项目定位

本仓库是 C++20 的机载总线监控后端（源码在 `avionics-backend/`）。当前目标包括：为一个
Python 研究项目提供**稳定的数据来源**，接口规范见 `avionics-backend/docs/interface-for-analysis.md`。

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
