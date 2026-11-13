# NexExclusive 编译、注入与宿主运行手册

本文记录开发机上从源码构建，到网易云进程注入，再到客户端重启后自动恢复的完整链路。它补充 `README.md` 的使用说明和 `docs/BUILDING.md` 的构建细节，不替代两份文档。

## 运行结构

项目由三部分组成：

- `NexExclusive.exe` 是常驻宿主。它读取 `hook.cfg`，监听 `127.0.0.1:9334`，寻找网易云主进程，把 `nexsink.dll` 注入进去，并在客户端重启后再次处理。
- `NexAudioHost.exe` 是可选的独立端点进程。当前实现把它用于严格原样的原格式 PCM 路径：DLL 通过共享内存把 PCM 交给它，由它独占打开设备并提交数据。这里的进程边界已经独立，输出策略仍与宿主选择绑定；原率优先下的转换路径继续由 DLL 内部负责，后续可把转换后的 PCM 也纳入共享环。
- `nexsink.dll` 在网易云进程内部工作。它挂接 `IMMDevice` 与 `IAudioClient`，把客户端的共享初始化改成独占输出，并负责缓冲、格式协商、重采样和端点迁移。
- `ui/panel.js` 是设置页脚本。宿主通过网易云的 CDP 调试端口 `127.0.0.1:9333` 注入它，所以面板显示与音频 DLL 是两条独立链路。
- `ui/simplify.js` 在顶栏头像左侧放置一个编辑入口。弹窗分为左侧导航、右侧大页、顶部按钮三组，模块逐项保存到 `ui.json`；创建的歌单和收藏的歌单不进入可编辑范围。

DLL 不是独立常驻程序。它只在当前网易云进程存在期间有效；网易云退出后，DLL 一起卸载。持续生效依靠宿主进程存活，并在新的网易云主进程出现时再次注入。

## 与 Apple Music-OpenHome / AMExclusive 的关系

参考项目采用的也是运行时加载，不是把功能静态编进 Apple Music：

- 独立 broker 负责查找并绑定 `AppleMusic.exe`。
- `am-exclusive-ui.dll` 进入 Apple Music 主进程，处理界面侧通信。
- `am-exclusive-hook.dll` 进入 `AMPLibraryAgent.exe`，处理音频侧路径。
- broker 监视这两个进程；进程退出或重新出现时，重新查找并注入对应模块。

网易云项目的分工更集中：`nexsink.dll` 进入 `cloudmusic.exe` 主进程，音频 hook、独占端点和实时缓冲都在这一进程内；`panel.js` 不是 DLL，而是宿主通过 CDP 注入设置页。两者都依赖独立宿主持续看护，模块本身不会在客户端退出后继续存在。

## 构建

项目使用 Zig 0.17.x 的 `zig cc`，目标为 `x86_64-windows-gnu`。本机路径记录在 `mod/zig.path`，该文件不会进入版本库。

```powershell
$zig = 'D:\Installed_Program\zig\zig-x86_64-windows-0.17.1\zig.exe'
$env:ZIG_LOCAL_CACHE_DIR = "$pwd\work\zig-cache-local"
$env:ZIG_GLOBAL_CACHE_DIR = "$pwd\work\zig-cache-global"

& $zig cc -target x86_64-windows-gnu -O2 -Wall -Werror `
  -shared -o work\nexsink.dll mod\src\nexsink.c `
  -lole32 -loleaut32 -luuid
```

宿主需要先用 `zig rc` 生成版本资源，再与 `nexhost.c` 一起链接。日常构建优先使用：

```bash
bash mod/build.sh all
bash mod/build.sh test
```

若本机 Git Bash 在启动阶段报 `couldn't create signal pipe, Win32 error 5`，可用同一 Zig 版本直接编译回归程序和宿主；这属于 Git Bash 环境限制，不代表源码测试失败。直接编译后必须运行 `regression.exe`，确认输出 `ALL TESTS PASSED: 0 failure(s)`。

## 安装测试包

测试包至少需要这些文件：

```text
NexExclusive.exe
nexsink.dll
hook.cfg
ui.json
ui\panel.js
ui\simplify.js
```

已有安装目录通常是 `%LOCALAPPDATA%\NexExclusive`。替换宿主、DLL 和面板脚本前，先结束旧宿主；若目录 ACL 拒绝写入，需要以管理员身份完成文件替换。`host.cfg` 保留客户端路径，不要用测试包里的空配置覆盖它。

## 启动宿主

宿主启动后会：

