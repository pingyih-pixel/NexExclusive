# NexExclusive

网易云音乐 PC 客户端的 WASAPI 独占输出，绕开系统混音器，按源采样率直接交给声卡。Windows 10/11 x64。

WASAPI exclusive output for the NetEase Cloud Music desktop client — no mixer, source sample rate. Windows 10/11 x64.

## 装

1. 下载 `NexExclusive-x.y.z.zip` 解压。暂无正式 Release，包从 [Actions](https://github.com/pingyih-pixel/NexExclusive/actions) 最近一次绿色构建的 Artifacts 取。
2. **先看下面的「已知问题」**，不加排除项会被 Defender 删掉。
3. 双击 `NexExclusive.exe`，首次运行问是否安装：装到 `%LOCALAPPDATA%\NexExclusive`、开机自启、建开始菜单快捷方式。
4. 装完自动拉起，托盘常驻。之后开客户端就生效。

命令行：`--install [--client "路径"]`、`--uninstall`、`--help`。

## 要求

| | |
|---|---|
| 系统 | Windows 10 / 11 x64 |
| 客户端 | 网易云音乐 **3.1.42.205594**（唯一验证过的版本） |
| 权限 | 普通用户，不需要管理员 |

## 已知问题

**Defender 会误报。** 程序要往客户端进程里加载 DLL，行为特征像注入类木马，会被判成 `Behavior:Win32/DefenseEvasion.A!ml`，30–60 秒内隔离 exe，并连带删掉快捷方式、自启项和卸载项。

这是误报，但确实会挡住安装。要用就先加排除项（管理员 PowerShell）：

```powershell
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\NexExclusive"
```

从解压目录直接跑的话，把那个目录也加进去。**不想加就别用，不要为此关掉实时保护。**

其他：

- 独占下调不了软件音量，是 WASAPI 的限制，不是 bug。端点有硬件音量节点时系统音量条仍然有效。
- 卸载后可能留下 exe——程序正被自己占用。关掉后手动删目录。
- 只在一台机器上验过（Win11 26200 + HyperX Cloud III / AudioQuest DragonFly）。

更完整的限制清单和后续计划见 [docs/ROADMAP.md](docs/ROADMAP.md)。

## 构建

需要 [Zig](https://ziglang.org/download/) 0.17.x，不需要 Visual Studio 或 Windows SDK。

```bash
bash mod/build.sh all     # nexsink.dll + NexExclusive.exe
bash mod/build.sh test    # 19 组回归
bash release.sh           # dist/NexExclusive-x.y.z.zip + 校验和
```

## 许可与免责

[MIT](LICENSE)。与网易公司无关，非官方工具；往第三方客户端进程里加载 DLL 可能违反其用户协议，风险自负。

---

## English

### Install

1. Download `NexExclusive-x.y.z.zip` and unzip. There is no official release yet — get the package from the Artifacts of the latest green [Actions](https://github.com/pingyih-pixel/NexExclusive/actions) run.
2. **Read "Known issues" first** — without an exclusion, Defender deletes the exe.
3. Run `NexExclusive.exe`. On first launch it offers to install to `%LOCALAPPDATA%\NexExclusive`, add an autostart entry and create a Start Menu shortcut.
4. It launches the client and stays in the tray. That's it.

Command line: `--install [--client "path"]`, `--uninstall`, `--help`.

### Requirements

| | |
|---|---|
| OS | Windows 10 / 11 x64 |
| Client | NetEase Cloud Music **3.1.42.205594** (the only version tested) |
| Privileges | Standard user, no admin |

### Known issues

**Defender flags it as malicious.** The program loads a DLL into another process, which looks like an injection trojan: it reports `Behavior:Win32/DefenseEvasion.A!ml` and quarantines the exe within 30–60 seconds, taking the shortcut, autostart entry and uninstall entry with it.

It is a false positive, but it does block installation. Add an exclusion (admin PowerShell):

```powershell
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\NexExclusive"
```

Add the extracted folder too if you run it from there. **If you don't want an exclusion, don't use it — do not disable real-time protection for this.**

Others:

- Software volume does not work in exclusive mode — a WASAPI limitation, not a bug. The system slider still works when the endpoint has a hardware volume node.
- Uninstalling may leave the exe behind; it is holding itself open. Close it and delete the folder.
- Tested on one machine only (Win11 26200 + HyperX Cloud III / AudioQuest DragonFly).

A fuller list of limitations and what is planned is in [docs/ROADMAP.md](docs/ROADMAP.md).

### Build

Requires [Zig](https://ziglang.org/download/) 0.17.x. No Visual Studio, no Windows SDK.

```bash
bash mod/build.sh all     # nexsink.dll + NexExclusive.exe
bash mod/build.sh test    # 19 regression groups
bash release.sh           # dist/NexExclusive-x.y.z.zip + checksums
```

### License

[MIT](LICENSE). Not affiliated with NetEase; loading a DLL into a third-party client may violate its terms of service — use at your own risk.
