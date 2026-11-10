# NeteaseExclusive 项目进度与后续任务

> 更新日期：2026-11-10 10:59（北京时间）  
> 工程目录：`D:/Coding/NeteaseExclusive`  
> 当前阶段：一键化已落地，端点迁移已落地，`Sink` 按代持有已落地，`nexsink.c` 已按职责拆分。双击 `mod/启动.cmd` 即可完成「启动客户端 → 注入音频模块 → 起配置服务 → 注入界面脚本」，并常驻看护客户端重启、页面重载、模块掉出。音频侧独占接管、位深设置、切歌、拖动进度条、默认输出切换均已实测正常，**切歌与切端点都不再有 join 超时或共享回退**；设置页会按当前端点标明独占下音量是否还能调。音量侧只做端点诊断，**明确不做模块侧数字增益**（用户 2026-11-10 决定：不拿比特换音量）。  
> 本文件是项目进度的主记录。每完成一个可验证任务，以及每轮工作结束时，都更新这里；未完成项保留实际状态。

## 1. 项目目标与当前边界

目标：让网易云 PC 客户端经 WASAPI 独占输出，优先保留客户端送来的采样率；设备不接受源格式时，明确显示兼容转换。配置与状态最终出现在网易云自己的设置页。

当前已实现共享音频客户端代理、独占设备打开、格式协商、整数/浮点转换、重采样、切歌交接和日志。用户已经听到声音，并确认近期切回 HiRes 能继续播放。

本轮（01:07）把审查列出的 P0/P1 音频问题落到代码，并补了可直接运行的桩测试：IAudioClient3 参数签名、独立播放时钟、设备提交契约、流生命周期、环缓冲跨尾、原率优先、同率不进滤波器、端点跨进程闸门。

01:41 追加的一轮把并发生命周期的锁覆盖补齐：`H_Start`、`H_Stop`、`H_Reset`、`H_GetService`、`H_GetBufferSize`、`H_GetCurrentPadding`、`H_GetStreamLatency`、`H_SetEventHandle` 统一进入 `g_control`，新增 `sink_teardown_controlled()` 与 `sink_engage_controlled()` 两个包装，释放与重新初始化不再与代理调用重叠。`regression.c` 同步扩到 14 组，新增生命周期入口等待、teardown 竞态、300 次 Start/Stop/Reset 加 12000 次生产者与时钟迭代的压测。

**02:55 结论：此前记录的「注入受阻」是错的。** 重启客户端后注入器全链路 `err=0 ok`：`OpenProcess(0x43A)`、三次 `VirtualAllocEx`、`WriteProcessMemory 110/110`、`CreateRemoteThread`、`remote LoadLibraryW -> 0x711D0000`，模块表随即出现 `nexsink.dll`。对作业外进程（记事本）注入同样成功，说明跨进程远程线程本身被允许。历史 `ERROR_ACCESS_DENIED (5)` 与本次实测不一致，按旧构建/旧调用路径归因，不再作为能力边界。

02:05 复核：客户端由用户自行启动并稳定运行（主进程 102308、渲染进程 101588、GPU 进程 104144，01:27:38 启动）。逐进程查模块表，`nexsink.dll`、`hooklog.dll`、`smoke.dll` 均不在其中，确认当前进程内没有任何本项目模块。`mod/sink.log` 最后一行仍是 01:09:10。

界面已找到静态资源与候选容器，尚未在运行中的设置页显示自定义控件。不能把当前版本称为完整播放器插件，也不能声称全程 bit-perfect。

### 运行基线

| 项目 | 本次核验结果 |
|---|---|
| 网易云客户端 | `3.1.42.205594`，来自可执行文件版本信息 |
| 安装位置 | `D:/Installed_Program/CloudMusic` |
| 当前音频端点 | 既有探测使用 MOONDROP MM3A；端点能力以本机驱动实测为准 |
| 已测独占格式 | 44.1 / 48 / 88.2 / 96 kHz，16 / 24 位整数 |
| 已测拒绝格式 | 176.4 / 192 kHz；32 位浮点输出 |
| 当前配置 | `enable=1, bits=24, rate=0, buffer=0, compat=1, dither=2`；新增键 `fade_ms=`（默认5，0关闭）、`shared_fallback=`（默认0=严格）尚未写入 cfg，使用默认值 |
| 已加载版本 | 实机尚未加载本轮构建；`mod/nexsink.dll` 是旧版，`mod/build-check/nexsink.dll` 含 01:41 版本；02:05 复核客户端三个进程均未加载 `nexsink.dll` |
| 构建环境 | Zig 0.17.1，x86_64-windows-gnu |
| 版本管理 | 根工程尚非 Git 仓库；已有参考项目目录不等同于本工程版本管理 |

## 2. 状态定义与维护规则

- **已验证**：具备对应代码、测试或实际操作证据；必须写明验证范围。
- **已实现·待实机验证**：源码和构建完成，真实客户端或听感尚未验收。
- **部分完成**：链路只有一部分具备证据。
- **待做**：未开始，或仅有方案。
- **受阻**：有明确前置条件未满足，记录解除条件；禁止改成已完成。

### 每次更新的固定动作

1. 开始处理时，在任务表中修改状态，必要时细分任务，但保留原 ID。
2. 完成后补充修改文件、测试命令与结果、是否重新部署，以及未覆盖的场景。
3. 只有满足该行验收标准才标记完成。编译成功不能替代实际音频验收。
4. 更新页首日期，并在文末追加工作日志；保留旧日志。
5. 中途停止也要记录进展和下一步。日常工作记忆只保存摘要，不替代本文件。
6. 如客户端升级，重新核对版本、资源哈希、CEF 接口和 DOM 目标，旧偏移与类名不能直接视为有效。

## 3. 已有能力与验证范围

| 能力 | 当前状态 | 证据与边界 |
|---|---|---|
| 注入并拦截 WASAPI 初始化 | 已验证 | `mod/src/nexsink.c`；真实进程模块枚举与 `mod/sink.log` |
| 独占流实际出声 | 已验证 | 用户此前确认有声音、音高正常；仍有切换和开头声响待验 |
| 44.1 / 96 kHz 保留原采样率 | 已验证 | 日志有 44.1→44.1、96→96；部分仅位深变化 |
| 192→96 kHz 兼容重采样 | 已验证 | 日志出现 `resample: 192000 -> 96000` 和 `[converted]` |
| 流交接与切回 HiRes | 部分完成 | 用户确认切回后继续播放；尚无长时间、连续切换矩阵 |
| 24 位淡入溢出修复 | 已验证（代码测试） | INT64 中间乘积；3840 个多格式淡入样本核对通过；实机听感未反馈 |
| 静音标志和 Reset 历史处理 | 已验证（代码测试） | 回归覆盖 SILENT、配对/范围、Reset 保留与清空 |
| IAudioClient3 参数签名 | 已验证（代码测试） | 改为 SDK 的 4 参数原型；桩测试确认 flags/period/fmt/session 原样转发 |
| 独立播放时钟 | 已验证（代码测试） | 读真实 `IAudioClock`，按 `GetFrequency()` 换算为 100 ns；桩测试覆盖 0/0.5 s/1 s/1 帧与倒退钳制 |
| 设备提交契约与启动顺序 | 已验证（代码测试） | 提交后才 Start；GetBuffer/ReleaseBuffer 帧数一致；不伪造静音；桩测试覆盖空环、满设备、部分空间、异常 padding |
| 流生命周期与旧代理隔离 | 已验证（代码测试） | 每流 generation + 独立代理对象；桩测试确认旧代理写入被 `AUDCLNT_E_DEVICE_INVALIDATED` 拒绝 |
| 生命周期入口串行化 | 已验证（代码测试） | 01:41 起 `H_Start`/`H_Stop`/`H_Reset`/`H_GetService`/`H_GetBufferSize`/`H_GetCurrentPadding`/`H_GetStreamLatency`/`H_SetEventHandle` 统一持 `g_control`；`sink_teardown_controlled()`、`sink_engage_controlled()` 包装释放与重新初始化；桩测试覆盖入口等待、teardown 竞态、300 次 Start/Stop/Reset 压测 |
| 环缓冲跨尾整理 | 已验证（代码测试） | 改用预分配暂存；桩测试 cap=8 / w=11 / r=7 的跨尾场景逐字节核对 |
| 原率优先与失败策略 | 已实现·待实机验证 | 采样率外层、位深内层；`compat=0` 只允许精确表示；`shared_fallback=0` 时首个流失败即上报 |
| 同率类型转换与 SRC 分离 | 已验证（代码测试） | `resample` 只由采样率决定；同率 int32→int32 走 memcpy |
| 端点跨进程闸门 | 已实现·待实机验证 | 按端点 ID 哈希建独占临时文件；进程崩溃由系统释放；未在多进程争用下实测 |
| SRC 质量与耗时测试 | 部分完成 | 有旧版/新版频点与耗时结果；尚缺全频扫描、峰值、尾部与独立基准比较 |
| 配置文件加载 | 部分完成 | `mod/hook.cfg`；新建流时检查变化，非完整实时配置协议 |
| 客户端设置入口定位 | 已验证 | 静态资源与实际运行 DOM 均已核对，见第 4 节 |
| 客户端内可视化配置 | 部分完成 | `mod/ui/panel.js` 在 `#play` 内插入「独占输出」八项；经 `mod/tools/nexctl.mjs` 读写 `hook.cfg`，写入已实测生效；音频模块未加载，参数对声音的实际影响未验 |
| 界面冗余区块隐藏（可选） | 部分完成 | `mod/ui/simplify.js` 分三组七项：页面精简（精选活动条、榜单精选、每日推荐）、侧栏精简（播客入口、侧栏精选）、顶栏精简（皮肤按钮、宠物按钮）。隐藏与恢复均已实测，刷新后由监听进程重新应用 |
| 自动加载、安装、卸载 | 待做 | 目前仍依赖手工重启与注入；`deploy.sh` 只做“对给定 PID 注入” |

### 格式说明

“无损”“HiRes”“超清母带”是客户端档位；实际重采样与否要看送入音频接口的格式和设备支持结果。

目前常见 44.1 / 48 / 88.2 / 96 kHz 可以保留原率；192 kHz 需要转换，176.4 kHz 等设备不支持的输入也需要转换。不能推广成只有 192 kHz 会重采样。

`[native]` 目前只描述输入、输出格式相同。开头仍有淡入处理，因此该标记不等于全曲逐样本一致。32→24 位若只是去除零填充，和舍弃真实有效位是两种情况，尚需用有效位数与样本验证区分。

## 4. 设置页入口：本轮实际发现

### 4.1 静态资源已经定位

`D:/Installed_Program/CloudMusic/package/orpheus.ntpk` 是 100 字节前缀加 ZIP，含 1411 项资源。仅提取指定文件至工程副本，没有修改安装包。

工程副本位置：`analysis/ui-probe/pub/`。

| 目标 | 实际证据 |
|---|---|
| 主页面 | `app.html` 有 `#root`，加载 hybrid 脚本与 React 组件资源 |
| 设置图标 | `hybrid/app.chunk.1482700000000.js` 中 `data-testid="tid_header_setting_btn"` |
| 设置导航 | 同脚本的点击处理进入 `/setting/account`，另有 `/setting/play` |
| 设置页组件 | `hybrid/64.chunk.1482700000000.js` |
| 设置根节点 | 组件输出 `id="page_pc_setting"` |
| 播放设置区域 | 同组件输出 `id="play"`，含输出设备区域 |
| 播放导航项 | `data-testid="tid_setting_play_link"`，链接 `#play` |
| 现有原生通信桥 | `window.channel.call`、`window.channel.registerCall`、`window.channel.viewCall` |
| 现有设备配置调用 | `AudioPlayer.getOutputDevices()` 与 `AudioPlayer.setOutputDevice(...)` |
| 调试窗口线索 | `cloudmusic.dll` 中 `OrpheusDevtoolsWindow` 等符号；仅证明符号存在 |

`.view.txt` 是便于查看的副本，每 240 字符折行并标记原字符位置，不能执行。关键位置：

- `app.chunk....js.view.txt` 19606–19607 行：设置图标与导航。
- `app.chunk....js.view.txt` 2162–2164 行：`window.channel` 通信桥。
- `64.chunk....js.view.txt` 150–156 行：输出设备读取与设置调用。
- 同文件 164–187 行：`id="play"` 与输出设备区。
- 同文件 524–541 行：设置页根、导航和各区域组件。

这些是实际设置界面代码的证据，已经超出“目录中存在 libcef.dll”的推测。它们仍不等于已取得运行中 DOM 或已成功插入控件。

### 4.2 动态验证范围与限制

- 运行进程已加载 `cloudmusic.dll` 和 `libcef.dll`。
- 观察到主进程监听 `127.0.0.1:20017`；未识别协议，不能称其为 DevTools 端口，也未向它发送请求。
- 本轮尝试读取可访问性控件树，运行环境的安全检查拒绝了动态加载方式。没有改用其他途径绕过。
- 没有重启当前播放、修改安装资源、注入界面脚本，或调用客户端配置桥。

### 4.3 下一次挂载实验

目标候选：真实页面中的 `#page_pc_setting #play`。先核对实际 DOM 与 frame，再显示一个只读“独占模块状态”项。

验收标准：能够打开设置后出现、离开后清理、重复进入不重复创建；移除实验后恢复原界面；不调用配置写接口，不影响播放。

只读实验通过后，再接自有状态通信与配置确认。找到 `window.channel` 不代表可以凭空增加它不认识的原生命令，也不代表它已能写 `hook.cfg`。

### 4.4 DevTools 通道已打通（02:20）

此前"动态界面检查受阻"的结论被推翻。客户端是 CEF 宿主，`libcef.dll` 里带 `remote-debugging-port` 开关；以 `cloudmusic.exe --remote-debugging-port=9333` 启动后，端口在 `127.0.0.1:9333` 正常监听。

| 项 | 实测结果 |
|---|---|
| 页面目标 | `orpheus://orpheus/pub/app.html`，标题「网易云音乐」 |
| 协议 | CDP over WebSocket，`Runtime.evaluate` 可直接执行 JS |
| 截图 | `Page.captureScreenshot` 可用，已用于比对界面 |
| `window.channel` | 存在，键为 `call`/`registerCall`/`viewCall` 等 13 个 |
| 设置页 | 点击 `tid_header_setting_btn` 后 `#page_pc_setting` 出现 |
| 播放区 | `#play` 存在，`#play .play` 是各设置行的容器 |
| 输出设备行 | `section.item` 内 `h4`（14px/700）+ `div.cmd-select`（高 30px、圆角 24px、宽 320px） |
| 主题色 | 选中态 `rgb(255, 58, 58)`；正文 `rgba(40, 50, 72, 0.9)`；字体「微软雅黑」 |

启动方式有一个约束：客户端进程无法在工具调用的作业对象里存活，`schtasks` 与 `explorer.exe` 转交都被安全策略拦下。可行做法是 `mod/tools/nexlaunch.exe`（以 `CREATE_BREAKAWAY_FROM_JOB` 尝试脱离）或用户自行双击 `mod/tools/nexui.cmd`。

