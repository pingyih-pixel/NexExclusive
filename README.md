# NexExclusive

[![build](https://github.com/pingyih-pixel/NexExclusive/actions/workflows/build.yml/badge.svg)](https://github.com/pingyih-pixel/NexExclusive/actions/workflows/build.yml)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**简体中文** · [English](README.en.md)

给网易云音乐 PC 客户端加上 WASAPI 独占输出：音频绕过 Windows 混音器，以音源本身的采样率直接交给声卡。适用于 Windows 10/11 x64。

<!-- TODO: 补一张设置面板截图，存为 docs/panel.png，再在这里引用 -->

适合使用外置 DAC、耳放，希望 44.1 kHz 的曲目就以 44.1 kHz 播出的听众。

## 特性

- **独占输出**：音频不经过系统混音器，没有那层强制的 48/96 kHz 重采样。
- **跟随音源**：44.1 的曲目以 44.1 播放，96 的曲目以 96 播放。设备拒绝这个格式时才做转换，面板里会标明本次是「直通」还是「转换过」。
- **切换时不中断**：切歌、拖动进度、更换输出设备时，新旧输出之间平滑交接。
- **装好即常驻**：一键安装，托盘常驻，开机自启；客户端重启或页面刷新后自动重新接入。

## 安装前须知

安装之前，下面三件事值得先读完。

**Defender 会隔离程序。** 宿主程序用 `CreateRemoteThread` 把 DLL 加载进客户端进程，这个行为本身触发了 Defender 的行为检测（`Behavior:Win32/DefenseEvasion.A!ml`）。程序会在 30–60 秒内被隔离，快捷方式、自启项和卸载项也一并被删除。项目认定这是误报，申诉材料见 [docs/defender-false-positive.md](docs/defender-false-positive.md)，源码全部公开，可以自行核对。

安装前，以管理员身份打开 PowerShell，给两个文件加上排除项：

```powershell
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\NexExclusive\NexExclusive.exe"
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\NexExclusive\nexsink.dll"
```

直接从 zip 展开出的目录运行时，同样给该目录下的这两个文件加上。愿意加排除项才使用本程序；也请保持实时保护开启。

**与网易公司无关。** 这是非官方工具。往第三方客户端进程里加载 DLL，有违反其用户协议的风险，后果自负。

**系统与设备。** 需要 Windows 10/11 x64。播放设备要在 Windows 声音属性的「高级」页里允许应用程序独占控制。客户端版本只验证过 3.1.42.205594，其余版本见[兼容性](#兼容性)。

## 下载与首次运行

**目前没有正式的 Release。** Defender 误报还没有结论，此时发布一个装完半分钟就被删除的版本，会白白用掉第一印象，所以 Release 暂缓，原因记录在 [docs/ROADMAP.md](docs/ROADMAP.md)。取得程序有两种途径：

1. 打开 [Actions](https://github.com/pingyih-pixel/NexExclusive/actions/workflows/build.yml)，进入最近一次成功的构建，下载产物 `NexExclusive-package`（下载产物需要登录 GitHub）。包内有 `NexExclusive-x.y.z.zip` 和 `SHA256SUMS.txt`。
2. 自行构建，步骤见 [docs/BUILDING.md](docs/BUILDING.md)。

Release 发布后，这里会换成下载链接。

下载后先核对哈希，输出应与 `SHA256SUMS.txt` 中对应的一行一致：

```powershell
Get-FileHash .\NexExclusive-x.y.z.zip -Algorithm SHA256
```

首次运行：

1. 展开 zip，双击 `NexExclusive.exe`。程序会询问是否安装：装到 `%LOCALAPPDATA%\NexExclusive`，写入开机自启，建立开始菜单快捷方式；安装步骤不需要管理员权限。
2. 安装完成后自动拉起客户端，右下角出现托盘图标。之后照常打开网易云即可。
3. 托盘右键菜单：打开网易云音乐 / 查看日志 / 打开程序目录 / 退出（停止注入）。

命令行：

```
NexExclusive.exe --install [--client "D:\...\cloudmusic.exe"]
NexExclusive.exe --show
NexExclusive.exe --uninstall
NexExclusive.exe --help
```

客户端装在非默认位置、程序探测不到时，用 `--client` 指定 `cloudmusic.exe` 的路径。
`--show` 会复用已有的网易云主进程并尝试呼出窗口；没有进程时才启动新实例。若日志出现 `OpenProcess(<pid>) 失败 5`，请结束普通宿主后以管理员身份启动宿主。

## 设置面板

设置页会新增一个独立的「NexExclusive」入口，里面集中显示独占状态、输出模式、输出规格和兼容处理。播放页仍保留「独占输出」面板，便于从当前播放设置快速查看。

播放页网易云原生的「无损」标签属于服务端音质选择，NexExclusive 不会修改它。独占模块只会在其左侧增加一张「音频规格」卡片，显示客户端 PCM 到独占端点的采样率、位深和 `Bit-perfect` / 转换状态；悬浮后可以查看设备与完整协商信息。

| 项 | 作用 | 何时调整 |
| --- | --- | --- |
| 启用独占 | 总开关，关闭后回到客户端默认输出 | 想临时对比共享模式的声音，或排查问题时 |
| 位深协商 | 原率优先跟随设备可用格式；严格原样跟随客户端格式 | 不在界面固定选择，避免人为改写源格式 |
| 采样率 | 跟随音源（默认），或强制 44.1 / 48 / 88.2 / 96 kHz | 想让所有曲目统一输出到同一个采样率时 |
| 重采样策略 | 带宽优先（默认）或整数比优先 | 88.2 kHz 音源在设备只接受 44.1 / 48 / 96 kHz 时，想优先 44.1 kHz 时调整 |
| 缓冲帧数 | 自动，或 512–4096 | 出现爆音时调大，在意延迟时调小 |
| 输出策略 | `原率优先（推荐）` 先尝试源格式，设备拒绝时继续使用独占转换；`严格原样` 只接受源格式 | 日常播放使用原率优先；验证设备是否支持源格式时使用严格原样 |
| 兼容转换 | 原率优先模式下允许独占端点转换采样率或位深 | 默认开启；严格原样会自动关闭 |
| 抖动 | 关闭 / 开启 / 自动 | 默认自动，在降低位深或做过重采样时生效，一般不用动 |
| 开头淡入 | 0–20 ms | 起播有爆音时调大 |
| 共享回退 | 不打开共享输出 | 当前产品路径固定关闭，避免把共享输出误认为独占 |
| 独立音频宿主 | 将严格原样路径的独占端点和实时提交移到 `NexAudioHost.exe` | 默认关闭，属于实验引擎；原率优先下的转换仍由 DLL 内部完成 |

同样的设置写在 `hook.cfg` 里（纯文本 `key=value`），改完重启客户端生效。面板是它的前端。`audiohost=1` 需要发布包里的 `NexAudioHost.exe`，独立进程只接受客户端原格式 PCM。
严格原样的验证对象是「客户端 PCM → 独占端点 PCM」；设备支持源格式时可以做到 bit-perfect 输出，但它不代表网易云解码前的源文件已经完成 bit-perfect 证明。设备不支持源格式时，面板会提示切换到原率优先，当前曲目不会退回共享输出。播放栏的音频规格详情会同时标出这条证据边界。

## 工作原理

客户端没有运行时，宿主程序会先尝试使用客户端的调试启动路径；当前未验证版本优先保证网易云窗口出现，再由看护循环处理主进程。随后宿主用 `CreateRemoteThread` 加 `LoadLibraryA` 把 `nexsink.dll` 加载进客户端进程。DLL 在进程内挂接 `IMMDevice` 与 `IAudioClient` 的 vtable：客户端协商输出格式时，由 DLL 依据设备在独占模式下的真实能力作答，缓冲、重采样和切换时的交接也在 DLL 里完成。

设置面板由宿主通过本机的 CDP 端口（9333）注入客户端页面。面板与宿主之间走 `127.0.0.1:9334` 上的配置接口，读写 `hook.cfg` 与 `ui.json`。如果当前客户端没有开放 9333，音频 DLL 仍可工作，但设置页不会出现自定义入口。

程序运行期间，本机回环地址上开着 9333 和 9334 两个端口，同一台机器上的其他进程可以访问。宿主的全部网络通信都发生在回环地址上，DLL 本身不含网络代码。客户端的文件不会被修改。

## 兼容性

| 项 | 已验证 | 尚未验证 |
| --- | --- | --- |
| 系统 | Windows 11 26200 家庭版 | Windows 10 22H2 |
| 客户端 | 3.1.42.205594 | 其他版本。日志里会标「未经验证」；客户端更新若改动了内部布局，会出现没有声音也没有报错的情形 |
| 设备 | HyperX Cloud III（44.1 / 48 / 96）；AudioQuest DragonFly MM3A（另有 88.2） | Realtek 集成声卡、HDMI 输出、352.8 / 384 kHz、5.1 / 7.1 多声道 |

## 已知问题

- **Defender 误报**：见[安装前须知](#安装前须知)。
- **独占下调不了软件音量**：WASAPI 的固有限制。端点有硬件音量节点时，系统音量条仍然有效。本项目明确不做模块侧的数字衰减，不拿比特换音量。
- **无管理员权限卸载时会留下 exe**：程序正被自己占用，此时无法登记「重启后删除」。关掉程序后手动删除目录即可。
- **设置面板仍缺少跨版本回归**：当前版本已验收独立模块、右侧对齐、编辑保存/取消和无损规格框；不同网易云版本仍需单独确认节点选择器。
- **88.2 kHz 的音源**：在只支持 44.1 / 48 / 96 的设备上会转换到 96 kHz，这是「保带宽」的取舍，代价是一次非整数比的重采样。

完整的限制清单和后续计划见 [docs/ROADMAP.md](docs/ROADMAP.md)。

## 文档

- [docs/BUILDING.md](docs/BUILDING.md)：从源码构建、运行回归测试、打发布包
- [docs/DEVELOPMENT_RUNTIME.md](docs/DEVELOPMENT_RUNTIME.md)：编译、注入、宿主看护、日志验收与常见故障
- [docs/OPENHOME_NETEASE_RESEARCH.md](docs/OPENHOME_NETEASE_RESEARCH.md)：OpenHome 架构借鉴与网易云本地数据库结论
- [docs/ROADMAP.md](docs/ROADMAP.md)：限制清单与后续计划
- [docs/defender-false-positive.md](docs/defender-false-positive.md)：向微软提交误报的材料

## 许可

[MIT](LICENSE)。
