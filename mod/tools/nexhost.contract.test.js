/* Source contracts for the visible launch and injection hand-off. */
'use strict';

const fs = require('fs');
const path = require('path');
// nexhost.c includes its sections from host_*.inc; the contract is about the
// host as a whole, so read them together.
const source = [fs.readFileSync(path.join(__dirname, 'nexhost.c'), 'utf8')]
  .concat(fs.readdirSync(__dirname).filter(f => /^host_.*\.inc$/.test(f)).sort()
    .map(f => fs.readFileSync(path.join(__dirname, f), 'utf8')))
  .join('\n');
const audioHost = fs.readFileSync(path.join(__dirname, 'nexaudiohost.c'), 'utf8');
const audioLink = fs.readFileSync(path.join(__dirname, 'nexaudiolink.h'), 'utf8');

function check(condition, message) {
  if (!condition) throw new Error(message);
}

check(source.includes('reveal_client_window'), 'host must try to reveal an existing client window');
check(source.includes('enable_debug_privilege'), 'host must initialise the debug privilege before injection');
check(source.includes('"--show"'), 'host must expose an explicit show command');
check(source.includes('client_window_visible'), 'watchdog must distinguish a visible client from a background process');
check(source.includes('IsWow64Process2'), 'host must reject cross-bitness injection before creating a remote thread');
check(source.includes('Access-Control-Allow-Origin: orpheus://orpheus'), 'config service must restrict browser origin');
check(source.includes('http_header_value'), 'config service must parse request headers');
check(source.includes('http_allowed_host'), 'config service must restrict the loopback host');
check(source.includes('http_json_content_type'), 'config service must validate JSON content type');
check(source.includes('POST requires application/json'), 'config service must reject non-JSON POST requests');
check(source.includes('nexsink.dll'), 'host must resolve an absolute DLL path beside itself');
check(!source.includes('(now - g_last_ui_time > 5000)'), 'watchdog must not re-register CDP scripts on a timer');
check(!source.includes('Sleep(2000)'), 'watchdog must not block the tray loop while waiting for a client');
check(!source.includes('Sleep(5000);\n        }'), 'watchdog must not pause the tray loop after restarting a client');
check(source.includes('/api/restart'), 'host must expose a current-track restart endpoint');
check(source.includes('restart_current_track'), 'restart endpoint must use the current CDP page');
check(source.includes('touch_config_file'), 'restart must create a new config generation even without field edits');
check(!source.includes('if (!v[9]) v[10] = 0;'), 'host config must allow converted audiohost independently of strict-native mode');
check(source.includes('"button.click();"'), 'restart must toggle the current client stream before rebuilding it');
check(source.includes('setTimeout(function(){'), 'restart must restore the original play state after the stream rebuild');
check(source.includes('returnByValue\\":true'), 'CDP evaluate must expose the script result to the host');
check(source.includes('awaitPromise\\":true'), 'restart must wait for the delayed playback restore');
check(source.includes('exceptionDetails'), 'CDP evaluate must reject JavaScript exceptions');
check(source.includes('value\\":false'), 'CDP evaluate must reject a script that returns false');
check(source.includes('function visibleButton()'), 'restart must centralize visible play-button selection');
check(source.includes('var b=visibleButton();'), 'restart must reselect a visible play button when restoring playback');
check(source.includes('resolve(finalPlaying===wasPlaying)'), 'restart must report the restored play state');
check(!source.includes("querySelectorAll('.thumb input"), 'restart must not reach into the progress slider');
check(!source.includes("range.value='0'"), 'restart must not reset playback by writing the progress value');
check(!source.includes("dispatchEvent(new Event('input'"), 'restart must not synthesize a progress input event');
check(!source.includes("dispatchEvent(new Event('change'"), 'restart must not synthesize a progress change event');
check(!source.includes('setTimeout(function(){button.click();},90)'), 'restart must not rely on the old blind double click');
check(audioHost.includes('AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED'), 'audio host must retry aligned exclusive buffers');
check(audioHost.includes('period_for_frames'), 'audio host must derive the retry period from aligned frames');
check(audioHost.includes('hash_bytes(destination'), 'audio host must verify device bytes before releasing the render buffer');
check(audioHost.includes('nex_audio_ring_peek') && audioHost.includes('nex_audio_ring_commit_read'), 'audio host must consume only after device release succeeds');
check(audioHost.includes('AvSetMmThreadCharacteristicsW') && audioHost.includes('AVRT_PRIORITY_CRITICAL'), 'audio host must elevate the render thread');
{
  /* inside host_pump: the prefill path marks the first packet before Start
     (host_apply_transport's resume Start comes earlier in the file) */
  const pump = audioHost.slice(audioHost.indexOf('static HRESULT host_pump('));
  check(pump.indexOf('first_buffer_submitted = 1') >= 0 && pump.indexOf('first_buffer_submitted = 1') < pump.indexOf('api->start(rt)'), 'audio host must submit and mark the first packet before Start');
}
check(audioHost.includes('host_startup_reason(hr)'), 'host startup taxonomy must be centralized and testable');
check(audioHost.includes('hr == AUDCLNT_E_UNSUPPORTED_FORMAT'), 'only an actual format rejection may select unsupported_format');
check(audioHost.includes('padding_ticks') && audioHost.includes('eos_target = position + padding_ticks'), 'audio host EOS must convert device padding into clock ticks');
check(audioHost.includes('host_publish_clock') && audioHost.includes('device_clock_position'), 'audio host must publish a real device clock snapshot');
check(audioHost.includes('device_clock_qpc') && audioHost.includes('device_padding_frames'), 'audio host clock snapshot must include QPC and padding');
check(audioHost.includes('short final packet') && audioHost.includes('host_eos_requested(rt) && available64'), 'natural EOS must accept a short final packet');
check(source.includes('verification_json'), 'status endpoint must expose layered verification evidence');
check(source.includes('clientToEndpoint'), 'status endpoint must distinguish client-to-endpoint verification');
check(source.includes('requestedEngine') && source.includes('activeEngine'), 'status endpoint must expose requested and active engines');
check(source.includes('convertedFrames') && source.includes('sourceWaits') && source.includes('backpressure'), 'status endpoint must expose remote transport counters');
check(source.includes('currentGeneration') && source.includes('eosSubmittedGeneration') && source.includes('eosCompleteGeneration'), 'status endpoint must expose generation and EOS counters');
check(source.includes('json_get_string') && source.includes('source_update_from_json'), 'source endpoint must validate string metadata fields');
check(source.includes('strcmp(path, "/api/source")') && source.includes('source_status'), 'host must accept pending, observed and unavailable source metadata');
check(source.includes('source_play_id') && source.includes('source_audio_generation'), 'status endpoint must expose playId binding and host generation stamp');
check(source.includes('source_bits_from_bytes') && source.includes('source_format_allowed'), 'host must validate sample byte units and codec names');
check(source.includes('source_uppercase') && source.includes('source_format_allowed(format)'), 'host must normalize codec labels before publishing status');
check(source.includes('source_format_is_lossless') && source.includes('source_quality_tier_hint'), 'host must validate PCM sample width and accept the quality tier hint');
check(source.includes('cache_quality_hint') && source.includes('cache_rate_match'), 'cache selection must publish the evidence used to choose a candidate');
check(source.includes('!_stricmp(format, "pcm")'), 'host must treat PCM as a lossless source with explicit sample bytes');
check(source.includes('!_stricmp(status, "reset")'), 'host must clear source metadata when a page bridge is recreated');
check(source.includes('> 30000'), 'pending source metadata must outlive the bridge query retry budget');
check(source.includes('GetTickCount64'), 'source metadata TTL must use a monotonic clock');
check(source.includes('if (!line || !*line)'), 'missing verifier evidence must return null instead of zeroed measurements');
check(source.includes('if (strstr(line, "=== nexsink attached ==="))'), 'a new client process must clear stale negotiated and verifier state');
check(source.includes('Page.removeScriptToEvaluateOnNewDocument'), 'CDP injection must remove the previous document script');
check(source.includes('cdp_document_has_panel'), 'watchdog must detect a same-target document refresh before reinjecting');
check(source.includes('|| !document_has_panel'), 'same-target page refresh must trigger UI reinjection');
check(source.includes('install_payload_atomic'), 'installation must stage files before committing them');
check(source.includes('stop_client_processes'), 'install and uninstall must wait for the client to exit');
check(source.includes('verify_package_dir'), 'host must verify a package manifest');
check(source.includes('--verify'), 'host must expose package verification');
check(audioLink.includes('dropped_frames'), 'audio link must expose dropped-frame accounting');
check(audioLink.includes('underrun_events'), 'audio link must expose underrun accounting');
check(audioLink.includes('NEX_AUDIO_PROTOCOL_MAJOR        3u'), 'audio link must use protocol v3');
check(audioLink.includes('NEX_AUDIO_BUILD_ID'), 'audio link must require an exact build identity');
check(audioLink.includes('input_rate') && audioLink.includes('output_rate'), 'audio link must carry both input and output formats');
check(audioLink.includes('eos_submitted_generation') && audioLink.includes('eos_complete_generation'), 'audio link must carry generation-scoped EOS');
check(audioLink.includes('device_clock_position') && audioLink.includes('device_clock_frequency'), 'audio link must carry Host device clock fields');
check(!audioLink.includes('unsigned char data[NEX_AUDIO_RING_FRAMES'), 'audio control must not embed a variable PCM ring');
check(audioHost.includes('--ready'), 'audio host must signal readiness through a named event');

