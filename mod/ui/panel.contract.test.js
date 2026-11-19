/* Lightweight contract checks for the injected settings panel. */
'use strict';

const fs = require('fs');
const path = require('path');
const source = fs.readFileSync(path.join(__dirname, 'panel.js'), 'utf8');
const simplify = fs.readFileSync(path.join(__dirname, 'simplify.js'), 'utf8');

function check(condition, message) {
  if (!condition) throw new Error(message);
}

check(source.includes("data-nex-section"), 'panel sections must expose data-nex-section');
check(source.includes("className = 'nex-item'"), 'settings rows must use the native-style item structure');
check(source.includes("className = 'nex-control'"), 'settings controls need a dedicated control column');
check(source.includes("className = 'nex-summary'"), 'status summary must have a dedicated top region');
check(source.includes('function fieldDisabled'), 'controls need a central disabled-state policy');
check(source.includes('if (!strictHiFi()) return false'), 'strict native mode must participate in disabled-state policy');
check(source.includes("'rate', 'compat', 'src_prefer'"), 'strict native policy must cover processing controls');
check(source.includes("setAttribute('aria-disabled'"), 'disabled rows must expose aria-disabled');
check(source.includes("role', 'checkbox'"), 'check controls must retain checkbox accessibility semantics');
check(source.includes("data-nex-section"), 'output policy must be inside a named section');
check(source.includes("function mountSettings"), 'settings page needs a dedicated NexExclusive mount path');
check(source.includes("nex-settings-panel"), 'settings page needs its own panel id');
check(source.includes("#page_pc_setting, #setting, #settings"), 'settings mount must support the current client settings container');
check(source.includes("PANEL_VERSION"), 'panel injection needs a versioned replay guard');
check(!source.includes("if (module) module.remove();"), 'settings mount must preserve the shared NexExclusive module');
check(source.includes("mount();\n      if (connected) refreshStatus();"), 'settings mount must retry after the client reveals the settings container');
check(source.includes("NexExclusive"), 'settings navigation must expose the NexExclusive module');
check(!source.includes("key: 'bits', label: '输出位深'"), 'bit depth must follow the client format instead of a UI selector');
check(source.includes("'严格原样'"), 'native-only mode must use unambiguous user-facing wording');
check(source.includes("'原率优先（推荐）'"), 'default mode must be exposed as source-rate-first');
check(source.includes("独占转换"), 'status wording must distinguish exclusive conversion from native output');
check(source.includes("切换到原率优先"), 'native-only rejection must tell the user how to recover playback');
check(source.includes("nex-recovery"), 'native-only rejection needs a direct recovery action');
check(source.includes("formatAudioSpec"), 'lossless badge must format negotiated audio specifications');
check(source.includes("音频规格"), 'settings window must name the audio specification');
check(simplify.includes("className = 'nex-open'"), 'settings page needs a single entry to the editor');
check(simplify.includes("EDIT_BUTTON_ID"), 'interface cleanup needs a dedicated edit-mode button');
check(simplify.includes("HEADER_ENTRY_ID"), 'edit entry must live in the client header');
check(simplify.includes("nex-simple-modal"), 'interface cleanup needs a modal editor');
check(simplify.includes("activeGroup"), 'modal editor needs a selected category');
check(simplify.includes("key: 'sidebar_recommend'"), 'left navigation needs per-item rules');
check(simplify.includes("key: 'main_recommend'"), 'main page needs per-block rules');
check(simplify.includes("function titleGroups"), 'main page blocks need a title-group scan before hiding');
check(simplify.includes("if (inside !== 1) break;"), 'block hiding must stop at the outermost single-section ancestor');
check(!simplify.includes("while (node && node.parentElement !== root) node = node.parentElement;"), 'block targets must never resolve to the page container');
check(simplify.includes("function containsMultipleSections"), 'block hiding must refuse containers that hold several sections');
check(simplify.includes("RULES_STORE"), 'custom rules need their own persisted key');
check(simplify.includes("function applyRules"), 'custom rules must go through CSS instead of inline styles');
check(simplify.includes("nth-of-type("), 'custom selectors must rely on structural position, not hashed class names');
check(simplify.includes('data-testid="'), 'custom selectors must prefer stable anchors when they exist');
check(simplify.includes("function startPick"), 'editor needs a hover picker for elements without a preset rule');
check(simplify.includes("'ArrowUp'"), 'picker must expand the selection to outer levels');
check(simplify.includes("function commitPick"), 'picker must record the chosen element as a rule');
check(simplify.includes("'自定义'"), 'editor modal needs a custom rules category');
check(simplify.includes("function wholePageTarget"), 'picker must refuse container-level targets');
check(simplify.includes("这一层是整页容器"), 'picker must explain why a container cannot be recorded');
check(simplify.includes("presetPaths"), 'preset switches must resolve into CSS paths instead of inline styles');
check(!simplify.includes("element.style.display = 'none'"), 'hiding must not write inline styles onto nodes any more');
check(simplify.includes("function pageBlocks"), 'page block list must be derived from the current page');
check(simplify.includes("function renderBlockRows"), 'editor must render one row per block on the current page');
check(simplify.includes("function migrateMainKeys"), 'old text-matched main keys must be converted into position rules');
check(simplify.includes("function refreshPageTab"), 'block list must refresh when the user switches pages');
check(simplify.includes("function visiblePage"), 'block list needs to know which page the user is on');
check(simplify.includes("这一页没有认得出的分块标题"), 'pages without section titles need the coarse-block fallback');
check(!simplify.includes("function currentPageRoot"), 'page detection must not depend on the home-only title groups');
check(simplify.includes("function titleCandidates"), 'block list must recognise the title rows used on other pages');
check(simplify.includes("function visiblePage") && simplify.includes("document.elementFromPoint"), 'current page must come from what the screen actually shows');
check(simplify.includes("nex-rule-clear"), 'custom tab needs a way to clear every rule at once');
check(simplify.includes("icons.length > 1"), 'icon slots must not swallow a wrapper shared with the window controls');
check(simplify.includes("nex-rule-drop"), 'custom rules need a removal control');
check(simplify.includes("key: 'header_message'"), 'top buttons need per-item rules');
check(simplify.includes("className = 'nex-group'"), 'modal categories need expandable groups');
check(simplify.includes("nex-visibility"), 'visibility controls need a shared switch control');
check(simplify.includes("data-hidden=\"false\""), 'visibility switch needs an enabled state');
check(simplify.includes("nex-visibility::after"), 'visibility switch needs a movable thumb');
check(simplify.includes("tid_createplaylist_section"), 'created playlists must be excluded explicitly');
check(simplify.includes("tid_collectingplaylist_title"), 'collected playlists must be excluded explicitly');
check(simplify.includes("nex-editor-save"), 'interface editor needs an explicit save action');
check(simplify.includes("nex-editor-cancel"), 'interface editor needs an explicit cancel action');
check(!simplify.includes("点击右侧眼睛切换显示状态"), 'interface editor must not show the old top instruction sentence');
check(simplify.includes("nex-settings-module"), 'settings page needs a dedicated NexExclusive module');
check(simplify.includes("var module = document.getElementById('nex-settings-module')"), 'interface editor must join the shared NexExclusive module');
check(source.includes("PANEL_VERSION = 'settings-source-v9'"), 'settings panel needs a current replay marker after moving source detection to the host cache probe');
check(source.includes("nex-format-block"), 'audio specification must remain in the settings window');
check(source.includes("function makeModuleDraggable"), 'NexExclusive settings window needs draggable behavior');
check(source.includes("data-drag-ready"), 'draggable settings window needs an idempotent mount guard');
check(source.includes("function makeModuleResizable"), 'NexExclusive settings window needs corner resizing');
check(source.includes("nex-resize-nw"), 'resizable settings window needs the northwest corner handle');
check(source.includes("nex-resize-se"), 'resizable settings window needs the southeast corner handle');
check(source.includes("function fitModuleToViewport"), 'settings window needs viewport fitting after resize');
check(source.includes("nex-module-foot"), 'settings window needs a distinct bottom frame');
check(source.includes("nex-module-frame-override-style"), 'settings window needs a compatibility frame style');
check(source.includes("data-positioned"), 'resized settings window needs to preserve user geometry');
check(simplify.includes("SCRIPT_VERSION = 'header-modal-v46'"), 'interface editor contract must match the current replay version');
check(simplify.includes("function ensureHomeNav") && simplify.includes("tid_navitem_recommend"), 'the custom home page must live in the left navigation');
check(simplify.includes("function moveBlocksIn") && simplify.includes("data-nex-home-slot"), 'collected blocks move into the page and leave a slot behind');
check(simplify.includes("function deactivateHome") && simplify.includes("homePageSnapshot"), 'leaving via a block must tear the composed page down');
check(!simplify.includes("HOME_PAGE_ID"), 'the floating card overlay is replaced by a real page');
check(simplify.includes("NEX_BUTTON_ID"), 'header needs a NexExclusive entry beside the editor entry');
check(simplify.includes("function openNexSettings"), 'header NexExclusive entry needs to open the shared settings window');
check(simplify.includes("window.__nexOpenNexSettings === 'function'"), 'settings navigation retry must use the panel opener once panel.js finishes loading');
check(simplify.includes("function waitForSettingsLauncher"), 'settings navigation must wait for a late panel mount after the native settings page opens');
check(source.includes("SETTINGS_LAUNCHER_ID"), 'settings page needs a standalone NexExclusive launcher');
check(source.includes("nex-settings-launcher"), 'standalone launcher needs a dedicated element');
check(source.includes("function mountSettingsLauncher"), 'standalone launcher needs a title-area mount path');
check(!source.includes("nav.appendChild(entry)"), 'NexExclusive must not be appended to the native settings navigation');
check(source.includes("setAttribute('aria-expanded', value)"), 'standalone launcher must expose drawer state');
check(source.includes("data-open"), 'settings entry must toggle the right-side NexExclusive drawer');
check(source.includes("nex-module-close"), 'settings drawer needs a close control');
check(source.includes("formatSummary.id = 'nex-format-summary'"), 'settings window needs a compact negotiated format summary node');
check(source.includes("sourceSummary.id = 'nex-source-format-summary'"), 'settings window needs a separate source metadata summary node');
check(source.includes("formatSummary.textContent = '等待音频协商'"), 'settings format summary needs an initial waiting state');
check(source.includes("formatSummary.textContent = formatSettingsAudioSpecSummary(snapshot || {})"), 'settings format summary must render from the latest status snapshot');
check(source.includes("renderAudioSpecStatus(snapshot);"), 'the main status refresh must update the settings format summary');
check(source.includes(".then(renderAudioSpecStatus)"), 'the lightweight refresh must update both quality and settings summaries');
check(source.includes("return input[3] + ' bit / '"), 'compact format summary must use valid bits');
check(source.includes("客户端 PCM → 独占端点"), 'settings audio specification details must expose verifier scope');
check(source.includes("源规格：未取得"), 'audio specification must show an honest unavailable source state');
check(source.includes("source_status") || source.includes("sourceStatus"), 'audio specification must read an explicit source metadata status');
check(source.includes("source_rate") || source.includes("sourceRate"), 'audio specification must read source sample rate metadata');
check(source.includes("source_valid_bits") || source.includes("sourceValidBits"), 'audio specification must read source valid bit depth metadata');
check(!source.includes('function normalizeSourceAudioFormat'), 'source format must come from the host cache probe, not the bridge');
check(source.includes("source_status: 'probe'"), 'panel must only report the playId and let the host probe the cache');
check(source.includes('sourcePlayId = event.playId'), 'new playId must clear the previous source metadata locally');
check(source.includes('/api/source'), 'source metadata must be posted to the host');
check(!source.includes("existing.className = 'nex-quality-card'"), 'audio specification must not be injected into the native playbar');
check(source.includes('source_bitrate_kbps'), 'lossy sources must show the host-reported bitrate');
check(source.includes('source_quality_tier_hint'), 'source probe must send the current quality tier hint to the host');
check(!source.includes('记录代次'), 'source generation must stay out of user-facing audio details');
check(source.includes("/\\[depth only\\]/"), 'audio specification details must report depth-only conversion');
check(source.includes("sub:0x00000003"), 'audio specification details must distinguish float PCM');
check(source.includes("sub:0x00000001"), 'audio specification details must distinguish integer PCM');
check(source.includes("snapshot.verification.line"), 'audio specification details must reject an empty verifier measurement');
check(source.includes("本次流尚未生成逐样本校验记录"), 'missing verifier data must have an explicit honest state');
check(source.includes("Bit-perfect"), 'native output must be labelled bit-perfect in the specification card');
check(source.includes("nex-config-save"), 'settings need an explicit save action');
check(source.includes("nex-config-restart"), 'settings need a current-track restart action');
check(source.includes("/api/restart"), 'save-and-restart must request a current-track restart through the host');
check(source.includes("saveButton.textContent = '保存（下一首应用）'"), 'save must explain that the next stream applies the config');
check(source.includes("restartButton.textContent = '保存（当前曲目重开应用）'"), 'restart must explain that the current track is rebuilt');
check(source.includes("saveButton.addEventListener('click', function () { save(false); })"), 'save must persist without restarting the current track');
check(source.includes("restartButton.addEventListener('click', function () { save(true); })"), 'restart must save pending changes before rebuilding the current track');
check(!source.includes("nex-config-apply"), 'the duplicate save-and-apply action must be removed');
check(source.includes("pendingConfig = next;\n    applyVisualState();\n  }"), 'editing a setting must stay in the draft until an explicit action');
check(!source.includes("if (field.key === 'hifi' && value === 0) next.audiohost = 0;"), 'converted audiohost must remain enabled outside strict native mode');
check(source.includes("field.key === 'enable' || field.key === 'hifi' || field.key === 'audiohost'"), 'audiohost must be an independent engine switch');
check(source.includes("height:30px"), 'select controls must match the client control height');
check(simplify.includes("rgba(40,50,72,.1)"), 'interface editor controls must use the client border color');
check(simplify.includes("height:34px"), 'interface editor buttons must match the client control height');

