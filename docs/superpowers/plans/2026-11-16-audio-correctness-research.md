# 音频正确性研究与验收计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在进入运行代码修改前，冻结当前唯一候选，逐项确认 PCM 格式、远程时钟、EOS、背压和 Host 生命周期的真实语义。

**Architecture:** 先以 `D:\Coding\NeteaseExclusive\mod` 作为唯一源码与构建入口，进程内 WASAPI 作为对照，audiohost 作为单独实验路径。每个阶段只改变一个变量，保存源码版本、配置、日志、状态 JSON 和设备信息；证据不足的项目保持“待验证”。

**Tech Stack:** Windows 11、Zig 0.17.1、Win32 WASAPI、现有 C 回归、Node 契约测试、Pingyi 账户真实网易云会话。

**Spec:** `docs/AUDIO_CORRECTNESS_AUDIT_2026-11-16.md` 与 `docs/REMOTE_AUDIO_PROTOCOL_DESIGN.md`

## Global Constraints

- 唯一候选目录为 `D:\Coding\NeteaseExclusive\mod`；`work\playback-fix` 只保存历史构建与证据，不作为第二源码树。
- 研究阶段不修改音频运行代码；先记录失败现象和调用链，再进入单变量修复。
- 进程内与 audiohost 必须使用同一曲目、同一端点、同一客户端格式作为对照。
- `client PCM`、`converted PCM`、`endpoint PCM`、服务端质量档位分开记录。
- 每次 Pingyi 实测结束后关闭测试会话中的网易云、NexExclusive 和 NexAudioHost。

### Task 1: 文档与版本冻结

**Files:**
- Modify: `PROJECT_STATUS.md`
- Modify: `README.md`
- Modify: `docs/REMOTE_AUDIO_PROTOCOL_DESIGN.md`
- Modify: `docs/ROADMAP.md`

**Interfaces:**
- Consumes: `docs/AUDIO_CORRECTNESS_AUDIT_2026-11-16.md`
- Produces: 当前候选、验证范围、协议现状与缺口的唯一文字口径。

- [x] 更新项目状态页首部，注明当前阶段是文档与证据整理，历史记录保留在后方。
- [x] 在 README 中把“无损档位、客户端 PCM、端点输出”分开描述，移除尚未完成的远程稳定性表述。
- [x] 在协议设计中标出已实现字段与仍属设计的语义，尤其是设备时钟和 EOS。
- [x] 在路线图中把远程时钟、EOS、背压和真实设备验收置于运行代码改动之前。

### Task 2: PCM 格式与音源规格分层研究

**Files:**
- Inspect: `mod/ui/panel.js`
- Inspect: `mod/src/sink_endpoint.inc`
- Inspect: `mod/src/sink_dsp.inc`
- Inspect: `mod/src/sink_device.inc`
- Evidence: `docs/AUDIO_CORRECTNESS_AUDIT_2026-11-16.md`

**Interfaces:**
- Consumes: Hook 日志、`/api/status`、verifier 行、面板格式卡。
- Produces: 一张同曲目格式对照表，以及“源规格、客户端 PCM、端点 PCM”的明确结论。

- [ ] 在进程内路径记录输入采样率、输入容器/有效位深、输出采样率、输出容器/有效位深，并把它们命名为客户端 PCM 与端点 PCM。
- [ ] 在 audiohost 路径记录相同字段和 `convertedFrames`，确认共享环里的 PCM 与 Host 初始化格式一致。
- [ ] 查找网易云播放图或解码器元数据的可复核来源；没有来源时记录 `source_status=unavailable`，不从 `input_*` 推导源位深。
- [ ] 对同一曲目相同时间段比较两条路径的提交帧数、变化字节、首个差异位置和 hash。
- [ ] 只有数据一致时才进入听感对照；听感差异单独记录，不把它写成源文件 bit-perfect 证据。
- [ ] 记录 `/api/status`、`lastEngaged` 和 Host telemetry 的代次，确认面板没有沿用上一代格式。

### Task 3: 远程时钟单变量验证

