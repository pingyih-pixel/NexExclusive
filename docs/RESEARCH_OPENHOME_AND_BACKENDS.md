# 外部后端研究：OpenHome、DLNA、RAAT 与本项目边界

状态：研究记录。本文只负责外部项目与未来后端的资料整理，不定义当前 WASAPI 远程协议，也不记录本机运行验收。

## OpenHome 研究结论

参考对象是公开项目 `dxdxdxlm123/AppleMusic-OpenHome` 的 `r-test-v0.3.5` 与 `r-test-v0.3.7`。可借鉴的结构有三层：宿主看护客户端，Hook 把客户端 PCM 写入跨进程环，独立 AudioHost 拥有设备句柄；验证则拆成客户端 PCM、转换后 PCM、端点 PCM。`BitPerfectVerifier` 只在映射没有改变样本时标记原样输出。

可复用的工程经验：

- 音频线程使用 MMCSS，并在退出时成对恢复；
- 首包预填、设备启动和运行中欠载分开计数；
- 独立宿主握手同时校验协议、布局和构建身份；
- EOS 使用源结束、环排空和设备时钟三个条件；
- 网络后端拥有自己的队列和低水位指标，不复用 WASAPI 欠载字段。

### OpenHome 的“音源规格”来源

这点需要和当前 NexExclusive 分开看。OpenHome 的 Hook 接口里有
`IApplePlayback::get_AudioFormatSampleRate()` 与
`get_AudioFormatBitDepth()`（`work/openhome-r-test-v0.3.5/.../mod/src/ApplePlayback.h:81-83`）。
`AudioFormatPolicy.h` 把这组播放图元数据放进 `ApplePcmFormat`，再复制到
`LocalPcmCandidate.sourceBitDepth`；`AudioCoreCoordinator` 另外保留
`SourceFormat()` 和 `OutputFormat()` 两个对象。这样它能同时回答“音源声明是什么”和
“端点实际使用什么格式”。

OpenHome 的 `SharedAudioRing.h` 又明确写着跨进程环传输的是 canonical Q31
interleaved int32。也就是说，它展示的源格式来自播放图元数据，环内的 32 位是传输
布局；两者没有混成一个字段。这正是当前项目需要补的分层。

不能直接移植的内容包括 Apple 私有数据库、解密密钥、封面表、解码器偏移和质量锁定逻辑。它们属于 Apple Music 的数据结构，没有网易云对应证据。

## v0.3.7 新信息

公开说明显示，后端升级集中在调度与网络队列：依据物理核效率等级选择音频核心，重新启用 MMCSS 动态提权，给 DLNA/RAAT 生产线程采用统一调优策略，并让网络队列容量不低于启动所需容量。这些信息能帮助本项目设计线程和网络队列，但不能替代 WASAPI 的设备时钟、padding 与端点格式验证。

## DLNA / OpenHome

DLNA/OpenHome 是网络渲染器模型：电脑提供 HTTP 音频资源，再通过 UPnP 控制点发送播放、暂停、停止和队列命令。设备会主动拉取资源，所以它和 WASAPI 的本机 push 路径不同。后续实现至少需要：

- SSDP 发现与设备描述缓存；
- `GetProtocolInfo` 能力判断；
- WAV/FLAC 流服务、内容类型、`Content-Length` 与 Range；
- `AVTransport` 的当前曲目、下一曲、seek 和断线重连；
- 网络 EOS、队列低水位和重连状态。

协议层可以预留：

```text
engine           = dlna
deployment       = network_service
target_id        = renderer UDN
network_location = device description URL
```

这些字段只表达未来接口，不能写成当前支持。

## RAAT

RAAT 需要设备发现、认证、控制地址、接受的 PCM 规格、队列深度和断线重连证据。没有真实设备时，只能记录接口假设，不能从公开源码推导网易云项目已经支持 RAAT。它的网络低水位、重连和播放确认要使用独立指标。

## ASIO

ASIO 属于本机驱动后端，需要驱动枚举、回调生命周期、采样率切换、设备冲突和线程优先级策略。它与当前 WASAPI audiohost 共享“部署位置”概念，但拥有不同的设备时钟和提交接口，暂不加入当前实现。

## 本项目的取舍

当前只验收 WASAPI 独占。外部研究的结果进入两个位置：

1. `REMOTE_AUDIO_PROTOCOL_DESIGN.md` 只保留字段、状态和握手需要的抽象；
2. `ROADMAP.md` 记录实现优先级和发布门槛。

长时大环、本地曲库、网络遥控、ASIO、DLNA/OpenHome、RAAT 都保持独立议题，不能混入本轮音频正确性实验。

当前项目在 `IAudioClient` 边界只能可靠取得客户端 PCM 的 `input_*`。没有网易云
播放图或解码器元数据时，不能把它称为源文件规格。源规格要单独增加来源、采样率、
有效位深、编码和可信度字段；拿不到时界面应显示“源规格未取得”，同时保留客户端
PCM 与独占端点两行。