const vm = require('vm');
const context = {};
const formatterStart = source.indexOf('  function formatRate');
const formatterEnd = source.indexOf('  function isPlaybackPaused');
check(formatterStart >= 0 && formatterEnd > formatterStart, 'audio specification formatter must be isolated for behavioral checks');
vm.runInNewContext(source.slice(formatterStart, formatterEnd), context);
const normalizedFlac = { source_status: 'observed', source_rate: 44100, source_valid_bits: 16,
  source_container_bits: 16, source_encoding: 'FLAC' };
const lossySource = { source_status: 'observed', source_rate: 44100, source_valid_bits: 0,
  source_encoding: 'MP3' };
const integerDepthOnlySnapshot = {
  lastEngaged: 'sink ENGAGED in=rate:44100/cont:32/valid:32/ch:2/tag:0xFFFE/sub:0x00000001 -> out=rate:44100/cont:24.24/ch:2 [depth only]',
  verification: null,
  endpoint: 'test endpoint'
};
const integerDepthOnly = context.formatAudioSpec(integerDepthOnlySnapshot);
check(integerDepthOnly.includes('32 bit 整数 PCM'), 'integer PCM must not be displayed as an unspecified or float format');
check(integerDepthOnly.includes('仅位深转换'), 'depth-only conversion must appear in expanded details');
check(integerDepthOnly.includes('本次流尚未生成逐样本校验记录'), 'null verification must explain that no measurement exists');
check(!integerDepthOnly.includes('暂无完整校验'), 'an absent measurement must not look like a failed measurement');
const floatConverted = context.formatAudioSpec({
  lastEngaged: 'sink ENGAGED in=rate:88200/cont:32/valid:32/ch:2/tag:0xFFFE/sub:0x00000003 -> out=rate:88200/cont:24.24/ch:2 [converted]',
  verification: { line: '', clientToEndpoint: 'unverified' }
});
check(floatConverted.includes('32 bit float32'), 'float32 input must be identified explicitly');
check(floatConverted.includes('独占转换'), 'full conversion must remain visible');
check(floatConverted.includes('本次流尚未生成逐样本校验记录'), 'an empty verifier line must not produce zeroed live data');
const waiting = context.formatAudioSpec({ lastLine: '=== nexsink attached ===', verification: null });
check(waiting.includes('等待新的音频协商'), 'a client without an engaged stream must show the waiting state');
const valid24InContainer32 = {
  lastEngaged: 'sink ENGAGED in=rate:44100/cont:32/valid:24/ch:2/tag:0xFFFE/sub:0x00000001 -> out=rate:44100/cont:24.24/ch:2 [converted]',
  verification: null
};
const compactValid24 = context.formatAudioSpecSummary(valid24InContainer32);
check(compactValid24.startsWith('24 bit / 44.1 kHz'), 'compact summary must use valid bits instead of the 32-bit container');
check(!compactValid24.startsWith('32 bit /'), 'compact summary must not present the container as the input precision');
check(context.formatSettingsAudioSpecSummary(integerDepthOnlySnapshot) ===
  '客户端 PCM 32 bit / 44.1 kHz → 独占端点 24 bit / 44.1 kHz · 已转换格式',
  'settings summary must label the client-to-endpoint evidence scope');
