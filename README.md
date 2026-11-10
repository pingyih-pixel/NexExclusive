# NexExclusive

[![build](https://github.com/pingyih-pixel/NexExclusive/actions/workflows/build.yml/badge.svg)](https://github.com/pingyih-pixel/NexExclusive/actions/workflows/build.yml)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

给网易云音乐 PC 客户端加上 WASAPI 独占输出：音频绕过 Windows 混音器，按源采样率直接交给声卡。

WASAPI exclusive output for the NetEase Cloud Music desktop client. Windows 10/11 x64.

## 特性

- **独占输出**：不经系统混音器，没有那层强制的 48/96 kHz 重采样
- **跟随音源**：44.1 放 44.1，96 放 96。设备不收这个格式时才转换，并在面板里标明「直通」还是「转换过」
- **不打断播放**：切歌、拖进度、换输出设备时按代交接
- **装完就不管**：一键安装、托盘常驻、开机自启，客户端重启或页面刷新会自动补回去

## 使用

1. 解压 `NexExclusive-x.y.z.zip`，双击 `NexExclusive.exe`。首次运行会问要不要安装——装到 `%LOCALAPPDATA%\NexExclusive`，写开机自启，建开始菜单快捷方式，**不需要管理员**。
2. 装完自动拉起客户端，右下角出现托盘图标。之后正常开网易云就生效。
3. 托盘右键：打开网易云音乐 / 查看日志 / 打开程序目录 / 退出（停止注入）。

客户端页面里会多出一个「独占输出」面板，可以调：

| 项 | 说明 |
|---|---|
| 启用独占 | 总开关，关掉就退回客户端的默认输出 |
| 输出位深 | 16 / 24 位 |
| 采样率 | 跟随音源（默认）或强制 44.1 / 48 / 88.2 / 96 kHz |
| 缓冲帧数 | 自动或 512–4096，越小延迟越低、越容易爆音 |
| 兼容转换 | 设备拒绝源格式时是否允许转换 |
| 抖动 | 关闭 / 开启 / 自动，降位深时用 |
| 开头淡入 | 0–20 ms，消掉起播的爆音 |
| 严格模式 | 打不开独占就静音，而不是偷偷退回共享模式 |

同样的设置也写在 `hook.cfg` 里（纯文本 `key=value`），改完重启客户端生效。面板只是它的一个前端。

命令行：

```
NexExclusive.exe --install [--client "D:\...\cloudmusic.exe"]
NexExclusive.exe --uninstall
NexExclusive.exe --help
```

## 从源码构建

已经下载了 zip 的话不需要看这节——里面就是编译好的产物。只有想改代码或自己验证时才需要。

**依赖**：Windows 10/11 x64、[Zig](https://ziglang.org/download/) 0.17.x、Git Bash（或 WSL 的 bash）。**不需要** Visual Studio，也不需要 Windows SDK——资源文件用 `zig rc` 编译。

```bash
git clone https://github.com/pingyih-pixel/NexExclusive.git
cd NexExclusive

bash mod/build.sh all     # → mod/NexExclusive.exe、mod/nexsink.dll，以及配置和界面脚本
bash mod/build.sh test    # 19 组回归，不开设备、不注入进程
bash release.sh           # → dist/NexExclusive-<版本>.zip + SHA256SUMS.txt
```

`build.sh` 按这个顺序找 zig：`$ZIG` → `PATH` 里的 `zig` → `mod/zig.path`。没装进 PATH 的话：

```bash
ZIG=/c/zig/zig-x86_64-windows-0.17.1 bash mod/build.sh all
```

产物默认落在 `mod/`，用 `OUT_DIR=/绝对路径` 可以改。`build.sh` 还支持单独构建 `sink`（只有 DLL）或 `host`（只有 exe）。

发布包只有 6 个文件：`NexExclusive.exe`、`nexsink.dll`、`hook.cfg`、`ui.json`、`ui/panel.js`、`ui/simplify.js`。`release.sh` 会顺带生成 `README.txt` 和校验和。

## 已知问题

**Windows Defender 会误报。** 程序要往客户端进程里加载 DLL，行为特征像注入类木马，会被判成 `Behavior:Win32/DefenseEvasion.A!ml`，30–60 秒内隔离 exe，并连带删掉快捷方式、自启项和卸载项。

这是误报——不联网、不加密、不装驱动、不碰安全软件、不改客户端文件。但确实会挡住安装。要用就先加排除项（管理员 PowerShell）：

```powershell
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\NexExclusive"
```

从解压目录直接跑的话，把那个目录也加进去。**不想加就别用，不要为此关掉实时保护。**

其他：

- **独占下调不了软件音量**，这是 WASAPI 的限制，不是 bug。端点有硬件音量节点时系统音量条仍然有效。本项目明确不做模块侧数字衰减——不拿比特换音量。
- **卸载后可能留下 exe**：程序正被自己占用，且无管理员权限无法登记「重启后删除」。关掉后手动删目录即可。
- **只在一台机器上验过**：Windows 11 26200 + HyperX Cloud III（44.1/48/96）、AudioQuest DragonFly MM3A（额外支持 88.2）。其他设备行为可能不同。
- **只在一个客户端版本上验过**：3.1.42.205594。其他版本会在日志里标「未经验证」。

更完整的限制清单和后续计划见 [docs/ROADMAP.md](docs/ROADMAP.md)。

## 许可

[MIT](LICENSE)。与网易公司无关，非官方工具；往第三方客户端进程里加载 DLL 可能违反其用户协议，风险自负。

---

## English

### Features

- **Exclusive output** — bypasses the Windows mixer, so there is no forced resample to 48/96 kHz
- **Follows the source** — 44.1 plays at 44.1, 96 at 96. It only converts when the device rejects the format, and says which happened ("passthrough" vs "converted") in the panel
- **Never interrupts playback** — track changes, seeks and endpoint switches hand over by generation
- **Install once** — one-click install, tray resident, autostart; re-attaches after a client restart or page reload

### Usage

1. Unzip `NexExclusive-x.y.z.zip` and run `NexExclusive.exe`. On first launch it offers to install to `%LOCALAPPDATA%\NexExclusive`, add an autostart entry and create a Start Menu shortcut — **no admin required**.
2. It launches the client and shows a tray icon. After that, just use the client normally.
3. Right-click the tray icon: open the client / view logs / open program folder / quit (stops injection).

An "Exclusive output" panel appears inside the client with these settings: enable, output bit depth (16/24), sample rate (follow source, or forced 44.1/48/88.2/96 kHz), buffer frames (auto or 512–4096), compatibility conversion, dither (off/on/auto), fade-in (0–20 ms) and strict mode (go silent rather than silently fall back to shared mode).

The same settings live in `hook.cfg` as plain `key=value`; restart the client to apply. The panel is just a front-end for that file.

Command line:

```
NexExclusive.exe --install [--client "D:\...\cloudmusic.exe"]
NexExclusive.exe --uninstall
NexExclusive.exe --help
```

### Build from source

If you downloaded the zip you don't need this section — it already contains the binaries.

**Requires**: Windows 10/11 x64, [Zig](https://ziglang.org/download/) 0.17.x, and Git Bash (or bash from WSL). **No** Visual Studio, **no** Windows SDK — resources are compiled with `zig rc`.

```bash
git clone https://github.com/pingyih-pixel/NexExclusive.git
cd NexExclusive

bash mod/build.sh all     # → mod/NexExclusive.exe, mod/nexsink.dll, config and UI scripts
bash mod/build.sh test    # 19 regression groups, no device and no injection involved
bash release.sh           # → dist/NexExclusive-<version>.zip + SHA256SUMS.txt
```

`build.sh` looks for zig in this order: `$ZIG` → `zig` on `PATH` → `mod/zig.path`. If zig is not on your PATH:

```bash
ZIG=/c/zig/zig-x86_64-windows-0.17.1 bash mod/build.sh all
```

Output goes to `mod/` by default; override with `OUT_DIR=/abs/path`. You can also build just `sink` (the DLL) or `host` (the exe).

The release package contains exactly six files: `NexExclusive.exe`, `nexsink.dll`, `hook.cfg`, `ui.json`, `ui/panel.js`, `ui/simplify.js`. `release.sh` also writes `README.txt` and checksums.

### Known issues

**Windows Defender flags it as malicious.** The program loads a DLL into another process, which looks like an injection trojan: it reports `Behavior:Win32/DefenseEvasion.A!ml` and quarantines the exe within 30–60 seconds, taking the shortcut, autostart entry and uninstall entry with it.

It is a false positive — no network, no encryption, no drivers, no tampering with security software, no modification of client files. But it does block installation. Add an exclusion (admin PowerShell):

```powershell
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\NexExclusive"
```

Add the extracted folder too if you run it from there. **If you don't want an exclusion, don't use it — do not disable real-time protection for this.**

Others:

- **Software volume does not work in exclusive mode** — a WASAPI limitation, not a bug. The system slider still works when the endpoint has a hardware volume node. This project deliberately does no digital attenuation in the module — no bits traded for volume.
- **Uninstalling may leave the exe behind**: it is holding itself open, and without admin rights it cannot be scheduled for deletion on reboot. Close it and delete the folder.
- **Tested on one machine only**: Windows 11 26200 with HyperX Cloud III (44.1/48/96) and AudioQuest DragonFly MM3A (88.2 as well).
- **Tested on one client version only**: 3.1.42.205594. Other versions are flagged "unverified" in the log.

A fuller list of limitations and what is planned is in [docs/ROADMAP.md](docs/ROADMAP.md).

### License

[MIT](LICENSE). Not affiliated with NetEase; loading a DLL into a third-party client may violate its terms of service — use at your own risk.
