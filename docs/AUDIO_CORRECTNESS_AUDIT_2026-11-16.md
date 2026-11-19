# 音频正确性审计：2026-11-16

状态：文档与证据阶段。本文只记录源码、回归和现有日志能支持的判断，不把一次构建或一次听感当成长期结论。

## 文档职责

本文负责四类证据：客户端 PCM 与端点 PCM 的格式边界、进程内和远程 WASAPI 的时间轴、自然结束语义、以及真实播放验收缺口。协议字段的规范写在 [REMOTE_AUDIO_PROTOCOL_DESIGN.md](REMOTE_AUDIO_PROTOCOL_DESIGN.md)，构建和注入步骤写在 [BUILDING.md](BUILDING.md) 与 [DEVELOPMENT_RUNTIME.md](DEVELOPMENT_RUNTIME.md)。

## 先区分四个“音质”对象

网易云页面的“无损（SQ）”是服务端质量档位；当前截图中的服务端标注为最高 48 kHz / 16 bit。真正的“音源规格”是服务端或解码器提供的源格式元数据。Hook 日志中的 `44.1 kHz / 32 bit` 是客户端交给 `IAudioClient` 的 PCM 格式，属于解码后的传输格式，不能直接当成源文件位深。端点日志中的 `24 bit` 是独占设备实际接收的容器格式。四层要分别记录，面板也应分别命名。

当前面板解析 `in=rate/cont/valid`，有效位显示在前，容器位深放在括号里。截图中的 `32-bit / 44.1 kHz → 24-bit / 44.1 kHz` 实际表示“客户端 PCM → 独占端点”，不等于“音源 32 bit → 设备 24 bit”。`32 bit` 只表示客户端容器或浮点传输格式时，不能据此推断网易云拿到了 32 位源文件；是否发生端点转换，要看 `input_*`、`output_*`、`convertedFrames` 与 verifier 结果。

当前项目还没有从网易云播放图或解码器元数据得到独立的 `source_*` 字段。因此“音源规格”目前只能显示为“未取得”，不能用 `input_*` 代替。

## UI 与音质选择的关系

当前代码没有证据表明 `panel.js` 或 CDP 注入会改写网易云的“无损（SQ）”质量选择。面板写入的是 `hook.cfg` 中的独占策略、采样率、转换、缓冲和 `audiohost` 等模块配置；网易云页面自己的质量档位由客户端保留。需要单独核对的是：面板保存后是否触发流重建、重建时选中了进程内还是 audiohost、以及 `hifi`/`compat` 是否让输出格式发生了改变。这个核对属于格式实验，不应把面板上的 `32 bit` 直接当成源音质。

## 音源规格字段的缺口

OpenHome 的做法说明了当前缺少的对象：`source_*` 描述播放图或解码器声明的源格式，`input_*` 描述客户端送进 WASAPI Hook 的 PCM，`output_*` 描述送进独占端点的 PCM。三者可以相同，也可以不同。

**当前实现（2026-11-16 晚）：源规格由 Host 读 `.uc` 缓存头取得。** 面板只负责拿到 `playId`，并读取播放栏的当前质量提示：它通过网易云自己的 webpack `require`（`webpackJsonp.push` 取得）复用已经实例化的 `AudioPlayer`，订阅 `playprogress`；换歌时向 `/api/source` 发送 `{source_status:"probe", source_play_id, source_quality_tier_hint}`。面板不再调用 `getPlaybackInfo({playId})`。质量提示只用于候选排序，不能单独证明源文件位深或采样率。

> 旧做法自己重新执行模块工厂，造出第二个 `AudioPlayer` 实例，它注册的原生回调会顶掉网易云自己的回调，播放栏进度立即停住。实测：用私有实例订阅后进度停在 02:03；改用共享实例后进度正常推进，同时能收到 `playId`。

Host 收到 `probe` 后以 `playId` 的数字前缀匹配 `%LOCALAPPDATA%\NetEase\CloudMusic\Cache\Cache\<id>-<品质档>-<hash>.uc`，XOR 0xA3 解码后只读文件头：

- FLAC：`STREAMINFO` → 采样率、声道、位深；
- MP4（320/192 档）：`moov/trak/mdia/minf/stbl/stsd/mp4a` → 采样率、声道，`esds` 的平均码率 → kbps（实测 320 档为 256 kbps AAC，192 档为 160 kbps AAC）；
- MP3：跳过 ID3v2 后的首个 Layer III 帧头 → 采样率、码率；VBR（`Xing`）用总字节/总帧数换算平均码率。

