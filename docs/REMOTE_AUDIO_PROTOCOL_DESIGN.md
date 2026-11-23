# 远程音频协议设计：NexAudioIpc v3

状态：v3 协议与实现对照。本文只负责 wire contract、状态机和回退语义；当前缺口与证据范围见 [AUDIO_CORRECTNESS_AUDIT_2026-11-16.md](AUDIO_CORRECTNESS_AUDIT_2026-11-16.md)。

## 目标与范围

第一条纵向路径是：网易云进程内的 hook 负责获取客户端 PCM、完成输出格式协商与转换，`NexAudioHost.exe` 只负责以转换后的 PCM 打开 WASAPI 独占端点并提交。现有进程内 WASAPI 路径继续作为回退路径。

这条路径需要同时解决四个边界：两个进程是否属于同一构建、共享环里究竟是哪一种 PCM、设备什么时候真正开始播放、旧曲目的尾部完成信号如何与新曲目区分。

本设计暂不实现 ASIO、DLNA、OpenHome、RAAT、长时大环和本地曲库。协议字段为后续输出引擎保留位置，但第一版只允许 `engine=wasapi`。

## 历史 v2 基线

早期 [`nexaudiolink.h`](../mod/tools/nexaudiolink.h) 的共享结构包含 magic、version、格式字段、读写位置、帧数、哈希、丢帧和欠载计数，并把数据固定放在 `16384 * 32` 字节的数组里。它能支撑“客户端原格式 PCM → 独立 WASAPI 宿主”，不能表达下面这些状态：

- `remote` 代表了进程位置，却没有表达实际输出引擎；
- hook 与 audiohost 只共享结构版本，没有构建身份；
- 格式字段只有一份，默认等于客户端输入格式；
- `remote_start()` 在首个有效 PCM 到达前就启动设备；
- 环满时 `remote_write()` 截短本次写入并增加 `dropped_frames`，调用方无法区分暂时背压与永久失败；
- 没有 EOS 提交代次和设备时钟确认代次。

这些差异先写进协议，再开始改代码。协议升级必须能让旧 DLL 和旧宿主在握手阶段失败并回退，不能让两端以相同的旧版本号继续运行出不同语义。

## 当前实现对照

当前工作树已经包含 v3 的固定宽度字段、build-id、输入/输出格式、format generation、首包状态、背压计数、Host 退出标记和状态接口字段。以下语义仍未完成真实播放验收：

- `fc_GetPosition()` 读取 Host 回传的 `IAudioClock` 位置快照，按 `device_clock_frequency` 换算成 100 ns（2026-11-23 之前误把原始 tick 当作 100 ns 相加），并扣除 Host 垫入的启动静音；`endpoint_frames` 只保留为提交统计；
- EOS 只在流仍在播放时提交并等待（暂停或切走后的拆除不再播放尾巴），等待上限按「共享环 + 设备缓冲时长 + 250 ms」计算，介于 300 ms 与 5 s 之间；
- 共同水位已进入遥测：每秒一行 `audiohost telemetry`，含 `ring_used / ring_min / ring_max / ring_capacity / device_padding / latency_ms / write_waits / write_wait_max_ms / primed`，`/api/status` 与面板同步显示；实机对照仍待做；
- `underrun_events` 已避开尚有 padding 的空事件；进程内路径的 `empty-buffer` 按 10 秒窗口报告；
- build-id 改为只由三份协议文件的内容生成（`nexaudiolink.h`、`nexaudiohost.c`、`sink_audiohost.inc`，换行归一），与 git HEAD 和其他源码无关：改面板或策略不会再让新 DLL 拒绝已部署的 Host。

已在代码中修正、仍需真机确认的项目：Host 进程退出检测、unsupported format 的进程内回退、`/api/status` 的 v3 字段和 format generation 递增原语。

## 当前协议决策

### 设备时钟的所有者

设备播放位置由拥有 `IAudioClient` 和 `IAudioClock` 的 Host 产生。Hook 只保存客户端与端点的帧数统计，不把 `endpoint_frames` 当成设备播放位置。Host 回传的 clock tick、QPC 和 padding 必须带上 `stream_generation` 与 `format_generation`，客户端丢弃过期代次的数据。

### EOS 的所有者

