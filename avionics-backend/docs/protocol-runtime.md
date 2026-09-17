# 多协议运行时：识别、路由与解析

本文记录在[协议抽象接口](protocol-abstractions.md)之上补全的具体实现：内置协议、自动识别算法、运行时路由器、消息解码器和命令行入口。协议编号是本程序内部标识，不表示任何真实总线编号。

## 内置协议

`BuiltinProtocolFactory` 描述并创建四种内置协议，均可通过 `IProtocolFactory` 查询：

| 内部编号 | 名称 | 输入表示 `capture_representation` | 说明 |
|---|---|---|---|
| `0x00010001`（65537） | `arinc429` | `captured_words` | ARINC 429 32 位字：标签/SDI/数据/SSM/奇校验 |
| `0x00010002`（65538） | `framed-crc16` | `wire_bytes` | 同步字加长度与 CRC 的通用分帧字节流 |
| `0x00010003`（65539） | `mil1553b` | `captured_mil_words` | MIL-STD-1553B 20 位字：同步/信息/奇校验 |
| `0x00010004`（65540） | `can20` | `wire_bytes` | CAN 2.0 帧容器：标识符/标志/DLC/数据/CRC-15 |

### ARINC 429 字格式

每个字为 4 字节大端整数，位分配自高位到低位：标签 `label[31:24]`、`SDI[23:22]`、`数据 data[21:3]`、`SSM[2:1]`、`parity[0]`。整字置位数必须为奇数；`label` 为 0 视为非法。标签按 ARINC 惯例在元数据里按位反序输出。本实现只处理已捕获的字，不实现物理层或电气层。

### MIL-STD-1553B 字格式

每个字以 4 字节小端容器保存一个 20 位字：同步 `sync[19:17]`（指令/状态字为 `100`，数据字为 `001`）、信息 `information[16:1]`、`parity[0]`。整字置位数必须为奇数，高 12 位必须为零。指令/状态字的信息位解析为 RT 地址、T/R、子地址和字计数；数据字解析为 16 位数据。

### CAN 2.0 帧容器

每帧以字节容器保存：4 字节小端标识符、1 字节标志（bit0 扩展帧、bit1 远程帧）、1 字节 DLC、DLC 个数据字节、2 字节小端 CRC-15。校验使用 CAN 的 CRC-15（多项式 `0x4599`，初值 0，按位 MSB 优先）覆盖标识符/标志/DLC/数据。标准帧标识符上限 `0x7FF`，扩展帧上限 `0x1FFFFFFF`，DLC 上限 8。这是记录侧的帧容器，不含总线位填充，不实现物理层。

### framed-crc16 帧格式

```text
偏移 0      : 0xB5 0x00                     同步字
偏移 2..3   : length                        小端 16 位，载荷字节数
偏移 4..    : payload                       length 字节
偏移 4+n..  : crc16                         小端 16 位，覆盖 length + payload
```

校验使用 CRC-16/CCITT-FALSE（多项式 `0x1021`，初值 `0xFFFF`）。载荷长度上限 4096 字节。

## 识别算法

`IProtocolDetector::probe()` 在同一数据流的有限观察窗口上无副作用地检查结构。每个探测器的结论只描述自身协议：

- arinc429：窗口内存在完整的 32 位字、所有字通过奇校验、标签非零，且累计不少于 4 个字时给出 `Strong` 候选并 `Identified`；字不足 4 个时为 `NeedMoreData`；出现结构不合法（长度非 4 的倍数、校验或标签失败）即不属于该协议，不产生候选。
- mil1553b：每个 20 位字的同步位合法（`100` 或 `001`）、奇校验通过、高位为零，且累计不少于 4 个字时 `Identified`；否则 `NeedMoreData` 或不产生候选。
- can20：从头解析帧容器并校验 CRC-15。至少 1 条完整且 CRC 正确的帧即可 `Identified`；数据不足为 `NeedMoreData`；长度、标识符、DLC 或 CRC 不符即不产生候选。
- framed-crc16：从头解析同步字、长度和 CRC。至少 1 条完整且 CRC 正确的记录即可 `Identified`；同步字匹配但数据不足为 `NeedMoreData`；同步字或 CRC 不符即不产生候选。

`ProtocolRouter::probeStream()` 汇总候选并判断冲突，规则与抽象接口契约一致：