1. 读取 `host.cfg`，确认 `cloudmusic.exe` 路径。
2. 找到网易云主进程；已有后台进程时先尝试呼出它的窗口，没有进程时才启动客户端。已验证版本可使用 `--remote-debugging-port=9333`，未验证版本优先走可见启动路径。
3. 通过 `OpenProcess`、`VirtualAllocEx`、`WriteProcessMemory`、`CreateRemoteThread` 和 `LoadLibrary` 注入 DLL。
4. 监听客户端重启，并在新的主进程出现后再次注入。
5. 访问 CDP 时注入 `panel.js` 与 `simplify.js`。脚本带版本标记，网易云进程未退出时再次注入也会替换旧版界面节点。

调试启动示例：

```powershell
Start-Process `
  -FilePath "$env:LOCALAPPDATA\NexExclusive\NexExclusive.exe" `
  -ArgumentList '--show','--console' `
  -WorkingDirectory "$env:LOCALAPPDATA\NexExclusive"
```

发布包带有 `manifest.txt`。安装前宿主会校验清单，手工检查可运行
`NexExclusive.exe --verify`。配置服务只接受网易云页面的 `orpheus://orpheus` 来源，
POST 请求必须使用 `application/json`。

宿主与网易云需要处于同一完整性级别。宿主日志出现 `OpenProcess(<pid>) 失败 5` 时，先结束普通权限宿主，再以管理员身份启动宿主；不要重复注入同一个已经加载 `nexsink.dll` 的进程。

## 日志验收

宿主日志位于安装目录的 `host.log`，重点行包括：

```text
HTTP: 配置服务已监听 127.0.0.1:9334
注入 pid=... -> LoadLibraryA=...
模块已挂载到 pid=...
界面脚本已就位 page=...
```

DLL 日志位于 `sink.log`。一次真正的播放链路至少应出现：

```text
=== nexsink attached ===
config: ...
sink ENGAGED ...
drain: thread up ...
sink: hardware started ...
```

严格原样打开后，还应看到 `hifi=1 map=client-native`。原率优先发生转换时会看到 `[converted]`。verifier 行会给出 `client_frames`、`endpoint_frames`、`changed_bytes`、首个差异位置和两端 hash。`client-native` 只证明客户端 PCM 到独占端点的映射，不能证明网易云解码前的源文件逐样本一致。

## 设置面板缺失时的判断

音频 DLL 已载入、但设置页没有“NexExclusive”入口或“独占输出”面板，优先检查 `host.log` 是否有 `界面脚本已就位`：

- 有 `模块已挂载`，没有 `界面脚本已就位`：音频链路已进入，CDP 端口未开放或页面目标未找到，面板不会出现。
- 没有 `模块已挂载`：宿主未运行、权限不匹配，或主进程 PID 已变化。
- 有面板、但没有 `sink ENGAGED`：页面脚本已经注入，当前还没有新的音频初始化或播放调用。

状态栏里的“独占未接管”表示最近一次独占尝试失败，播放仍由客户端当前路径提供。常见错误码：`0x8889000A` 是上一代独占流仍占用端点，`0x88890008` 是设备不接受当前原格式，严格原样下应切换到原率优先，`0x8889000F` 是端点创建失败。共享回退始终关闭。

`/api/status` 里的 `verification` 对象来自最近一条 `hifi verifier` 日志，包含 `clientFrames`、`endpointFrames`、`verifiedFrames`、`changedBytes`、`droppedFrames` 和 `underrunEvents`。`clientToEndpoint=bit-perfect` 只代表客户端 PCM 到独占端点逐样本一致；`source=unverified` 明确保留网易云解码器入口之前没有逐样本证据这一边界。

网易云从资源管理器普通启动时，通常没有 `9333` 调试端口；这时面板脚本无法从宿主注入。要同时得到面板与音频链路，应由宿主启动客户端，并确认 `host.log` 没有连续的 `CDP 端口未就绪`。

## 本机实测记录

2026-11-12 的新构建实测：

- Zig 0.17.1 编译出的 DLL 与宿主成功生成。
- 高权限宿主对可见网易云主进程 `27972` 注入成功，`nexsink.dll` 模块路径为 `%LOCALAPPDATA%\NexExclusive\nexsink.dll`。
- `sink.log` 已记录 44.1 kHz 客户端输入、24 位端点输出、`sink ENGAGED`、硬件启动和 drain 线程运行。
- 普通权限宿主对同一可见进程返回错误 5；这是权限级别问题，不是 DLL 初始化失败。
- 当前这台机器通过宿主启动路径已开放 9333；设置页已验收为单一 `NexExclusive` 模块，旧说明句已清除，编辑弹窗有保存和取消，播放页无损标签旁会显示音频规格框。若页面重绘后入口消失，先看宿主是否仍记录 `界面脚本已就位`，再重新打开设置页触发脚本挂载。

OpenHome 与网易云项目的架构取舍，以及 Apple Music 离线数据库解密为什么不直接适用，见 [OPENHOME_NETEASE_RESEARCH.md](OPENHOME_NETEASE_RESEARCH.md)。
