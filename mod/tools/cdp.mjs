// Minimal CDP client for the client's CEF page.
// Usage: node cdp.mjs <js-file>   evaluate a file, print JSON result
//        node cdp.mjs --list      list targets
const HOST = process.env.NEX_CDP || 'http://127.0.0.1:9333';

async function targets() {
  const r = await fetch(`${HOST}/json/list`);
  return r.json();
}

async function connect() {
  const list = await targets();
  const page = list.find(t => t.type === 'page');
  if (!page) throw new Error('no page target');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((res, rej) => {
    ws.addEventListener('open', res, { once: true });
    ws.addEventListener('error', rej, { once: true });
  });
  let id = 0;
  const pending = new Map();
  ws.addEventListener('message', ev => {
    const msg = JSON.parse(ev.data);
    if (msg.id && pending.has(msg.id)) {
      const { res, rej } = pending.get(msg.id);
      pending.delete(msg.id);
      msg.error ? rej(new Error(JSON.stringify(msg.error))) : res(msg.result);
    }
  });
  const send = (method, params = {}) => new Promise((res, rej) => {
    const mid = ++id;
    pending.set(mid, { res, rej });
    ws.send(JSON.stringify({ id: mid, method, params }));
  });
  return { ws, send, page };
}

const arg = process.argv[2];
if (!arg) { console.error('usage: node cdp.mjs <js-file> | --list | --shot <png>'); process.exit(2); }

if (arg === '--list') {
  for (const t of await targets()) console.log(t.type, '|', t.title, '|', t.url);
  process.exit(0);
}

if (arg === '--reload') {
  const { ws, send } = await connect();
  await send('Page.enable');
  await send('Page.reload', { ignoreCache: false });
  console.log('reloaded');
  ws.close();
  process.exit(0);
}

if (arg === '--clip') {
  const fs = await import('node:fs');
  const [x, y, w, h, out, scale] = process.argv.slice(3);
  const { ws, send } = await connect();
  await send('Page.enable');
  const shot = await send('Page.captureScreenshot', {
    format: 'png',
    clip: { x: +x, y: +y, width: +w, height: +h, scale: scale ? +scale : 3 },
  });
  fs.writeFileSync(out, Buffer.from(shot.data, 'base64'));
  console.log('saved', out);
  ws.close();
  process.exit(0);
}

if (arg === '--shot') {
  const fs = await import('node:fs');
  const { ws, send } = await connect();
  await send('Page.enable');
  const shot = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: false });
  fs.writeFileSync(process.argv[3], Buffer.from(shot.data, 'base64'));
  console.log('saved', process.argv[3]);
  ws.close();
  process.exit(0);
}

const fs = await import('node:fs');
const expr = fs.readFileSync(arg, 'utf8');
const { ws, send } = await connect();
await send('Runtime.enable');
const out = await send('Runtime.evaluate', {
  expression: expr,
  returnByValue: true,
  awaitPromise: true,
  userGesture: true,
});
if (out.exceptionDetails) {
  console.error('EXCEPTION:', JSON.stringify(out.exceptionDetails, null, 2));
  ws.close();
  process.exit(1);
}
const v = out.result?.value;
console.log(typeof v === 'string' ? v : JSON.stringify(v, null, 2));
ws.close();
process.exit(0);