**Files:**
- Inspect: `mod/src/sink_device.inc`
- Inspect: `mod/tools/nexaudiohost.c`
- Test: `mod/test/regression.c`
- Evidence: `work/` 下新增带构建身份的日志，不新增源码目录。

**Interfaces:**
- Consumes: `endpoint_frames`、设备 padding、Host `IAudioClock`、QPC。
- Produces: 远程时钟误差记录与是否需要改为 Host 回传设备位置的决定。

- [ ] 记录当前 `fc_GetPosition()` 返回值与 `endpoint_frames`、设备周期的对应关系。
- [ ] 在 Host 侧增加只读诊断输出，采集真实 clock tick、QPC、padding 和当前代次；本步骤不改变公开时钟。
- [ ] 在首包、持续播放、暂停恢复、seek、换轨五个场景分别比较客户端时间轴和设备时间轴。
- [ ] 若差异随 padding 变化，建立最小回归再进入时钟实现修改。

### Task 4: EOS、背压与 Host 退出

**Files:**
- Inspect: `mod/src/sink_lifecycle.inc`
- Inspect: `mod/src/sink_audiohost.inc`
- Inspect: `mod/tools/nexaudiohost.c`
- Test: `mod/test/regression.c`, `mod/test/nexaudiohost_test.c`

**Interfaces:**
- Consumes: generation、ring read/write、EOS submitted/complete、backpressure、source waits、underrun。
- Produces: EOS 产品定义、短尾策略、Host 提前退出策略和重复欠载统计定义。

- [ ] 分别触发自然源结束、暂停、Stop、Reset、seek、换轨和当前曲目重开，记录是否写入 EOS。
- [ ] 用短于一个设备包、恰好一个设备包、空流三种输入确认 Host 状态与错误码。
- [ ] 填满共享环，确认生产者等待或失败时读写位置不前移，且没有静默截短。
- [ ] 在运行中终止 NexAudioHost，确认 `host_exit`、当前代次结束和下一次进程内回退。
- [ ] 统计连续空事件，区分一次欠载区间与多个设备事件。

### Task 5: 真实 Pingyi 回归门槛

