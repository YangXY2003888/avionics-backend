# 机载总线后端 · v0.2

这是采用 C++20 面向对象设计的后端工程。支持模拟采集、动态插件管理、原始记录、配置化参数解码、时间质量处理、跨参数关联和文件回放，可在进程运行期间替换一个采集实例。

当前没有可视化、HTTP 服务或真实采集卡依赖。Windows/MinGW 的 g++ 15.2 已编译验证；POSIX 动态库加载代码已提供，Linux 构建尚未实测。

新增了面向多来源、多通道的[协议扩展抽象接口](docs/protocol-abstractions.md)：约定默认自动识别，也支持手动指定协议；通过继承接入现有 `IParameterDecoder`。在其上补齐了[协议运行时](docs/protocol-runtime.md)：两种内置协议、自动识别算法、运行时路由器、消息解码器、管线接入和命令行入口。现有运行程序默认仍按报文中已知的协议编号分派；`bus_protocol` 用于运行期检查协议识别。

## 已实现

- 版本化 C 插件接口；插件内部和核心使用 C++ 类；ABI 头文件也经过 C 编译检查。
- 一个动态库创建多个独立实例；按实例管理启停、替换和移除。
- 替换前检查插件及配置；新实例启动失败时尝试恢复旧实例，并明确返回失败。
- 停止等待线程和回调退出；队列持有宿主自己的报文副本；最后一个实例销毁后释放库句柄。
- 有界接收队列，满时拒绝最新报文；宿主和插件均记录拒绝计数。
- 原始二进制归档，明确小端格式、逐记录 CRC32、长度边界和截断检测；拒绝覆盖已有文件。
- 按协议注册解码器；统一参数值支持有符号整数、无符号整数、double、bool 和枚举。
- 模拟温度解码及阈值上穿示例，保留原始记录编号和解码版本。
- 回放插件复用采集后的同一处理链路，保留原始来源、原始代次和记录编号。
- 控制台命令用于开发调试，没有图形界面。
- INI 参数字典：按来源、协议与通道选择字段，支持位提取、大小端、整数、浮点、布尔、枚举、比例与偏置、有效位。
- 显式时钟偏移与误差范围，标记未映射时钟、重复、序号缺口、乱序和无效数据。
- 双参数一致性比较与指令上升沿—响应上升沿的延迟分析；区分超时、观测缺口、时序不确定、取消和未完成。
- 参数和分析事件输出 JSONL，保留原始报文编号及配置副本。在线配置模式和离线分析使用同一处理对象。
- 两种内置协议的探测、解析与工厂：奇校验 32 位字和 CRC-16 分帧字节流。
- 运行时路由器：默认按内容自动识别，支持按来源/通道手动指定和可信来源提示，遵守观察预算和候选上限，按来源、通道、代次和回放原来源隔离；坏帧不切换协议。
- 消息解码器和 `IProtocolPipelineDecoder` 接入，将已校验协议消息映射为工程参数；可通过 `bus_protocol` 检查路由决定。
- 可配置的随机数据源插件 `bus_random`，用 `random=0|1` 切换确定性递增值或随机值，用于演示和压力测试。

## 快速构建：当前 Windows 环境

在本项目目录执行：

```powershell
.\scripts\build-mingw.ps1 -RunTests -RunDemo
```

脚本默认使用 `C:\msys64\mingw64\bin`，从 PATH 找到 CMake、Ninja。需要 PowerShell 和 CMake 3.24+（脚本使用 `--fresh`）；项目本身要求 CMake 3.20+。

当前机器上，直接从中文绝对路径调用已有 CMake/Ninja/g++ 工具链会遇到路径编码和运行库搜索问题。脚本优先使用匹配的 MinGW 运行库，并临时映射一个空闲盘符以提供纯 ASCII 构建路径，结束后解除映射；源码和产物仍保存在本目录，不修改系统 PATH。重复构建和 CTest 请继续使用脚本，因为生成文件引用临时盘符。

产物位置：

```text
build-gcc-debug/bin/bus_backend.exe
build-gcc-debug/bin/bus_simulated.dll
build-gcc-debug/bin/bus_replay.dll
build-gcc-debug/bin/bus_analyze.exe
build-gcc-debug/bin/bus_protocol.exe
build-gcc-debug/build.log
build-gcc-debug/test.log
build-gcc-debug/demo.log
runs/demo_<monotonic-time>.avbus
runs/correlation_<timestamp>/analysis/
```

`bus_incompatible.dll` 只用于 ABI 拒绝测试。Release 可使用 `-Configuration Release`，输出到独立目录。