Hook 负责声明源已经结束，Host 负责确认环排空和设备时钟越过最后一次真实提交。`eos_submitted_generation` 与 `eos_complete_generation` 只表示代次，不表示暂停或 Stop。最终销毁前需要保留一个等待窗口；窗口结束前，Host 不得因为普通关闭通知跳过 `DRAINING`。

### 背压的所有者

Host 负责通过 `read_frame` 和 `space_event` 表示可消费空间，Hook 负责把一次完整批次作为原子写入。等待成功记为 `backpressure_events`，超时进入 `FAILED`；正常等待不增加 `dropped_frames`，生产者不把批次缩短来绕过环满。

### 格式命名

`input_*` 描述网易云交给 Hook 的 PCM，`output_*` 描述写入共享环并交给 Host 的 PCM。面板的“无损（SQ）”仍属于网易云质量选择；`input_valid_bits`、`input_container_bits` 和 `output_valid_bits` 分别显示传输格式，不把其中任意一个字段命名为源文件位深。

## 两层共享对象

v3 共享对象拆成固定头部和 PCM 数据区。当前控制页布局版本为 `2`，头部保持定长、可校验，数据区依据有效输出格式计算大小：

```text
NexAudioControl  (固定布局，包含协议、状态、格式和统计)
NexAudioData     (capacity_frames * output_frame_bytes)
```

这样可以让 output frame bytes 随转换结果变化，也避免以后扩容环时改变控制头部的 ABI。第一版实现若继续使用单一 mapping，仍必须写出 `data_bytes` 和 `output_frame_bytes`，并在映射前验证 `capacity_frames * output_frame_bytes` 没有溢出；独立数据 mapping 是后续推荐形态。

## 字段表

下表是 v3 的控制头部字段。`required` 表示第一条 WASAPI 转换路径必须存在；`later` 表示字段先进入协议和状态模型，后端尚未实现时保持 `wasapi`。

