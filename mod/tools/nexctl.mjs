// nexctl - 本地配置服务，供客户端内面板读写 hook.cfg。
//
// 为什么单独起一个进程：面板跑在 CEF 里，没有写文件的能力；
// 客户端自带的 window.channel 也没有开放任意文件写入。用一个本机
// HTTP 服务做中转，比往客户端里注入第二个 DLL 简单，也不需要改资源包。
//
// 用法：
//   node nexctl.mjs serve            启动服务（默认 127.0.0.1:9334）
//   node nexctl.mjs inject <js>      把脚本注入当前页面
//   node nexctl.mjs watch <js>       注入并在页面刷新后重新注入
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CFG = process.env.NEX_CFG || path.join(HERE, '..', 'hook.cfg');
const UI = process.env.NEX_UI || path.join(HERE, '..', 'ui.json');
const LOG = process.env.NEX_LOG || path.join(HERE, '..', 'sink.log');
const PORT = Number(process.env.NEX_PORT || 9334);
const CDP = process.env.NEX_CDP || 'http://127.0.0.1:9333';

const ORDER = ['enable', 'bits', 'rate', 'buffer', 'compat', 'dither', 'fade_ms', 'shared_fallback'];
const NUMERIC = new Set(ORDER);
const UI_KEYS = ['banner', 'charts', 'daily', 'podcast', 'choice', 'skin', 'pet'];

function readUi() {
  const out = {};
  for (const k of UI_KEYS) out[k] = 0;
  try {
    const raw = JSON.parse(fs.readFileSync(UI, 'utf8'));
    for (const k of UI_KEYS) if (k in raw) out[k] = raw[k] ? 1 : 0;
  } catch {}
  return out;
}

function writeUi(patch) {
  const cur = readUi();
  for (const k of UI_KEYS) if (k in patch) cur[k] = patch[k] ? 1 : 0;
  fs.writeFileSync(UI, JSON.stringify(cur, null, 2) + '\n', 'utf8');
  return cur;
}

function readCfg() {
  const out = {};
  let text = '';
  try { text = fs.readFileSync(CFG, 'utf8'); } catch { return out; }
  for (const line of text.split(/\r?\n/)) {
    const m = /^\s*([a-z_]+)\s*=\s*(-?\d+)/i.exec(line);
    if (m) out[m[1]] = Number(m[2]);
  }
  return out;
}

function writeCfg(patch) {
  let text = '';
  try { text = fs.readFileSync(CFG, 'utf8'); } catch { text = ''; }
  const seen = new Set();
  const lines = text.split(/\r?\n/).map(line => {
    const m = /^\s*([a-z_]+)\s*=/i.exec(line);
    if (!m) return line;
    const key = m[1];
    if (!(key in patch) || !NUMERIC.has(key)) return line;
    seen.add(key);
    return `${key}=${patch[key]}`;
  });
  for (const key of ORDER) {
    if (key in patch && !seen.has(key)) lines.push(`${key}=${patch[key]}`);
  }
  const body = lines.filter(l => l.trim() !== '').join('\n') + '\n';
  fs.writeFileSync(CFG, body, 'utf8');
  return readCfg();
}

function readStatus() {
  const out = { loaded: false, lastEngaged: null, lastLine: null, fault: null };
  let text = '';
  try { text = fs.readFileSync(LOG, 'utf8'); } catch { return out; }
  const lines = text.trimEnd().split(/\r?\n/);
  out.lastLine = lines[lines.length - 1] || null;
  for (let i = lines.length - 1; i >= 0 && i > lines.length - 400; i--) {
    const l = lines[i];
    if (!out.lastEngaged && l.includes('sink ENGAGED')) out.lastEngaged = l.slice(l.indexOf(']') + 2);
    if (!out.fault && /sink failed|sink_fault|drain join|CreateThread failed/.test(l)) out.fault = l.slice(l.indexOf(']') + 2);
    if (out.lastEngaged && out.fault) break;
  }
  out.loaded = out.lastEngaged !== null;
  return out;
}

const CORS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET,POST,OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type',
  'Cache-Control': 'no-store',
};

function json(res, code, body) {
  const data = JSON.stringify(body);
  res.writeHead(code, { ...CORS, 'Content-Type': 'application/json; charset=utf-8' });
  res.end(data);
}