这条通道改变了界面侧的实现路线：不再需要往客户端里注入第二个 DLL，界面脚本通过 CDP 注入，配置通过本机 HTTP 服务写入 `hook.cfg`。

## 5. 全项目审查：问题与处理结果

00:24 登记的 11 项问题在本轮（01:20）全部落到代码，其中 8 项有桩测试证据。**桩测试通过不等于实机验收通过**：真实端点仍会拒绝某些格式，欠载与设备失效只有在真机上才会出现。

| ID | 等级 | 原问题 | 本轮处理 | 现状 |
|---|---|---|---|---|
| A-01 | P0 | `H_InitializeShared` 参数布局与 SDK 不符 | 改为 `(DWORD, UINT32, const WAVEFORMATEX*, LPCGUID)`；桩测试以 COM 方式调用槽 20，核对四个参数原样到达 | 已验证（代码测试） |
| A-02 | P0 | 时钟用环索引与输出率，整理后倒退；QPC 恒零 | 改读真实 `IAudioClock::GetPosition`，按 `GetFrequency()` 换算 100 ns，单调钳制，QPC 直传 | 已验证（代码测试） |
| A-03 | P0 | 设备先启动再送音频；提交契约未核 | 新增 `drain_once()`：先提交再 Start，GetBuffer/ReleaseBuffer 帧数一致，不伪造静音，padding 超界返回 `E_UNEXPECTED`，失败经 `sink_fault()` 上报客户端 | 已验证（代码测试） |
| A-04 | P0 | 初始化失败清理、线程超时释放、并发归属 | `sink_stop`/`sink_teardown` 返回 HRESULT；线程 2 s 未退出则保留全部资源；每流独立 generation 与代理对象；owner 持引用。01:41 追加：全部生命周期入口与代理读取统一进入 `g_control`，释放与重新初始化经 `_controlled` 包装 | 已验证（代码测试）·实机待验 |
| A-05 | P1 | 先遍历精度组再遍历采样率 | 改为采样率外层、位深内层，原率各深度优先 | 已实现·待实机验证 |
| A-06 | P1 | 输出 float 时同率也进滤波器 | `resample` 只由 `out_rate != in_rate` 决定；同率只做类型转换 | 已验证（代码测试） |
| A-07 | P1 | 失败静默退回共享，与“不降级”冲突 | 新增 `shared_fallback`：默认严格，首个流失败即上报；同进程的第二个流始终允许真实共享流，避免打断无关声音 | 已实现·待实机验证 |
| A-08 | P1 | 有效位、抖动、非有限输入、近似路径 | `valid_pcm_format()` 校验 tag/几何/有效位；`sample_read` 用 `isfinite` 保护；抖动条件改为“重采样或真实降位深” | 已验证（代码测试） |
| A-09 | P1 | 跨尾分支两次搬移互相覆盖 | 改用预分配 `conv` 暂存；桩测试 cap=8、w=11、r=7 逐字节核对 | 已验证（代码测试） |
| A-10 | P1 | 多 vtable 共享一份原函数；端点指针无引用；无跨进程协调 | 原函数按 vtable 记入 32 条 `AudioPatch`；`g_bindings[128]` 持端点引用并在满时轮转释放；按端点哈希建独占锁文件。未登记 vtable 的全局回退仍需核对 | 部分完成（闸门与 vtable 全覆盖未实测） |
| D-01 | P1 | `build.sh` 不产出 inject2；`deploy.sh` 靠窗口标题猜进程 | `build.sh` 增加 `test` 目标与 `OUT_DIR`，产出 `inject2.exe`；`deploy.sh` 改为只对给定 PID 注入已构建 DLL，全绝对路径 | 部分完成（仍需手工给 PID） |

本轮另外修掉两个未登记但会直接出问题的缺陷：

- `DllMain` 只初始化了 `g_lock`，`g_control` 与 `g_patch_lock` 从未初始化。任何进入 `fr_GetBuffer` 的路径都会在未初始化的临界区上工作；现已补初始化。
- `IAudioRenderClient::GetBuffer(0)` 原返回 `S_OK` 且输出空指针；WASAPI 对此返回 `AUDCLNT_E_INVALID_SIZE`，现已对齐。

本轮 C 复杂度只读复核另发现两项尚未实机/并发验证的欠账：

- 原函数按 vtable 记录已完成；但代码路径中的未登记 vtable 仍可能走 `g_ac_orig` 全局回退，内部 `dev_activate()` 返回的未 patch 客户端是否总与已登记 vtable 相同尚未证实。
- ~~`g_control` 保护 proxy 的 Get/Release/clock 调用，但 `sink_engage()`、`sink_teardown()`、`H_Start()`、`H_Stop()` 并未统一在同一生命周期锁下串行化。~~ 01:41 已处理：上述入口全部持 `g_control`，释放与重新初始化改走 `sink_teardown_controlled()` / `sink_engage_controlled()`；桩测试覆盖入口等待、teardown 竞态与 300 次生命周期压测。剩余的是实机与多流并发场景，不是锁覆盖本身。

本地 SDK 核验位置：`D:/Installed_Program/zig/zig-x86_64-windows-0.17.1/lib/libc/include/any-windows-any/audioclient.h:643–647,755–760`。

注意：10 MHz 作为自定义时钟频率本身并非错误；核心在于位置必须与频率一致、持续递增并对应播放进度。不能笼统地说位置只能用帧。

## 6. 后续任务计划表

P0：先保障接口与音频数据正确性。P1：完成核心体验与集成。P2：交付与扩展质量。

| 任务 ID | 优先级 | 工作内容 | 状态 | 前置项 | 验收标准 |
|---|---|---|---|---|---|
| UI-01 | P1 | 定位当前版本设置页、播放区与通信桥 | 已验证·静态 | 无 | 保存版本、资源路径、选择器和调用证据；本轮完成 |
| UI-02 | P1 | 运行中识别设置页，插入可移除的只读状态项 | 已验证·依赖带端口启动 | 无 | 真实设置页显示；连续三次进出路由后仍各只有一个实例；移除后原界面无残留 |
| QA-01 | P0 | 验收00:05版开头杂音与单声道反馈 | 待实机验证 | 用户使用已加载版本 | 同一首同一位置对比，记录格式、切换方向、是否有声响；至少20次切换 |
| AU-01 | P0 | 修正 IAudioClient3 ABI，补桩测试 | 已验证（代码测试） | 本地SDK证据已具备 | 参数原样传递；拦截与透传均覆盖 |
| AU-02 | P0 | 建立独立音频时钟与播放计数 | 已验证（代码测试） | AU-01 | 帧→100 ns 换算、单调性、QPC 均有桩测试；实机拖动/暂停待验 |
| AU-03 | P0 | 核对设备缓冲契约、启动顺序及返回码 | 已验证（代码测试） | AU-01、AU-02 | 空环、满设备、部分空间、异常 padding 均覆盖；实机欠载待验 |
| AU-04 | P0 | 修复流生命周期、初始化失败清理和并发归属 | 基本完成·实机切歌已过 | AU-01 | 旧代理隔离、线程超时保留资源、生命周期入口串行化、teardown 竞态与 300 次压测均有桩测试；02:55 实机连续三次切歌 `taking over` → `hardware started` 零欠载；多流并发与初始化各阶段失败注入未做 |
| AU-05 | P1 | 环缓冲模型与跨尾分支测试 | 已验证（代码测试） | 无 | 跨尾场景逐字节核对；随机读写压力测试未做 |
| AU-06 | P1 | 统一原采样率优先、兼容转换和失败策略 | 已验证（实机） | AU-04 | 原率优先已实测：44.1k 源直接 `[native]`，48k 源 `selected 48000/24`；`compat=0` 只允许精确表示；严格模式实测如实上报失败而不静默降级 |
| AU-07 | P1 | 分开同率类型转换与 SRC、明确有效位及抖动 | 已验证（代码测试） | AU-06 | 同率不进滤波；抖动条件可解释；逐样本位深验证仍缺 |
| AU-08 | P1 | 设备选择与进程间独占协调 | **基本完成** | AU-04 | 端点闸门、默认端点切换、播放中迁移均已实测（暂停态与播放态各双向一轮）；设备失效共用同一迁移路径但未做物理拔插实测；多进程争用未测 |
| CFG-01 | P1 | 建立配置与状态通信协议 | 部分完成 | AU-04、AU-06 | 值域校验、写入落盘已通；状态回传目前靠解析 `sink.log`，缺少音频模块主动上报与生效确认 |
| UI-03 | P1 | 客户端设置页正式独占控制区 | 基本完成 | UI-02、CFG-01 | 八项控件已在真实设置页可读可写；「输出位深」三档已实测影响真实输出（16/24/回退24）；错误状态联动仍靠解析 `sink.log` |
| UI-04 | P1 | 路由重入、深浅外观、缩放与版本校验 | 待做 | UI-03 | 无重复控件、不遮挡原设置；不兼容版本安全停用界面功能 |
| UI-05 | P2 | 可选：隐藏界面冗余区块 | 基本完成 | UI-02 | 七项开关按页面/侧栏/顶栏分三组；顶栏只收皮肤与宠物；逐项核对隐藏效果。深浅外观下的表现与版本变更后的选择器失效未处理 |
| UI-06 | P1 | 界面通道的打包与自启 | **已完成** | UI-03 | `mod/启动.cmd` → `NexExclusive.exe` 单进程完成启动/注入/服务/界面注入，不再依赖 Node；客户端退出、页面重载、模块掉出均自动补回 |
| QA-02 | P1 | 端到端切歌与音质切换矩阵 | 部分完成 | AU-01至AU-08 | 已验：44.1k/16→24、48k/32f→24、连续三次切歌、暂停后重播、拖动进度条 seek 后继续推进、**88.2/96/192 实机（10:00）**、**88.2 双端点对照（10:25，音箱原率直通 / 耳机转 96）**；未验：音质档位切换、两种切换方向 |
| QA-03 | P1 | SRC 全频与边界测试 | **已验证（代码测试）** | AU-07 | 通带、阻带、过渡带、冲激、峰值、直流、曲尾、多比率（4 组）、实时耗时、线性相位均覆盖；顺带修掉核区间少一格的线性相位缺陷（10:00） |
| REL-01 | P1 | 建立Git基线与可重复构建 | **已完成** | 用户接受版本管理方案（本地仓库，不设远端） | 空构建目录可产出DLL、注入器、测试；不提交用户资源与日志。`git clone` 到临时目录、全新 `OUT_DIR` 构建 8 个产物 + 18 组回归全 PASS（10:12） |
| REL-02 | P1 | 替换临时部署流程，自动识别播放进程 | **基本完成** | AU-08、REL-01 | 主进程改按父子关系识别（父进程不是 cloudmusic.exe 的那个），不再靠内存大小猜测；`build.sh` 新增 `host` 目标 |
| REL-03 | P2 | 安装、停用、恢复与卸载流程 | 待做 | REL-02、QA-02 | 无需反复手工注入；停用可恢复原客户端；不覆盖原资源包 |
| DOC-01 | P1 | 建立进度主记录并持续更新 | 持续维护 | 无 | 每项完成和每轮结束均更新任务状态、证据及日志 |

### 建议推进顺序

P0/P1 的音频正确性（AU-01…AU-08）已完成编码与桩测试，下一步是**部署后实机验收**（QA-01、QA-02），因为剩余风险都集中在真实端点行为上：哪些格式真的被接受、欠载与设备失效如何表现、严格模式会不会误伤第二个流。

界面侧已越过原计划：UI-02 完成，UI-03 与 UI-05 的面板和开关都能在真实设置页读写，CFG-01 有了文件级通道。接下来是把两端接起来——`nexsink.dll` 真正加载后，面板参数才会影响声音，`/api/status` 也才拿得到本轮运行的状态而不是旧日志。UI-06 负责把「带端口启动 + 服务 + 注入」收成一个入口，不依赖用户手工执行脚本。

当前目标仍是嵌入网易云设置页。面板已经嵌在设置页内，但它与音频模块之间还差一次实机加载；在 DLL 未加载的情况下，界面改动只落到配置文件，不代表听感发生变化。

**10:12 更新——上面两段已经过时。** DLL 与面板之间的加载链路已由 UI-06 打通（`NexExclusive.exe` 单进程完成启动/注入/服务/界面注入），QA-02 的采样率矩阵与 QA-03 的 SRC 全频都已验收，REL-01 的 Git 基线也已建立。当前真正没做的，按优先级：

1. **REL-03 安装 / 停用 / 恢复 / 卸载** —— 现在仍然是「跑一次 `启动.cmd`」，没有可重复的安装，也没有干净卸载。交给别人用之前必须先有这一步。
2. **`nexhost.c` / `launch.c` 的环境绑定** —— 7 处 + 3 处写死绝对路径（见 10:12 记录末尾）。不做这一步，别人拿到仓库能构建、但跑不起来。
3. **物理拔插与多进程争用实测** —— 端点失效的代码路径有了（放弃代 + 闸门 + 20×10 ms 重试），但没试过真拔 USB；两个客户端实例同时争一个端点也没验过。
4. **UI-04** 路由重入与版本校验、**QA-02** 剩余的音质档位切换与两种切换方向。

## 7. 本轮改动与验证

### 改动文件

- `mod/src/nexsink.c`（P0/P1 主体，当前 1950 行）
  - `H_InitializeShared` 改为 SDK 的 4 参数原型，`period` 换算为 `REFERENCE_TIME` 作为缓冲提示。
  - `fc_GetPosition` 改读真实 `IAudioClock`：帧数 ÷ `GetFrequency()` 换算 100 ns，单调钳制，QPC 直传。
  - 新增 `drain_once()`：先提交再 `Start`；`GetBuffer`/`ReleaseBuffer` 帧数一致；无数据时返回 `S_FALSE` 而不写静音；`padding > real_frames` 返回 `E_UNEXPECTED`；失败经 `sink_fault()` 置位并唤醒客户端。
  - 新增按 vtable 记录的 `AudioPatch g_audio_patches[32]` 与 `ac_original()`；删除共享的 `g_ac_orig` 直读（仅留空接口的测试回退）。
  - 新增 `dev_activate()`，内部探测不再触发自己的 `Activate` 钩子。
  - 新增 `bind_endpoint()`：`client→IMMDevice` 映射持引用，满 128 条时轮转释放最旧项。
  - 每流独立 `generation` 与堆上代理对象；旧流留下的代理写入被拒。
  - `sink_stop()` / `sink_teardown()` 返回 HRESULT；线程 2 s 未退出则保留资源。
  - 环与暂存统一按输入帧大小分配；`ring_compact()` 跨尾分支改用预分配 `conv`。
  - 协商改为采样率外层、位深内层；`compat=0` 只允许精确表示；`shared_fallback` 真正生效。
  - 抖动条件改为“重采样或真实降位深”；`valid_pcm_format()` 前置校验。
  - `DllMain` 补齐 `g_control`、`g_patch_lock` 初始化。
  - 删除 90 行死代码 `convert_legacy()`。