check(context.formatSettingsAudioSpecSummary({}) === '等待音频协商',
  'settings summary must have a waiting state before negotiation');
const unavailableSource = context.formatAudioSpec({
  lastEngaged: integerDepthOnlySnapshot.lastEngaged,
  source_status: 'unavailable'
});
check(unavailableSource.includes('源规格：未取得'), 'missing source metadata must remain explicitly unavailable');
check(!unavailableSource.includes('源规格　44.1 kHz'), 'client PCM must not be promoted to source metadata');
check((unavailableSource.match(/源规格/g) || []).length === 1, 'unavailable source metadata must not be repeated in the detail card');
const observedSource = context.formatAudioSpec({
  lastEngaged: integerDepthOnlySnapshot.lastEngaged,
  source_status: 'observed',
  source_rate: 44100,
  source_container_bits: 24,
  source_valid_bits: 24,
  source_encoding: '整数 PCM'
});
check(observedSource.includes('源规格　44.1 kHz / 24 bit 整数 PCM（24 bit 容器）'), 'observed source metadata must be shown separately from client PCM');
const observedFlac = context.formatSourceAudioSpec(normalizedFlac);
check(observedFlac.includes('源规格　44.1 kHz / 16 bit FLAC'), 'FLAC source metadata must avoid PCM container wording');
check(!observedFlac.includes('容器'), 'FLAC source metadata must not be labelled as a PCM container');
check(context.formatSourceAudioSpec(lossySource).includes('44.1 kHz / MP3'),
  'lossy source metadata must show codec and rate without bit depth');
