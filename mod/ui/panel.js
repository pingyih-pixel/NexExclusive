// nexsink 独占输出面板 - 注入到客户端设置页「播放」区域。
//
// 设计约束：
//   1. 只挂一次，路由来回切换不重复插入；
//   2. 视觉复用客户端自己的排版：14px 雅黑、rgba(40,50,72,0.9)、行距 30px、
//      控件高 30px 圆角 24px，强调色取客户端复选框的 #FF3A3A；
//   3. 不接管原生控件，不改动原有 DOM，只在「输出设备」之后插入一段。
//
// 配置来源：本地 nexctl 的 /api/cfg，失败时退回 localStorage 并显示未连接。
(function () {
  'use strict';

  if (window.__nexPanelLoaded) return;
  window.__nexPanelLoaded = true;

  var API = 'http://127.0.0.1:9334/api/cfg';
  var STORE = 'nexsink.cfg.mirror';
  var PANEL_ID = 'nexsink-panel';

  var DEFAULTS = {
    enable: 1, bits: 24, rate: 0, buffer: 0,
    compat: 1, dither: 2, fade_ms: 5, shared_fallback: 0
  };

  var state = Object.assign({}, DEFAULTS);
  var connected = false;

  var FIELDS = [
    { key: 'enable', label: '启用独占', type: 'switch' },
    { key: 'bits', label: '输出位深', type: 'select',
      options: [[16, '16 位'], [24, '24 位']] },
    { key: 'rate', label: '采样率', type: 'select',
      options: [[0, '跟随音源'], [44100, '44.1 kHz'], [48000, '48 kHz'],
                [88200, '88.2 kHz'], [96000, '96 kHz']] },
    { key: 'buffer', label: '缓冲帧数', type: 'select',
      options: [[0, '自动'], [512, '512'], [1024, '1024'],
                [2048, '2048'], [4096, '4096']] },
    { key: 'compat', label: '兼容转换', type: 'switch' },
    { key: 'dither', label: '抖动', type: 'select',
      options: [[0, '关闭'], [1, '开启'], [2, '自动']] },
    { key: 'fade_ms', label: '开头淡入', type: 'select',
      options: [[0, '关闭'], [3, '3 ms'], [5, '5 ms'], [10, '10 ms'], [20, '20 ms']] },
    { key: 'strict', label: '严格模式', type: 'switch' }
  ];

  function styleOnce() {
    if (document.getElementById('nex-panel-style')) return;
    var css = [
      '#nexsink-panel{max-width:460px;margin:0 0 30px;}',
      '#nexsink-panel .nex-head{display:flex;align-items:center;gap:8px;margin:0 0 14px;}',
      '#nexsink-panel .nex-head h4{margin:0;font-size:14px;font-weight:700;',
      'line-height:18px;color:rgba(40,50,72,0.9);}',
      '#nexsink-panel .nex-badge{font-size:12px;line-height:18px;padding:0 8px;',
      'border-radius:9px;color:rgba(40,50,72,0.5);background:rgba(40,50,72,0.06);}',
      '#nexsink-panel .nex-badge.ok{color:#0f8a4a;background:rgba(15,138,74,0.1);}',
      '#nexsink-panel .nex-row{display:flex;flex-direction:column;align-items:flex-start;',
      'min-height:30px;margin:0 0 12px;}',
      '#nexsink-panel .nex-label{font-size:14px;line-height:20px;margin:0 0 4px;',
      'color:rgba(40,50,72,0.9);}',
      '#nexsink-panel .nex-switch{width:34px;height:20px;border-radius:10px;border:0;',
      'padding:0;cursor:pointer;background:rgba(40,50,72,0.18);position:relative;',
      'transition:background .15s;flex:none;}',
      '#nexsink-panel .nex-switch[aria-checked="true"]{background:#FF3A3A;}',
      '#nexsink-panel .nex-switch::after{content:"";position:absolute;top:2px;left:2px;',
      'width:16px;height:16px;border-radius:50%;background:#fff;transition:left .15s;}',
      '#nexsink-panel .nex-switch[aria-checked="true"]::after{left:16px;}',
      '#nexsink-panel .nex-select{width:180px;height:30px;border-radius:24px;',
      'border:1px solid rgba(40,50,72,0.06);background:rgba(40,50,72,0.03);',
      'font-size:14px;color:rgba(40,50,72,0.9);padding:0 12px;cursor:pointer;',
      'font-family:inherit;outline:none;flex:none;}',
      '#nexsink-panel .nex-select:hover{background:rgba(40,50,72,0.06);}',
      '#nexsink-panel .nex-status{font-size:12px;line-height:18px;',
      'color:rgba(40,50,72,0.45);margin:2px 0 0;}',
      '#nexsink-panel .nex-vol{font-size:12px;line-height:18px;',
      'color:rgba(40,50,72,0.45);margin:4px 0 0;white-space:pre-line;}',
      '#nexsink-panel .nex-vol.warn{color:#b26a00;}'
    ].join('');
    var s = document.createElement('style');
    s.id = 'nex-panel-style';
    s.textContent = css;
    document.head.appendChild(s);
  }

  function readValue(field) {
    if (field.key === 'strict') return state.shared_fallback ? 0 : 1;
    return state[field.key];
  }

  function writeValue(field, raw) {
    var v = typeof raw === 'string' ? parseInt(raw, 10) : raw;
    if (field.key === 'strict') state.shared_fallback = v ? 0 : 1;
    else state[field.key] = v;
    save();
  }

  function makeRow(field) {
    var row = document.createElement('div');
    row.className = 'nex-row';

    var left = document.createElement('div');
    var label = document.createElement('div');
    label.className = 'nex-label';
    label.textContent = field.label;
    left.appendChild(label);
    if (field.hint) {
      var hint = document.createElement('span');
      hint.className = 'nex-hint';
      hint.textContent = field.hint;
      left.appendChild(hint);
    }
    row.appendChild(left);

    if (field.type === 'switch') {
      var sw = document.createElement('button');
      sw.className = 'nex-switch';
      sw.setAttribute('role', 'switch');
      sw.setAttribute('aria-label', field.label);
      sw.setAttribute('aria-checked', readValue(field) ? 'true' : 'false');
      sw.addEventListener('click', function () {
        var next = sw.getAttribute('aria-checked') !== 'true';
        sw.setAttribute('aria-checked', next ? 'true' : 'false');
        writeValue(field, next ? 1 : 0);
      });
      row.appendChild(sw);
    } else {
      var sel = document.createElement('select');
      sel.className = 'nex-select';
      sel.setAttribute('aria-label', field.label);
      field.options.forEach(function (opt) {
        var o = document.createElement('option');
        o.value = String(opt[0]);
        o.textContent = opt[1];
        sel.appendChild(o);
      });
      sel.value = String(readValue(field));
      sel.addEventListener('change', function () { writeValue(field, sel.value); });
      row.appendChild(sel);
    }
    return row;
  }

  function build() {
    styleOnce();
    var panel = document.createElement('section');
    panel.id = PANEL_ID;

    var head = document.createElement('div');
    head.className = 'nex-head';
    var h4 = document.createElement('h4');
    h4.textContent = '独占输出';
    var badge = document.createElement('span');
    badge.className = 'nex-badge';
    badge.id = 'nex-badge';
    badge.textContent = connected ? '已连接' : '未连接';
    if (connected) badge.classList.add('ok');
    head.appendChild(h4);
    head.appendChild(badge);
    panel.appendChild(head);

    FIELDS.forEach(function (f) { panel.appendChild(makeRow(f)); });

    var status = document.createElement('div');
    status.className = 'nex-status';
    status.id = 'nex-status';
    status.textContent = '等待音频模块上报状态';
    panel.appendChild(status);

    /* 端点音量能力。系统音量条在独占下是否还算数，取决于端点有没有硬件
       音量节点；没有就必须说一声，否则用户按音量键看到 OSD 在动、声音不
       变，只会以为模块坏了。查询失败时整行隐藏，不猜。 */
    var vol = document.createElement('div');
    vol.className = 'nex-vol';
    vol.id = 'nex-vol';
    vol.style.display = 'none';
    panel.appendChild(vol);

    return panel;
  }

  function mount() {
    var play = document.querySelector('#play .play');
    if (!play) return false;
    var existing = document.getElementById(PANEL_ID);
    if (existing) {
      if (existing.isConnected) return true;
      existing.remove();
    }
    var anchor = null;
    var h4s = play.querySelectorAll('h4');
    for (var i = 0; i < h4s.length; i++) {
      if (h4s[i].textContent.trim() === '输出设备') { anchor = h4s[i].closest('section'); break; }
    }
    var panel = build();
    if (anchor && anchor.parentElement === play) play.insertBefore(panel, anchor.nextSibling);
    else play.insertBefore(panel, play.firstChild);
    return true;
  }

  function save() {
    try { localStorage.setItem(STORE, JSON.stringify(state)); } catch (e) {}
    if (!connected) return;
    fetch(API, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(state)
    }).catch(function () { setBadge(false); });
  }

  function setBadge(ok) {
    connected = ok;
    var badge = document.getElementById('nex-badge');
    if (!badge) return;
    badge.textContent = ok ? '已连接' : '未连接';
    badge.classList.toggle('ok', ok);
  }

  function load() {
    try {
      var cached = localStorage.getItem(STORE);
      if (cached) state = Object.assign({}, DEFAULTS, JSON.parse(cached));
    } catch (e) {}
    fetch(API, { method: 'GET' })
      .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
      .then(function (cfg) {
        state = Object.assign({}, DEFAULTS, cfg);
        setBadge(true);
        var play = document.querySelector('#play .play');
        var panel = document.getElementById(PANEL_ID);
        if (play && panel) play.replaceChild(build(), panel);
        refreshStatus();
      })
      .catch(function () { setBadge(false); });
  }

  function formatRate(hz) {
    if (!hz) return '';
    return (hz % 1000 === 0 ? (hz / 1000).toFixed(1) : String(hz / 1000)) + ' kHz';
  }

  function refreshStatus() {
    var el = document.getElementById('nex-status');
    if (!el || !connected) return;
    fetch(API.replace('/cfg', '/status'), { method: 'GET' })
      .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
      .then(function (st) {
        var line = '暂无接入记录';
        if (st.lastEngaged) {
          var m = /out=rate:(\d+)\/cont:(\d+)/.exec(st.lastEngaged);
          var kind = /\[converted\]/.test(st.lastEngaged) ? '重采样'
                   : /\[depth only\]/.test(st.lastEngaged) ? '仅位深转换'
                   : /\[native\]/.test(st.lastEngaged) ? '原样输出' : '';
          if (m) line = '上次输出 ' + formatRate(Number(m[1])) + ' / ' + m[2] + ' bit' + (kind ? ' · ' + kind : '');
          else line = st.lastEngaged.slice(0, 60);
        }
        if (st.fault) line += '　最近错误：' + st.fault.slice(0, 40);
        el.textContent = line;

        var vol = document.getElementById('nex-vol');
        if (vol) {
          /* hwVolume 为 null 表示能力未知（端点查不到，或 sink 还没接管过），
             这时整行隐藏——不猜。 */
          if (!st.endpoint || st.hwVolume === null || st.hwVolume === undefined) {
            vol.style.display = 'none';
          } else if (st.hwVolume === true) {
            vol.className = 'nex-vol';
            vol.textContent = '端点「' + st.endpoint +
              '」· 硬件音量可用，独占时系统音量条仍有效';
            vol.style.display = '';
          } else {
            /* 实测（NEX_DUMP 抓设备侧帧 + 播放器音量归零）：客户端自己的音量
               也不作用在样本上，它走的是会话音量，独占下同样被绕过。所以这
               个端点在独占时根本没有可用的音量控制，只能靠设备旋钮。 */
            vol.className = 'nex-vol warn';
            vol.textContent = '端点「' + st.endpoint + '」· 无硬件音量\n' +
              '独占时系统音量与播放器音量均不生效，输出为音源原电平\n' +
              '如需调音量，请换用带硬件音量的输出或关闭独占';
            vol.style.display = '';
          }
        }
      })
      .catch(function () {});
  }

  function watch() {
    mount();
    var pending = null;
    var obs = new MutationObserver(function () {
      if (pending) return;
      pending = setTimeout(function () {
        pending = null;
        if (!document.getElementById(PANEL_ID)) mount();
      }, 250);
    });
    obs.observe(document.body, { childList: true, subtree: true });
    setInterval(refreshStatus, 5000);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', watch);
  } else {
    watch();
  }
  load();
})();