`-RunDemo` 同时运行旧版插件热替换演示和新版确定性关联分析演示。后者生成 17 条模拟报文，得到 1 项一致、1 项不一致、1 项 12 ms 响应、1 项观测超时和 1 项记录结束时未完成的关联。

## 配置化解码与关联分析

直接生成样例记录，然后离线分析（文件和输出目录必须尚未存在）：

```powershell
$env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH
.\build-gcc-debug\bin\bus_analyze.exe --fixture runs/example.avbus
.\build-gcc-debug\bin\bus_analyze.exe --analyze config/correlation-demo.ini runs/example.avbus runs/example_analysis
```

输出包括 `parameters.jsonl`、`events.jsonl`、`configuration.ini`、`input_archive.txt` 和 `summary.json`。`summary.json` 中的 complete 只在整个输入归档成功读取、分析输出完成刷新后写入；失败时可能存在部分 JSONL，不应当作完整交付。

实时采集启用同一套字典和规则：

```powershell
.\build-gcc-debug\bin\bus_backend.exe --interactive runs/session_002.avbus config/correlation-demo.ini
```

这会在 `runs/session_002.avbus.analysis/` 写入参数和结果。按配置示例创建 `control`、`feedback`、`sensor_a`、`sensor_b` 实例时可匹配各自参数；名称不匹配的报文仍会被记录，但不产生配置参数。`stats` 和 `events` 可查询当前处理情况。当前字典在启动时加载，修改配置需重新启动分析会话；总线插件实例的热替换仍可运行中执行。

详细字段定义和结果语义见 [v0.2 参数与关联设计](docs/processing-v0.2.md)。这个文件是我们自己的字典格式，需要将设备 ICD 转录并核对后使用，不是直接导入任意厂家 ICD 的通用解释器。

## 协议识别与路由检查

`bus_protocol` 用内置协议运行期检查识别和路由决定。生成样例流并逐条打印结果：

```powershell
.\build-gcc-debug\bin\bus_protocol.exe --demo
```

对已有归档逐条路由并汇总：

```powershell
.\build-gcc-debug\bin\bus_protocol.exe runs/session.avbus
.\build-gcc-debug\bin\bus_protocol.exe runs/session.avbus --source capture_a --channel 0 --manual 65537
```

选项包括 `--source`、`--channel`、`--manual ID`、`--representation REP`、`--hint ID` 和 `--authoritative`；不指定 `--source` 时配置作用于遇到的每个地址。该工具不改变 `bus_backend` 和 `bus_analyze` 的默认行为。协议实现和路由语义见[协议运行时](docs/protocol-runtime.md)。

另提供可选的 Python 标准库验收脚本 `python scripts/verify_live_roundtrip.py`，验证公开命令接口、运行中换插件，以及在线与离线 JSONL 逐字节一致。它不是后端运行或 CMake/CTest 的依赖。

## Linux / g++ 构建命令（尚未在 Linux 实测）

```bash
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/bin/bus_backend --demo ./build/bin/bus_simulated.so
```

核心和内置插件仅依赖 C++ 标准库、系统线程与动态库加载接口，没有下载依赖的步骤。

## 交互式管理

在项目目录运行，确保 MinGW 运行库在搜索路径前部：

```powershell
$env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH
.\build-gcc-debug\bin\bus_backend.exe --interactive runs/session_001.avbus
```

归档父目录需要已存在，文件名必须尚未使用。以下命令在程序提示符中输入，逐条执行：

```text
add bus_a build-gcc-debug/bin/bus_simulated.dll period_ms=10;initial_milli=20000;step_milli=100;channel=0
add bus_b build-gcc-debug/bin/bus_simulated.dll period_ms=10;channel=1
start bus_a
start bus_b
status bus_a
stats
replace bus_a build-gcc-debug/bin/bus_simulated.dll period_ms=5;initial_milli=21000;step_milli=250
events
stop bus_a
remove bus_a
quit
```

`replace` 保持旧实例的运行意图：旧实例运行中则自动启动新实例；旧实例未运行则只替换并配置。替换有采集间隙，不保证无缝。同一个库里的其他实例不会被销毁。

更换动态库版本时，请使用新的文件名，例如 `bus_vendor_v2.dll`，不要覆盖正在加载的文件。相同规范化路径会共享同一个模块句柄。

回放一个已经关闭、写入完成的归档：

```text
add replay_a build-gcc-debug/bin/bus_replay.dll path=runs/session_001.avbus;speed=1
start replay_a
status replay_a
```

`speed=1` 按原记录的宿主接收间隔播放；正整数表示倍速；`speed=0` 表示尽快发出，队列满时仍会拒绝并计数。回放保持原采集时间，时钟域标记为 recorded，并生成新的宿主接收时间。回放结束后可先 `stop` 再 `start` 重播。