function serve() {
  const server = http.createServer((req, res) => {
    if (req.method === 'OPTIONS') { res.writeHead(204, CORS); res.end(); return; }
    const url = new URL(req.url, `http://127.0.0.1:${PORT}`);

    if (url.pathname === '/api/cfg' && req.method === 'GET') {
      return json(res, 200, { ...readCfg(), _file: CFG });
    }

    if (url.pathname === '/api/status' && req.method === 'GET') {
      return json(res, 200, readStatus());
    }

    if (url.pathname === '/api/ui' && req.method === 'GET') {
      return json(res, 200, readUi());
    }

    if (url.pathname === '/api/ui' && req.method === 'POST') {
      let body = '';
      req.on('data', c => { body += c; if (body.length > 4096) req.destroy(); });
      req.on('end', () => {
        let patch;
        try { patch = JSON.parse(body || '{}'); } catch { return json(res, 400, { error: 'bad json' }); }
        try { json(res, 200, writeUi(patch)); }
        catch (e) { json(res, 500, { error: String(e.message || e) }); }
      });
      return;
    }

    if (url.pathname === '/api/cfg' && req.method === 'POST') {
      let body = '';
      req.on('data', c => { body += c; if (body.length > 4096) req.destroy(); });
      req.on('end', () => {
        let patch;
        try { patch = JSON.parse(body || '{}'); } catch { return json(res, 400, { error: 'bad json' }); }
        const clean = {};
        for (const key of ORDER) {
          if (key in patch) {
            const v = Number(patch[key]);
            if (!Number.isFinite(v)) return json(res, 400, { error: `bad value for ${key}` });
            clean[key] = Math.trunc(v);
          }
        }
        try { json(res, 200, writeCfg(clean)); }
        catch (e) { json(res, 500, { error: String(e.message || e) }); }
      });
      return;
    }

    json(res, 404, { error: 'not found' });
  });
  server.listen(PORT, '127.0.0.1', () => {
    console.log(`nexctl listening on 127.0.0.1:${PORT}`);
    console.log(`cfg: ${CFG}`);
    console.log(`log: ${LOG}`);
  });
}

async function pageTarget() {
  const r = await fetch(`${CDP}/json/list`);
  const list = await r.json();
  return list.find(t => t.type === 'page');
}

async function inject(files, watchMode) {
  const src = files.map(f => fs.readFileSync(f, 'utf8')).join('\n;\n');
  let ws = null;
  let id = 0;
  const pending = new Map();

  async function connect() {
    const page = await pageTarget();
    if (!page) throw new Error('no page target; is the client running with --remote-debugging-port?');
    ws = new WebSocket(page.webSocketDebuggerUrl);
    await new Promise((res, rej) => {
      ws.addEventListener('open', res, { once: true });
      ws.addEventListener('error', rej, { once: true });
    });
    ws.addEventListener('message', ev => {
      const msg = JSON.parse(ev.data);
      if (msg.id && pending.has(msg.id)) {
        const { res, rej } = pending.get(msg.id);
        pending.delete(msg.id);
        msg.error ? rej(new Error(JSON.stringify(msg.error))) : res(msg.result);
      }
      if (msg.method === 'Page.loadEventFired' && watchMode) {
        setTimeout(() => send('Runtime.evaluate', { expression: src, awaitPromise: true }), 600);
      }
    });
    await send('Page.enable');
    await send('Runtime.enable');
  }

  function send(method, params = {}) {
    return new Promise((res, rej) => {
      const mid = ++id;
      pending.set(mid, { res, rej });
      ws.send(JSON.stringify({ id: mid, method, params }));
    });
  }

  await connect();
  const out = await send('Runtime.evaluate', { expression: src, awaitPromise: true, returnByValue: true });
  if (out.exceptionDetails) console.error('inject failed:', JSON.stringify(out.exceptionDetails));
  else console.log('injected into', (await pageTarget()).url);
  if (!watchMode) { ws.close(); return; }
  console.log('watching for reloads; ctrl-c to stop');
  await new Promise(() => {});
}

const [cmd, ...rest] = process.argv.slice(2);
if (cmd === 'serve') serve();
else if (cmd === 'inject' && rest.length) inject(rest, false).catch(e => { console.error(e.message); process.exit(1); });
else if (cmd === 'watch' && rest.length) inject(rest, true).catch(e => { console.error(e.message); process.exit(1); });
else {
  console.error('usage: node nexctl.mjs serve | inject <file...> | watch <file...>');
  process.exit(2);
}