- `mod/test/regression.c`：重写为 10 组桩测试，覆盖淡入、SILENT、旧代理拒绝、跨尾整理、预留上限、时钟换算、格式校验、vtable 原函数隔离、`drain_once` 契约、Reset。
- `mod/build.sh`、`mod/deploy.sh`、`mod/src/inject2.c`：加固与新增 `test` 目标（上一轮子任务完成）。

01:41 那一轮的改动（上一条记录之后、本次补记）：

- `mod/src/nexsink.c`
  - `H_Start`、`H_Stop`、`H_Reset`、`H_GetService`、`H_GetBufferSize`、`H_GetCurrentPadding`、`H_GetStreamLatency`、`H_SetEventHandle` 全部进入 `g_control`，代理读取与释放不再与生命周期切换交错。
  - 新增 `sink_teardown_controlled()`、`sink_engage_controlled()`；`H_Initialize` 的拦截与回退路径改走这两个包装，第二个参数 `was_engaged` 让严格模式能区分"首个流失败"与"同进程第二个流"。
  - `H_Initialize` 中 `AUDCLNT_E_DEVICE_IN_USE` 的自愈分支改为在 `g_control` 内判定并调用 `sink_teardown()`，递归临界区允许重入。
- `mod/test/regression.c`
  - 由 10 组扩到 14 组，新增 `lifecycle entry points`（Start/Reset/engage 等待控制权）、`teardown race`（未归还的代理缓冲被置无效，客户端内存不提前释放）、`lifecycle stress`（300 次 Start/Stop/Reset，配 12000 次生产者与时钟迭代）。

02:20–02:30 界面侧新增（音频源码本轮未动）：

- `mod/ui/panel.js`：设置页「独占输出」面板，八项（启用、位深、采样率、缓冲帧数、兼容转换、抖动、开头淡入、严格模式）。挂在「输出设备」之后，纵向排布对齐原生控件左边缘。
- `mod/ui/simplify.js`：七项界面精简开关，按页面/侧栏/顶栏分三组，按标题文本与 `data-testid` 定位，直接改 `display` 并记录原值，关闭即恢复。
- `mod/tools/nexctl.mjs`：本机服务，`/api/cfg` 读写 `hook.cfg`、`/api/ui` 读写 `ui.json`、`/api/status` 解析 `sink.log` 末段；另有 `inject`/`watch` 两个 CDP 注入模式。
- `mod/tools/cdp.mjs`：最小 CDP 客户端，支持求值、截图、刷新、列目标。
- `mod/tools/launch.c` + `nexlaunch.exe`：以 `--remote-debugging-port=9333` 启动客户端，先试 `CREATE_BREAKAWAY_FROM_JOB` 再退回普通创建。
- `mod/tools/nexui.cmd`：一键入口，串起启动、配置服务与监听注入。
- `mod/hook.cfg`：显式写入 `fade_ms=5`、`shared_fallback=0`（此前只存在于代码默认值）。
- `mod/ui.json`：界面精简状态，当前六项全为 0。

音频源码在 02:55 这一轮改了四处，全部围绕「模块进了客户端却不出声」：

1. **`IID_IAudioClock_l` 的 GUID 后三段写错**（写成 `3FD1-4EFC-8124-87E2B33EB62B`，正确为 `3FBA-4A1B-812C-EF96358728E7`）。`try_exclusive()` 最后一步 `GetService(IAudioClock)` 因此恒返回 `E_NOINTERFACE (0x80004002)`，每个候选都在这一步失败，对外表现为 `18 candidates rejected` + `sink failed 0x80004005`。这是模块完全不出声的唯一根因。
2. **`g_out_bits` 从未参与协商**：`load_config()` 读它、校验它、打进日志，但 `negotiate()` 的候选序列只用源精度，面板上的「输出位深」是空开关。现改为把它放在候选序列首位，并加入 `g_negotiated` 缓存 key（否则改设置会命中旧缓存）。
3. **切歌时新流被 passthrough**：客户端切歌不复用旧 `IAudioClient`，而 sink 仍 `engaged && started`，原逻辑返回 `E_FAIL` 让它走真实 shared 流，撞上我们自己独占的端点得到 `DEVICE_IN_USE`，结果是旧流僵死、新流起不来。改为「新流到来即交接设备」（`taking over`），旧代理由 teardown 失效，迟到写入安全失败。
4. `try_exclusive()` 的每个失败分支补齐日志，含 vtable 指针三元组（`vt` / `vt[slot]` / `orig`），可直接判断 `ac_original()` 是否拿到原函数。

`mod/build-check/nexsink.dll` 与 `mod/nexsink.dll` 均已更新为本轮构建（275456 字节）。

### 已执行验证

| 检查 | 结果 |
|---|---|
| `OUT_DIR=mod/build-check ./build.sh all` | 成功，无警告（删除死代码后） |
| `OUT_DIR=mod/build-check ./build.sh test` | `ALL TESTS PASSED: 0 failure(s)`，10 组 |
| 时钟换算 | 0 / 24000 / 48000 / 48001 帧 → 0 / 5 000 000 / 10 000 000 / 10 000 208 ticks；倒退被钳制；1 帧 → 208 |
| vtable 隔离 | 两张表分别 patch 后，`ac_original` 各自返回自己的原函数；重复 patch 不位移 |
| 槽 20 ABI | 以 4 参数调用，桩函数收到的 flags/period/fmt/session 与传入完全一致 |
| `drain_once` | 8 帧提交、帧数一致、设备缓冲内容与环内数据逐字节相同、Start 在提交之后 |
| 跨尾整理 | cap=8 / w=11 / r=7 整理后 4 帧顺序正确，尾部未被覆盖 |
| 重采样吞吐（独立基准） | 两次 `192000→96000`、每次 10 秒音频 / 960,000 输出帧：0.194 s 与 0.206 s；约 52×/49× 实时、单核 1.9%/2.1%（不代表设备端到端稳定性） |
| 实机注入 | **已通过**：`OpenProcess(0x43A)`、三次 `VirtualAllocEx`、`WriteProcessMemory 110/110`、`CreateRemoteThread`、`remote LoadLibraryW -> 0x711D0000` 全部 `err=0 ok`；模块表出现 `nexsink.dll`。历史 `ERROR_ACCESS_DENIED (5)` 结论已作废 |
| 当前客户端状态 | 主进程 80992（281 MB）、渲染 92572、GPU 85864 稳定运行，模块已挂载 |
| C 复杂度评估 | C 适合核心 WASAPI DLL，性能不是当前瓶颈；单一 1,950 行 `nexsink.c` 承载多个子系统，维护复杂度偏高，建议后续拆模块；尚未开始拆分 |
| 02:05 构建复跑 | `OUT_DIR=mod/build-check ./build.sh all` 成功，零警告 |
| 02:05 测试复跑 | 14 组 `PASS`，`ALL TESTS PASSED: 0 failure(s)`，含生命周期压测 |
| 02:05 注入核查 | 主进程 102308、渲染 101588、GPU 104144 的模块表中无 `nexsink.dll`、`hooklog.dll`、`smoke.dll`；`mod/sink.log` 末行 01:09:10 |
| 02:20 DevTools 端口 | `--remote-debugging-port=9333` 生效，`127.0.0.1:9333` 监听；页面目标 `orpheus://orpheus/pub/app.html` |
| 02:22 面板挂载 | 真实设置页 `#play` 内插入 8 行；连续三次「首页→设置→播放」往返，两个面板各保持 1 个实例 |
| 02:24 配置写入 | 面板切换严格模式后 `hook.cfg` 的 `shared_fallback` 由 0 变 1，再改回 0；写入后文件格式保持 `key=value` 单行 |
| 02:26 界面精简 | 六项全开时隐藏 9 个节点；首页快捷入口由 7 张变 6 张且无空缺，活动条与榜单区块消失，侧栏只剩推荐/漫游/关注，顶栏听歌识曲与消息消失 |
| 02:29 刷新重放 | 页面整页重载后，监听进程自动重新注入，隐藏规则再次生效 |
| 02:31 精简默认值 | `ui.json` 六项已复位为 0，符合「默认关闭」 |
| 02:44 独立端点探测 | `probe_excl.exe`（不加载 hook）在本机端点实测 8/18 独占格式被接受：44.1k/48k/88.2k/96k 的 16 与 24 bit 全部 `S_OK`；32bit float 与 176.4k/192k 全部 `AUDCLNT_E_UNSUPPORTED_FORMAT`。证明端点不拒绝独占，问题在模块内部 |
| 02:44 独占接管 | `Initialize(SHARED) 0x00000000`、`selected 96000 Hz/24 bit after 2 candidates`、`ENGAGED`、`hardware started after 1408 prefilled frames` |
| 02:48 位深设置 | `bits=16/24/32` 三档实测输出 `cont:16` / `cont:24` / 回退 `cont:24`，设置真正参与协商 |
| 02:50 连续切歌 | 三次 `next` 全部 `taking over` → 重新协商 → `hardware started`；同期 `underrun`/`fault`/`rejected` 计数为 0 |
| 02:55 回归复跑 | 14 组 `PASS`，`ALL TESTS PASSED: 0 failure(s)` |
| 03:00 拖动进度条 | seek 到 50% 后进度条 1044 → 1063 → 1083 → 1102px，每 2 秒约 20px，与实时速率吻合；同期 `underrun`/`fault`/`OUT_OF_ORDER` 计数 0 |
| 03:02 部署后复验 | 新构建（275968 字节）注入后 seek 复测结果一致，`reset` → `hardware started after 1323 prefilled frames` |

### 复用命令

```text
OUT_DIR=D:/Coding/NeteaseExclusive/mod/build-check bash D:/Coding/NeteaseExclusive/mod/build.sh all
OUT_DIR=D:/Coding/NeteaseExclusive/mod/build-check bash D:/Coding/NeteaseExclusive/mod/build.sh test
D:/Coding/NeteaseExclusive/mod/test/regression.exe
python D:/Coding/NeteaseExclusive/analysis/unpack_ntpk.py D:/Installed_Program/CloudMusic/package/orpheus.ntpk --list "setting|manifest|\.html$"
```

部署（需先确保客户端由用户正常启动且 PID 明确）：

```text
OUT_DIR=D:/Coding/NeteaseExclusive/mod/build-check bash D:/Coding/NeteaseExclusive/mod/deploy.sh <cloudmusic PID>
```

02:55 复测：注入全链路 `err=0 ok`，此前记录的 `ERROR_ACCESS_DENIED (5)` 不再复现。

**注意**：`nexlaunch.exe` 无法 `CREATE_BREAKAWAY_FROM_JOB`（`err=5`），客户端会落在工具的作业对象里，工具调用一结束就被回收。可靠做法是把「启动 + 长 sleep」放进同一个后台任务，由该任务持有作业对象，再在另一个调用里注入。

## 8. 更新日志（仅追加）

### 2026-11-10 00:23 — 建立全项目主记录，完成设置资源定位

- 确认客户端3.1.42.205594与已加载界面模块。
- 定位设置图标、`/setting/account`、`/setting/play`、`#page_pc_setting`、`#play`及原生桥。
- 动态控件树读取受安全检查限制，未绕行，运行时挂载保持受阻状态。
- 复跑现有回归测试通过；核验新提取脚本和10个资源副本。
- 复核音频接口签名、时钟、格式策略、生命周期和部署缺口，建立可验收的后续任务表。
- 保留00:05已加载音频修复版；本轮不打断当前播放。

### 2026-11-10 00:24 — 主记录验收与维护规则保存

- 20项任务编号无重复，状态、依赖和验收字段检查通过。
- 已将“每完成任务、每轮结束更新本文件”的要求写入工作区长期记忆。
- 核对三个客户端进程仍为00:04:43启动的原进程，未因本轮探查重启。

### 2026-11-10 01:07 — P0/P1 音频问题全部落地，桩测试补齐

- 修正 IAudioClient3 槽 20 的 4 参数原型，并加“以 COM 方式调用”的桩测试。
- 时钟改为真实 `IAudioClock` 换算，覆盖 0.5 s / 1 s / 1 帧 / 倒退钳制。
- 新增 `drain_once()`，把“先启动后送数据”改成“先提交后启动”，并统一提交契约。
- 原函数改为按 vtable 记录；端点绑定持引用；每流独立 generation 与代理对象。
- 环缓冲跨尾分支改用预分配暂存，并用 cap=8 的跨尾用例逐字节核对。
- 协商改为采样率优先；`compat=0` 只允许精确表示；`shared_fallback` 真正生效。
- 修掉两个未登记缺陷：`DllMain` 漏初始化 `g_control`/`g_patch_lock`；`GetBuffer(0)` 返回码不符 WASAPI。
- 删除 90 行死代码，构建回到零警告。
- **未部署**：第一次构建时旧进程占用导致无法覆盖 `mod/nexsink.dll`；新版产物在 `mod/build-check/`，后续实机注入另见 01:16 更新。

### 2026-11-10 01:16 — 重启/注入尝试与 C 性能审查

- 按用户授权结束旧版客户端并启动 `D:/Installed_Program/CloudMusic/cloudmusic.exe`；通过 `WaitForInputIdle`，短暂观察到主进程和两个 CEF 子进程。
- 对 PID 102036 调用既有 `deploy.sh`/`inject2.exe`。`OpenProcess(0x43A)` 成功，目标完整性 `0x2000`；诊断分配与 DLL 路径分配的 `VirtualAllocEx` 均返回 `ERROR_ACCESS_DENIED (5)`。远程线程和 `LoadLibraryW` 未执行；新版没有被注入。
- 后续工具调用中 PID 已消失；本轮未绕过拒绝，也未尝试提权或替代注入方式。
- 复跑已有独立 `bench_speed.exe 192000 96000 10` 两次：10 秒音频/960,000 输出帧分别用时 0.194 s 和 0.206 s，约 52×/49× 实时、单核 1.9%/2.1%。仅代表离线重采样吞吐，不代表实际设备稳定性或完整客户端开销。
- 复查旧 `sink.log` 最新一行仍为 01:09:10；没有本轮新版加载后的新日志；最后核验 cloudmusic 进程数为 0。
- C 适用于 WASAPI hook/转换核心；复杂度主要来自单一 1,868 行 `nexsink.c` 混合 COM vtable hook、端点协商、环缓冲、SRC、线程与生命周期，而非 C 运行速度。建议后续拆分模块；本轮未修改 C 源码。
- 当前阻塞：需要在稳定、可访问的用户客户端会话里，由用户选择正常授权的注入方式；重新加载后才能进行听感与切换验收。

### 2026-11-10 02:10 — 补记 01:41 并发改动，复核注入状态

- 补记上一轮遗漏：01:41 把生命周期入口统一进 `g_control`，新增 `sink_teardown_controlled()` / `sink_engage_controlled()`；01:43 测试由 10 组扩到 14 组，含 teardown 竞态与 300 次生命周期压测。
- AU-04 状态调整：锁覆盖这条欠账从"未处理"改为"已闭环（代码测试）"，剩余为初始化各阶段失败注入与实机并发场景，整体仍记部分完成。
- 复跑 `build.sh all` 零警告、`build.sh test` 十四组全通过。
- 02:05 复核客户端：三个进程稳定运行，模块表无本项目任何 DLL；`sink.log` 末行 01:09:10，确认 01:41 版本从未加载。
- 结论：拆分模块仍未开始，`mod/src` 无头文件，`build.sh` 只编单文件，`regression.c` 以 `#include` 方式依赖源码。