| 字段 | 类型 | 方向 | 级别 | 语义 |
| --- | --- | --- | --- | --- |
| `magic` | `uint32` | 双方 | required | 固定魔数，映射后第一项校验 |
| `protocol_major` | `uint16` | 双方 | required | 不兼容布局或状态语义时递增 |
| `protocol_minor` | `uint16` | 双方 | required | 向后兼容字段或诊断能力增加时递增 |
| `layout_version` | `uint32` | 双方 | required | C 结构布局版本；与行为协议版本分开 |
| `build_id` | `uint64` | 双方 | required | hook 与宿主构建身份；握手必须完全相等 |
| `engine` | `uint32` enum | hook → host | required | `wasapi=0`；预留 `dlna=1`、`raat=2`，ASIO 先不启用 |
| `deployment` | `uint32` enum | hook → host | required | `in_process=0`、`audiohost=1`；不再从 `remote` 推导引擎 |
| `state` | `uint32` enum | 双方 | required | 见下方状态机 |
| `error` | `HRESULT` / `uint32` | host → hook | required | 最近一次不可恢复错误；正常背压不写成永久错误 |
| `stream_generation` | `uint64` | hook → host | required | 每次新的打开/格式代次递增 |
| `format_generation` | `uint64` | hook → host | required | 输出格式变化时递增；同一 stream 可有多个格式代次 |
| `input_rate` | `uint32` | hook → host | required | 客户端输入采样率 |
| `input_channels` | `uint16` | hook → host | required | 客户端输入声道数 |
| `input_container_bits` | `uint16` | hook → host | required | 客户端容器位深 |
| `input_valid_bits` | `uint16` | hook → host | required | 客户端有效位深 |
| `input_encoding` | `uint16` enum | hook → host | required | PCM 或 float |
| `input_channel_mask` | `uint32` | hook → host | required | 客户端声道布局 |
| `output_rate` | `uint32` | hook → host | required | 实际写进共享环、交给端点的采样率 |
| `output_channels` | `uint16` | hook → host | required | 实际输出声道数 |
| `output_container_bits` | `uint16` | hook → host | required | 实际输出容器位深 |
| `output_valid_bits` | `uint16` | hook → host | required | 实际输出有效位深 |
| `output_encoding` | `uint16` enum | hook → host | required | PCM 或 float |
| `output_channel_mask` | `uint32` | hook → host | required | 实际输出声道布局 |
| `output_frame_bytes` | `uint32` | hook → host | required | 一个输出 frame 在环中的字节数 |
| `capacity_frames` | `uint32` | 双方 | required | 映射后实际可用帧数 |
| `data_bytes` | `uint64` | 双方 | required | PCM 数据区大小，必须覆盖整个环 |
| `write_frame` | `uint64` | hook → host | required | 单调递增写位置 |
| `read_frame` | `uint64` | host → hook | required | 单调递增读位置 |
| `first_buffer_submitted` | `uint8` / flags | host → hook | required | 至少一次完整设备包已提交 |
| `device_started` | `uint8` / flags | host → hook | required | `IAudioClient_Start` 已成功返回 |
| `eos_submitted_generation` | `uint64` | hook → host | required | 源已经结束且该代次的 PCM 已写完 |
| `eos_complete_generation` | `uint64` | host → hook | required | 设备时钟确认该代次已经排空 |
| `device_clock_position` | `uint64` | host → hook | required | `IAudioClock::GetPosition` 返回的 clock tick |
| `device_clock_qpc` | `uint64` | host → hook | required | 与 clock tick 同次采样的 QPC |
| `device_clock_frequency` | `uint64` | host → hook | required | `IAudioClock::GetFrequency` |
| `device_padding_frames` | `uint32` | host → hook | required | 同次采样的端点 padding，单位为输出帧 |
| `source_waits` | `uint64` | host → hook | required | 设备运行前或提交间等待源数据的次数/事件数 |
| `underrun_events` | `uint64` | host → hook | required | 设备已经运行后，设备事件到达而没有可提交 PCM 的次数 |
| `dropped_frames` | `uint64` | 双方 | required | 明确丢弃的帧；正常等待和部分写入不能计入 |
| `backpressure_events` | `uint64` | 双方 | required | 环空间不足导致的等待或拒绝次数 |
| `getbuffer_failures` | `uint64` | host → hook | required | 端点 `GetBuffer` 失败次数 |
| `releasebuffer_failures` | `uint64` | host → hook | required | 端点 `ReleaseBuffer` 失败次数 |
| `active_engine` | `uint32` enum | host → hook | required | 实际使用的引擎；回退后不能仍显示请求值 |
| `fallback_reason` | `uint32` enum | host → hook | required | 未回退时为 `none`，否则记录握手、格式、端点或运行时原因 |
| `transport` | `uint32` enum | hook → host | 3.1 | `PLAY` / `PAUSE`：客户端的 Start/Stop。暂停时 Host 停设备、保留环内音频 |
| `device_paused` | `uint32` | host → hook | 3.1 | Host 已因暂停停止设备 |
| `flush_request_generation` | `uint64` | hook → host | 3.1 | 客户端 Reset（拖进度）：Host 停止并 Reset 设备、丢弃环、重新预填 |
| `flush_ack_generation` | `uint64` | host → hook | 3.1 | Host 完成 flush 后写入请求值；hook 最多等 500 ms |
| `prime_request_frames` | `uint32` | hook → host | 3.1 | 下一次首包前要垫的静音帧数（一个设备缓冲；跨 44.1k/48k 族再加 `resync_ms`） |
| `primed_frames_total` | `uint64` | host → hook | 3.1 | Host 已提交的启动静音帧数，flush 时清零；hook 从设备时钟中扣除 |
| `target_id` | 字符串 | hook → host | required | WASAPI 设备 ID（面板选定的设备或客户端自己的设备）；未来可承载 DLNA UDN 或 RAAT device ID |
| `network_location` | 字符串 | hook → host | later | DLNA 描述地址或 RAAT 控制地址 |

`client_frames`、`converted_frames`、`endpoint_frames` 和哈希可以继续保留作为验证遥测，但它们不能替代 `write_frame/read_frame`。前者描述样本证据，后者描述传输进度。公开播放位置只使用 `device_clock_position` 与 `device_clock_frequency`；`device_clock_qpc` 和 `device_padding_frames` 用于同刻诊断。

## 枚举与状态机

### 引擎与部署

```text
engine:
  wasapi = 0
  dlna   = 1   // 设计保留，暂不实现
  raat   = 2   // 设计保留，暂不实现

deployment:
  in_process = 0
  audiohost  = 1
```

ASIO 暂时不进入枚举承诺。以后需要时可以加入 `asio=3`，同时提供独立的格式与设备能力探测。

### 生命周期

