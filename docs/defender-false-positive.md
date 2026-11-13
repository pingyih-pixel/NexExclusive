# Defender 误报：提交材料

> 目标：让 `Behavior:Win32/DefenseEvasion.A!ml` 不再命中本程序。
> 提交入口：<https://www.microsoft.com/en-us/wdsi/filesubmission>
> 选 **「Submit a file for analysis」** → 下一步选 **「I believe this file is incorrectly detected (false positive)」**。

## 提交前准备

1. 把**安装目录**里的 `NexExclusive.exe` 和 `nexsink.dll` 一起打包成 zip（WDSI 接受 zip，不接受散装多个文件）。
2. 填下面这些字段。哈希必须和上传的文件一致，否则分析会打回。

| 字段 | 值 |
|---|---|
| Detection name | `Behavior:Win32/DefenseEvasion.A!ml` |
| Threat ID | `2147738096` |
| Severity | Severe (5) |
| Product | NexExclusive 1.0.0 |
| Publisher | 无签名（unsigned，个人项目） |

文件哈希：**必须填你实际上传的那两个文件**，不要照抄旧值。Zig 的产物换一个输出路径哈希就会变
（PDB 绝对路径写进了 PE 调试目录），重新编译一次就对不上了。在安装目录里取：

```powershell
Get-FileHash .\NexExclusive.exe, .\nexsink.dll -Algorithm SHA256
```

## 可直接粘贴的英文正文

### Description

```
NexExclusive is a small, local-only audio utility for NetEase Cloud Music
(cloudmusic.exe) on Windows. The music player always routes audio through the
Windows shared-mode audio engine, which resamples everything to the system mix
format. NexExclusive makes the player open its output device in WASAPI exclusive
mode instead, so the original sample rate reaches the DAC unmodified.

The application is a single 324 KB executable plus one 278 KB DLL. It has no
installer framework, no auto-updater, no telemetry, and no outbound network
access. The only sockets it opens are local: an HTTP server bound to
127.0.0.1:9334 that serves the settings panel, and a client connection to the
player's own Chrome DevTools port on 127.0.0.1:9333. Nothing leaves the machine.
```

### Why it loads a DLL into another process

```
WASAPI exclusive mode is negotiated inside the player process: the player calls
IMMDevice::Activate and IAudioClient::Initialize itself, and the format it asks
for comes from the player's own settings. There is no documented, supported way
for a third-party component to change that negotiation from outside the process.
The only workable mechanism is to load a DLL into the player process and hook the
IAudioClient / IMMDevice vtable entries, then answer the negotiation with the
device's real exclusive-mode capabilities.

The DLL is loaded by the launcher with the standard sequence:
OpenProcess -> VirtualAllocEx -> WriteProcessMemory -> CreateRemoteThread(LoadLibraryA).
That is the textbook process-injection pattern, and we understand it is what
triggers a behavior-based detection. It is used here for a legitimate purpose:
the target process is a local music player started by our own launcher, the DLL
is our own code shipped next to the executable, and the only effect is that the
player's audio stream opens the sound device in exclusive mode.
```

### What the software does NOT do

```
- No outbound network access and no telemetry. The host opens two loopback-only
  sockets: an HTTP server on 127.0.0.1:9334 that serves the settings panel, and a
  client connection to the player's own DevTools port on 127.0.0.1:9333. There is
  no Internet traffic, no update check, and no data collection
- No defence evasion: it never touches Defender, AMSI, ETW, event logs, or any
  other security product, and it does not unhook or hide anything belonging to
  the system. The only memory it modifies is the vtable of the player's own audio
  interfaces (IMMDevice / IAudioClient), inside the player's process
- No privilege escalation: it runs as the current user, never requests admin
- No kernel component: no driver, no service, no boot-time component
- No persistence beyond two standard, user-visible entries written by the
  built-in installer:
    HKCU\Software\Microsoft\Windows\CurrentVersion\Run\NexExclusive
    HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\NexExclusive
  Both are removed by "NexExclusive.exe --uninstall" and by Settings > Apps
- No encryption, packing, or obfuscation of its own binaries
- No collection or transmission of user data
- Does not modify the music player's files on disk; it only loads a DLL into a
  running process, and the DLL is unloaded when the player exits
```

### The detection name is misleading

```
The classification "DefenseEvasion" suggests the file tries to avoid or disable
security controls. It does not. The behaviour that most likely drives the score is
"writes an executable into %LOCALAPPDATA%, creates a Run key, creates a Start Menu
shortcut, and injects a DLL into another process". Each of those is also common in
droppers, so the combination scores high - but in this case every step is
performed openly, by a user-initiated installer, with a visible uninstall entry,
and the injected code is an unsigned but fully inspectable 278 KB DLL that
only implements audio format negotiation.
```

### Steps to reproduce

```
1. Download and extract NexExclusive-1.0.0.zip (contains NexExclusive.exe,
   nexsink.dll, hook.cfg, ui.json, ui/).
2. Run: NexExclusive.exe --install --client "<path to cloudmusic.exe>"
   This copies the files to %LOCALAPPDATA%\NexExclusive, writes the two HKCU
   entries above, and creates a Start Menu shortcut.
3. Run: %LOCALAPPDATA%\NexExclusive\NexExclusive.exe
   It starts the music player and injects nexsink.dll into it.
4. Within roughly one minute, Defender quarantines NexExclusive.exe and also
   removes the shortcut and both registry entries.
```

### Requested outcome

```
Please reclassify Behavior:Win32/DefenseEvasion.A!ml so it no longer matches this
file. If the behaviour-based detection cannot be adjusted for an unsigned binary,
please tell us which specific behaviour needs to change and we will redesign
accordingly.
```

## 如果被打回

WDSI 常见回信是「仍判定为威胁」。可以做的：

1. **改注入方式**再提交一次。现在是最典型的 `VirtualAllocEx` + `CreateRemoteThread`。换成「代理 DLL」——把转发导出表的 `version.dll` / `winmm.dll` 放在客户端目录旁，客户端加载它时顺带加载 `nexsink.dll`。没有跨进程写入，没有远程线程，行为特征完全不同。代价：要写客户端安装目录，装在 `Program Files` 时没权限。
2. **代码签名**。对 `!ml` 行为判定帮助有限（签名不能证明行为无害），但能显著降低 SmartScreen 和云查杀的分数，也更容易让分析员认真看材料。
3. **开源仓库 + 公开构建**（已完成）。仓库 <https://github.com/pingyih-pixel/NexExclusive> 公开，
   CI 在 `windows-latest` 上跑同一套脚本出包。分析员可以直接对照源码与产物。

## 期间的分发办法

在误报解决之前，Release 页面必须写清楚：

- 首次运行前给 `NexExclusive.exe` 和 `nexsink.dll` 这两个文件加排除项（管理员），否则 exe 会被隔离
- 不要给整个目录加排除：那个目录对当前用户可写，整体排除等于让放进去的任何文件都不再被扫描
- 给出确切命令，不要只说「加个白名单」
- 明确写「不想加排除项就别用，不要为了这个关掉实时保护」

发布包里已经在 `README.txt` 的「杀软报毒」一节写了这段。