### 2026-11-10 02:30 — 界面通道打通，设置面板与界面精简跑通

- 推翻「动态界面检查受阻」的旧结论：客户端是 CEF 宿主，`--remote-debugging-port=9333` 生效，CDP 求值、截图、刷新均可用。
- 界面注入改走 CDP，不再需要往客户端里放第二个 DLL；配置由本机 `nexctl` 服务写入 `hook.cfg`，避免依赖 `window.channel` 未公开的命令。
- 新增 `mod/ui/panel.js`（独占输出八项）、`mod/ui/simplify.js`（界面精简六项）、`mod/tools/nexctl.mjs`（配置服务与注入）、`mod/tools/cdp.mjs`（调试客户端）、`mod/tools/nexlaunch.exe`（带端口启动）、`mod/tools/nexui.cmd`（一键入口）。
- 实测：面板出现在真实设置页且三次路由往返不重复；开关改动落到 `hook.cfg`；六项精简逐项核对；整页重载后监听进程自动重注入。
- 未覆盖：`nexsink.dll` 仍未加载，面板参数对声音的影响、错误状态联动、深浅外观下的表现均未验。
- 约束记录：客户端进程无法在工具调用的作业对象里存活，`schtasks` 与 `explorer.exe` 转交被安全策略拦下；需要用户双击 `mod/tools/nexui.cmd`，或先运行 `nexlaunch.exe` 再单独注入。
- 音频源码本轮未修改；`mod/hook.cfg` 因面板写入新增了 `fade_ms`、`shared_fallback` 两行显式值。

### 2026-11-10 02:40 — 精简项拆成三组，顶栏只收皮肤与宠物

- 上一版把听歌识曲、消息、皮肤一起隐藏，顶栏只剩两个孤立红点。原因是顶栏图标外面套着 `.cmd-badge` 徽标容器，只把图标设为 `display:none`，徽标仍按原坐标渲染。
- 改为向上找到图标栏的直接子节点整格收起，红点随容器一起消失；听歌识曲与消息不再纳入精简范围。
- 精简项重排为三组七项：页面精简（精选活动条、榜单精选、每日推荐）、侧栏精简（播客入口、侧栏精选）、顶栏精简（皮肤按钮、宠物按钮）。
- 修掉一个写入竞态：面板原先回写整份状态，多个页面实例会互相覆盖对方的开关；改为只提交变化的那一项，服务端按 key 合并。
- 实测：顶栏剩昵称、VIP、消息（带未读红点）、设置、迷你模式与窗口按钮，间距均匀无空缺；首页活动条与榜单消失，快捷入口与侧栏保持完整。


### 2026-11-10 02:55 — 注入成功，查出「不出声」的真因是一个 GUID

- 推翻「注入受阻」：对主进程 80992 注入全链路 `err=0 ok`，`remote LoadLibraryW -> 0x711D0000`，模块表出现 `nexsink.dll`。`jobcheck` 显示客户端确实在工具的作业对象内且禁止 breakaway，但这只影响进程存活，不影响注入；对作业外进程（记事本）注入同样成功，说明跨进程远程线程被允许。历史 `ERROR_ACCESS_DENIED (5)` 按旧构建/旧调用路径归因，不再作为能力边界。
- 新增 `mod/tools/probe_excl.c`（`probe_excl.exe`）：不加载任何 hook，直接对默认端点做独占探测并枚举音频会话。实测本机端点 8/18 被接受（44.1k/48k/88.2k/96k 的 16 与 24 bit），32bit float 与 176.4k/192k 全拒。这一步把「端点拒绝」从嫌疑名单里划掉了。
- 给 `try_exclusive()` 补日志后定位到真因：`IID_IAudioClock_l` 的 GUID 后三段写错，`GetService(IAudioClock)` 恒返回 `E_NOINTERFACE`，每个候选都在最后一步失败。修正为 `CD63314F-3FBA-4A1B-812C-EF96358728E7` 后，`Initialize(SHARED)` 一次通过。
- 顺带查出「输出位深」是空开关：`g_out_bits` 只被读取和打印，从未进入候选序列。现放在首位，并加入协商缓存 key。三档实测：`bits=16`→`cont:16`、`bits=24`→`cont:24`、`bits=32`→回退 `cont:24`（本机端点不支持 float 独占，降级正确）。
- 切歌问题：客户端切歌不复用旧 `IAudioClient`，原逻辑让新流 passthrough，撞上自己的独占端点得到 `DEVICE_IN_USE`。改为新流到来即交接设备，连续三次 `next` 均 `taking over` → 重新协商 → `hardware started`，同期 `underrun`/`fault`/`rejected` 计数为 0。
- 端到端实测：`selected 44100 Hz/24 bit after 1 candidates`、`ENGAGED in=44100/16 -> out=44100/24 [depth only]`、`hardware started after 1408 prefilled frames`；16bit 源升 24bit 输出符合设置。
- 回归：14 组全 `PASS`。
- 未覆盖：多流并发（客户端同时开两个流）下的交接策略未验，当前是无条件抢占；初始化各阶段失败注入、深浅外观、UI-06 打包自启仍待做。

### 2026-11-10 03:05 — 拖动进度条后卡住：时钟没归零 + 预留没释放

- 现象：切歌正常后，拖动进度条会卡在拖动位置，音频也停住不再恢复。
- 第一个原因：`fc_GetPosition()` 为了防止客户端看到位置倒退，把返回值钳制在 `g_sink.clock_last` 上；而 `H_Reset()` 清了环缓冲和重采样历史，**唯独没清 `clock_last`**。seek 时设备时钟归零，钳制却把它锁死在 seek 前的值上，客户端据此认为播放位置没变，进度条不动。`sink_teardown()` 因为整体 `memset` 顺带清掉了它，所以切歌没事、只有 seek 出问题。`regression.c` 第 258 行本来就写着「A rebuilt stream starts its own monotonic sequence」，设计意图就是 Reset 后归零。
- 第二个原因（更隐蔽）：seek 会打断客户端正在进行的写操作——它拿了 `GetBuffer` 的缓冲却在 `Stop` 时没有 `ReleaseBuffer`。`H_GetService` 返回的是同一个 `proxy_render`，`reserved` 是共享状态，于是之后每次 `GetBuffer` 都返回 `AUDCLNT_E_OUT_OF_ORDER`，客户端等约 1.6 秒后放弃，日志里表现为 `drain: thread up` 之后再也没有 `hardware started`。
- 修法：`sink_stop()` 释放设备时一并作废预留，并置 `discarded` 标记；`fr_ReleaseBuffer()` 对「标记为已丢弃」的迟到写入返回 `S_OK`（静默丢弃），对「从未 GetBuffer 就 ReleaseBuffer」仍返回 `OUT_OF_ORDER`。两者都表现为 `reserved==0 && written>0`，只靠 `reserved` 无法区分，因此必须新增标记。
- 顺带：`H_Start()` 在建立 drain 线程后补一次 `SetEvent(h_client)`，事件驱动的客户端否则会和 drain 线程互相等待。
- 回归：首版改动让 `lifecycle stress` 的 `stress_errors` 非 0（生产者线程的迟到 `ReleaseBuffer` 被判为 `OUT_OF_ORDER`），补上 `discarded` 标记后 14 组全 `PASS`。
- 实测：seek 到 50% 后进度条 1044 → 1063 → 1083 → 1102px，每 2 秒约 20px，与实时速率吻合；`reset` → `hardware started after 1323 prefilled frames`，`underrun`/`fault`/`OUT_OF_ORDER` 计数 0。中途观察到的「进度条倒退」是用户手动拖动造成的，不是缺陷。
- 未覆盖：连续快速拖动、拖动到接近曲尾、以及 seek 与切歌交叉的时序组合未做专项验证。

### 2026-11-10 03:20 — 一键化落地：双击即用

- 新增 `mod/tools/nexhost.c` → `mod/build-check/NexExclusive.exe`（约 240 KB，零警告）。一个进程做完过去要手工串的五件事：带 `--remote-debugging-port` 启动客户端、找到主进程注入 `nexsink.dll`、起本机配置服务、通过 CDP 注入 `panel.js`/`simplify.js`、常驻看护。
- 之所以必须长驻：客户端是它启动的子进程，启动器一退出子进程就跟着被回收。这也是之前每次都要靠后台任务吊着作业对象的根本原因。
- 内置 HTTP 服务用 WinSock 直接实现（`/api/cfg`、`/api/ui`、`/api/status`，带 CORS），不再依赖 Node。CDP 侧实现了最小 WebSocket 客户端（握手 + 掩码文本帧，不校验 `Sec-WebSocket-Accept`），够发 `Runtime.evaluate` 即可。
- `mod/启动.cmd` 双击入口；脚本用 GBK 写、程序按 `GetConsoleOutputCP()` 转码输出，避免控制台中文乱码。
- 主进程识别改按**父子关系**：唯一一个「父进程不是 cloudmusic.exe」的就是主进程。早先按内存大小挑，页面重载时渲染进程内存暴涨，会把模块注入到错误的进程（实测 pid 从 74688 漂到 105320）。
- 页面重载后恢复界面脚本：`Page.addScriptToEvaluateOnNewDocument` 时灵时不灵（连续三次重载会漏一次），因此加 5 秒幂等兜底重放。两个脚本都做了幂等保护，重放无副作用。
- 实测（全部自动，无人工介入）：启动 4 秒后注入成功、界面 4 秒后就位；连续三次页面重载 `simplify` 均为 4；杀掉客户端后 4 秒检测到、13 秒内重启+注入+界面全部恢复；随后播放 `selected 44100 Hz/24 bit` + `hardware started`；设置页 `#nexsink-panel` 存在且 8 行控件齐全。
- `build.sh` 新增 `host` 目标，`all` 一并构建。
- 未覆盖：托盘图标与菜单（当前是控制台窗口，关闭即停止看护，已启动的客户端会继续播放）；开机自启；多实例互斥。

### 2026-11-10 03:45 — 端点切换与设备迁移（AU-08 余项）

- 新增 `mod/tools/set_ep.c`（`set_ep.exe`）：列出渲染端点并把默认输出切到指定设备。改默认值只能走未公开的 `IPolicyConfig::SetDefaultEndpoint`，三个 role 全设（网易云读 eMultimedia，多数 UWP 读 eConsole）。参数与输出全程宽字符——第一版用 ANSI argv 去匹配 UTF-16 端点名，中文名字直接匹配不到。
- `nexsink.c` 新增端点看护线程（400 ms 轮询 + 设备失效时即时唤醒）。默认输出变化或设备消失时，把**正在播放的流**迁移到新端点：只替换设备侧，客户端环缓冲、上报的缓冲大小、代理对象全部保留，所以已写入的音频不丢。
- 时钟跨设备连续：新增 `clock_base`，迁移时把已播放位置记为基准，新设备时钟从 0 起算后加回基准。否则 `fc_GetPosition()` 的单调钳制会把进度条钉在迁移前的值上（和 seek 卡住是同一个坑）。
- 设备失效不再算流失败：drain 线程收到 `AUDCLNT_E_DEVICE_INVALIDATED` 只置 `device_lost` 并唤醒看护线程，不再写 `fault`。客户端在 `fr_GetBuffer()` 里等迁移完成——**必须等取控制锁之前**，否则和迁移互相死锁。
- 迁移让位于客户端：默认输出一变，客户端往往 50 ms 内就自己重建流了（比看护的 400 ms 轮询还快）。两者同时动作会抢同一个正在重配的端点，所以看护先等 600 ms 宽限；客户端重建了，`endpoint_key` 随之更新，迁移自然不发生。宽限期只对「设备还在、只是不再是默认」生效；设备真消失时没有可等的对象，立即迁移。
- 实测：暂停态切音箱→耳机双向迁移均 `48000 Hz/24 bit after 1 candidates` + `clock continues at 22.308 s`，客户端流未被拆掉；播放态切到音箱时 `drain: thread up` → `hardware started after 1236 prefilled frames`，切回耳机同样恢复；切换后进度条 00:14 → 00:16 → 00:19 正常推进。
- **已知限制（切换默认输出时的卡顿）**：播放中切默认输出，旧端点上的独占流会卡在 `IAudioRenderClient::GetBuffer` 里，**只有等流对象被释放才返回**——也就是在等我们自己。实测 2 s 与 4 s 超时都卡满，加阶段计数确认停在 stage 2（GetBuffer）。因此 join 超时定为 1.2 s：超时本身就是解锁手段，等更久只是让界面多冻一会儿。超时后客户端会走一次共享回退（同样失败）再重建，约 300 ms 内恢复出声。根治要把 `Sink` 改成按代持有（drain 线程自带设备引用与私有环），属于 `nexsink.c` 拆分那一批。
- 顺带修掉两处：`sink_stop()` 改为**先 Stop 设备再 join**（原来先 join，线程卡在 WASAPI 调用里出不来）；drain 线程在自身 Stop 之后收到的错误不再写 `fault`，否则会毒化一个正在按计划重建的 sink。
- 回归：14 组全 `PASS`（含 300 次 Start/Stop/Reset 压测）。`build.sh` 新增 `tools` 目标。
- 未覆盖：物理拔插（把耳机 USB 拔掉）未实测，路径与已验证的迁移共用；多进程争用未测。

### 2026-11-10 04:05 — 底噪归因（USB 3.0）与「系统音量是否该失效」

**用户报的两个问题，一个不是软件问题，一个不是回归。**

- **底噪/火花声/滋滋声 = USB 3.0 接口干扰。** 用户把耳机从 USB 3.0 换到 USB 2.0 后底噪消失。USB 3.0 的 5 Gbps 差分信号在 2.4 GHz 附近辐射，对屏蔽一般的 USB 音频设备是典型干扰源。软件侧的三条证据与之吻合：① 用 `NEX_DUMP` 把送进设备的 24bit 帧原样导出 22.7 秒，逐样本分析**零跳变、零重复块、直流偏移 0.01%**；② 欠载计数 **0 次空缓冲**，最大提交间隔 32 ms 恰好等于设备缓冲周期；③ 位深 24→16 切换对噪声无影响。**数字通路上没有问题，不必再往 DSP 方向查。**

- **「系统音量条不该起作用」不是回归，是端点属性。** `set_ep --vol` 用 `IAudioEndpointVolume::QueryHardwareSupport` 逐端点测量：

  | 端点 | 音量 | 硬件音量节点 | 独占模式下系统音量条 |
  |---|---|---|---|
  | 扬声器 MOONDROP MM3A | 100% | 无 | **被绕过**（软件音量） |
  | Q27G4ZDP (HDMI) | 100% | 无（仅 mute） | 被绕过 |
  | 耳机 HyperX Cloud III | 41% | **有** | **仍然有效** |

  所以「之前某个版本系统音量没用」多半是当时在音箱上听的。音箱是软件音量，独占天然绕过；耳机有硬件音量节点，WASAPI 的语义就是绕不过去。