同一首歌常常缓存了多个品质档（同 id 不同档的文件可达 3 个，规格可能不同，例如 96 kHz/24 bit 与 48 kHz/16 bit）。Host 的候选顺序是：先匹配面板质量提示对应的文件名档位（Hi-Res→1999、无损/SQ→999、极高/HQ→320；母带、臻效、天空、臻彩分别对应 4999、3999、5999、6999，杜比对应 2999），提示缺失时再匹配 DLL 最近一次 `sink ENGAGED in=rate:` 的客户端采样率，最后才取最近写入的文件。源文件已经重采样时，质量提示的优先级高于客户端输入采样率，避免 44.1 kHz/16 bit 与 44.1 kHz/24 bit 之间误选。`/api/status` 同时输出 `source_quality_tier_hint` 与 `source_selection`，后者记录本次使用的是质量提示、采样率还是文件时间。在线播放、之前没有缓存的歌：网易云边播边写缓存，换歌后 30 秒确认窗口内 Host 每 1.5 秒重新挑一次，实测新歌在 1.5 秒内即可取得；窗口结束仍没有缓存才标为 `unavailable`。`/api/status` 新增 `source_bitrate_kbps` 与 `source_quality_tier`。离线测试见 `mod/test/source_probe_test.c`（`build.sh test` 会运行）。

以下为本轮之前的记录，保留作背景。

```text
source_status = unavailable | observed | inferred
source_rate
source_valid_bits
source_container_bits
source_encoding
source_origin = service_label | decoder_probe | client_metadata | unknown
```

`source_status=unavailable` 时，面板显示“源规格未取得”；缓存头、解码器 metadata 或客户端明确元数据取得有效字段后才显示源规格。`service_label` 和当前质量提示只说明质量档位或服务端上限，不能自动填充位深。PCM 的 `source_sample_bytes` 必须是 1–4，零值、越界和缺失会被拒绝，避免界面显示 `0 bit`。

## 状态与面板的时效性

当前 `/api/status` 不是从共享控制页实时读取全部字段。`nexhost.c` 会从最近的 `host.log`、`sink.log` 和 `audiohost telemetry` 行解析数值，同时保存 `/api/source` 的源规格状态。换轨时 `pending` 会清理上一首，Host 重启后内存状态重新为空；面板只在 `source_play_id` 与当前播放 ID 相同且状态为 `observed` 时显示。真实页面仍需验证桥数据可用时的 `observed` 时序，以及查询超时后的 `unavailable`。

这解释了“界面显示 32 bit，但端点是 24 bit”这类表面冲突：一部分是输入/输出层次不同，另一部分是状态来源存在日志滞后。运行阶段需要把 `currentGeneration` 与格式行绑定，只有代次一致时才更新面板；在此之前，审计记录以 `sink.log` 的当前 `ENGAGED` 行和 Host 的 ready 行为准。

## 当前代码逐项判断

| 议题 | 判断 | 证据 | 仍缺的验证 |
| --- | --- | --- | --- |
| 远程时钟使用提交帧估算 | **代码已修正，真机未验收** | `NexAudioControl` 现在携带 Host 的 `device_clock_position`、`device_clock_frequency`、`device_clock_qpc` 和 `device_padding_frames`。`fc_GetPosition()` 直接读取 `IAudioClock` tick，不再从 `endpoint_frames` 扣设备周期。回归 `test_remote_clock_uses_host_device_clock()` 验证位置和 QPC。 | 在 Pingyi 会话验证暂停、换轨、seek 后的连续性，并确认状态字段来自当前代次。 |
| EOS 写入后立即关闭远程宿主 | **代码已修正，真机未验收** | `sink_teardown()` 提交 EOS 后调用 `remote_wait_eos_complete()`；Host 保持 `DRAINING`，等待环排空且设备时钟越过尾部，再写入 `eos_complete_generation`。短于一个设备周期的自然尾包由 Host 作为首个短包提交。 | 仍需验证自然换轨、源结束、暂停、Stop、Reset 的区别，以及 Host 提前退出时的回退。 |
| 远程生产端没有读取设备 padding | **成立** | `drain_once()` 的远程分支使用 `g->real_frames` 作为本轮目标，设备 padding 只在 `NexAudioHost.exe` 的 `host_pump()` 内参与提交空间计算。远程生产者与设备消耗之间由共享环吸收。 | 需要记录客户端提交间隔、Host padding、环水位和设备时钟，确认是否形成周期性跳跃或听感异常。 |
| 欠载统计跨轮询重复计数 | **部分成立** | Host 当前已用 `if (!padding) add_counter(&underrun_events, 1)` 避开仍有设备 padding 的场景，但每次空设备事件都可能递增，同一次真实欠载可能跨多个轮询重复计算。 | 需要明确“事件计数”还是“欠载区间计数”，并用设备事件或状态边沿验证。 |
| Host 崩溃没有进程级检测 | **当前树已修正，仍需实机验收** | `sink_audiohost.inc` 的 `remote_state()` 对 `g_remote_process.hProcess` 调用 `WaitForSingleObject(..., 0)`，退出后写入 `host_exit`、`FAILED` 和 `NEX_AUDIO_FALLBACK_HOST_EXIT`；回归已有 `test_remote_host_exit_is_visible_to_drain()`。 | 需要在真实播放中杀掉 Host，确认 drain 及时退出、当前代次结束、下一次初始化可回退进程内。 |
| `unsupported_format` 不回退进程内 | **当前树已修正，仍需实机验收** | `remote_fallback_is_local()` 已包含 `NEX_AUDIO_FALLBACK_UNSUPPORTED_FORMAT`；启动失败路径会关闭远程对象并设置本地回退标记。回归 `test_remote_fallback_policy_and_eos_stamp()` 已覆盖策略。 | 需要让 Host 真实拒绝格式，确认端点没有被第二次同时占用，且日志与 `/api/status` 显示 active engine。 |
| `/api/status` 缺少 v3 状态字段 | **当前树已修正，数值链路未验收** | `mod/tools/nexhost.c` 的 `status_json()` 已输出 requested/active engine、deployment、converted frames、source waits、backpressure、underrun、generation 和 EOS 两个 generation。 | 需要在 Pingyi 会话里从播放开始到换轨读取状态，确认这些字段来自当前代次，而不是上一条日志。 |
| build-id 是固定字面量 | **部分成立** | `mod/build.sh` 已从协议源和当前工作树生成 `WIRE_HASH`，并以编译定义注入 DLL 与 Host；`nexaudiolink.h` 不再接受未定义的固定字面量。两者仍使用同一次构建生成的相同值，Host 尚未拥有独立发行身份。 | 需要比较不同工作树、不同构建产物和旧 Host 的握手结果，并把身份来源写进发布清单。 |
| `format_generation` 只初始化一次 | **当前树已有递增原语，生命周期覆盖不足** | `nexaudiolink.h` 提供 `nex_audio_next_format_generation()`；`remote_try_exclusive()` 也为每个远程候选递增代数。 | 需要模拟同一 stream 内格式变化，确认每次格式重协商都会递增并被 Host 校验。 |
| 协议字段仍是 `long` 或有符号位置 | **与当前树不符** | `NexAudioControl` 已使用 `uint32_t`、`uint64_t` 和固定宽度整型；`state`、读写位置、帧计数均已改成固定宽度字段。 | 需要加静态布局断言，并在 32 位编译器不可用的情况下保持 wire layout 文档与头文件一致。 |

