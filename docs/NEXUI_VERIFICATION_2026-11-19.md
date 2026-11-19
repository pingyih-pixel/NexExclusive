# nexui.dll 真机验证（2026-11-19）

验证对象：分支 `nexui-renderer-panel` 的宿主与 `nexui.dll`。客户端 3.1.43.205627，CEF 91.2.3 / Chromium 91.0.4472.169。

## 环境

- 暂存目录：`build.sh all` 输出到临时目录，再用安装目录 `%LOCALAPPDATA%\NexExclusive` 的 `nexsink.dll`、`hook.cfg`、`host.cfg` 覆盖同名文件，删去 `NexAudioHost.exe`（`audiohost=0`）。音频 DLL 与安装目录逐字节一致，未部署新的音频 DLL。
- 宿主以 `NEX_NO_REDIRECT=1` 从暂存目录启动，标准输出重定向，不弹交互提示。已安装的 `NexExclusive.exe` 全程未运行。
- 页面状态通过 9333 只读查询（场景 A、C、D）；场景 B 的客户端没有 9333，用日志与窗口截图判断。

## 结果

| 场景 | 结果 | 关键证据 |
| --- | --- | --- |
| A 宿主拉起客户端 | 通过 | 启动后约 1.1 s `界面链路：渲染进程就绪`；全程没有 `界面脚本已就位`（未走 CDP）；`nexui.log` 记录 `document #1 … panel loaded`；播放 12 s 原生进度从 0 走到 10.94；`sink ENGAGED` 正常出现 |
| B 用户先开客户端、宿主后到 | 通过 | 客户端单独启动 15 s 后 9333 未开放；宿主启动 5.6 s 后面板加载，未触发 CDP 回退；顶栏截图可见 “NexExclusive” 与 “编辑界面” |
| C 页面刷新、客户端重启 | 通过 | 刷新后同一渲染进程记录 `document #2 … panel loaded`；结束客户端后宿主拉起新实例，新渲染进程约 0.9 s 后就绪 |
| D 强制回退 | 通过 | 放置 `nexui.disable` 后渲染进程报 `unsupported`，宿主立即 `回退到 CDP（渲染进程不支持…）`，约 5.6 s 后 `界面脚本已就位`；页面标记 `__nexUiMarker` 为空、面板与入口各 1 份 |
| D2 两条路都注入同一页面 | 通过 | nexui 就绪后再经 CDP 执行一遍 `panel.js`、`simplify.js`：入口按钮、样式数量、版本标记前后一致，无重复 |
| E 宿主重启、客户端不动 | 通过 | 新宿主没有向渲染进程再注入 `nexui.dll`，启动后 11 ms 判定就绪 |
| F 两条链路分开 | 通过 | `nexsink.dll` 只在主进程；`nexui.dll` 只在 GPU 与渲染子进程，GPU 进程中记录 `inert in gpu-process process` |

## 未覆盖

- 场景 B 没有 9333，未能在页面内读取进度条；同一机制下的进度在场景 A 已确认。
- 页面长时间空闲后才启动宿主的极端情况：本次客户端空闲 15 s，面板 5.6 s 后出现；空闲更久时要等页面下一次原生调用。
- 多个渲染进程同时存在的情况未出现过（迷你播放器、桌面歌词不另起渲染进程，见此前探针验证）。
