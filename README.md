# NexExclusive

给**网易云音乐 PC 客户端**加上 WASAPI 独占输出——让音频绕开 Windows 混音器，按原始采样率直接交给声卡。

> **English**: A WASAPI exclusive-mode enabler for the NetEase Cloud Music desktop
> client on Windows. It loads a DLL into the player process and hooks the
> `IMMDevice` / `IAudioClient` vtables so the player's audio stream opens the
> device in exclusive mode, at the source sample rate whenever the hardware
> accepts it. Windows 10/11 x64 only. Unsigned, unofficial, no affiliation with
> NetEase. **Windows Defender currently flags it** — see [Known issues](#known-issues).

---

## 它解决什么问题

网易云客户端只会用 Windows 共享模式（shared mode）输出：所有音频先被系统混音器
重采样到「引擎格式」（通常 48 kHz 或 96 kHz / 32-bit float），再交给声卡。
哪怕你放的是 44.1 kHz 的无损，也要过一道 SRC。

NexExclusive 让客户端自己用 WASAPI 独占模式打开输出设备：不经过混音器，
采样率按源文件的来（44.1 / 48 / 88.2 / 96 …），位深按设置来。
设备不接受源格式时才做转换，并且在日志和面板里**明确标出来**是「直通」还是
「转换过」。

## 装

1. 下载 `NexExclusive-x.y.z.zip`，解压。
2. **先看下面的「已知问题」**——不加排除项的话，exe 会被 Defender 隔离。
3. 双击 `NexExclusive.exe`。第一次会问要不要安装，选「是」：
   - 装到 `%LOCALAPPDATA%\NexExclusive`
   - 开机自启（`HKCU\...\Run`，不需要管理员）
   - 开始菜单快捷方式；「设置 → 应用」里可以卸载
4. 装完直接拉起，右下角托盘出现图标。之后开网易云就自动生效。

命令行也行：

```
NexExclusive.exe --install [--client "D:\...\cloudmusic.exe"]
NexExclusive.exe --uninstall
NexExclusive.exe --help
```

托盘右键：打开客户端 / 看日志 / 打开程序目录 / 退出。

## 要求

| | |
|---|---|
| 系统 | Windows 10 / 11 x64 |
| 客户端 | 网易云音乐 **3.1.42.205594**（唯一验证过的版本，其他版本会在日志里标「未经验证」） |
| 权限 | 普通用户即可，**不需要管理员** |

客户端装在非常规目录也能用：路径按 `NEX_CLIENT` 环境变量 → `host.cfg` →
运行中的进程 → 注册表 → 桌面/开始菜单快捷方式 → 常见安装位置 依次探测，
全都找不到会弹文件选择框让你点一次。

## 已知问题

### Windows Defender 会误报（重要）

程序要往客户端进程里加载 DLL，行为特征和注入类木马相似。Defender 的机器学习
模型会判成 `Behavior:Win32/DefenseEvasion.A!ml`，**在 30–60 秒内隔离 exe**，
并连带删掉快捷方式、开机自启项和卸载项。

这是误报——程序不联网、不加密、不装驱动、不碰安全软件、不改客户端文件。
但误报确实会挡住安装。

**要用就先加排除项**（管理员 PowerShell）：

```powershell
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\NexExclusive"
```

从解压目录直接跑的话，把解压目录也加进去。

> 不想加排除项就别用。**不要为了这个关掉实时保护。**

已经向微软提交误报的材料在 [`docs/defender-false-positive.md`](docs/defender-false-positive.md)。

### 其他

- **软件音量端点在独占下调不了音量**：这是 WASAPI 独占的固有限制，不是 bug。
  端点有硬件音量节点（大部分 USB 声卡/耳机）时系统音量条仍然有效。
  本项目**明确不做模块侧数字衰减**——不拿比特换音量。
- **卸载后可能留下 exe**：从安装目录运行卸载时，主程序文件正被自己占用，
  Windows 不允许删正在运行的 exe；没有管理员权限也无法登记「重启后删除」。
  关掉程序后手动删掉那个文件夹即可。
- **只在一台机器上验过**：Windows 11 26200、HyperX Cloud III（44.1/48/96）、
  AudioQuest DragonFly MM3A（额外支持 88.2）。其他设备的行为可能不同。
- 32-bit float 输出在两个测试端点上都被拒绝（硬件只吃整数 PCM），这是设备限制。

## 从源码构建

需要 [Zig](https://ziglang.org/download/) 0.17.x。不需要 Visual Studio、不需要
Windows SDK（资源编译用 `zig rc`）。

```bash
bash mod/build.sh all        # 产物落在 mod/
bash mod/build.sh test       # 19 组回归测试
bash release.sh              # 打包：dist/NexExclusive-x.y.z.zip + 校验和 + Release 说明
```

`build.sh` 按这个顺序找 zig：`$ZIG` → `PATH` 里的 `zig` → `mod/zig.path`
（本机覆盖文件，已 gitignore）。产物目录用 `OUT_DIR=/绝对路径` 指定。

CI 在 GitHub Actions 上跑同样的 `release.sh`，产物可在 Actions 页面下载
（`.github/workflows/build.yml`）。

## 它是怎么工作的

```
NexExclusive.exe ──┬─ 启动客户端（带 --remote-debugging-port）
                   ├─ OpenProcess → VirtualAllocEx → WriteProcessMemory
                   │  → CreateRemoteThread(LoadLibraryW) 加载 nexsink.dll
                   ├─ 本机 HTTP 配置服务 127.0.0.1:9334（给设置面板用）
                   ├─ 通过 CDP 把 ui/panel.js 注入客户端页面
                   └─ 托盘常驻：客户端重启、页面刷新、模块掉出，都会自动补回去

nexsink.dll ───────┬─ hook IMMDevice / IAudioClient 的 vtable
                   ├─ 独占格式协商：按源率优先，逐个试端点接受的白名单
                   ├─ 重采样（多相 FIR，精确有理数比）+ 位深转换
                   └─ 切歌 / 切端点时按代交接，不打断播放
```

更细的设计取舍、实测数据和踩过的坑都在 [`PROJECT_STATUS.md`](PROJECT_STATUS.md)
（中文，工程日志，很长）。

## 还差什么

[`docs/ROADMAP.md`](docs/ROADMAP.md) 按优先级列了离「可以发给任何人」还差哪些事。
一句话版本：**Defender 误报不解决，对外分发就不可用**；其余是兼容性覆盖
（更多客户端版本、更多声卡）和功能缺口（ASIO / 网播通路、面板回归）。

## 免责

- 与网易公司无关，非官方工具。
- 往第三方客户端进程里加载 DLL 可能违反其用户协议，**风险自负**。
- 按 MIT 许可提供，不提供任何担保。因使用本程序造成的任何损失（包括但不限于
  设备损坏、数据丢失、账号问题）由使用者承担。

## 许可

[MIT](LICENSE)