## 证据索引

| 观察点 | 源码位置 | 当前测试 |
| --- | --- | --- |
| 远程公开时钟 | [`sink_device.inc:483`](../mod/src/sink_device.inc#L483)–[`sink_device.inc:548`](../mod/src/sink_device.inc#L548) | `test_remote_clock_uses_host_device_clock()` |
| 远程整批写入和超时 | [`sink_audiohost.inc:328`](../mod/src/sink_audiohost.inc#L328)–[`sink_audiohost.inc:371`](../mod/src/sink_audiohost.inc#L371) | `test_remote_backpressure_is_atomic()`、`test_remote_drain_backpressure_is_atomic()` |
| Host 进程退出检测 | [`sink_audiohost.inc:24`](../mod/src/sink_audiohost.inc#L24)–[`sink_audiohost.inc:51`](../mod/src/sink_audiohost.inc#L51) | `test_remote_host_exit_is_visible_to_drain()` |
| EOS 等待与短尾 | [`sink_lifecycle.inc:188`](../mod/src/sink_lifecycle.inc#L188)–[`sink_lifecycle.inc:227`](../mod/src/sink_lifecycle.inc#L227) | `test_eos_clock_and_stale_generation()`、`test_empty_eos_completes_without_start()`；真实销毁等待仍需设备验收 |
| Host padding、欠载和 EOS | [`nexaudiohost.c:116`](../mod/tools/nexaudiohost.c#L116)–[`nexaudiohost.c:235`](../mod/tools/nexaudiohost.c#L235) | `test_eos_clock_and_stale_generation()`、`test_empty_eos_completes_without_start()` |
| 设置窗口音频规格与输入/输出格式 | [`panel.js:330`](../mod/ui/panel.js#L330)–[`panel.js:370`](../mod/ui/panel.js#L370) | `panel.contract.test.js` 的设置窗口与格式契约 |
| 服务端质量入口 | [`panel.js:851`](../mod/ui/panel.js#L851)–[`panel.js:910`](../mod/ui/panel.js#L910) | 只验证 DOM 选择器与挂载，不证明服务端档位内容 |

当前回归能证明函数之间的局部契约，不能证明真实设备的时间轴、听感或网易云自然换轨行为。审计中的“当前树已修正”只表示静态代码和 mock 测试有对应路径。

## 源规格与播放栏修订

本轮把 `pcm` 纳入可表达位深的源格式。`samplesize=0`、缺失或超出 1–4 字节继续保持 `unavailable`；`samplesize=2` 映射为 16 bit。Host 发布状态时统一编码名大小写，MP3、AAC、OGG 等有损格式只显示编码与采样率。`pending` 的保留窗口延长到 30 秒，覆盖桥查询的最坏重试时间。为保护网易云原生播放循环，播放事件回调只记录 `playId`；`/api/source` 与 `getPlaybackInfo` 延迟到播放器暂停且用户打开音频详情时执行。

为保护网易云原生播放循环，播放栏不再插入音频规格卡片。音频规格只保留在 NexExclusive 设置窗口中，标题为“音频规格”，内容分为源规格、客户端 PCM、独占端点和状态/校验四行。源规格已取得时不再出现“源文件 / 解码器未接入”的旧说明。

本轮 Pingyi 页面观察到的一个独立现象是：CDP 页面没有收到新的 `playprogress`，`getPlaybackInfo({})` 返回空 `playId`，进度 range 的 `value` 连续保持 0；同一时间安装目录的 `sink.log` 仍有音频线程写入记录。这说明该次观察无法把进度栏停滞归因到端点写入，后续需要在确认页面处于实际播放状态后重新采集桥事件与原生进度节点。

## 远程整批背压的边界

旧描述“空间不足时生产端先把批次数截成 `space`”已经不适用于当前 `drain_once()`：远程分支把目标设为完整设备周期，实际空间检查和 1000 ms 等待由 `remote_write()` 完成，源环在写入成功前保持不前移。现有回归也分别覆盖直接写入和真实 `drain_once()` 组合。

仍有一个相邻问题：远程生产端只知道自己的设备周期，不知道 Host 当前 padding。共享环承担了两者之间的水位变化；当设备消耗速度和客户端提交节奏不一致时，环水位会先增长，再触发等待。下一步实验要记录水位曲线，不能只看 `backpressure_events == 0`。

## 建议的实验记录格式

每次实验保存一份 JSON 或纯文本记录，字段固定为：

```text
build_id
client_version
account
endpoint_id
quality_label
requested_engine
active_engine
input_rate/input_container/input_valid/input_encoding
output_rate/output_container/output_valid/output_encoding
stream_generation/format_generation
client_frames/converted_frames/endpoint_frames
source_waits/backpressure_events/underrun_events/dropped_frames
clock_position/clock_qpc/padding/ring_used
eos_submitted_generation/eos_complete_generation
first_buffer_submitted/device_started
```

听感备注单独放在记录末尾，写明时间点和现象，不替代这些数字字段。

## 单变量实验矩阵

| 场景 | 只改变的条件 | 预期状态变化 | 失败含义 |
| --- | --- | --- | --- |
| 首包不足一个设备周期 | 输入帧数 | `first_buffer_submitted=0`、`device_started=0`，记录 source wait | 若提前 Start，首包顺序错误 |
| 首个完整设备周期 | 输入补到 `device_frames` | 先 `PREFILLED`，后 `RUNNING` | 若顺序反过来，Host 启动契约错误 |
| 运行中仍有 padding、环暂时为空 | 只改变 padding | 不增加真实欠载 | 若计数增加，欠载统计范围过宽 |
| 运行中 padding 为零、环为空 | 只改变环水位 | 记录一次欠载区间 | 连续轮询持续增加，需改计数粒度 |
| 环空间小于完整批次 | 只改变 ring used | `remote_write()` 等待或失败，读写位置不动，丢帧不增加 | 出现短写或读位置前进，破坏全有全无语义 |
| Host 运行中退出 | 只终止 `NexAudioHost.exe` | `host_exit`、`FAILED`、当前代次结束 | 共享状态继续 RUNNING，进程级检测失效 |
| 恰好一个完整尾包 | 只改变 EOS 时机 | 环排空后进入 `DRAINING`，设备时钟越过目标才 COMPLETE | 提前 COMPLETE，尾部被截断 |
| 短于一个设备周期的尾包 | 只改变尾部长度 | 记录产品定义的短尾策略和错误码 | 与完整尾包走同一分支且无定义，EOS 语义不完整 |
| 输入与输出格式不同 | 只改变端点候选 | `convertedFrames` 增长，`input_*` 与 `output_*` 可区分 | 转换后仍显示 client-native，格式证据错误 |

## 远程时钟的根因链

当前路径把“已经交给端点的帧数”当成“设备已经播放的位置”。设备还有 padding 时，这两个位置天然相差一段队列；代码用一个固定设备周期抵消，遇到 padding 波动、首包启动、暂停恢复和迁移时，公开时钟会出现阶梯或回退修正。客户端随后依据这个时钟管理缓冲与换轨，音频表现就可能出现短暂停顿或跳过一小段。

Host 已回传同一时刻采集的 `IAudioClock::GetPosition()` clock tick、对应 QPC、频率和 `IAudioClient::GetCurrentPadding()` 帧数。客户端只把设备时钟用于公开时间轴，`endpoint_frames` 继续保留为样本提交统计，不再承担播放位置语义。真实设备上的连续性仍是下一阶段证据。

## EOS 的根因链

当前 `remote_submit_eos()` 位于最终销毁流程，随后由 `remote_wait_eos_complete()` 等待 Host 完成环排空和设备时钟收尾。短于一个设备包的尾部进入 Host 的短包首提交分支；空流仍可直接 COMPLETE。暂停、Stop、Reset、seek 和换轨的代次语义需要在真实客户端链路继续确认。

产品定义需要先固定：自然源结束是否等待设备排空；暂停是否保留代次；seek、换轨和“当前曲目重开”是否直接使旧代次失效；Host 提前退出时客户端是否立即切回进程内。定义确定后再补生产端信号与回归，不把 Stop 或 Reset 一律当成 EOS。

## 解码器探针的证据边界

`work/decoder_probe_agent.js` 只观测网易云已经加载的 FFmpeg/FLAC 解码入口，不修改实时路径。只有 `AVFrame` 元数据完整、音频平面位于可读内存、平面长度通过 `nb_samples × sample_bytes`、`linesize` 和 `AVBufferRef::size` 三重检查时，才保存 PCM；元数据不完整时只保存帧头。

探针能回答“解码入口交给了什么格式”，不能单独证明“源文件到端点逐样本一致”。端点证据仍来自 DLL 的 client、converted、endpoint 三层帧数和哈希。当前开发机的 `20261114-000222` 结果为 `decode_frames=0`，因此尚未形成源文件级证据。

### 2026-11-16：网易云本机解码入口静态核对

新增独立只读工具 `work/decoder_source_audit.ps1`，从 `D:/Installed_Program/CloudMusic` 根目录枚举 PE 导出，并重算既有探针 JSONL；不连接播放进程、不读写进程内存、不注入，也不改播放器源码。完整 JSON 输出为 `work/decoder-source-audit-2026-11-16.json`。

本次枚举 38 个根目录 DLL，找到以下可复核入口：

- `avcodec-58.dll` 导出 `avcodec_receive_frame`、`avcodec_decode_audio4`。
- `avutil-56.dll` 导出 `av_frame_get_sample_rate`、`av_frame_get_channels`、`av_frame_get_plane_buffer`、`av_get_sample_fmt_name` 等 accessor。
- `FLAC.dll` 导出 `FLAC__stream_decoder_get_sample_rate`、`FLAC__stream_decoder_get_bits_per_sample`、`FLAC__stream_decoder_process_until_end_of_metadata`、`FLAC__stream_decoder_init_stream`。

证据解释：

1. `20261114-000222` 只有 10 条事件：5 条导出清单、4 个钩子安装记录、1 条 ready；没有 `decode_frame`、`flac_init` 或 `flac_frame`。这只证明探针在 PID 15392 中找到并挂上了目标函数，不证明该 PID 在观测窗口处理了解码帧。
2. 较早的 `20261113-182026` 有 97 个 `avcodec_receive_frame` 返回事件，但 97 个样本全部无效：`nbSamples=1`、`sampleRate=0`、`channels=0`，格式名缺失，`metadataValid` 未成立。那次累积的 6,356,992 bytes 原始内存没有可靠的帧长/格式依据，不能视作 PCM，也不能读成源规格。
3. runner 找到任何带 `nexsink.dll` 的 PID 后会只返回该组 PID；网易云播放通常还存在 renderer 子进程。当前进程快照包含主进程、GPU 与 renderer；已有探针记录没有证明它们都被覆盖。JS agent 只在 attach 时枚举模块，不观察之后载入的新模块。
4. 现有 FLAC 钩子只在 init 函数上捕获 write callback，没有读取同一初始化调用的 metadata callback，也没有解析 STREAMINFO；若流在探针附加前已经初始化，init 与 write callback 均不会被重新登记。

下一次可复核方案：先在所有当前 cloudmusic PID 上布置只读观察，并在开始新曲或新解码流前启动；补抓 FLAC init_stream 的第 8 个参数（Frida `args[7]`，metadata callback），从 STREAMINFO 读取采样率、声道与 coded bits-per-sample。FFmpeg 路径则只接受 accessor 读出的完整 AVFrame，并把 `sample_fmt` 标成解码 PCM 格式。压缩编码没有天然等同 PCM 容器的“源位深”，需记录编码与服务端/容器元数据来源。未得到任一有效元数据时，`source_status` 仍为 `unavailable`。

### 2026-11-16：当前安装包与播放格式桥二次核对

将独立审计脚本扩展为直接读取当前 `D:/Installed_Program/CloudMusic/package/orpheus.ntpk`：跳过 100 字节产品头后，成功枚举 ZIP 的 1411 项，并扫描到 `pub/hybrid/app.chunk.5d77cd4.js` 与 `subApp.chunk.5d77cd4.js`。当前安装的 `cloudmusic.exe` 文件/产品版本为 `3.1.43.205627`；旧提取副本中的 chunk 名称较旧，当前判断以安装包内资源为准。

当前安装包仍包含 `AudioPlayer.getPlaybackInfo({playId})` 到 `Nt.call("audioplayer.getPlaybackInfo", e)` 的桥调用；遥测构造分别读取 `sourceAudioFormat`、`deviceAudioFormat`、`playAudioFormat`。这是“播放器可查询格式对象”的静态证据，不是字段值或定义的实测证据；现阶段不能断言 `sourceAudioFormat` 等于压缩文件元数据，也未确认单位、编码表达及返回时效。播放 URL 中的 `level`、`format`、`br` 仍不提供逐曲源位深证明；当前 bundle 扫描未发现逐曲 `bitsPerSample` / `bitDepth` 字段。

运行时只读查询后来完成了一次：通过 CDP 的页面 `webpackJsonp` 注册表取出模块 4 的 `AudioPlayer`，订阅 `playprogress` 取得真实 `playId=3456047164_TUIGVJ`，再调用 `getPlaybackInfo({playId})`。返回 `result=false`、`playId=""`，`sourceAudioFormat`、`deviceAudioFormat`、`playAudioFormat` 三个对象全部为 `{channels:0,samplerate:0,samplesize:0}`，`wholeCached=true`、`playedTime=0`。同一 `playId` 的 `getPlayedTime` 返回 `{playedAudioTime:31.25, playedTime:30.58, result:true}`，说明桥本身可用，但这次查询没有填充格式对象。未取得可信逐曲源元数据前，`source_status=unavailable`，WASAPI `input_*` 只标为客户端 PCM。后续缓存文件证据见下一节。

本次在当前 Pingyi 播放会话再次验证：`playId=254101_6KF56D`，`getPlayedTime` 返回 `playedAudioTime=94.89`、`playedTime=94.838`、`result=true`；`getPlaybackInfo` 返回 `result=false`，且 `sourceAudioFormat`、`deviceAudioFormat`、`playAudioFormat` 都是 `{channels:0,samplerate:0,samplesize:0}`。页面同时显示 `Hi-Res`，但这只是服务端档位标签。对应的 `.uc` 文件为 0 字节占位文件，当前没有可解析的容器头。该结果再次确认：实时桥可调用，当前版本没有提供可用的逐曲源格式字段。

随后在同一 Pingyi 会话对已完成的 `playId=254101_P4GDI2` 重试桥查询，得到一条完整记录：`sourceAudioFormat={format:flac, channels:2, samplerate:44100, samplesize:2}`，`playAudioFormat={channels:2, samplerate:96000, samplesize:2}`，`deviceAudioFormat={channels:2, samplerate:96000, samplesize:4}`，并且 `result=true`。这证明“播放事件后按同一 playId 重试”确实可以拿到逐曲源规格；此前的全零结果是时序/代次窗口问题，不能作为桥永远不可用的结论。`samplesize=2/4` 与 16/32 位的对应关系已经和容器及端点日志相互吻合，但协议仍应把它记录为 `samplesize`，在代码中显式转换展示，避免把单位猜写进源数据。

### 2026-11-16：缓存 FLAC STREAMINFO 与 32-bit 边界

同一轮运行时查询之后，客户端把当前曲目的音频缓存写入本地。缓存扩展名为 `.uc`；按公开逆向结果，其载荷是 FLAC 全文件逐字节异或 `0xA3`。最新缓存 `2722491891-999-6d9cb7e56e32c4175e640110237def89.uc` 与 `3456047164-1999-0418e5c420529b80831297f0e29c3119.uc` 都能在异或后得到 `fLaC` 魔数，说明这是可验证的容器头，不是播放 URL 或服务端档位。

| 缓存 | 大小 | FLAC STREAMINFO | 解释 |
| --- | --- | --- | --- |
| `3456047164-1999-0418e5c420529b80831297f0e29c3119.uc` | 75,786,090 bytes | `sample_rate=88200`、`channels=2`、`bits_per_sample=24`、`total_samples=19138824`、约 216.99 秒 | 文件名首段与观测到的 `playId=3456047164_TUIGVJ` 一致；这是当前 Hi-Res 曲目的源容器证据 |
| `2722491891-999-6d9cb7e56e32c4175e640110237def89.uc` | 19,824,640 bytes | `sample_rate=44100`、`channels=2`、`bits_per_sample=16`、`total_samples=9119039`、约 206.78 秒 | 另一条缓存，曲目关联未单独确认；只用于证明源文件并非固定 32-bit |

因此，页面里的 `32-bit / 88.2 kHz -> 24-bit / 88.2 kHz` 需要拆成两层：前一段是客户端交给 Hook 的 PCM 容器，后一段是独占端点接收的 PCM 容器；源缓存文件的 STREAMINFO 是 `24-bit / 88.2 kHz`。`32-bit` 不能直接反推成源文件位深，也不能单独证明采集链路损坏。若某首歌确实把 24-bit 源装进 32-bit PCM 传输，这属于解码/传输表示，仍需用端点哈希、样本比对或完整 AVFrame 才能判断是否损坏。

### 源规格的无缓存读取边界

当前 UI 已把三层对象分开：

```text
源规格：逐曲源容器或解码器元数据；目前未取得
当前档位：网易云页面选择的服务端质量档位，例如 Hi-Res、无损
客户端 PCM：网易云交给 Hook 的 WASAPI PCM
独占端点：Hook 最终提交给设备的 PCM
```

页面中的 `Hi-Res`、`无损` 可以直接读取当前播放条的质量标签，但它们只说明网易云当前选择的服务端档位，不提供逐曲位深或采样率。因此 UI 继续只显示“源规格：未取得”，档位只作为审计证据保存，不作为源规格替代品。

安装包静态代码还显示了 `AudioPlayer.getPlaybackInfo({playId})` 和 `sourceAudioFormat`、`deviceAudioFormat`、`playAudioFormat` 三个字段。真实查询曾返回 `result=false` 和三个零值对象，所以当前不能把它当成可靠的实时来源。后续可以继续在播放事件之后、同一 `playId` 上重试，并记录字段有效期；若仍为零值，保留“未取得”。

若播放 URL 在当前会话中可访问，可以研究只读 HTTP Range 请求：只取容器头和 STREAMINFO，解析完立即释放响应，不等待整首歌写入缓存。该方案仍需要确认 URL 的授权、过期时间、`.uc` 封装和服务器是否允许 Range；对加密或无容器头的响应不能推断源位深。若 Range 不可用，解码器 metadata callback 是下一条证据路径，仍需把 callback 与当前 `playId` 和 stream generation 绑定。

### 探针修正与无缓存 Range 实现（2026-11-16）

`work/decoder_probe_agent.js` 已按 FLAC 原生签名拆开初始化参数：

- `FLAC__stream_decoder_init_stream`：`args[6]` 是 write callback，`args[7]` 是 metadata callback；
- `FLAC__stream_decoder_init_file`：`args[2]` 是 write callback，`args[3]` 是 metadata callback。

metadata callback 现在只读取 `FLAC__StreamMetadata` 的 STREAMINFO 块，记录采样率、声道、coded bits-per-sample、总样本数、PID、decoder 地址和 callback 地址；不读取或保存完整源文件。初始化已发生在探针附加前时，仍不会补造事件。

新增 `work/range_source_probe.py` 作为独立只读探针。它对传入的当前播放 URL 发出 `Range: bytes=0-65535`，只在内存中读取响应并解析 `fLaC`/STREAMINFO，输出 HTTP 状态、Content-Range 和源规格；响应不会写入缓存。对 `.uc`、加密载荷、Range 被拒绝或头部不完整的响应返回 `unsupported_or_encrypted`、`request_failed` 或 `header_incomplete`，不把 `level`、`format`、`br` 推导成位深。

当前还没有把 URL 自动绑定到 `playId` 的运行时代码。桥查询已优先于 URL Range：`work/playinfo-current.js` 现在会在播放事件后对同一 `playId` 最多重试 8 次，只有 `result=true` 且三个源字段均有效时才结束。URL Range 仍作为桥字段缺失时的第二路径；若两者都没有可信结果，再使用解码器 metadata callback，并把结果与当前 generation 绑定。绑定成立前，UI 继续显示“源规格：未取得”。

同一轮 FLAC getter 探针在新播放流上没有捕获 `process_single`，FFmpeg 探针在新的播放进程上触发 Frida 注入内存分配失败，随后客户端进程与 CDP 端口退出；这两条运行时探针路线仍不足以作为源规格证据。缓存 STREAMINFO 是独立文件证据，`sourceAudioFormat` 桥字段在这轮真实 `playId` 查询中仍为零值。若要把 `source_status` 设为可用，必须同时满足“缓存文件或完整解码器元数据”与“当前 `playId`/当前代次”的绑定；仅桥对象存在、仅 UI 显示 32-bit、仅服务端档位或仅播放 URL 的 `level`/`format`/`br` 都不满足。

### 2026-11-16 14:01：修正后的解码器探针实测

在 Pingyi 会话对主进程及其 renderer/gpu 进程布置修正后的探针，并用脚本触发播放。主进程捕获到 1,526 次 `avcodec_receive_frame`，有效 AVFrame 均为 `s16 / 44100 Hz / 2 ch / 1024 samples`；PCM 平面按 4,096 字节读取并通过 `AVBufferRef` 大小检查。该记录证明当前解码输出 PCM 的格式读取路径已经工作，但它仍属于 decoder output，不能单独声明压缩源文件的编码位深。该窗口没有发生 FLAC 初始化或 STREAMINFO metadata callback，因此没有新增源文件规格证据。

## 进入运行阶段前的门槛

1. 文档中的当前实现、测试名称和日志字段保持一致。
2. 进程内路径与 audiohost 路径分别采集同一曲目、同一区段、同一端点的格式和提交数据。
3. 远程时钟、EOS、背压、Host 退出各自有单变量实验，不在一次试听里同时改变多个条件。
4. Pingyi 会话结束后关闭测试用网易云、NexExclusive 与 NexAudioHost，恢复用户当前账户的桌面状态。

## 2026-11-16：同格式重建循环的根因与复核

真实 Pingyi 播放日志曾在同一代次内每隔数秒出现一次同格式 `IAudioClient` 交接。回归原始路径后，`fr_ReleaseBuffer()` 通过 `ring_reserve()` 写入客户端环；该函数使用 `TryEnterCriticalSection(&g_lock)`，恰好遇到 drain 线程复制一段 PCM 时就返回空指针，调用方把这次正常的写入判成 `AUDCLNT_E_DEVICE_INVALIDATED`。网易云随后重建同格式客户端，旧包被丢弃，听感出现周期性卡顿、爆音和电流声。

修复包含三点：环预留等待这段短临界区；同格式交接保留同一设备代次和各自 staging 缓冲；交接后的 `Start` 返回成功，表示新客户端已经加入正在运行的代次。新增回归 `test_ring_reserve_waits_for_drain()` 先让旧实现失败，再验证锁竞争不会丢包；同格式交接回归同时验证旧代理的未完成包和后续包都能提交。

部署到 Pingyi 安装目录后的连续观测结果：约 150 秒播放窗口内 `same-format client handoff=0`、`sink fault=0`、`empty-buffer=0`，drain 日志的最大提交间隔为 31–32 ms。该结果覆盖了触发重建循环的协议边界；听感仍需在同一首歌上由用户完成最终确认。

本轮继续追踪到一处更窄的并发窗口：`fr_ReleaseBuffer()` 原先先通过 `ring_reserve()` 取得环写地址，释放 `g_lock` 后才复制 staging PCM；排空线程可以在复制期间消费或压缩环，令目标地址失效。这个错误不会必然增加 `empty-buffer`，但会把一包样本写到错误偏移，符合连续换轨后出现细碎底噪的表现。现已新增 `ring_write_copy()`，把地址选择、环压缩、PCM 复制和写指针推进放进同一临界区，静音包也走同一路径。回归和 Pingyi 三首连续换轨观察均通过，仍需长时间播放复核；一次同格式交接窗口记录到 63 ms 提交间隔，后续需单独收窄。

## PCM 静态对照结论

OpenHome 的做法支持“音频规格”应优先指向源格式，但它具备 Apple 播放图的源元数据接口；当前 NexExclusive 只有 WASAPI 客户端边界，因此暂时无法取得同等证据。设置窗口中的四行信息仍需区分客户端 PCM 与独占端点，不能把 `32 bit` 传输容器写成源文件位深。网易云源元数据继续由桥查询和后备探针提供，来源确认前不新增“源文件 32 bit”之类的推断。

## 2026-11-16：1–7 顺序验收记录

本轮使用当前源码构建，客户端版本 `3.1.43.205627`，账户为 Pingyi，端点为 `扬声器 (MOONDROP MM3A)`。构建前重新执行 `bash mod/build.sh all` 和 `bash mod/build.sh test`；回归输出为 `ALL TESTS PASSED: 0 failure(s)`。

### 1. 远程诊断与 2. 端点 PCM

`sink.log` 与 `NexAudioHost.log` 已记录提交间隔、QPC、generation、ring 水位、设备 padding、设备 clock position、Host 状态、backpressure、underrun 和 dropped frames。同一端点分别抓取了进程内和 audiohost 路径的设备 PCM：

| 路径 | 文件 | 端点格式 | 帧数 | 时长 | RMS | 峰值 | 静音帧比例 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| in-process | `work/current-candidate/inprocess-device.pcm` | 44.1 kHz / 2 ch / 24-bit packed | 5,928,974 | 134.44 s | 0.2237 | 1.0000 | 0.139% |
| audiohost | `work/current-candidate/remote-compare.pcm` | 44.1 kHz / 2 ch / 24-bit packed | 14,147,280 | 320.80 s | 0.2829 | 1.0000 | 0.317% |

完整字段见 `work/current-candidate/pcm-comparison-20261116.json`。两份文件来自不同曲目区段，统计值用于发现断流、突变和格式错配；它们不能证明同一输入的逐样本一致。

### 3. 自然 EOS

本轮尚未得到真实曲目自然结束的证据。此前把播放位置跳到极大值的实验属于 seek/reset，不能替代自然 EOS；当前记录仍是 `natural_eos_observed=false`。桩回归覆盖 EOS 字段、短尾包和设备 clock 收尾，但真机必须在曲目自然到达末尾后看到 `eos_submitted_generation == eos_complete_generation`，才能把这项改成已验收。

### 4. Host 提前退出

远程流运行中强制结束 `NexAudioHost.exe` 后，客户端日志出现：

```text
audiohost: host_exit generation=1 state=running
sink fault: device submission 0x8007042B
drain: thread exit (gen 1, running=0, retired=0, fault=0x8007042B)
```

这次还记录了 `dropped=0`。进程级退出检测能够把共享头部仍为 RUNNING 的情况转成客户端故障。

### 5. 背压水位

远程路径约 5 分钟窗口中，`ring_capacity=22050`，`ring_used` 长时间约 20,000–21,000 帧，设备 `padding` 约 3,700 帧，`backpressure` 从 0 增到 6,402；同时 `underrun=0`、`dropped=0`。日志中的最大提交间隔约 78–94 ms，说明完整设备周期会等待环空间；还需要把这段水位曲线与同一曲目的听感和同曲目 PCM 对照联系起来，才能判断是否存在真实的播放空洞。

### 6. 进程内与 audiohost 对照

两条路径使用同一端点和同一输出格式，采样文件均为 24-bit packed stereo。由于采集发生在不同曲目区段，本轮只完成格式、帧数、RMS、峰值、静音比例和突变统计；逐字节对照仍需让同一 `playId` 从起点分别经过两条路径，并固定端点、质量档位和代次。

### 7. 文档状态

本节、`PROJECT_STATUS.md`、`docs/ROADMAP.md`、`docs/BUILDING.md` 已同步本轮构建、回归、采样和 Host 退出证据。设备矩阵、Realtek、HDMI、多声道及 176.4/192 kHz 仍按当前决定暂缓；自然 EOS 和同曲目逐字节对照保留为下一轮 P1 验收。