check(context.formatSourceAudioSpec({ source_status: 'observed', source_rate: 48000, source_encoding: 'AAC',
  source_bitrate_kbps: 256 }).includes('48.0 kHz / 256 kbps AAC'), 'lossy AAC must show the bitrate instead of a bit depth');
check(context.formatSourceAudioSpec({ source_status: 'observed', source_rate: 96000, source_valid_bits: 24,
  source_encoding: 'FLAC', source_bitrate_kbps: 0 }).includes('96.0 kHz / 24 bit FLAC'), 'lossless FLAC must keep the bit depth');
check(context.formatSourceAudioSpec({ source_status: 'observed', source_rate: 96000, source_valid_bits: 24,
  source_encoding: 'FLAC', source_bitrate_kbps: 1411 }).includes('96.0 kHz / 24 bit · 1411 kbps FLAC'),
  'source metadata must show both bit depth and bitrate when both are present');
check(context.formatSourceAudioSpec({ source_status: 'observed', source_rate: 44100,
  source_valid_bits: 0, source_container_bits: 0, source_encoding: '整数 PCM' }) === '源规格：未取得',
  'PCM without a valid sample width must remain unavailable instead of showing 0 bit');
check(!observedFlac.includes('记录代次'), 'source specification must keep generation out of user-facing audio details');
check(context.formatSourceAudioSpec({
  source_status: 'pending', source_play_id: 'next'
}) === '源规格：查询中', 'pending source metadata must have a distinct UI state');
check(source.includes("formatTitle.textContent = '音频规格'"), 'settings audio specification title must stay short');
check(source.includes('max-width:calc(100vw - 16px)'), 'settings window must fit narrow viewports');
check(!source.includes("existing.className = 'nex-quality-card'"), 'audio specification must not create a playbar card');
check(source.includes("formatBlock.className = 'nex-format-block'"), 'audio specification must stay in the settings window');
check(!source.includes('    renderQualityBox(snapshot);'), 'audio specification must stay in the NexExclusive settings window instead of the native playbar');
check(source.includes('if (open) syncSourcePlayId();'), 'settings opening must re-report the current playId');
check(source.includes("if (module && module.getAttribute('data-open') === 'true') syncSourcePlayId();"), 'open settings must keep the host in sync with the playId');
check(source.includes('sourcePlayId !== sourceReportedPlayId') && source.includes('sourceQualityTierHint()'), 'a new playId or quality hint from playprogress must be reported during playback');
check(!/getPlaybackInfo\(\{\s*playId/.test(source), 'per-track getPlaybackInfo queries must not run any more');
check(source.indexOf('function syncSourcePlayId') < source.indexOf('player.getPlaybackInfo({})'), 'playId discovery must stay inside the settings sync path');
check(source.includes('function isPlaybackPaused'), 'playId discovery must only query the player while paused');
check(!source.includes("postSourceState({ source_status: 'reset'"), 'source bridge must not reset the host during active playback initialization');
check(source.includes('function sourceAppRequire') && source.includes('req.c'), 'source bridge must reuse the client AudioPlayer instance from the webpack module cache');
check(!source.includes('registry[id](module, module.exports, req)'), 'source bridge must never instantiate a second AudioPlayer (it freezes the native progress bar)');

console.log('PASS panel contract: grouped native-style rows and Hi-Fi state semantics');

/* The sink tags a value-exact container change as [repacked] and any opening
   fade as +fade; the panel must neither hide the fade nor call a repack a
   conversion. bits=0 and fade_ms=0 are the defaults. */
check(source.includes('repacked'), 'panel must recognise the [repacked] path tag');
check(source.includes('+fade'), 'panel must recognise a +fade path tag');
check(source.includes('开头淡入会改动'), 'panel must say that the opening fade changes samples');
check(/bits:\s*0,/.test(source) && /fade_ms:\s*0,/.test(source), 'panel defaults must be bits=0 and fade_ms=0');