**Files:**
- Evidence: `D:\Coding\NeteaseExclusive\work\audio-correctness-2026-11-16\`
- Runtime: 安装目录与当前 Pingyi 网易云会话
- Verify: `docs/DEVELOPMENT_RUNTIME.md`

**Interfaces:**
- Consumes: Tasks 2–4 的单变量记录。
- Produces: 进入代码修改前的验收表，或明确阻塞项。

- [ ] 构建唯一候选并保存 `git diff --check`、回归、Host 契约和面板契约结果。
- [ ] 只在 Pingyi 会话播放一首歌，保持端点和曲目不变，记录约 30 秒的连续日志。
- [ ] 分别测试进程内与 audiohost，期间不切换质量档位、不改变系统默认设备。
- [ ] 测试完成后关闭三个进程，确认当前账户可以再次正常打开网易云。
- [ ] 只有审计表中的关键缺口都有证据时，才创建第一项运行代码修复任务。

### Task 6: 修正 UI 的格式语义

**Files:**
- Modify: `mod/ui/panel.js`
- Modify: `mod/ui/panel.contract.test.js`
- Modify: `README.md`

**Interfaces:**
- Consumes: `input_*`、`output_*`、`source_status` 的定义。
- Produces: 面板不再把“客户端 PCM → 独占端点”显示成“音源规格”。

- [x] 将紧凑卡片标题改成“客户端 PCM → 独占端点”，保留输入和输出各自的容器/有效位深。
- [x] 增加“源规格：未取得”占位；没有独立 `source_*` 证据时禁止显示猜测值。
- [x] 为 `source_status=observed` 和 `source_status=unavailable` 各写一个契约测试。
- [x] 运行 Node 契约、语法检查和 Markdown 链接检查。

### Task 7: 确认音源元数据来源，再决定协议字段

**Files:**
- Inspect: `work/decoder_probe_agent.js`
- Inspect: `work/decoder-probe-results/20261114-000222/analysis.json`
- Inspect: `mod/tools/nexaudiolink.h`
- Modify only after evidence: `mod/tools/nexaudiolink.h`, `mod/tools/nexhost.c`, `mod/ui/panel.js`

**Interfaces:**
- Consumes: 解码器探针或网易云播放图的可复核元数据。
- Produces: `source_status`, `source_origin`, `source_rate`, `source_valid_bits` 和 `source_encoding`。

- [x] 先确认探针能稳定捕获带格式元数据的解码帧；现有 `20261114-000222` 为 `decode_frames=0`，面板保持 unavailable。
- [ ] 若拿到真实源元数据，增加固定宽度 `source_*` 字段并递增 layout/build identity。
- [ ] 若只能拿到服务端质量标签，只记录 `quality_label`，不填充源位深。
- [ ] 为 observed、unavailable、旧 Host 拒绝新布局分别增加协议和面板测试。

### Task 8: 将远程公开时钟改为 Host 设备位置

**Files:**
- Modify: `mod/tools/nexaudiolink.h`
- Modify: `mod/tools/nexaudiohost.c`
- Modify: `mod/src/sink_audiohost.inc`
- Modify: `mod/src/sink_device.inc`
- Test: `mod/test/regression.c`, `mod/test/nexaudiohost_test.c`

**Interfaces:**
- Consumes: Host `IAudioClock`, QPC、padding、stream/format generation。
- Produces: generation-scoped device clock telemetry and monotonic client position.

- [x] 在控制页加入 clock tick、clock frequency、clock QPC、padding 和对应代次。
- [x] Host 每次采样这些值时保持同一代次；Hook 只接受当前代次。
- [x] `fc_GetPosition()` 删除固定周期修正，使用 Host 设备位置换算 100 ns 时间。
- [ ] 回归覆盖首包、padding 变化、暂停恢复、seek、换轨和时钟倒退。

### Task 9: 让 EOS 等待窗口真实存在

**Files:**
- Modify: `mod/src/sink_lifecycle.inc`
- Modify: `mod/src/sink_audiohost.inc`
- Modify: `mod/tools/nexaudiohost.c`
- Test: `mod/test/regression.c`, `mod/test/nexaudiohost_test.c`

**Interfaces:**
- Consumes: `eos_submitted_generation`、环读写位置、Host 设备时钟。
- Produces: `DRAINING → COMPLETE` 的真实等待与短尾策略。

- [x] 只在确认源结束后提交 EOS；暂停、Stop、Reset、seek 和换轨先使旧代次失效。
- [x] `remote_close()` 在 EOS 代次等待窗口内不直接发送 `CLOSING`，超时才报告失败。
- [x] 短尾、完整尾包、空流分别记录状态和错误码，不用静音填充掩盖定义缺口。
- [ ] 回归验证旧代次完成戳不能结束新代次，Host 提前退出能够结束当前代次。

### Task 10: 真实设备验收

**Files:**
- Evidence: `D:\Coding\NeteaseExclusive\work\audio-correctness-2026-11-16\`
- Runtime: Pingyi 账户的网易云、NexExclusive、NexAudioHost

**Interfaces:**
- Consumes: Tasks 6–9 的日志和状态字段。
- Produces: 进程内与 audiohost 的同曲目对照结论。

- [ ] 同一端点、同一首歌、同一区段分别测试进程内和 audiohost。
- [ ] 记录源规格状态、客户端 PCM、端点 PCM、设备时钟、padding、环水位和听感时间点。
- [ ] 覆盖持续播放、暂停恢复、seek、换轨、端点切换和 Host 退出。
- [ ] 测试结束后关闭三个进程，确认 Pingyi 会话可以重新打开网易云。

## Verification gate

进入实现前必须重新读取本计划与审计文档，并逐项确认：格式层次清楚、远程时钟证据来自真实设备、EOS 规则不会把暂停当作源结束、背压没有静默丢帧、Host 退出能被客户端看见、所有日志对应同一个构建身份。
