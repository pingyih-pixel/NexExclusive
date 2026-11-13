/* Source contracts for the visible launch and injection hand-off. */
'use strict';

const fs = require('fs');
const path = require('path');
const source = fs.readFileSync(path.join(__dirname, 'nexhost.c'), 'utf8');
const audioHost = fs.readFileSync(path.join(__dirname, 'nexaudiohost.c'), 'utf8');

function check(condition, message) {
  if (!condition) throw new Error(message);
}

check(source.includes('reveal_client_window'), 'host must try to reveal an existing client window');
check(source.includes('enable_debug_privilege'), 'host must initialise the debug privilege before injection');
check(source.includes('"--show"'), 'host must expose an explicit show command');
check(source.includes('client_window_visible'), 'watchdog must distinguish a visible client from a background process');
check(audioHost.includes('AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED'), 'audio host must retry aligned exclusive buffers');
check(audioHost.includes('PeriodForFrames'), 'audio host must derive the retry period from aligned frames');
check(audioHost.includes('hash_bytes(dst, total'), 'audio host must verify device bytes before releasing the render buffer');
check(source.includes('verification_json'), 'status endpoint must expose layered verification evidence');
check(source.includes('clientToEndpoint'), 'status endpoint must distinguish client-to-endpoint verification');

console.log('PASS host contract: visible launch and injection hand-off');