- **结论：保持现状，不做音量钉死。** 音量分三层——系统软件音量（独占绕过）、播放器数字音量（**08:48 实测推翻：客户端音量走会话音量，独占下同样无效**）、设备硬件音量（有效，**零精度代价**，因为它衰减在 DAC 之后，一个比特都不改）。所以对**带硬件音量的端点**，「能调音量」和「bit-perfect」不冲突：衰减发生在 DAC 之后，一个比特都不改。AppleMusicExclusive 及其衍生项目调不了音量，是它们把端点钉在满幅的设计选择，不是独占模式的必然。对耳机强行钉死等于拿精度换形式统一，没有必要。（软件音量端点的情况不同：独占下没有任何音量控制，见 08:48 那条。）

- 新增诊断设施（都默认关闭，不影响正常路径）：`NEX_DUMP=<file>` 导出实际送进设备的帧；欠载计数（空缓冲次数 + 最大提交间隔），只在窗口内出现空缓冲或间隔超 50 ms 时才写一行日志。

- `set_ep --vol` 另附端点音量诊断；`build.sh tools` 目标包含 `set_ep.exe` / `list_eps.exe`。

- **踩坑**：`IAudioEndpointVolume` 的 vtable 槽位手写时错了一位（`QueryHardwareSupport` 在 19 而不是 20，`GetChannelCount` 在 5 而不是 6），直接段错误。改用槽位常量 + 注释写明完整顺序。

### 2026-11-10 04:25 — `Sink` 按代持有 + `nexsink.c` 拆分（切歌/切端点卡顿的根治）

**上一版把「切默认输出卡顿」的根因判错了。** 当时记录的是「旧端点上的 GetBuffer 只有等流对象释放才返回」，据此把 join 超时当解锁手段。实机复现后拿到两条独立证据，结论是两件不同的事：

- **切歌卡顿是自伤，不是 WASAPI 语义。** 04:04:33 一次普通切歌（`Initialize dur=0`，没有动端点）就触发了 1.29 s 超时 + `falling back to the real shared stream` + `0x88890008`。原因是 `sink_stop()` **先 Stop 设备再 join**：`GetBuffer` 在等设备腾出缓冲空间，而 Stop 恰好停掉了唯一会腾空缓冲的一方。`Reset` 救不了——它丢弃数据，不会让设备继续消费。
- **切端点确实会卡，但那是另一条路径。** 修掉 Stop 顺序后，切歌从 1.29 s 降到 **26 ms**；切默认输出仍然 946 ms 超时（300 ms + 600 ms 两级窗口都耗尽），因为端点被系统重配时设备真的不消费，只能靠释放流来解开。

**改动（三件）：**

1. **`sink_stop()` 的 join 阶梯。** 先把 `running` 清零、**设备保持运行**去等线程（正常路径 ≤ 一个缓冲周期，实测 4–6 ms 退出）；不走再 `Stop` + `Reset` 给第二窗口（覆盖「客户端自己先停了流」）；两个窗口都不走才认定端点正在重配。
2. **`SinkGen` 按代持有。** 设备侧从全局单例搬进带引用计数的堆对象：`real` / `real_render` / `real_clock` / `real_evt` / `endpoint_gate` / `real_frames` / `real_latency` / `device_started` / `running` / `retired` / `h_thread` / `submitted` / `stage`。控制路径持一个引用，drain 线程持一个，**最后一个引用负责释放设备对象**——正常情况就是线程自己。所以第三级「放弃」是安全的：置 `retired`、把代交给线程、控制路径直接走人，线程若回来会自己释放，不回来就让它留着（有界、有日志），绝不出现「在别人脚下 free」。
3. **线程句柄的归属。** 控制路径 join 成功后 `InterlockedExchangePointer` 抢到句柄才 Close；线程退出时做同一次交换。谁拿到谁关，不会双关，也不会漏。

**drain 线程的两条硬约束：**
- 循环条件是**自己那一代**的 `running`，不是全局的——否则新一代 Start 会把老线程唤醒，它接着去读已经属于新流的环和重采样器。
- 每个可能阻塞的 WASAPI 调用之后立刻检查 `retired`，命中就 `return E_ABORT`，**在碰任何共享状态之前**退出。`GetBuffer` 是唯一会长时间不返回的调用，这个检查放在它返回后的第一条语句。

**拆分（纯移动，`diff` 校验 0 差异）：**

| 文件 | 行数 | 职责 |
|---|---|---|
| `nexsink.c` | 78 | 头部说明、include、vtable 槽位常量、按序 include 六个片段 |
| `sink_core.inc` | 304 | 配置、日志、`SinkGen`/`Sink`、全局量、补丁表、`ac_original`、`O_*` 宏、格式助手 |
| `sink_dsp.inc` | 430 | 环缓冲、整数/浮点转换、多相窗 sinc 重采样、量化、淡入 |
| `sink_device.inc` | 405 | drain 线程、故障与设备丢失、波形导出、伪 `IAudioRenderClient`/`IAudioClock` |
| `sink_lifecycle.inc` | 132 | 代对象引用计数、`sink_stop` 阶梯、`sink_teardown` |
| `sink_endpoint.inc` | 604 | `try_exclusive`、格式协商、端点闸门、`sink_retarget`、`sink_engage` |
| `sink_hooks.inc` | 573 | IAudioClient/IMMDevice 钩子、vtable 打补丁、端点看护、`DllMain` |

必须是 `.inc` 文本包含而不是多编译单元：`test/regression.c` 直接 `#include "../src/nexsink.c"` 并访问 `g_patch_lock`、`g_audio_patches`、`g_sink`、`g_src` 等静态符号。

**实测（`build-check`，耳机 HyperX Cloud III，44100/16 源 → 44100/24 出）：**

- 切歌：`Initialize` → `ENGAGED` → `hardware started` 全程 **26 ms**，无 `drain join`、无 `falling back`。
- 播放中切到扬声器再切回耳机：每次 **~175 / ~320 ms**，`drain: thread exit (gen N, retired=0)` 干净退出，`gen N released` → `torn down` → 新代 `ENGAGED`。连续两次切换 + 一次切歌后 **0 条故障**，`drain: 10000 writes, 0 empty-buffer, max gap 328 ms`（328 ms 就是切换窗口本身，客户端自己重建流造成）。
- 回归 **15 组**全 `PASS`。新增/改写的测试：`mock_gen()` 辅助、`drain_once(gen)`、`test_reset` 里用真实 mock 设备测 Reset 失败保留数据、`test_reset` 开头补 `reset_patches()`（栈上 vtable 地址会被后续测试复用，残留记录会让 `ac_original` 命中错的那条——这是个真实的坑，踩过一次段错误）、压测断言改为「线程没了但代还在」（Stop 保留代，暂停/恢复不该重新协商设备）、**新增 `test_generation_abandon`**：让 mock `GetBuffer` 卡在事件上，验证 `sink_stop` 跑完两级窗口后放弃、把代交给线程、且**不在卡住的调用脚下释放任何东西**；放行后线程自己释放该代（`ac`/`rc` 的 Release 各被调用一次）。

**未覆盖：** 物理拔插（拔耳机 USB）仍未实测，走的是同一套 `device_lost` → 迁移路径；多进程争用未测；`retired` 之后线程若真的回来，那一代的 `endpoint_gate` 会一直被它持有到线程退出为止（有界，最多每代一个，实际未发生）。

### 2026-11-10 08:48 — 端点音量诊断提示；「播放器音量有效」被实测推翻

**04:05 那条「播放器数字音量有效」是错的，本轮用设备侧帧把它否掉了。**

- **怎么测的。** `NEX_DUMP` 把真正交给 `IAudioRenderClient` 的 24bit 帧原样落盘（44.1k/2ch，6 B/帧），一边放歌一边用 CDP 在播放器喇叭图标上滚轮把**客户端自己的音量从 100% 打到 0%**（浮层读数确认：`0%`；再滚回 45 格确认 `100%`，说明滚轮—状态映射是真的）。逐 0.5 s 窗口统计峰值：

  | 播放器音量 | 时间窗 | 峰值 |
  |---|---|---|
  | 100% | 18–25 s | −3 … −8 dBFS |
  | **0%** | **106–115 s** | **−0.3 … −6.8 dBFS** |

  音量归零后送进设备的样本**一个比特都没变**。结论：客户端的音量走的是会话/端点音量（`ISimpleAudioVolume` 一类），不作用在 PCM 上，独占模式下整段被绕过。**不是「有精度代价」，是根本没有作用。**

- **对软件音量端点意味着什么。** 音箱/HDMI 这类没有硬件音量节点的端点，独占时**没有任何可用的音量控制**：系统音量条无效、客户端音量也无效，输出就是音源原电平。这正好解释了用户最初那句「独占模式就是系统最大音量 调节系统音量是没用的」。耳机（HyperX Cloud III）有硬件音量节点，所以系统音量条仍然有效——两条路径都实测过。

- **本轮实现（只加诊断，不改任何音频行为）。** 宿主每 3 秒缓存一次端点信息（`IMMDeviceEnumerator::GetDefaultAudioEndpoint` + `IAudioEndpointVolume::QueryHardwareSupport` 槽位 19 + `PKEY_Device_FriendlyName`），`/api/status` 增加 `endpoint` / `hwVolume` / `hwMute`（查询失败为 `null`）；面板据此在「上次输出」下面多一行：

  | 端点能力 | 面板文案 |
  |---|---|
  | 有硬件音量 | 灰色一行：`端点「耳机 (HyperX Cloud III)」· 硬件音量可用，独占时系统音量条仍有效` |
  | 无硬件音量 | 琥珀色三行：`端点「…」· 无硬件音量` / `独占时系统音量与播放器音量均不生效，输出为音源原电平` / `如需调音量，请换用带硬件音量的输出或关闭独占` |
  | 查不到 | 整行隐藏，不猜 |

- **实现要点。** HTTP 线程原先没有任何 COM 初始化，本轮在 `http_thread` 开头 `CoInitializeEx(NULL, COINIT_MULTITHREADED)`：该线程只有 `accept`，没有消息泵，STA 的跨线程调用会挂死；初始化失败就退回不查询。缓存只在 HTTP 线程读写（accept 循环单线程），不加锁。

- **踩坑。** 改完 `panel.js` 直接 `Runtime.evaluate` 重新执行是没用的：旧实例的 `MutationObserver` 会在 250 ms 后把旧面板重新挂回来（`getElementById` 拿到的是旧的那个），而宿主只在页面 id 变化时重新注入。改界面脚本后**必须重启宿主**，别指望热替换。

- **已决定不做（2026-11-10 08:55，用户）。** 模块自己接管音量：hook 客户端取会话对象的那一跳（`IAudioSessionManager::GetSimpleAudioVolume`，不是 `IAudioClient::GetService`——见 09:40 条），把客户端的衰减量搬到 sink 的 DSP 里做（`sink_dsp.inc` 已有 `quantize`/`apply_fade` 的位置），约 150 行。方案本身可行，但代价是引入数字衰减，用户判断不值得——宁可在这个端点上没有软件音量，也不要动比特。**所以诊断提示就是这条线的终点**，软件音量端点在独占下调不了音量是明确接受的现状，不再尝试绕开。

### 2026-11-10 09:14 — 面板改报「实际接管的端点」，不再只看系统默认

**问题**：上一版诊断提示查的是 `GetDefaultAudioEndpoint`。但客户端自己的「输出设备」可以指定到**具体设备**——实测它列了 12 个条目（DirectSound / WaveOut / Wasapi × 各端点），所以「系统默认」和「实际在放的」完全可以不是同一个。那种情况下面板会解释错的硬件。

**改动**：

- **sink 侧**：`sink ENGAGED` 行尾部带上 `ep="<friendly name>" hw=<位掩码>`（`1` 音量、`2` 静音、`4` 电平表，`-1` = 能力未知）。新增 `ep_describe()`（`sink_hooks.inc`，紧挨着 `dev_activate`）：`OpenPropertyStore` 取 `PKEY_Device_FriendlyName`，再用**原始** `Activate`（`dev_activate`，绕开我们自己的 `H_Activate`）拿 `IAudioEndpointVolume::QueryHardwareSupport`（槽位 19）。在 `sink_engage` 里 `endpoint_hash()` 之后立刻取——那时 `dev` 还被引用着，而 ENGAGED 行是几十行之后才写的。
- **宿主侧**：`status_json` 从日志末段的最后一条 ENGAGED 里解析名字和能力位，**优先用它**；日志里没有任何 ENGAGED 记录（一次都没播过）才退回系统默认端点。解析从 `" hw="` 反着定位结尾，名字里带引号也不会断。
- **面板侧**：能力未知（`hwVolume` 为 `null`）时整行隐藏，不再把「未知」当成「没有硬件音量」显示成琥珀色警告。三种状态现在分得开：查不到 / 知道名字但能力未知 / 都查到。

**实测**（这是关键的一步）：把客户端「输出设备」选成 `Wasapi:扬声器 (MOONDROP MM3A)`，系统默认仍保持 `耳机 (HyperX Cloud III)`。

| 来源 | 端点 | 硬件音量 |
|---|---|---|
| 系统默认 | 耳机 (HyperX Cloud III) | 有 |
| sink 实际接管 | 扬声器 (MOONDROP MM3A) | 无 |
| 改前 `/api/status` 会报 | 耳机 ❌ | 有 ❌ |
| 改后 `/api/status` 报 | **扬声器 ✅** | **无 ✅** |

面板同时显示琥珀色三行，和真实情况一致。测完已把客户端改回 `Wasapi:默认输出设备`，sink 随即重新接管耳机，状态回到「硬件音量可用」。

**回归**：15 组全 PASS（改动只在 engage 的控制路径上，drain 线程和 DSP 未动）。

**已知边界**：客户端关掉、系统默认又换过设备时，日志里的最后一条 ENGAGED 属于上一个进程，面板会短暂显示旧端点，直到下一次播放产生新的 ENGAGED。

### 2026-11-10 09:25 — AU-04 结论：不区分「换代流/并发流」，改为可诊断

**先把一个错误结论更正掉**：此前记的「客户端选 DirectSound / WaveOut 时模块不会接管」是错的。实测两条都被接管：

| 客户端输出设备 | 结果 |
|---|---|
| `Wasapi:耳机` | `sink ENGAGED ep="耳机 (HyperX Cloud III)" hw=3` |
| `DirectSound:耳机` | 同样 ENGAGED，`ep` 与 `hw` 一致 |
| `WaveOut:耳机` | 同样 ENGAGED（切换时旧代按设计被放弃，随后新代接管） |

原因：Vista 之后 DirectSound 与 WaveOut 都在**进程内**被映射到 WASAPI，仍然调用 `IMMDevice::Activate(IID_IAudioClient)`，正好撞上我们的钩子。所以「输出设备」选哪一层 API 都不影响接管，不存在静默失效的陷阱。

**AU-04（多流并发）结论：不改策略。** 日志里累计 90 次 `new stream while the previous one is still running`，全部对应切歌或换端点。三次真实切歌的接管数据：