首版命令解析要求插件路径不含空格；配置为分号分隔的 `key=value`，没有转义语法，配置文件路径不能含分号。推荐在项目目录使用相对路径。

## 模拟数据说明

模拟插件不是 ARINC 429、1553B、AFDX 或 CAN 的物理层/协议仿真器。

- 协议编号 `65535`，数据载荷为一个小端有符号 64 位整数，单位为毫摄氏度。
- 示例值为 `initial_milli + (sequence % 1000) * step_milli`，每 1000 个样本重复。
- 默认分析规则只检测示例温度向上越过 22 °C 的事件，不代表航空诊断阈值。
- 插件的 `fail_start=1` 仅用于验证启动失败恢复。

随机数据源插件 `bus_random` 与模拟插件使用相同的载荷约定（协议编号 `65535`，小端有符号 64 位毫值），配置项：

```text
random=0|1       0 为确定性递增（默认），1 为随机模式
seed=N           随机模式下非零则固定随机种子，便于复现
initial_milli=N  基准值（毫）
step_milli=N     确定性模式下的步长（毫）
spread_milli=N   随机模式下相对基准值的上下随机范围（毫）
period_ms=N      发送周期
channel=N        通道
fail_start=0|1   仅用于验证启动失败恢复
profile=none|command|feedback|sensor_a|sensor_b   场景模式：多实例协调产生相关数据
scenario_seed=N  场景模式下所有实例共享的随机种子
response_delay=N 场景模式下反馈相对指令延迟的节拍数
```

随机模式示例：`add rng build-gcc-debug/bin/bus_random.dll random=1;spread_milli=3000;period_ms=5`。

**场景模式**用于演示有意义的相关分析。把四个实例配置成不同 `profile`，共用同一个 `scenario_seed` 和 `period_ms`；所有实例按宿主的同一单调时钟对齐到相同节拍，因此时间戳一致、不会出现“迟到”质量标记：

```text
add control  build-gcc-debug/bin/bus_random.dll profile=command;scenario_seed=1001;period_ms=20
add feedback build-gcc-debug/bin/bus_random.dll profile=feedback;scenario_seed=1001;period_ms=20;response_delay=1
add sensor_a build-gcc-debug/bin/bus_random.dll profile=sensor_a;scenario_seed=1001;period_ms=20
add sensor_b build-gcc-debug/bin/bus_random.dll profile=sensor_b;scenario_seed=1001;period_ms=20
```

反馈跟随指令并偶尔不响应，两路温度多数时候一致、偶尔不一致，于是分析会得到“一致/不一致/响应正常/响应超时”等有意义的结论。数据每次运行不同（种子可换），但内部是协调的。未设置 `profile` 时保持原有递增或随机的独立行为。

## 当前边界与下一阶段

当前是单进程、每个采集实例独立线程、一个处理线程。尚未实现采集/处理双进程隔离、共享内存 IPC、真实总线驱动、真实时钟同步、自动重排缓冲、插值、统计融合算法、数据库或网络 API。v0.2 提供了显式时钟映射、有限时间窗口配对及两类规则；这些不能替代硬件同步和真实系统校准。配置模式会将原始报文、参数和结果分别持久化，原有无配置演示仍只持久化原始报文。

队列满时拒绝的报文不会进入原始归档；统计会反映损失。存储失败使处理链路故障并拒绝后续输入；`accepted - completed` 可帮助识别未完成数据。宿主入队成功不等于记录已写入磁盘。关机时的流刷新不提供断电持久性承诺。归档当前为单文件，自动分段、落盘策略及持久化生命周期事件留待下一阶段。

插件在同一进程运行，因此不隔离驱动崩溃；C ABI 解决接口与所有权边界，不承诺任意编译器、架构或系统之间二进制通用。支持软件插件切换不等于设备支持带电插拔。

协议运行时已补齐识别、路由、解析和命令行入口，但内置的两种协议是软件侧组帧与校验，不是设备 ICD 或总线物理层实现；`bus_backend` 默认数据路径也仍按报文中已知协议编号分派。下一步可确定一种真实总线及其采集卡 SDK，按厂家 ICD 派生子协议实现，并核对其参数更新周期和时钟语义。后续平台接入可以围绕现有配置、参数流和分析对象添加 API。

详细设计见 [架构说明](docs/architecture.md)、[插件开发约定](docs/plugin-contract.md) 和 [v0.2 验证记录](docs/verification-v0.2.md)。初版来源快照已保存到 `snapshots/`，原 [v0.1 验证记录](docs/verification.md) 作为历史保留。