1. 配置了可信来源提示（`hint_is_authoritative`）时直接选择，依据为 `SourceDescriptor`，不做内容验证。
2. 否则运行所有匹配输入表示的探测器。只有一个协议给出强候选时选定，依据为 `ContentEvidence`。
3. 多个协议给出强候选，或多个暂定候选无法排除，返回 `Ambiguous`，保留候选与原因。
4. 证据不足返回 `NeedMoreData`；达到观察预算仍无法识别返回 `Rejected`；没有任何证据返回 `Unknown`。
5. `capture_representation` 非空时只运行声明该表示的协议；声明了无法匹配任何协议的表示直接 `Rejected`，不会默认成某种格式。

自动识别成功后会创建解析器，并回放该流已缓存的观察窗口（不丢弃识别前的数据），然后清空观察缓存。手动选择在设置时校验编号已注册，未注册编号抛出 `std::invalid_argument` 并保留原策略；手动选择得到的 `Identified` 依据为 `Manual`，表示路由决定而非内容验证。

## 路由器行为

`ProtocolRouter` 实现抽象契约中的全部规则：

- 状态按 `StreamAddress`（来源、通道、回放原来源）和 `ProtocolStreamKey`（再加采集代次和原代次）隔离，多个来源、通道和回放原来源可交错输入。
- 同一流确认协议后复用解析器；坏帧返回 `Malformed`，不触发无声切换。
- `setSelection` 仅在策略实际变化时清空该地址的识别与解析状态；未注册的手动编号抛异常且不改变原策略。
- `setInputDescriptor` 在描述实际变化时清空该地址状态。
- `resetStream` 清空运行时状态，保留策略和输入描述；`removeStream` 同时删除策略和描述。
- `setLimits` 拒绝含零的预算并保留原设置；预算缩小时先清空观察和解析状态，再应用新预算，策略与描述保留。
- 活动流数达到 `max_streams`、观察帧数或字节数达到上限、候选数超过上限时返回 `Rejected` 并给出原因。

## 消息解码与管线接入

`DictionaryMessageDecoder` 按 `ProtocolMessage.protocol` 和 `ProtocolMessage.stream.address.channel` 选择设备字典字段，用消息载荷完成位提取、编码转换、比例偏置和有效位判断，产出的 `ParameterSample` 以消息证据中的首条记录作为代表记录，并保留来源、代次、原代次和时间。

`ProtocolPipelineDecoder` 继承现有 `IParameterDecoder`，内部组合路由器和消息解码器：`decode()` 先路由原始报文，仅当解析状态为 `Complete` 时把每条消息解码为工程参数。识别未定、需要更多数据或解析失败时返回空结果，由现有管线计为 unsupported。字段级位提取已抽到 `include/core/field_value.hpp`，与原 `DictionaryDecoder` 共用同一实现。

## 命令行入口

新增 `bus_protocol`，用于运行期配置和检查路由决定：

```powershell
.\build-gcc-debug\bin\bus_protocol.exe --demo
```

`--demo` 生成字协议、分帧协议和无法识别的样例流，逐条打印识别状态、依据、选定协议、解析状态和消息数。

对已有归档逐条路由，并按来源/通道/代次汇总：

```powershell
.\build-gcc-debug\bin\bus_protocol.exe runs/session.avbus
```

可用选项（`--source` 指定地址，缺省时对遇到的每个地址生效）：

```text
--source S            地址来源
--channel N           通道，默认 0
--manual ID           手动固定协议编号
--representation REP  显式输入表示，如 captured_words 或 wire_bytes
--hint ID             来源协议提示
--authoritative       将提示标记为可信
```

该入口不修改现有 `bus_backend` 和 `bus_analyze` 的默认行为，仅在需要时用于检查和验证协议识别。

## 本轮验证

`scripts/build-mingw.ps1 -RunTests` 构建全部目标并运行 CTest，新增：

- `protocol_runtime`：工厂描述与创建、选择策略校验、字与分帧自动识别、分帧跨帧重组、通道/代次/回放原来源隔离、坏帧不切换、歧义判定、路由预算与预算收缩、输入描述、重置与移除、消息解码与管线接入。
- `protocol_cli`：`bus_protocol --demo` 输出识别与拒绝结果。

现有 `backend_integration`、`processing_integration`、`protocol_contract` 保持通过。位提取重构后原 `DictionaryDecoder` 行为不变。

这些协议实现是软件侧的组帧与校验，不是任何设备 ICD 或总线物理层的认证实现；接入真实设备前仍需核对厂家 ICD、更新周期和时钟语义。