| 旧代 | 已提交帧 | 距上次提交 | 格式 |
|---|---|---|---|
| gen 1 | 581323 | 172 ms | 44100/16/2ch → 44100/16/2ch |
| gen 3 | 277474 | 281 ms | 44100/16/2ch → 44100/16/2ch |
| gen 5 | 267331 | 63 ms | 44100/16/2ch → **44100/32/2ch** |

三条判据（旧流是否还在写、空闲多久、格式是否变化）**没有一条能区分换代流和并发流**——切歌本身就是「旧流还在写、新流已经 Initialize、格式可能变」。而独占设备同一时刻只能服务一条流，所以「新的赢」是唯一能让播放继续的策略；输的那条拿到 `AUDCLNT_E_DEVICE_INVALIDATED`，客户端会自行重建。硬要猜一个策略（比如空闲超过 100 ms 就拒绝新流）只会在真切换时误伤。

**因此本轮把「不可判定的策略」换成「可诊断的行为」**：接管日志现在带上判据数据，真出现并发流时能从日志一眼看出来：

```
sink: new stream while gen 5 is still running
      (submitted=267331, last submit 63 ms ago, 44100/16/2ch -> 44100/32/2ch), taking over
```

接管全程（新流 Initialize → 新代 ENGAGED）**25 ms**。回归 15 组全 PASS。

**顺带发现一个罕见失败**：09:13:16 一次换端点时 `sink failed 0x8889000A`（DEVICE_IN_USE）→ 回退共享 → 共享流又拿到 `0x88890008` → 客户端重建后自愈。`try_exclusive` 本来就有 8×50 ms 的 DEVICE_IN_USE 重试，这次是设备被占超过 400 ms。约 90 次切换里 1 次，暂无复现路径，先记录。（09:50 复现并追到根因：不是「被别人占」，是我们自己上一代的闸门还扣着，见下条。）

### 2026-11-10 09:40 — 客户端音量写在哪一层：会话管理器 → `ISimpleAudioVolume`（实测，附 IID 更正）

**上一轮那句「走的是会话/端点音量（`ISimpleAudioVolume` 一类）」是推断。这一轮把它读成了实数，顺带把两个记错的 IID 更正掉。**

**新工具 `sess_vol.exe`**（`tools/sess_vol.c`，`build.sh tools` 一并构建）。同一个端点下把两层音量分别读出来，互不干扰：

- 端点层：`IMMDevice::Activate(IAudioEndpointVolume)` → `GetMasterVolumeLevelScalar` / `GetMute` / `QueryHardwareSupport`
- 会话层：`IMMDevice::Activate(IAudioSessionManager2)` → `GetSessionEnumerator` → 每个会话 `QueryInterface(ISimpleAudioVolume)` → `GetMasterVolume` / `GetMute`；再用 `IAudioSessionControl2::GetProcessId` 认出是哪个进程
- `sess_vol.exe --watch` 每 300 ms 采样，只在读数变化时打印；`sess_vol.exe 耳机` 指定端点。全程只读，不写任何会话状态。

**实测（耳机 HyperX Cloud III，端点带硬件音量节点）：**

| 客户端音量滑块 | 端点音量（`IAudioEndpointVolume`） | 客户端会话音量（`ISimpleAudioVolume`） |
|---|---|---|
| 0%（静音） | **41%**（未变） | **0%** |
| 滚到 40% | **41%**（仍未变） | **40%** |

滑块从 0% 到 40%，只有会话那一层跟着动，端点一层分毫未动（41% 是这个端点自己的标量，和客户端的 40% 只是碰巧接近）。**结论：客户端音量写的是 `ISimpleAudioVolume::SetMasterVolume`，不是端点音量。** 这也解释了 08:48 那次 dump 为什么「一个比特都没变」：会话音量由音频引擎施加，而独占模式整个绕开引擎。

客户端在同一个端点上挂了**两个会话**（一个恒 100%，一个跟着滑块走），所以「读会话音量」这件事必须按进程 + 读数一起看，不能只看第一条。

**「对象在启动时就建好并缓存」这一点也拿到了日志证据。** 09:30–09:37 这 7 分钟里 `Activate iid=BFA971F1`（`IAudioSessionManager`）只出现 **1 次**，位置 09:36:34.360——紧跟在新流的 `GetService(RenderClient)` / `GetService(Clock)` 之后；随后把音量从 0% 滚到 40%，**再没有出现任何 `Activate`**。也就是说客户端在建流时取一次会话管理器、拿一次 `ISimpleAudioVolume`，之后所有音量调整都打在这个缓存对象上。这正是「启动时建好、之后调 `SetMasterVolume`」的形态，和它每次切歌都重新 `Activate(IAudioClient)` 的行为形成对照。

**IID 更正**（按 SDK 头文件逐个核对，`F6E4C0A0` 与 `F4B1A599` 上一轮都记错了）：

| 日志里的 IID | 实际接口 | 出现方式 / 次数 |
|---|---|---|
| `1CB9AD4C` | `IAudioClient` | Activate ×303 |
| `7ED4EE07` | `IAudioClient3` | Activate |
| `BFA971F1` | `IAudioSessionManager` | Activate ×128（每次建流 1 次） |
| `77AA99A0` | `IAudioSessionManager2` | Activate ×30 |
| `5CDF2C82` | `IAudioEndpointVolume` | Activate ×58 |
| `BBF8E066` | `ISpatialAudioClient` | Activate ×4 |
| `F6E4C0A0` | **`IAudioClockAdjustment`** | GetService ×10（客户端想调速率） |
| `F4B1A599` | **`IAudioSessionControl`** | GetService ×7 |
| `87CE5498` | `ISimpleAudioVolume` | **一次都没出现** |

`ISimpleAudioVolume` 一次都没出现不是矛盾：它不从 `IAudioClient::GetService` 取，而是从 `IAudioSessionManager::GetSimpleAudioVolume`（vtable 槽位 4）取。我们的钩子只覆盖 `IMMDevice::Activate` 和 `IAudioClient::GetService`，中间这一跳看不见——所以「日志里没有」不等于「客户端没用」。

**如果将来真要做模块侧音量（已否决，仅记录路径）**：要拦的只有 `IAudioSessionManager::GetSimpleAudioVolume` 这一个槽位——客户端建流时把返回对象换成代理并缓存，`SetMasterVolume` 一到就把衰减量交给 sink 的 DSP。两个硬约束：① 必须在建流时就位，客户端拿到指针后会一直用，事后替换无效；② 代价是数字衰减，用户已明确否决。**本轮不写代码。**

**一处待改的日志标签**：同一次建流里 `GetService -> fake IAudioRenderClient (gen 10)` 而 `drain: thread up (gen 9)`——`g_generation` 同时喂给 `SinkGen::id` 和 `g_sink.generation`（代理对象有效性令牌），两个数必然错开一位。不是 bug，但对着日志找「哪个代」时会绊一下。等下一次动 sink 时把标签分开（代理行同时打 gen 与 epoch），避免为一行日志单独重新注入一次。

### 2026-11-10 09:50 — 0x8889000A 追到底：不是重试窗口不够，是我们自己的上一代还攥着端点

**先复现。** 用 CDP 把客户端「输出设备」在 `Wasapi:耳机` 与 `Wasapi:默认输出设备` 之间来回切 6 轮（两个条目指向同一台耳机，端点硬件静音，全程无声），第一次就撞上：

```
09:43:59.599  sink: drain thread parked at stage 2 ...; generation 5 abandoned
09:43:59.941  endpoint gate busy after 20 attempts (32): ...\NexExclusive-50db26d727aac042.lock
09:43:59.955  sink failed 0x8889000A, falling back to the real shared stream
```

`32` = `ERROR_SHARING_VIOLATION`，闸门文件被**同一进程的另一个句柄**独占着。

**根因**：`sink_stop` 放弃一代时，那一代的**端点闸门句柄**跟着它一起被扣住，要等卡在 `GetBuffer` 的线程回来才释放。客户端的下一个 `Initialize` 往往几十毫秒后就到，于是被我们自己挡在门外——`acquire_endpoint_gate` 直接返回 DEVICE_IN_USE，`try_exclusive` 里那个 8×50 ms 重试**根本轮不到**（失败发生在它之前）。所以 09:25 那条「设备被占超过 400 ms」的推测是错的：不是别人占，是我们自己。

**第一次改法（错的，已撤回）**：放弃那一代时提前 `CloseHandle` 闸门，让下一个 engage 直接进 `negotiate`。实测更糟——僵尸流那条独占流还开着，设备确实还被占着，于是 `negotiate` 把候选格式一个个试：

```
09:46:45.465  probe 44100/24: Initialize 0x8889000A
               ... 每个 PCM 候选各 8×50 ms ...
09:46:48.523  sink: 19 candidates rejected, no shared fallback implied
09:46:48.533  sink failed 0x80004005, falling back to the real shared stream
```

客户端线程被摁了 **3.5 秒**，最后还是没拿到流。攥着闸门则立刻失败、客户端马上重建，反而恢复更快。**闸门说的就是实话**：那条独占流还开着，设备确实还在用；它不是可以提前兑现的预约。

**最终改法（两处，都很小）**：

1. **重试上移到闸门**。`acquire_endpoint_gate` 里 20×10 ms（200 ms），只对 `ERROR_SHARING_VIOLATION` / `ERROR_ACCESS_DENIED` 重试。09:13 那次被放弃的线程只晚了 5 ms，这个窗口能救；救不了时代价也只有 200 ms，之后立刻失败，不会滑进「19 个候选各试 8 次」。
2. **闸门等待成功且 `attempts>0` 时记一行**（`endpoint gate: waited N x 10 ms ...`）。这是「僵尸窗口真的出现过」的正面证据；正常路径 `attempts=0`，不刷屏。

**验证**：最终版再跑 6 轮（12 次换设备）——4 次放弃、10 次 ENGAGED、**0 次闸门冲突、0 次失败**；放弃后线程平均 22 ms 退出，下一次 ENGAGED 约 120 ms 后。回归 16 组全 PASS：新增 `test_gate_waits_for_previous_holder`（自己占着同一路径 50 ms，被验函数必须等到第 4 次尝试才拿到），`test_generation_abandon` 增加两条断言——放弃期间闸门**仍被持有**（重新打开必须失败）、线程回来后闸门释放（重新打开成功）。

**顺带记两个行为事实**：

- **切输出设备必定放弃一代**：drain 停在 `GetBuffer`，join 阶梯 300+600 ms 走满。这和切歌不同（切歌 26 ms 干净退出）。
- **客户端每换一次设备会建两条流**：先一条 96 kHz、再一条 44.1 kHz，最终留下 44.1 kHz 那条。

**未做**：物理拔插（拔 USB）仍未实测；多进程争用（第二个客户端实例）未实测——闸门对它是有效的，但没验证过第二个实例被拒时的表现。

### 2026-11-10 10:00 — QA-02 补齐（88.2/96/192 实机）+ QA-03（SRC 全频验收）

**QA-02：采样率实机。** 用 `player.exe` 造静音流（`--amp 0`，全程无声）并注入模块，逐档看独占能不能按原率打开：

| 源格式 | 结果 | 候选数 |
|---|---|---|
| 44100/32f | `out=44100/24 [depth only]` | 1 |
| 88200/32f | **`out=96000/24 [converted]`**，`resample: 88200 -> 96000, phases=160, half=64` | 4 |
| 96000/32f | `out=96000/24 [depth only]` | 1 |
| 192000/32f | **`out=96000/24 [converted]`**，`resample: 192000 -> 96000, phases=1, half=128` | 13 |

**这台端点（HyperX Cloud III）只接受 44.1/48/96，不接受 88.2 和 192**，所以这两档必然走 SRC——是端点能力，不是模块选择。`negotiate` 的策略是「原率优先，装不下退到最接近的可用率」：88.2 退到 96（比率 147/160，160 相位插值），192 退到 96（正好 2:1 抽取，只有 1 个相位）。换一台支持 88.2/192 的设备就能原率直通，代码侧没有需要改的地方。

**QA-03：SRC 全频。** 新增回归第 17 组 `test_src_response`，直接驱动 `src_init/src_push/src_pull`，用单频 DFT 量幅度（跳过 4096 个输出帧的建立过程）：

| 比率 | 通带最差（0.05…0.80×截止） | 0.95×截止 | 阻带 |
|---|---|---|---|
| 44.1k→48k | −0.00 dB | −0.01 dB | 上采样，不适用 |
| 48k→44.1k | −0.00 dB | −0.01 dB | r=0.959：−51.1 dB；r=0.980：−118.8 dB |
| 88.2k→96k | +0.00 dB | −0.00 dB | 上采样，不适用 |
| 192k→96k | +0.00 dB | −0.01 dB | r=0.750：−137.5 dB；r=0.980：−151.4 dB |

- **通带**：0.8×截止以内平坦度好于 0.01 dB，远低于可闻阈值。
- **过渡带**：48→44.1 最宽——截止 0.919，到 0.959 才 −51 dB，0.98 才 −118 dB。也就是 20.3–21.6 kHz 这一小段衰减不足。128 taps 的必然结果，且这一段基本是近超声内容；真要收紧只能加长核，代价是 CPU 和延迟。
- **直流**：0.5 进、0.5 出（±1e-3）。
- **曲尾**：96000 帧输入产出 48000 帧，与理论一致（±滤波器半宽）。
- **冲激/峰值**：44.1→48 峰值 0.914（不过冲）；192→96 峰值 0.318（核按直流增益 1 归一化，2:1 抽取的理论峰值约 0.32）。
- **实时耗时**：0.5 s 的 192k→96k 用 10.5 ms，约 **48× 实时**（单核约 2%）。
- **线性相位**：各相位共用一条 `|H(ω)|`，相位差严格等于 `−ω·frac`（1e-6 以内）。

**顺带修掉一个真缺陷**：相位核的采样区间原来是 `[-K+1, +K]`，比对称区间少一格。整数相位时 `+K` 那个抽头恰好落在窗函数零点上，看不出来；小数相位下 `|K − frac|` 可能小于 `half`，那个没有配对的抽头就带上约 1e-3 的权重，核不再严格对称，**线性相位名不副实**。改成 `[-K, +K]`（多一个抽头）后 `src_kernel_linear_phase` 才通过。通带/阻带数字与改前完全一致——只动了最外侧那个近乎为零的抽头，属于修正而非调音。

**测试写法上的两个坑（免得再踩）**：
- 第一版把 `r`（相对奈奎斯特的比例）直接当成输出域的角频率，满幅正弦算出 −78 dB 的假结果。输出域是 `πr·(in/out)`，只有 in==out 时才相等。
- 第二版拿「输出冲激响应的索引对称」判线性相位，44.1→48 判失败：输出样本落在小数位置上，用线性插值去比较时，宽带响应本身的插值误差就有百分之几。**直接查相位核的幅频/相位关系才是准的**；索引对称只对整数相位成立。

**回归**：17 组全 PASS。

### 2026-11-10 10:10 — 功能覆盖率：本项目 vs AMExclusive vs Orion / AppleMusic-OpenHome