```text
INIT
  -> NEGOTIATING       已校验协议、构建身份和输入/输出格式
  -> READY              端点已初始化，尚未 Start，没有完整设备包
  -> PREFILLED          至少一个完整设备包已提交
  -> RUNNING            Start 成功，设备时钟开始推进
  -> DRAINING           收到 eos_submitted_generation，等待环排空和设备时钟
  -> COMPLETE           写回 eos_complete_generation

任何阶段 -> FAILED     不可恢复错误，写 error/fallback_reason
任何阶段 -> CLOSING    主动 Stop、Reset、换轨或协议关闭
```

`READY` 与 `PREFILLED` 分开，目的是让“首包是否提交”和“设备是否启动”有可检查的先后关系：

1. `READY` 阶段不得调用 `IAudioClient_Start`；
2. 只有 `write_frame - read_frame` 足够覆盖一个完整设备包，并且 `GetBuffer/ReleaseBuffer` 成功，才置 `first_buffer_submitted=1`；
3. 首次完整提交成功后才调用 `Start`；
4. `device_started=1` 只能来自 `Start` 的成功返回；
5. 启动后的空设备事件才计入 `underrun_events`，启动前等待数据计入 `source_waits`。

### 背压、丢帧和失败

第一版选择“等待或失败，不静默截短”策略：

- host 通过 `space_event` 告知环有可用空间；
- hook 的实时回调不等待，泵线程可以进行有上限的等待；
- 空间不足时保留源环里的帧，不把本次写入截成较短数量；
- 等待成功只增加 `backpressure_events`；
- 超过等待上限，置 `state=FAILED`、填写 `error` 和 `fallback_reason=ring_backpressure`；
- `dropped_frames` 只在明确执行丢弃策略时增加，第一版默认不执行丢弃；
- 在 host 尚未占有设备前，握手/格式/启动失败可以关闭共享对象并回退进程内；host 已经启动后出现运行时故障，则先结束这一代，再让客户端下一次 Initialize 触发回退，避免两个进程同时占有端点。

## 转换后的 PCM 路径

现有进程内路径已经拥有候选协商、SRC、位深转换、抖动和声道映射。第一条远程纵向路径复用这些结果，但把最终输出放入共享环：

1. hook 读取客户端输入格式，记录 `input_*`；
2. 通过现有候选逻辑决定 `output_*`；
3. 转换线程把输出 PCM 写入环，更新 `converted_frames`；
4. host 只读取 `output_*`，构造 `WAVEFORMATEXTENSIBLE` 并打开 WASAPI；
5. 只有 `input_*` 与 `output_*` 每个样本完全一致时，验证状态才允许写成 `client-native`；发生 SRC、位深转换或映射时写成 `converted`；
6. host 端的 `endpoint_frames` 和 endpoint hash 继续用于确认共享环到设备提交的边界。

`hifi` 是输出策略，`engine` 是输出后端，`deployment` 是进程位置。三者需要在状态接口、日志和面板里分别显示。

## 握手与回退

握手分两段：

1. **映射前的命名对象握手**：确认 control mapping、data mapping、事件句柄和会话属于同一宿主；
2. **映射后的内容握手**：依次校验 magic、major/minor、layout_version、build_id、engine、deployment、format_generation、output frame bytes 和 data bytes。

失败分支必须写出具体原因：

```text
protocol_mismatch
layout_mismatch
build_mismatch
unsupported_engine
unsupported_format
endpoint_open_failed
ring_backpressure
runtime_device_lost
```

`build_id` 不匹配时，host 不得打开端点；hook 关闭共享对象后回到进程内引擎。状态接口同时报告 `requested_engine`、`active_engine` 和 `fallback_reason`，避免把“请求 audiohost”显示成“audiohost 正在输出”。

## 3.1 传输控制与启动静音（2026-11-23）

v3.0 只有数据通道：客户端暂停时 Host 继续把环放空再欠载，拖进度后先播约 0.5 s 拖动前的音频。3.1 从 `reserved` 尾部划出字段（控制页大小不变，`reserved` 由 128 字节减到 88 字节，`layout_version = 3`）：

1. 客户端 Stop → `transport = PAUSE`，Host 停止设备、置 `device_paused`，环内音频保留；
2. 客户端 Start → `transport = PLAY`，Host 重新 Start；
3. 客户端 Reset → 先写 `prime_request_frames`，再递增 `flush_request_generation`；Host 停止并 Reset 设备、`read_frame = write_frame`、清 `primed_frames_total`、回到 READY 并写 `flush_ack_generation`；
4. 首包前若 `prime_request_frames > 0` 且环里已有一个设备缓冲，Host 先提交一个 `AUDCLNT_BUFFERFLAGS_SILENT` 缓冲再 Start，剩余的跨族静音在后续周期提交，全部计入 `primed_frames_total`；
5. 环的深度由 hook.cfg `host_ring_ms` 决定（默认 500 ms，200 ms～4 s）。