console.log('PASS host contract: visible launch and injection hand-off');

/* CDP is the panel's transport only. A client without port 9333 keeps its
   audio module; the host reports the panel as unavailable and never restarts
   the client to get a panel. */
{
  const start = source.indexOf('static void watchdog_tick(void)');
  const end = source.indexOf('static void tray_add(HWND hwnd)');
  check(start > 0 && end > start, 'watchdog_tick must be locatable for the CDP contract');
  const watchdog = source.slice(start, end);
  check(!source.includes('relaunch_client_for_cdp'), 'host must not restart the client to obtain a CDP port');
  check(!source.includes('g_cdp_relaunch'), 'host must not keep CDP relaunch state');
  check(!watchdog.includes('stop_client_processes'), 'watchdog must never stop a running client');
  check(!watchdog.includes('g_injected_pid = 0;\n            g_ui_injected = 0;\n            g_last_page[0] = 0;\n            if'),
    'a missing CDP port must not discard the audio injection state');
  check(watchdog.includes('面板不可用'), 'a missing CDP port must be reported as panel unavailable');
  check(watchdog.includes('音频不受影响'), 'the panel-unavailable report must state that audio is unaffected');
}

/* Audio and panel are separate chains. nexsink.dll goes only into the main
   process from the watchdog; nexui.dll goes into its children from ui_thread;
   CDP runs only when the UI route says the renderer path failed. */
{
  const start = source.indexOf('static void watchdog_tick(void)');
  const end = source.indexOf('static void tray_add(HWND hwnd)');
  const watchdog = source.slice(start, end);
  check(/join_into\(kUiDll,\s*sizeof\(kUiDll\),\s*"nexui\.dll"\)/.test(source), 'host must resolve nexui.dll beside itself');
  check(source.includes('CreateThread(NULL, 0, ui_thread'), 'the UI chain must run on its own thread');
  check(/g_ui_route\s*[!=]=\s*NEX_UI_ROUTE_CDP/.test(watchdog), 'CDP injection must be gated by the UI route');
  check(!watchdog.includes('kUiDll'), 'the watchdog must not inject the UI module');
  check(source.split('inject_dll(pid, kSinkDll)').length === 2, 'nexsink.dll must be injected from one place only');
  const uiStart = source.indexOf('static DWORD WINAPI ui_thread');
  const uiBody = source.slice(uiStart, uiStart + source.slice(uiStart).search(/\r?\n}\r?\n/));
  check(uiStart > 0 && uiBody.includes('ui_round_inject'), 'ui_thread must run the injection round');
  check(/static int ui_op_inject\([^)]*\) \{[^}]*inject_dll\(pid, kUiDll\)/.test(source), 'the UI chain must inject nexui.dll');
  check(!/static int ui_op_inject\([^)]*\) \{[^}]*kSinkDll/.test(source), 'the UI chain must not inject the audio module');
  check(!uiBody.includes('kSinkDll'), 'the UI thread must not touch the audio module');
}