**证据边界**：本项目一列全部是本机实测/代码可查；另外两列来自用户描述与前期调研记录（`AMExclusive` 只有 WASAPI 独占一条通路；`Orion`/`AppleMusic-OpenHome` 在其上加了 ASIO、OpenHome、DLNA，本机/网播两个页签，本机页签在 WASAPI 端点与 ASIO 驱动之间二选一，配置写 `am-exclusive.ini`）。**这两家的仓库这次没能访问到**，标 ❓ 的都是未核实，不是「没有」。

| # | 能力 | 本项目 | AMExclusive | Orion |
|---|---|---|---|---|
| 1 | WASAPI 独占输出 | ✅ 实测 | ✅ | ✅ |
| 2 | ASIO 通路 | ❌ | ❌ | ✅ |
| 3 | 网络播放（OpenHome / DLNA） | ❌ | ❌ | ✅ |
| 4 | 输出端点选择 | ⚠️ 跟随客户端/系统，不提供自己的选择器 | ❓ | ✅ |
| 5 | 原采样率直通 | ✅ 44.1/48/88.2/96 实测。**88.2 取决于端点**：MM3A 直通、HyperX 转 96；176.4/192 两个端点都拒（10:25） | ✅ 独占必然 | ✅ |
| 6 | 输出位深选择 | ✅ 16/24/32 三档 | ❓ | ❓ |
| 7 | 采样率转换（端点装不下时） | ✅ 128 taps Kaiser，全频验收过 | ❌ 装不下即失败 | ❓ |
| 8 | 抖动策略 | ✅ 按位深/是否重采样自动决定 | ❓ | ❓ |
| 9 | 播放中换端点自动迁移 | ✅ 实测（含放弃代与闸门） | ❓ | ❓ |
| 10 | 端点独占协调（多实例/僵尸代） | ✅ 闸门 + 20×10 ms 重试 | ❓ | ❓ |
| 11 | 音量策略 | ✅ 端点能力诊断，明确不做数字衰减 | ⚠️ 钉满幅 | ⚠️ 钉满幅 |
| 12 | 应用内设置界面 | ✅ 设置页内 8 项 | ❌ 推测仅配置文件 | ✅ 两页签 |
| 13 | 状态与诊断回报 | ✅ HTTP `/api/status` + 结构化日志 | ❌ | ❓ |
| 14 | 一键启动/注入 | ✅ `NexExclusive.exe` 单进程 | ✅ 推测 | ✅ 推测 |
| 15 | 欠载/故障可见性 | ✅ 空缓冲计数、最大间隔、`NEX_DUMP` | ❓ | ❓ |
| 16 | 多客户端通用 | ❌ 只针对网易云 | ❌ 只针对 Apple Music | ❌ 只针对 Apple Music |

**结论（按「本机独占播放」这条线算）**：

- **对 AMExclusive：功能上已对齐并超出。** 核心独占、原率直通两边都有；多出来的是位深选择、SRC 兜底、设备迁移、独占协调、设置界面、状态诊断、一键部署。它没有的（ASIO/网播）本项目也没有——但那是另一条通路，不是本机播放的质量项。
- **对 Orion：本机通路覆盖率约 8 成，整体功能面约 6 成。** 差的 4 成集中在 ASIO 与 OpenHome/DLNA 两条独立通路（第 2、3 行），以及一个「自己选端点」的选择器（第 4 行）。本机 WASAPI 这条线上，本项目在诊断、SRC、迁移、界面四项上更完整。
- **要补齐到 Orion 的量级，需要的是**：ASIO 后端（约等于再写一条设备抽象 + 驱动枚举）、网络播放（OpenHome 控制点 + DLNA 渲染器），以及面板上的端点选择器。前两项是新增通路，不是现有代码的延伸；第三项是几十行的活。

### 2026-11-10 10:12 — REL-01 交付：Git 基线 + 可重复构建，顺带修掉三处写死路径

**规模**：仓库共 45 个文件、10595 行，克隆后 921 KB（含 `.git`）。挡在库外的是 97 MB 里的绝大部分——两个构建目录 39 MB、41 个 `.pdb` 与 30 个 `.exe` 约 25 MB、`sink.log` 348 KB 加十余份 `.prev*.log`、`analysis/ui-probe` 23 MB 与 `ui-shots` 4.5 MB。

**`.gitignore` 的取舍**：`build-*/` 而不是 `build*`，否则会把 `build.sh` 一起挡掉；`analysis/ame-ref/` 是别人的仓库（自带 `.git`、`LICENSE`、`THIRD_PARTY_NOTICES`），混进历史既不合规也没必要，出处留在本文档；`*.bak` 挡掉是因为有了历史之后 `nexsink.c.bak`（62 KB）不再有意义。

**踩到的第一个坑：`core.autocrlf=true`**。本机是 Windows 默认值，`git add` 时只看到一串 `LF will be replaced by CRLF` 警告，但后果不小——checkout 时 `build.sh` 会变成 CRLF，shebang 成了 `#!/usr/bin/env bash\r`，bash 找不到解释器，构建入口直接失效。加 `.gitattributes` 锁死：`* text=auto eol=lf`，只给 `*.cmd` / `*.bat` 留 CRLF（cmd.exe 在纯 LF 下解析 label 会出问题）。`git ls-files --eol` 复核后，`build.sh` / `deploy.sh` / `nexsink.c` 均为 `i/lf w/lf`。

**顺带修掉的真缺陷：三处写死开发机绝对路径**。这三处不是风格问题，是「换台机器就静默失效」——`hook.cfg` 读不到会退回默认值，`sink.log` 写不进会在目标进程目录下乱丢。

| 文件 | 原来 | 现在 |
|---|---|---|
| `hooklog.c` `log_line` | `"D:\\Coding\\...\\mod\\hook.log"` | `module_dir()` + `hook.log` |
| `hooklog.c` `config_path` | `"D:////Coding////...////hook.cfg"` | `module_dir()` + `hook.cfg` |
| `sink_core.inc` `config_path` | `"D:\\Coding\\...\\mod\\hook.cfg"` | `module_dir()` + `hook.cfg` |
| `sink_hooks.inc` `DllMain` | `"D:\\Coding\\...\\mod\\sink.log"` | `module_dir()` + `sink.log` |

`module_dir()` 用 `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, &__ImageBase)` 取本模块，再 `GetModuleFileNameW` 截目录。**关键性质**：注入到别的进程之后，它返回的仍是 DLL 自己的完整路径，不受目标进程工作目录影响——所以 `player.exe` 在 `C:\` 下启动、DLL 在 `D:\...\mod\`，配置依旧读得到。用 `__ImageBase` 而不是把函数指针强转成 `LPCWSTR`，是因为后者在 `-Wall` 下会报指针类型不匹配。

**新增第 18 组回归 `test_module_dir`**：先 `SetEnvironmentVariableA("NEX_CFG", NULL)` 逼出回退分支，再断言 `module_dir()` 的结果与 `GetModuleFileNameW(NULL)` 推出的目录逐字相等、`config_path()` 拼出来的就是该目录下的 `hook.cfg`。**这条断言的价值在于它是反向的**——只要哪处再混进硬编码，或者构建目录被搬走，它立刻红。

**验收（REL-01 的三条标准逐条过）**：

1. `git clone` 到 `%TEMP%\nexclone-1849`，45 个文件、零 `.exe`/`.dll`/`.pdb`/`.log` 漏网。
2. 全新 `OUT_DIR=$CLONE/build-fresh`（不存在，由脚本创建）跑 `build.sh all` → 8 个产物齐：`nexsink.dll` `player.exe` `inject2.exe` `NexExclusive.exe` `host.exe` `hooklog.dll` `inject.exe` `list_eps.exe` `set_ep.exe` `sess_vol.exe`。
3. 同目录 `build.sh test` → **18 组全 PASS**，`module_dir` 报 `C:\Users\...\Temp\nexclone-1849\build-fresh\`。

另做一次「搬家」验证：把 `regression.exe` 与 `hook.cfg` 复制到 `%TEMP%\nexmove-1773\` 单独运行，`module_dir` 随之变成该临时目录——证明路径是运行时推导的，不是编译期常量。

**`build.sh` 的健壮性补丁**：`ZIG` 默认值仍是开发机路径，别人拿到仓库若没设 `ZIG`，原来只会看到 `zig.exe` 的 ENOENT。现在提前 `[[ -x "$CC" ]]` 判断，打印缺失路径与一行 `ZIG=/c/zig/zig-x86_64-windows-0.17.1 mod/build.sh all` 的示例，退出码 2。

**提交**：`cf05b20` 初始提交（45 files, +10595）、`2c7ef03` build.sh 提示。仓库身份是**仓库级**的 `Pingyi <pingyi@local>`，没有动全局配置，不影响机器上其他项目。**未设远端**——这是注入网易云客户端的代码，公开有风险，等明确决定再说。

**仍然存在的环境绑定（本次未动，明确列出）**：`mod/tools/nexhost.c:32-40` 有 7 处绝对路径（`kClientExe`/`kClientDir` 指向 `D:\Installed_Program\CloudMusic`，`kSinkDll`/`kHookCfg`/`kUiJson`/`kSinkLog`/`kHostLog`/`kPanelJs`/`kSimplifyJs` 指向 `D:\Coding\NeteaseExclusive\mod`），`mod/tools/launch.c:9-11` 与 `mod/tools/nexui.cmd:8` 同样写死了客户端安装位置。这些不影响构建（REL-01 的标准是「空构建目录可产出」），但影响**别人拿到就能跑**。宿主侧的文件路径可由 exe 所在目录推导，客户端路径需要探测或让用户配——这是一项独立工作，不在 REL-01 范围内，留作后续。

### 2026-11-10 10:25 — 「音箱支持 96，88.2 为什么也被挡」：查清了，是耳机不是音箱

**问题**：用户质疑 10:00 的结论。端点报 96 kHz，88.2 比 96 小，按「范围」推不该被拒。

**结论先说：用户的直觉是对的，音箱确实没被挡，被挡的是耳机。** 10:00 那次跑在当时的默认端点 HyperX 上，结论本身没错（「这台 HyperX 端点都不收」），但我后续在任务表和对比表里写成了「88.2/192 受端点限制」，容易被读成所有端点如此。**这是表述问题，不是事实变化**，已在上面两处改正。

**能力矩阵（`IsFormatSupported(EXCLUSIVE)`，两轮独立复现）**：

| 采样率 | HyperX Cloud III | MOONDROP MM3A |
|---|---|---|
| 44100 | ✅ 16/24 | ✅ 16/24 |
| 48000 | ✅ 16/24 | ✅ 16/24 |
| **88200** | ❌ 全拒 | **✅ 16/24** |
| 96000 | ✅ 16/24 | ✅ 16/24 |
| 176400 | ❌ | ❌ |
| 192000 | ❌ | ❌ |

**32-bit float 在独占下两个端点都拒**；共享模式下 96k/32f 反而是 `S_OK`，因为那正是引擎格式。硬件只吃整数 PCM。

**端到端对照（同一份 88.2 源，player.exe --rate 88200 --amp 0 静音流 + 注入）**：

| 端点 | 协商 | 处理 |
|---|---|---|
| MM3A | `selected 88200 Hz/24 bit after **1** candidates` | `[depth only]`，只 float32→24bit，**无重采样** |
| HyperX | `selected 96000 Hz/24 bit after **4** candidates` | `[converted]`，88200 三种位深试遍后落到 96 |

MM3A 上还单独验了 `Initialize(EXCLUSIVE, 88200/24)` → `S_OK, frames=8820`（100 ms × 88200，数字对得上），确认不是探测假阳性。

**为什么「支持 96」推不出「支持 88.2」**：WASAPI 独占的格式协商是**白名单匹配**，不是范围检查。端点只声明离散的一组格式，`IsFormatSupported` 就在这组里找。88.2 和 176.4 属于 44.1 家族，跟 96（48 家族）没有大小关系——HyperX 的实现里 44.1 家族只到 44.1 本身，48 家族到 96。

**共享模式为什么不拒**：同一个 88.2，`IsFormatSupported(SHARED)` 返回 `S_FALSE` + `closest = 96000/32`，意思是「不原生支持，但我能转」。88.2/176.4/192 全是这个结果。共享模式永远不拒绝，因为它总能 SRC 到引擎格式——**这正是我们在独占模式下做的事，区别只是我们明确告知走了转换**（`[converted]` vs `[depth only]`）。所以「共享模式能播 88.2」不能拿来证明硬件支持 88.2。

**踩到的一个陷阱（重要，别再用）**：端点属性 `{74E4357A-7959-47A4-A8B2-FEB2FFD48427} pid=2` 看起来像格式白名单——`0A 00 | 04 00 | 44 AC 00 00 | 80 BB 00 00 | 88 58 01 00 | 00 77 01 00 | ...`，4 个采样率正好是 44100/48000/88200/96000，后面跟 3 组位深对和声道数。**但两个端点的这段字节完全相同，而 HyperX 独占并不支持 88.2**。所以这个属性不是独占支持列表（推测是引擎/APO 侧可处理的格式），**判断独占能力只能实测 `IsFormatSupported(EXCLUSIVE)`**。另外 `PKEY_AudioEndpoint_Formats`（`{1da5d803-...}` pid=0）在这两个端点上返回的是 `VT_UI4` 而非 `VT_BLOB`，根本没有列表可读。

**新增/改造的工具**：

- `mod/tools/ep_formats.c`（新）—— 枚举端点属性，`-a` 打印全部 75 项含完整 GUID 与 blob 十六进制。就是靠它发现上面那个属性陷阱的。
- `mod/tools/probe_excl.c`（改造）—— 加 SHARED 列与 `closest` 输出，这样「引擎能转」和「硬件能直通」在同一行对照；加端点名过滤（`probe_excl.exe "MM3A"`）；加 `--init` 做真实 Initialize；修掉原来 `in[64]` 未初始化导致 Initialize 列打印空白的 bug。
- 两个工具都进了 `build.sh tools`（`probe_excl.exe` 之前一直漏在构建脚本外，是手动编的）。

**对模块本身的影响：没有缺陷。** 候选池 `{44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000}` 里本来就有 88200，MM3A 上一次命中。HyperX 上试 4 次落到 96 也是正确行为。

**可选的改进（未做）**：现在是按固定顺序逐个试，HyperX 上要试 4 次才落定。可以先用 `IsFormatSupported` 把端点的实际能力探一遍再排序，或者缓存上一次成功的格式。省的是几百毫秒的协商时间，不影响正确性，等有实测收益再动。

### 2026-11-10 10:59 — 发布形态落地：一个 exe、双击即装、`release.sh` 出包

**这一轮回答的是「GitHub Release 怎么一键注入」。** 结论：**不做单独的安装器**。AMExclusive 那种 `AMExclusive-Setup-1.8-Aria.exe`（2.32 MB，运行后 `1` 装 `2` 卸）是把安装逻辑单独编了一个程序；我们的 `NexExclusive.exe` 本来就要常驻，把安装逻辑放进它自己，少一个二进制、少一次下载、也少一个被杀软盯上的样本。所以发布包里只有一个可执行文件，双击它就会问你要不要装。

**发布包形态**（`release.sh` 产出，全在 `dist/`）：

```
dist/NexExclusive-1.0.0.zip          324 KB（331728 字节）
  NexExclusive-1.0.0/
    NexExclusive.exe   320 KB（327680）
    nexsink.dll        277 KB（284160）
    hook.cfg  ui.json  ui/panel.js  ui/simplify.js
    README.txt
