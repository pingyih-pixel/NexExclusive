# NexExclusive 运行与验收手册

本文只负责运行中的组件关系、注入、日志阅读、Pingyi 账户测试和清理。源码构建、回归编译与发布包制作见 [BUILDING.md](BUILDING.md)；音频正确性判断见 [AUDIO_CORRECTNESS_AUDIT_2026-11-16.md](AUDIO_CORRECTNESS_AUDIT_2026-11-16.md)。

## 运行结构

- `NexExclusive.exe` 是常驻宿主，读取 `hook.cfg`，寻找 `cloudmusic.exe`，注入 `nexsink.dll`，提供 `127.0.0.1:9334` 配置接口，并在客户端重启后再次处理。
- `nexsink.dll` 在网易云进程内挂接 `IMMDevice` 与 `IAudioClient`，负责客户端 PCM 获取、格式协商、转换、缓冲和端点迁移。
- `NexAudioHost.exe` 是实验性的独立 WASAPI 端点进程。它读取共享 PCM 环、打开端点、提交设备缓冲并回写状态。
- `nexui.dll` 由宿主的界面线程注入主进程的每个子进程，只在渲染进程里动作：在网易云主页面里执行 `ui/panel.js` 与 `ui/simplify.js`，不需要 9333。它用命名事件 `Local\NexUI-Ready-<pid>`、`Local\NexUI-Unsupported-<pid>` 告诉宿主结果，日志写在 `nexui.log`。
- 渲染进程报不支持、注入失败或 20 秒内未就绪时，宿主回退到 CDP 端口 `127.0.0.1:9333` 注入同样的脚本。同目录放一个 `nexui.disable` 文件可以强制走 CDP。界面链路与音频链路相互独立；面板消失不等于音频 DLL 没有加载。

## 唯一候选与安装目录

源码和构建入口固定为：

```text
D:\Coding\NeteaseExclusive\mod
```

`work\playback-fix` 只用于保存历史构建、诊断程序和日志。测试前应从 `mod` 重新构建到一个明确的输出目录，再把同一批文件复制到安装目录 `%LOCALAPPDATA%\NexExclusive`。不要在多个 `v` 目录之间选择 DLL、Host 和面板脚本。

替换前先结束当前账户里的三个进程：

```powershell
Get-Process NexExclusive,NexAudioHost,cloudmusic -ErrorAction SilentlyContinue |
  Stop-Process -Force
```

若安装目录写入受到权限限制，以管理员 PowerShell 完成复制；`host.cfg` 和用户修改过的 `hook.cfg` 不要被测试包的空文件覆盖。

## 启动 Pingyi 会话

优先使用当前 Pingyi 桌面账户已经打开的网易云窗口。设置页默认由 `nexui.dll` 在页面里加载，普通资源管理器启动、没有 CDP 端口的实例也会出现面板；宿主后启动时，面板要等页面下一次调用原生接口才出现，空闲页面可能要一分钟左右，操作一下界面会更快。宿主拉起客户端时仍带 `--remote-debugging-port=9333`，供回退使用。

音频测试的最小顺序：

1. 确认宿主、客户端和端点属于同一 Windows 账户与完整性级别。
2. 只播放一首歌，记录客户端质量档位、输入 PCM、端点格式和当前 engine/deployment。
3. 先测试进程内路径，再单独启用 `audiohost=1`；两次使用同一端点和同一时间段。
4. 运行约 30 秒后保存 `host.log`、`sink.log`、`/api/status` 和构建身份。
5. 测试结束后关闭网易云、NexExclusive、NexAudioHost，确认 Pingyi 可以重新打开网易云。

## 日志验收

宿主日志在安装目录的 `host.log`，至少确认：

```text
HTTP: 配置服务已监听 127.0.0.1:9334
注入 pid=... -> LoadLibraryA=...
模块已挂载到 pid=...
界面脚本已就位 page=...
```

DLL 日志在 `sink.log`，一次真实播放至少需要看到：

```text
=== nexsink attached ===
config: ...
sink ENGAGED ...
drain: thread up ...
sink: hardware started ...
```

进程内路径重点记录 `input`、`output`、`client_frames`、`endpoint_frames`、`converted_frames`、首个差异和 hash。audiohost 路径另外记录 `source_waits`、`backpressure`、`underrun`、`currentGeneration`、`eosSubmittedGeneration` 和 `eosCompleteGeneration`。这些字段来自当前代次时才可以用于对照。

`client-native` 只证明客户端 PCM 到独占端点的映射；它不能证明网易云解码前的源文件逐样本一致。`44.1 kHz / 32 bit` 也只说明客户端 PCM 的容器或编码格式，源质量档位要看网易云页面和服务端信息。

## 面板缺失的判断

| 现象 | 先看什么 |
| --- | --- |
| 没有“NexExclusive”入口 | `host.log` 的 `界面链路：` 行。`渲染进程就绪` 时看 `nexui.log` 有没有 `panel loaded`；`回退到 CDP` 时看括号里的原因，再检查 9333 和 CDP 页面目标 |
| `nexui.log` 有 `unsupported` | 渲染进程挂接不可用（CEF 版本或结构体大小变了等），宿主会走 CDP；原因写在同一行 |
| 有面板，没有 `sink ENGAGED` | 页面脚本已注入，但尚未出现新的音频初始化或播放调用 |
| 有 `sink ENGAGED`，没有面板 | 音频 DLL 已工作，UI 注入链路单独失败 |
| `OpenProcess` 错误 5 | 结束普通权限宿主，再让宿主与网易云处于同一完整性级别 |
| `0x8889000A` | 端点仍被上一代流占用，先清理旧 Host 和旧客户端 |
| `0x88890008` | 设备拒绝当前格式，切换到原率优先以测试转换路径 |

## 状态接口

`GET http://127.0.0.1:9334/api/status` 用于查看当前运行状态。重点字段如下：

```text
requestedEngine
requestedDeployment
activeEngine
deployment
convertedFrames
sourceWaits
backpressure
underrunEvents
currentGeneration
eosSubmittedGeneration
eosCompleteGeneration
```

如果字段长期停在上一代、远程状态已关闭或 Host 已退出，应先结束当前测试会话，再重新建立一代音频流。不要只依据面板缓存文字判断当前输出路径。

## 测试后清理

测试结束时执行：

```powershell
Get-Process NexAudioHost,NexExclusive,cloudmusic -ErrorAction SilentlyContinue |
  Stop-Process -Force
```

确认没有残留进程后再启动用户原来的网易云入口。若安装目录中的 DLL、Host 或面板脚本需要恢复，使用测试前保存的哈希逐个核对，不从旧版本目录盲目复制。
