# NexExclusive

给**网易云音乐 PC 客户端**加上 WASAPI 独占输出，让音频绕开 Windows 混音器，按原始采样率直接交给声卡。Windows 10/11 x64。

A WASAPI exclusive-mode enabler for the NetEase Cloud Music desktop client on Windows. It bypasses the system mixer and hands audio to the device at the source sample rate.

**中文** ｜ [English](#english)

---

## 中文

### 它解决什么问题

网易云客户端只用共享模式输出：音频先被系统混音器重采样到「引擎格式」（通常 48 或 96 kHz / 32-bit float），再交给声卡——哪怕是 44.1 kHz 的无损也要过一道 SRC。

NexExclusive 让客户端自己用独占模式打开设备：不过混音器，采样率按源文件来。设备不接受源格式时才做转换，并在日志和面板里标明是「直通」还是「转换过」。

### 装

1. 拿 `NexExclusive-x.y.z.zip` 并解压。目前没有正式 Release（[原因](docs/ROADMAP.md#为什么还不发-release)），包从 [Actions](https://github.com/pingyih-pixel/NexExclusive/actions) 里最近一次绿色构建的 Artifacts 下载，或自行构建。
2. **先看「已知问题」**——不加排除项的话 exe 会被 Defender 隔离。
3. 双击 `NexExclusive.exe`，首次运行问是否安装：装到 `%LOCALAPPDATA%\NexExclusive`、开机自启（`HKCU\...\Run`，不需要管理员）、建开始菜单快捷方式。
4. 装完自动拉起，托盘出现图标。之后开客户端就生效。

命令行：

```
NexExclusive.exe --install [--client "D:\...\cloudmusic.exe"]
NexExclusive.exe --uninstall
NexExclusive.exe --help
```

客户端装在非常规目录也能用：按 `NEX_CLIENT` → `host.cfg` → 运行中的进程 → 注册表 → 桌面/开始菜单快捷方式 → 常见位置 依次探测，都找不到会弹文件选择框。

### 要求

| | |
|---|---|
| 系统 | Windows 10 / 11 x64 |
| 客户端 | 网易云音乐 **3.1.42.205594**（唯一验证过的版本，其他版本日志里标「未经验证」） |
| 权限 | 普通用户，**不需要管理员** |

### 已知问题

**Windows Defender 会误报。** 程序要往客户端进程里加载 DLL，行为特征和注入类木马相似，Defender 会判成 `Behavior:Win32/DefenseEvasion.A!ml` 并在 30–60 秒内隔离 exe，连带删掉快捷方式、自启项和卸载项。

这是误报——不联网、不加密、不装驱动、不碰安全软件、不改客户端文件。但确实会挡住安装。要用就先加排除项（管理员 PowerShell）：

```powershell
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\NexExclusive"
```

从解压目录直接跑的话，把解压目录也加进去。**不想加就别用，不要为此关掉实时保护。**

申诉材料在 [`docs/defender-false-positive.md`](docs/defender-false-positive.md)。

其余：

- **独占下调不了软件音量**：WASAPI 的固有限制。端点有硬件音量节点时系统音量条仍然有效。本项目明确不做模块侧数字衰减——不拿比特换音量。
- **卸载后可能留下 exe**：程序正被自己占用，且无管理员权限无法登记「重启后删除」。关掉后手动删目录即可。
- **只在一台机器验过**：Windows 11 26200、HyperX Cloud III（44.1/48/96）、AudioQuest DragonFly MM3A（额外支持 88.2）。
- 32-bit float 在两个测试端点上都被拒（硬件只吃整数 PCM），是设备限制。

### 从源码构建

需要 [Zig](https://ziglang.org/download/) 0.17.x。不需要 Visual Studio 或 Windows SDK（资源编译用 `zig rc`）。

```bash
bash mod/build.sh all        # 产物落在 mod/
bash mod/build.sh test       # 19 组回归测试
bash release.sh              # 打包：dist/NexExclusive-x.y.z.zip + 校验和 + Release 说明
```

`build.sh` 找 zig 的顺序：`$ZIG` → `PATH` → `mod/zig.path`（本机覆盖，已 gitignore）。产物目录用 `OUT_DIR=/绝对路径` 指定。CI 跑的是同一条 `release.sh`。

**关于可复现构建**：同一输出路径下重复构建逐字节一致；换路径哈希就变，因为链接器把 PDB 绝对路径写进了 PE 调试目录。所以 `SHA256SUMS.txt` 只证明下载完整性，不证明「由这份源码编出来」。

### 工作原理

```
NexExclusive.exe ──┬─ 启动客户端（带 --remote-debugging-port）
                   ├─ OpenProcess → VirtualAllocEx → WriteProcessMemory
                   │  → CreateRemoteThread(LoadLibraryW) 加载 nexsink.dll
                   ├─ 本机 HTTP 配置服务 127.0.0.1:9334（给设置面板用）
                   ├─ 通过 CDP 把 ui/panel.js 注入客户端页面
                   └─ 托盘常驻：客户端重启、页面刷新、模块掉出都会补回去

nexsink.dll ───────┬─ hook IMMDevice / IAudioClient 的 vtable
                   ├─ 独占格式协商：按源率优先，逐个试端点接受的白名单
                   ├─ 重采样（多相 FIR，精确有理数比）+ 位深转换
                   └─ 切歌 / 切端点时按代交接，不打断播放
```

更细的设计取舍、实测数据和踩过的坑在 [`PROJECT_STATUS.md`](PROJECT_STATUS.md)（中文工程日志）。离「可以发给任何人」还差什么见 [`docs/ROADMAP.md`](docs/ROADMAP.md)。

### 免责

与网易公司无关，非官方工具。往第三方客户端进程里加载 DLL 可能违反其用户协议，风险自负。按 MIT 许可提供，不提供任何担保。

### 许可

[MIT](LICENSE)

---

<a id="english"></a>

## English

### What it solves

The NetEase Cloud Music desktop client only ever opens its output in *shared* mode: everything is resampled by the Windows audio engine to a fixed mix format (usually 48 or 96 kHz / 32-bit float) before it reaches the device. Even a 44.1 kHz lossless file goes through an SRC.

NexExclusive makes the client open the device in *exclusive* mode instead — no mixer, source sample rate. It only converts when the hardware rejects the source format, and says so explicitly in the log and the panel ("passthrough" vs "converted").

### Install

1. Get `NexExclusive-x.y.z.zip` and unzip. There is no official release yet ([why](docs/ROADMAP.md#为什么还不发-release)) — grab the package from the Artifacts of the latest green [Actions](https://github.com/pingyih-pixel/NexExclusive/actions) run, or build it yourself.
2. **Read "Known issues" first** — without an exclusion, Defender will quarantine the exe.
3. Run `NexExclusive.exe`. On first launch it offers to install itself to `%LOCALAPPDATA%\NexExclusive`, register a `HKCU\...\Run` autostart entry (no admin needed), and create a Start Menu shortcut.
4. It launches the client and sits in the tray. After that it just works.

Command line:

```
NexExclusive.exe --install [--client "D:\...\cloudmusic.exe"]
NexExclusive.exe --uninstall
NexExclusive.exe --help
```

A non-standard client location is fine: the path is probed through `NEX_CLIENT` → `host.cfg` → running processes → registry → desktop/Start Menu shortcuts → common paths, with a file picker as the last resort.

### Requirements

| | |
|---|---|
| OS | Windows 10 / 11 x64 |
| Client | NetEase Cloud Music **3.1.42.205594** (the only version tested; others are flagged "unverified" in the log) |
| Privileges | Standard user, **no admin required** |

### Known issues

**Windows Defender flags it as malicious.** The program loads a DLL into another process, which looks like an injection trojan to the ML classifier: it reports `Behavior:Win32/DefenseEvasion.A!ml` and quarantines the exe within 30–60 seconds, taking the shortcut, autostart entry and uninstall entry with it.

It is a false positive — no network, no encryption, no drivers, no tampering with security software, no modification of client files. But it does block installation. Add an exclusion (admin PowerShell) before running:

```powershell
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\NexExclusive"
```

If you run it straight from the extracted folder, add that folder too. **If you don't want an exclusion, don't use it — do not disable real-time protection for this.**

The write-up we submitted is in [`docs/defender-false-positive.md`](docs/defender-false-positive.md).

Others:

- **Software volume does not work in exclusive mode.** That's a WASAPI limitation, not a bug. The system slider still works when the endpoint has a hardware volume node. This project deliberately does no digital attenuation in the module — no bits traded for volume.
- **Uninstalling may leave the exe behind.** It is running and holding itself open, and without admin rights it cannot be scheduled for deletion on reboot. Close it and delete the folder.
- **Tested on one machine only**: Windows 11 26200, HyperX Cloud III (44.1/48/96), AudioQuest DragonFly MM3A (88.2 as well).
- 32-bit float was rejected by both test endpoints (integer PCM only) — a hardware limitation.

### Build from source

Requires [Zig](https://ziglang.org/download/) 0.17.x. No Visual Studio, no Windows SDK (`zig rc` compiles the resources).

```bash
bash mod/build.sh all        # output lands in mod/
bash mod/build.sh test       # 19 regression groups
bash release.sh              # dist/NexExclusive-x.y.z.zip + checksums + release notes
```

`build.sh` looks for zig in this order: `$ZIG` → `PATH` → `mod/zig.path` (local override, gitignored). Use `OUT_DIR=/abs/path` to redirect the output. CI runs the same `release.sh`.

**On reproducible builds**: repeated builds into the *same* output path are byte-identical. Change the path and the hash changes, because the linker embeds the absolute PDB path in the PE debug directory. So `SHA256SUMS.txt` proves download integrity, not "this binary came from this source".

### How it works

```
NexExclusive.exe ──┬─ launches the client with --remote-debugging-port
                   ├─ OpenProcess → VirtualAllocEx → WriteProcessMemory
                   │  → CreateRemoteThread(LoadLibraryW) to load nexsink.dll
                   ├─ local HTTP config server on 127.0.0.1:9334
                   ├─ injects ui/panel.js into the client page over CDP
                   └─ stays in the tray: re-injects on client restart, page
                      reload, or module unload

nexsink.dll ───────┬─ hooks the IMMDevice / IAudioClient vtables
                   ├─ exclusive format negotiation against the endpoint's
                   │  whitelist, preferring the source sample rate
                   ├─ resampling (polyphase FIR, exact rational ratio) and
                   │  bit-depth conversion when needed
                   └─ hands over by generation on track/endpoint change, so
                      playback is not interrupted
```

Design trade-offs, measurements and the bugs behind them are in [`PROJECT_STATUS.md`](PROJECT_STATUS.md) (Chinese engineering log). What is still missing before it can be handed to strangers is in [`docs/ROADMAP.md`](docs/ROADMAP.md).

### Disclaimer

Not affiliated with NetEase. Loading a DLL into a third-party client may violate its terms of service — use at your own risk. Provided under the MIT license, without warranty of any kind.

### License

[MIT](LICENSE)