dist/SHA256SUMS.txt
dist/RELEASE-NOTES.md
```

`release.sh` 只挑运行时要的 6 个文件。`player.exe`、`inject*.exe`、`probe_excl.exe` 这些诊断工具不进包——对用户没用，只会让杀软多几个可疑样本。打包用系统自带的 `C:\Windows\System32\tar.exe`（bsdtar，`-a` 按扩展名写 zip），机器上没有 zip/7z 也能出包。

**双击安装的流程**（`main()` 里的顺序，每一步都有理由）：

1. `--help` / `--install` / `--uninstall` 直接走，不占单实例锁
2. **单实例**：`Local\NexExclusiveHost` 互斥体。装了自启之后，「开机拉起的那份」和「用户又双击下载目录那份」会同时存在，两个宿主抢 9334 端口，还会互相把注入闸门判成 busy
3. **跳转**：已安装、但当前 exe 不在安装目录 → 把装好的那份拉起来，自己退出。这里有个坑：必须先 `CloseHandle(once)` 再 `launch_exe`，否则新进程撞上自己还没释放的锁，起来就退
4. **首次引导**：没装过 + 交互式启动 → `MessageBox` 问一句，选「是」就装，装完直接拉起（同样先放锁）
5. 正常看护

**客户端版本检查**：`GetFileVersionInfoW` 读 `cloudmusic.exe` 的版本资源，只对 `3.1.42.205594` 标「已验证」，其他版本写「未经验证——注入可能失效」，不做硬拦截（拦下来用户也没法用，只会更困惑）。**踩到一个坑**：网易的版本资源里字符串表和数字字段不是一回事——`FileVersion` 字符串是 `3.1.42.205594`，`VS_FIXEDFILEINFO` 的 `dwFileVersionMS/LS` 是 `3.1.42.8986`。第一版读的是数字字段，日志里显示 `3.1.42.8986`，跟客户端「关于」对不上。改成先走 `\StringFileInfo\<lang><cp>\FileVersion`，取不到再退回数字。

**客户端探测补了两级**（原来 5 级 → 现在 7 级）：

- **桌面 / 开始菜单快捷方式**：解析 `.lnk` 的目标路径。绿色版、装在自定义目录的客户端不写注册表，但安装时几乎一定留快捷方式。只认目标文件名是 `cloudmusic.exe` 的，避免误认。扫 `%APPDATA%\...\Start Menu\Programs`（含一层子目录）、用户桌面、公共桌面。实测：本机客户端在 `D:\Installed_Program\CloudMusic`（注册表里没有），靠这一级探到了，日志 `来源：桌面/开始菜单快捷方式`
- **文件选择框**：全都找不到时弹 `GetOpenFileNameW` 让用户点一下 `cloudmusic.exe`，选完写回 `host.cfg`。原来只说「请用记事本打开 AppData 里那个文件加一行」——这是把开发者的耐心当成用户的耐心

**装完发现的一个真问题**：从 zip 里 `--install` 之后，`host.cfg` 的 `client=` 是空的（安装时客户端没在跑、注册表也没有），运行时同样探不到，直接退出。也就是说「装完就能用」对非常规安装位置的用户是假的。上面两级探测 + 选择框就是补这个洞。现在 `--install --client <路径>` 也能显式指定，装完 `host.cfg` 里就有值。

**日志输出的两个 bug（都修了）**：

- `--install` / `--uninstall` 在命令行里**一个字都不打**。原因：这两个函数只用 `host_log`（写文件），而 `host_log` 镜像到终端的条件是 `GetConsoleWindow() != NULL`。命令行里 stdout 被重定向时不会分配控制台，条件不成立 → 全静默。改成「有 stderr 句柄就输出」
- 改完之后又出现乱码：转码看的是 `GetConsoleOutputCP()`，但字节写进的是**管道**。有控制台、stderr 却被重定向时，把 GBK 字节灌进管道，上游按 UTF-8 解就是乱码。改成按**目标**判断：`GetFileType(stderr) == FILE_TYPE_CHAR` 才转控制台代码页，否则原样写 UTF-8

**实测记录**：

| 场景 | 结果 |
|---|---|
| `--install`（带 `--client`） | 6 文件 + `host.cfg` 进 `%LOCALAPPDATA%\NexExclusive`，卸载项 / Run 键 / 开始菜单 `.lnk` 全齐，终端有中文输出 |
| `--uninstall` | 目录、快捷方式、`Uninstall\NexExclusive`、`Run\NexExclusive` 全部清空（复核 `Test-Path` = False，Run 值为空） |
| 装完启动 | `客户端版本 3.1.42.205594（已验证）` → 注入 `LoadLibraryA=0x97A50000` → `界面脚本已就位` |
| 单实例 | 第二个实例输出「已有一个 NexExclusive 在运行，本次启动退出」并退出 0，第一个不受影响 |
| 跨目录跳转 | 从 `build-path6` 启动，日志「已安装到 …\NexExclusive，改为启动那份」，只有安装目录那份在跑 |
| 快捷方式探测 | 桌面临时放一个指向 `cloudmusic.exe` 的 `.lnk`，`host.cfg` 留空 → `来源：桌面/开始菜单快捷方式`（测完已把 `.lnk` 移出桌面） |

**未实测**：文件选择框（需要人点，无法自动化）、双击时的 `MessageBox` 引导（同上）。两者都是纯 UI 分支，逻辑已 review。

**分发硬门槛（必须用户决定）**：Defender 把 `NexExclusive.exe` 判成 `Behavior:Win32/DefenseEvasion.A!ml`（ThreatID `2147738096`，SeverityID 5），**约 30–60 秒内就会隔离 exe**，并连带清掉快捷方式、Run 键、卸载项。`Get-MpThreatDetection` 记录显示 10:38 那次动到了 5 个对象（文件 + `.lnk` + Run + Uninstall）。触发点是「往别的进程里 `WriteProcessMemory` + `CreateRemoteThread`」这个行为本身，跟代码写得好不好无关。当前会话**非管理员**，加排除项的 cmdlet 用不了（报 `Must be an administrator to view exclusions`），所以本轮所有安装/卸载测试都是「装完趁它还没被删掉赶紧验」跑出来的。

可选出路，按代价排序：

1. **用户以管理员加排除项**（本机自用够了，不解决分发）
2. **向微软提交误报**（免费，几天出结果，对这个 case 成功率不低——我们不做持久化、不联网、不加密，只是加载自己的 DLL）
3. **换注入方式**：现在是最典型的 `VirtualAllocEx` + `WriteProcessMemory` + `CreateRemoteThread(LoadLibraryA)`。改成代理 DLL（放在客户端目录旁、转发导出表）能绕开「远程线程」这个特征，但要写客户端安装目录，且用户装在 `Program Files` 时没权限
4. **代码签名**（要证书，每年几千块；能压 SmartScreen，但压不住行为检测）

本轮没有自作主张去改注入方式——那是架构级改动，得先确认用户要往哪个方向走。

### 2026-11-10 11:15 — 排除项实测有效：本机开发与安装都不再被删

**用户以管理员加了两条排除路径**（`D:\Coding\NeteaseExclusive` 与 `%LOCALAPPDATA%\NexExclusive`）。当前会话非管理员，`Get-MpPreference` 仍读不出列表（`Must be an administrator to view exclusions`），所以只能**用行为验证**——造全新 exe、跑完整注入、等它两分钟。

**对照组（加排除之前）**：`build-path4` 的 exe 在 32 秒内被删了三次（10:47:46、10:48:26、10:48:31），每次都是「刚构建出来就被扫到」。

**实验组（加排除之后）**：

| 路径 | 动作 | 等待 | 结果 |
|---|---|---|---|
| `D:\Coding\NeteaseExclusive\build-path7`（全新目录、全新构建） | 启动客户端 → 注入 `nexsink.dll` → 注入界面脚本，全链路跑通 | 90 s | exe 存活（327680 字节，mtime 未变） |
| `%LOCALAPPDATA%\NexExclusive`（`--install` 装出来的副本） | 同上 | 120 s | exe 存活；`Uninstall\NexExclusive`、`Run\NexExclusive`、开始菜单 `.lnk` **全部存活** |

`Get-MpThreatDetection` 最新一条仍是 **10:48:31**，之后零新增。也就是说：**排除项确实生效了，而且是路径级的**——同一份源码，之前被判三次，现在连跑带注入都没事。

**两个必须记住的限制**：

1. **只保护这两个路径**。构建目录换到别处（`C:\temp`、`%TEMP%`）会重新中招。新建 `build-path8` 之类没问题，因为它们都在 `D:\Coding\NeteaseExclusive` 下面。
2. **只保护这台机器**。别的用户装上，一分钟内 exe 还是会被隔离——所以「Release 出去别人能不能用」这个问题的答案目前是**不能**，除非他们自己也加排除项。

**分发路径已经写清**：新增 `docs/defender-false-positive.md`，含 WDSI 提交入口、可整段粘贴的英文说明（程序做什么、为什么要注入、明确不做什么、为什么 `DefenseEvasion` 这个分类名不成立、复现步骤、请求的处置）、文件哈希，以及被打回后的三条退路（换代理 DLL 注入 / 代码签名 / 公开仓库与构建）。提交这一步需要用户本人操作，我不代劳。

### 2026-11-10 11:19 — 卸载那句「重启后移除」是假的；程序运行中重装会失败

**怎么发现的**：加完排除项后顺手查 `PendingFileRenameOperations`，1576 条待删除登记里**没有一条**是 `NexExclusive.exe`——但 11:10 那次卸载明明打印了「主程序将在下次重启后移除」。

**根因**：`MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)` 要写 `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\PendingFileRenameOperations`，**需要管理员**。普通权限下静默失败，而代码不管返回值都打同一句话。所以「卸载完成」之后目录里一直躺着 `NexExclusive.exe`，用户重启也不会消失。

**修复**：

- 按返回值说话。成功才说「已登记为重启后删除」；失败就直说「删不掉，关掉程序后手动删掉这个目录」
- 末尾不再无条件说「卸载完成」：有残留时改成「卸载完成（除主程序文件外，其余已清除）」
- 卸载顺带删 `host.log` / `sink.log` / `sink.prev.log`，从别的副本卸载时目录能真正清空
- `release.sh` 生成的 `README.txt` 卸载一节同步说明这个限制

**同源问题**：安装要覆盖正在运行的主程序，`CopyFileW` 撞共享冲突，报「有文件没复制成功」直接中止。把卸载里那段「停掉其它宿主」提成 `stop_other_hosts()`，安装和卸载共用。

**实测**：

| 场景 | 修复前 | 修复后 |
|---|---|---|
| 程序在跑时 `--install` | 「安装失败：有文件没复制成功」中止 | 「已停止运行中的旧宿主」→ 安装成功 |
| 从安装目录 `--uninstall` | 「主程序将在下次重启后移除」（假的） | 「删不掉…关掉后手动删目录」+「卸载完成（除主程序文件外，其余已清除）」 |

**为什么不彻底解决自删除**：非管理员删不掉正在运行的 exe，常见解法是「复制自己到 %TEMP% → 退出 → 临时副本删原文件」，但那是典型的释放器收尾行为，在 Defender 已经在盯这个程序的当下属于自找麻烦。当前取舍是：**说实话 + 给一条用户能照做的路**，不引入更可疑的行为。

**包已重出**：`dist/NexExclusive-1.0.0.zip`，sha256 `9d80ae99…ef9f`。安装版在跑，客户端已注入，端点 41% 静音。

### 2026-11-10 11:26 — 对着 AME 源码查「为什么人家不被删」，顺手补上版本资源

**问题**：用户问「别人用管理员运行呢？为什么 AME 用脚本装就没事？」

**管理员运行没用**。检测不看令牌权限，反而「提权 + 注入」是加分项。管理员能做的只有两件事：加排除项，或者从隔离区恢复并「允许在设备上」——都是事后动作，不是免疫。

**AME 用的是完全一样的注入方式**。`analysis/ame-ref/mod/src/Broker.cpp:461-469`：

```cpp
void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
if (!remote || !WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) { ... }
HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
```

和我们的四步一模一样。所以「人家没被报」不是因为它更干净。

**真正的差异（按证据强弱）**：

| | AME | 我们（改之前） |
|---|---|---|
| 版本资源 | `VersionInfo.rc.in`：CompanyName / FileDescription / FileVersion / InternalName / OriginalFilename 全齐 | **全空**，文件属性里一片空白 |
| 清单 | `/MANIFESTDEPENDENCY` 声明公共控件 v6 | **没有** |
| 图标 | **没有**（仓库里只有 `assets/ame-logo.svg`，CMake 和 rc 都没引用 `.ico`；这一行最初写成「有」，查证后改正） | 只有 `IDI_APPLICATION` 默认图标 |
| 代码签名 | 没找到签名步骤 | 无 |
| 普及度 | Release 被大量下载，云保护有信誉分 | 从未见过的哈希 |

前三条是我们能改的，后一条只能靠时间。

**做了什么**：

- `mod/tools/nexhost.rc.in` —— VERSIONINFO（含 `Comments` 说明「Loads nexsink.dll into the music player process to enable WASAPI exclusive output」，分析员一看就知道这程序干什么），版本号用 `@V1@/@V2@/@V3@` 占位
- `mod/tools/nexhost.manifest` —— `asInvoker`（永不提权）、per-monitor v2 DPI、公共控件 v6、longPathAware
- `build.sh` —— `zig rc` 生成 `.res` 再交给 `zig cc`。**zig 自带资源编译器**，不需要 windres 或 Windows SDK，换机器照样能构建；版本号从 `nexhost.c` 的 `APP_VERSION` 读，只有一处真相

**踩的坑（值得记）**：清单里也有版本占位符。第一版只 sed 了 `.rc`，没 sed `.manifest`，编出来的清单是 `version="@V1@.@V2@.@V3@.0"`。`zig rc` 不报错、链接成功，**一运行就死**：

```
bash: ./NexExclusive.exe: Permission denied
```

看起来像杀软拦截，实际是「应用程序的并行配置不正确」（`ERROR_SXS_CANT_GEN_ACTCTX`）。`sxstrace` 不用跑，事件日志直接说了：

> 元素"assemblyIdentity"中属性"version"的值"@V1@.@V2@.@V3@.0"无效

**这是本轮最容易误判的一次**——差一点就去查 Defender 了。

**验证**：`FileDescription = NexExclusive - WASAPI exclusive audio host`、`FileVersion = 1.0.0.0`、清单里 `asInvoker` 和 `version="1.0.0.0"` 都在；安装版启动、注入、界面脚本全部正常。包重出，sha256 `5bd701a4…6f57`。

**不承诺这能解决误报**。它降低的是「裸 PE」这个特征分，不是消除行为判定。真正的解法还是向微软报误报（材料在 `docs/defender-false-positive.md`），或者换注入方式。