/* nexui.dll ships, installs and uninstalls with the rest of the package. */
{
  const body = (name) => {
    const at = source.indexOf(name);
    return at < 0 ? '' : source.slice(at, at + source.slice(at).search(/\r?\n}\r?\n/));
  };
  check(body('static void remove_install_tree(').includes('L"nexui.dll"'), 'remove_install_tree must delete nexui.dll');
  check(body('static int install_payload_atomic(').includes('L"nexui.dll"'), 'install must copy nexui.dll');
  const uninstall = body('static int do_uninstall(');
  check(uninstall.includes('L"nexui.dll"') && uninstall.includes('L"nexui.log"') && uninstall.includes('L"nexui.prev.log"'),
    'uninstall must remove nexui.dll and its logs');
  const release = fs.readFileSync(path.join(__dirname, '..', '..', 'release.sh'), 'utf8');
  check(/^FILES=\(.*\bnexui\.dll\b.*\)$/m.test(release), 'release package must include nexui.dll');
}

/* The host carries its own icon (tray, Explorer, shortcuts) instead of the
   stock application icon, and falls back to it if the resource is missing. */
{
  const rc = fs.readFileSync(path.join(__dirname, 'nexhost.rc.in'), 'utf8');
  const build = fs.readFileSync(path.join(__dirname, '..', 'build.sh'), 'utf8');
  check(/ICON\s+"nexhost\.ico"/.test(rc), 'resource script must embed nexhost.ico');
  check(fs.existsSync(path.join(__dirname, 'nexhost.ico')), 'nexhost.ico must be in the repository');
  check(build.includes('nexhost.ico'), 'build.sh must stage nexhost.ico next to the resource script');
  check(source.includes('MAKEINTRESOURCEW(IDI_NEXHOST)'), 'tray must load the embedded icon');
  check(source.includes('MAKEINTRESOURCEW(32512)'), 'tray must fall back to the stock icon');
}