诊断：NetEase 进程环境里设 `NEX_DUMP=<文件>`（DLL 交给 Host 的 PCM）与 `NEX_HOST_DUMP=<文件>`（Host 交给设备的 PCM，不含启动静音），用 `python mod/tools/compare_pcm.py a b --frame-bytes N` 逐字节对照；`audiohost=1` 时两者必须完全一致。

## EOS 与设备时钟

EOS 需要区分源结束、共享环排空和设备播放完成：

1. hook 写完某一代最后一帧后，设置 `eos_submitted_generation = G`；
2. host 看到 `eos_submitted_generation = G`，确认 `write_frame == read_frame`；
3. host 记录最后一次真实提交对应的设备位置；
4. `IAudioClock` 越过该位置后，host 写 `eos_complete_generation = G`；
5. hook 只接受自己当前 `stream_generation == G` 的完成戳；
6. Stop、Reset、拖动和换轨会先使旧代次失效，旧代次的完成戳不能结束新代次。

字段和状态已经进入当前协议；自然换轨等待策略仍需经过首包、背压和回退的真实设备验收。暂停不等同于 EOS，拖动也不应为了等待尾部而阻塞客户端控制线程。

## 分阶段改造顺序

### 阶段 0：冻结当前证据

- 保留现有协议、Host mock、完整回归、宿主契约和面板契约结果；
- 将真实设备探针收敛为显式选择设备的工具；
- 分别记录进程内路径和 audiohost 路径的首包、启动、提交间隔、等待、欠载、EOS 和回退；
- 暂时不把听感或单次设备日志写成自动化结论。

### 阶段 1：协议静态复核（已有实现）

- 复核固定头部和数据区的布局定义；
- 复核协议、布局和 build-id 校验；
- 复核 engine/deployment、input/output 双格式和 format generation；
- 复核状态机、首包标志、设备启动标志和背压字段；
- 用 mock 继续覆盖旧宿主、构建身份不符、输出格式不符和环满等待，新增同一 stream 内格式代次变化。

### 阶段 2：WASAPI audiohost 真实路径复核

- 确认 hook 把转换后的 PCM 写入环；
- 确认 host 使用 output 格式打开端点；
- 确认首个完整设备包提交后再 Start；
- 确认 build-id 或格式失败时释放共享对象并回退进程内；
- 确认运行时错误结束当前代次，下一次客户端 Initialize 再回退；
- 在 Pingyi 会话中核对日志和 `/api/status` 的 requested/active engine、deployment 和三层帧数。

### 阶段 3：EOS、时钟与自然换轨（当前缺口）

- 让 submitted/complete generation 在真实销毁流程之间保留等待窗口；
- 用 Host 设备时钟确认尾部排空，替代客户端提交帧数估算；
- 单独验证自然换轨、暂停、拖动、Reset 和“当前曲目重开应用”；
- 为旧代次完成戳、host 提前退出和客户端提前 Stop 增加回归。

### 阶段 4：其他输出后端的接口准备

- DLNA/OpenHome 只先确定 `engine`、目标标识、流形态和状态接口；
- RAAT 只先确定网络设备标识、控制地址、格式和队列状态；
- ASIO 暂不进入实现范围；
- 长时大环和本地曲库维持独立议题。

## 验收条件

第一条远程纵向路径进入真机验收前，至少需要看到：

- protocol、layout 和 build-id 不一致时不会占有端点；
- `first_buffer_submitted=1` 先于 `device_started=1`；
- `output_*` 与 host 实际初始化格式一致；
- 转换路径的 `clientFrames`、`convertedFrames`、`endpointFrames` 可分别读取；
- 环空间不足时没有未经声明的帧截短；
- `source_waits` 与 `underrun_events` 数值能区分启动前等待和运行中欠载；
- host 退出、设备拒绝格式和协议不匹配都能回退或报告明确失败；
- EOS 完成戳只对当前 generation 生效；
- 进程内路径在远程路径不可用时仍能播放。
