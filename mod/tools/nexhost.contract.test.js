/* Source contracts for the visible launch and injection hand-off. */
'use strict';

const fs = require('fs');
const path = require('path');
const source = fs.readFileSync(path.join(__dirname, 'nexhost.c'), 'utf8');
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
check(audioHost.includes('AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED'), 'audio host must retry aligned exclusive buffers');
check(audioHost.includes('PeriodForFrames'), 'audio host must derive the retry period from aligned frames');
check(audioHost.includes('hash_bytes(dst, total'), 'audio host must verify device bytes before releasing the render buffer');
check(source.includes('verification_json'), 'status endpoint must expose layered verification evidence');
check(source.includes('clientToEndpoint'), 'status endpoint must distinguish client-to-endpoint verification');
check(source.includes('Page.removeScriptToEvaluateOnNewDocument'), 'CDP injection must remove the previous document script');
check(source.includes('install_payload_atomic'), 'installation must stage files before committing them');
check(source.includes('stop_client_processes'), 'install and uninstall must wait for the client to exit');
check(source.includes('verify_package_dir'), 'host must verify a package manifest');
check(source.includes('--verify'), 'host must expose package verification');
check(audioLink.includes('dropped_frames'), 'audio link must expose dropped-frame accounting');
check(audioLink.includes('underrun_events'), 'audio link must expose underrun accounting');
check(audioHost.includes('--ready'), 'audio host must signal readiness through a named event');

console.log('PASS host contract: visible launch and injection hand-off');
