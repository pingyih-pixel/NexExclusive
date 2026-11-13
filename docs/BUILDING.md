# 从源码构建

想改代码，或者想自己验证发布包里的二进制，读这一篇。只想使用的话回到 [README](../README.md)。

## 构建方式

源码是 C。构建脚本用 Zig 自带的 `zig cc` 交叉编译到 `x86_64-windows-gnu`，资源文件用 `zig rc` 编译。Zig 在这里充当编译器，源码本身不含 Zig 代码。构建不需要 Visual Studio，也不需要 Windows SDK。

## 依赖

- Windows 10/11 x64
- [Zig](https://ziglang.org/download/) 0.17.x（CI 固定使用 0.17.1，并校验下载包的 SHA256）
- Git Bash。`mod/build.sh` 调用了 `cygpath`，Git Bash 自带；WSL 的 bash 没有这个命令，尚未验证过能否使用
- 打发布包时另需要 `C:\Windows\System32\tar.exe`（系统自带的 bsdtar）和 `sha256sum`

## 构建、测试、打包

```bash
git clone https://github.com/pingyih-pixel/NexExclusive.git
cd NexExclusive

bash mod/build.sh all     # → mod/NexExclusive.exe、mod/NexAudioHost.exe、mod/nexsink.dll，以及配置和界面脚本
bash mod/build.sh test    # 19 组回归，不开设备、不注入进程
node mod/ui/panel.contract.test.js
node --check mod/ui/panel.js
node --check mod/ui/simplify.js
bash release.sh           # → dist/NexExclusive-<版本>.zip + SHA256SUMS.txt
```

`build.sh` 还支持单独构建 `sink`（只有 DLL）、`host`（宿主 exe）或 `audiohost`（独立 WASAPI 端点进程）。产物默认落在 `mod/`，用 `OUT_DIR=/绝对路径` 可以改，必须是绝对路径。

`release.sh 1.0.1` 可以临时指定版本号，不改源码。不带参数时，版本号取自 `mod/tools/nexhost.c` 里的 `APP_VERSION`，那里是版本号的唯一来源。

## Zig 的查找顺序

1. 环境变量 `ZIG`，指向 Zig 的安装目录（内含 `zig.exe` 与 `lib/`），或直接指向 `zig` 可执行文件
2. `PATH` 里的 `zig`
3. `mod/zig.path`，把本机路径写在里面，已被 `.gitignore` 排除

Zig 没有放进 `PATH` 时：

```bash
ZIG=/c/zig/zig-x86_64-windows-0.17.1 bash mod/build.sh all
```

## 代码布局

| 路径 | 内容 |
| --- | --- |
| `mod/src/nexsink.c` | 注入客户端进程的 DLL 的入口，依次包含下面六个 `.inc`，合成一个编译单元 |
| `mod/src/sink_core.inc` | 配置、日志、状态、vtable 记录 |
| `mod/src/sink_dsp.inc` | 环形缓冲、采样格式转换、重采样 |
| `mod/src/sink_device.inc` | 输出线程、故障处理、伪造的客户端对象 |
| `mod/src/sink_lifecycle.inc` | 设备「代」的切换、停止与拆除 |
| `mod/src/sink_endpoint.inc` | 格式协商、迁移、接管 |
| `mod/src/sink_hooks.inc` | `IAudioClient` / `IMMDevice` 的钩子与看门狗 |
| `mod/tools/nexhost.c` | 宿主 exe：安装卸载、托盘、注入、本机配置服务、CDP 界面注入 |
| `mod/tools/nexaudiolink.h` / `nexaudiohost.c` | 独立音频宿主协议与 WASAPI 端点进程 |
| `mod/ui/` | 注入客户端页面的 `panel.js` 与 `simplify.js` |
| `mod/test/regression.c` | 19 组回归，直接包含 `nexsink.c`，所以能测到内部的静态函数 |

回归测试覆盖 DLL 一侧：vtable、环形缓冲、重采样、生命周期、闸门。宿主一侧的安装、卸载、托盘、单实例目前靠手工验证。

## 发布包内容

发布包包含 `NexExclusive.exe`、`NexAudioHost.exe`、`nexsink.dll`、`hook.cfg`、`ui.json`、`ui/panel.js`、`ui/simplify.js`。`release.sh` 会另外生成 `README.txt`、校验和与 Release 说明。

编译成功、文件却消失时，基本是杀软隔离了 `NexExclusive.exe`。在 PowerShell 里查一下：

```powershell
Get-MpThreatDetection | Sort-Object InitialDetectionTime -Descending | Select -First 3
```

## CI

`.github/workflows/build.yml` 在 `windows-latest` 上运行与本地相同的 `bash mod/build.sh test` 和 `bash release.sh`，推送 `v*` 标签时再创建 Release。
