// NexExclusive settings injected into the client's Playback or Settings page.
// The page owns navigation and typography; this script only adds native-style
// sections and talks to the local host API.
(function () {
  'use strict';

  var PANEL_VERSION = 'settings-source-v9';
  if (window.__nexPanelLoaded === PANEL_VERSION) return;
  window.__nexPanelLoaded = PANEL_VERSION;

  var API = 'http://127.0.0.1:9334/api/cfg';
  var SOURCE_API = 'http://127.0.0.1:9334/api/source';
  var RESTART_API = 'http://127.0.0.1:9334/api/restart';
  var ENDPOINTS_API = 'http://127.0.0.1:9334/api/endpoints';
  var ENDPOINT_API = 'http://127.0.0.1:9334/api/endpoint';
  var PANEL_ID = 'nexsink-panel';
  var SETTINGS_PANEL_ID = 'nex-settings-panel';
  var SETTINGS_LAUNCHER_ID = 'nex-settings-launcher';
  var DEFAULTS = {
    enable: 1, bits: 0, rate: 0, buffer: 0,
    compat: 1, dither: 2, fade_ms: 0, shared_fallback: 0,
    src_prefer: 0, hifi: 0, audiohost: 0
  };

  /* Replace DOM and styles from an older injected script version. */
  ['nex-panel-style', 'nex-quality-override-style', 'nex-module-frame-override-style', PANEL_ID, SETTINGS_PANEL_ID, 'nex-settings-module', 'nex-settings-entry', SETTINGS_LAUNCHER_ID, 'nex-quality-card'].forEach(function (id) {
    var stale = document.getElementById(id);
    if (stale) stale.remove();
  });

  var SECTIONS = [
    {
      key: 'mode',
      title: '输出模式',
      fields: [
        {
          key: 'enable', label: '启用独占输出', type: 'switch',
          description: '让网易云绕过系统混音器，直接连接当前播放设备。'
        },
        {
          key: 'hifi', label: '输出策略', type: 'select',
          options: [[0, '原率优先（推荐）'], [1, '严格原样']],
          description: '先尝试源格式；设备不支持时，原率优先会继续使用独占转换。严格原样只接受源格式。'
        },
        {
          key: 'endpoint', label: '独占输出设备', type: 'endpoint',
          description: '默认跟随网易云自己的输出设备。选定的设备拔出或不可用时，自动回到网易云的设备。下一首生效。'
        },
        {
          key: 'audiohost', label: '独立音频宿主', type: 'switch',
          description: '把独占端点和实时提交移到独立进程；原率优先时，先在 DLL 内完成转换，再发送最终 PCM。'
        }
      ]
    },
    {
      key: 'format',
      title: '输出规格',
      fields: [
        {
          key: 'rate', label: '采样率', type: 'select',
          options: [[0, '跟随音源'], [44100, '44.1 kHz'], [48000, '48 kHz'],
            [88200, '88.2 kHz'], [96000, '96 kHz']],
          description: '优先使用音源采样率，设备不支持时再转换。'
        },
        {
          key: 'buffer', label: '缓冲帧数', type: 'select',
          options: [[0, '自动'], [512, '512'], [1024, '1024'],
            [2048, '2048'], [4096, '4096']],
          description: '爆音或欠载时可以增加缓冲。'
        }
      ]
    },
    {
      key: 'processing',
      title: '兼容处理',
      fields: [
        {
          key: 'compat', label: '允许兼容转换', type: 'switch',
          description: '设备不接受原规格时允许重采样或位深转换。'
        },
        {
          key: 'src_prefer', label: '重采样策略', type: 'select',
          options: [[0, '带宽优先'], [1, '整数比优先']],
          description: '带宽优先保留高频，整数比优先减少转换复杂度。'
        },
        {
          key: 'dither', label: '抖动', type: 'select',
          options: [[0, '关闭'], [1, '开启'], [2, '自动']],
          description: '只在整数输出降位深时有意义。'
        },
        {
          key: 'fade_ms', label: '开头淡入', type: 'select',
          options: [[0, '关闭'], [3, '3 ms'], [5, '5 ms'],
            [10, '10 ms'], [20, '20 ms']],
          description: '淡入可以减少起播爆音，但会改变开头样本。'
        }
      ]
    }
  ];

  var state = Object.assign({}, DEFAULTS);
  var connected = false;
  var pendingConfig = null;
  var saveInFlight = false;
  var controls = {};
  var sourcePlayId = '';
  var sourceBridge = null;
  var sourceBridgePromise = null;
  var sourceReportedPlayId = '';
  var sourceReportedQualityTierHint = 0;

  function allFields() {
    var result = [];
    SECTIONS.forEach(function (section) {
      section.fields.forEach(function (field) { result.push(field); });
    });
    return result;
  }

  function qualityStyleOnce() {
    if (document.getElementById('nex-quality-override-style')) return;
    var style = document.createElement('style');
    style.id = 'nex-quality-override-style';
    style.textContent = '#nex-settings-module .nex-item{grid-template-columns:minmax(0,1fr) 210px!important;}#nex-settings-module .nex-control{width:210px!important;}#nex-settings-module .nex-select{width:190px!important;font-size:13px!important;}';
    document.head.appendChild(style);
  }

  function moduleFrameStyleOnce() {
    if (document.getElementById('nex-module-frame-override-style')) return;
    var style = document.createElement('style');
    style.id = 'nex-module-frame-override-style';
    style.textContent = '#nex-settings-module[data-open="true"]{display:flex!important;flex-direction:column!important;overflow:hidden!important;}#nex-settings-module:not([data-positioned="true"]){top:50%!important;left:50%!important;right:auto!important;bottom:auto!important;width:min(560px,calc(100vw - 32px))!important;height:min(680px,calc(100vh - 96px))!important;min-width:min(360px,calc(100vw - 24px))!important;min-height:min(420px,calc(100vh - 24px))!important;max-width:calc(100vw - 16px)!important;max-height:calc(100vh - 16px)!important;transform:translate(-50%,-50%)!important;border:1px solid rgba(40,50,72,.1)!important;border-radius:8px!important;}#nex-settings-module .nex-module-content{min-height:0!important;flex:1 1 auto!important;overflow:auto!important;}#nex-settings-module .nex-module-foot{display:block!important;position:relative!important;flex:0 0 16px!important;height:16px!important;margin:14px -26px -30px!important;border-top:1px solid rgba(40,50,72,.1)!important;background:rgba(40,50,72,.025)!important;}';
    document.head.appendChild(style);
  }

  function styleOnce() {
    qualityStyleOnce();
    moduleFrameStyleOnce();
    if (document.getElementById('nex-panel-style')) return;
    var css = [
      '.nex-panel{width:100%;max-width:none;margin:0;font-family:inherit;background:rgba(255,255,255,.5);border:1px solid rgba(40,50,72,.06);border-radius:6px;padding:0 16px 16px;}',
      '.nex-panel .nex-summary{padding:0 0 14px;margin:0 0 4px;border-bottom:1px solid rgba(40,50,72,.08);}',
      '.nex-panel .nex-summary-head{display:flex;align-items:center;gap:8px;min-height:22px;}',
      '.nex-panel .nex-summary-title{margin:0;font-size:14px;font-weight:700;line-height:20px;color:rgba(40,50,72,.92);}',
      '.nex-panel .nex-badge{font-size:12px;line-height:18px;padding:0 8px;border-radius:9px;color:rgba(40,50,72,.5);background:rgba(40,50,72,.06);}',
      '.nex-panel .nex-badge.ok{color:#0f8a4a;background:rgba(15,138,74,.1);}',
      '.nex-panel .nex-summary-line{font-size:12px;line-height:18px;color:rgba(40,50,72,.58);margin:4px 0 0;white-space:pre-line;}',
      '.nex-panel .nex-summary-line.warn{color:#b26a00;}',
      '.nex-panel .nex-format-block{margin:12px 0 0;padding:10px 12px;border:1px solid rgba(40,50,72,.08);border-radius:6px;background:rgba(40,50,72,.025);}',
      '.nex-panel .nex-format-title{margin:0;font-size:13px;font-weight:700;line-height:18px;color:rgba(40,50,72,.88);}',
      '.nex-panel .nex-format-summary{margin-top:4px;font-size:12px;line-height:18px;color:rgba(40,50,72,.62);white-space:normal;overflow-wrap:anywhere;}',
      '.nex-panel .nex-recovery{display:none;margin-top:8px;height:28px;padding:0 10px;border:1px solid rgba(255,58,58,.35);border-radius:4px;background:#fff7f7;color:#c42f2f;font:inherit;font-size:12px;cursor:pointer;}',
      '.nex-panel .nex-recovery:hover{border-color:#ff3a3a;background:#fff0f0;}',
      '.nex-panel .nex-config-actions{display:flex;align-items:center;justify-content:flex-end;gap:8px;margin:18px 0 2px;padding-top:14px;border-top:1px solid rgba(40,50,72,.08);}',
      '.nex-panel .nex-config-action{height:30px;padding:0 12px;border:1px solid rgba(40,50,72,.12);border-radius:15px;background:rgba(40,50,72,.03);color:rgba(40,50,72,.82);font:inherit;font-size:12px;line-height:28px;cursor:pointer;}',
      '.nex-panel .nex-config-action:hover:not(:disabled){border-color:rgba(255,58,58,.45);background:#fff;color:#d73535;}',
      '.nex-panel .nex-config-action.primary{border-color:var(--colorPrimary1,#ff3a4a);background:var(--colorPrimary1,#ff3a4a);color:#fff;}',
      '.nex-panel .nex-config-action.primary:hover:not(:disabled){background:#f23847;color:#fff;}',
      '.nex-panel .nex-config-action:disabled{opacity:.45;cursor:default;}',
      '.nex-panel .nex-section{margin:16px 0 0;}',
      '.nex-panel .nex-section-title{margin:0 0 2px;font-size:14px;font-weight:700;line-height:22px;color:rgba(40,50,72,.9);}',
      '.nex-panel .nex-item{display:grid;grid-template-columns:minmax(0,1fr) 210px;align-items:center;gap:18px;min-height:52px;padding:8px 0;border-bottom:1px solid rgba(40,50,72,.06);}',
      '.nex-panel .nex-item:last-child{border-bottom:0;}',
      '.nex-panel .nex-item[aria-disabled="true"]{opacity:.48;}',
      '.nex-panel .nex-copy{min-width:0;flex:1;}',
      '.nex-panel .nex-label{font-size:14px;line-height:20px;color:rgba(40,50,72,.9);}',
      '.nex-panel .nex-description{font-size:12px;line-height:18px;color:rgba(40,50,72,.48);margin-top:2px;white-space:normal;}',
      '.nex-panel .nex-control{display:flex;align-items:center;justify-content:flex-end;width:210px;min-width:0;}',
      '.nex-panel .nex-switch{position:relative;display:inline-flex;align-items:center;justify-content:center;width:20px;height:20px;padding:0;border:1px solid rgba(40,50,72,.24);border-radius:4px;background:rgba(40,50,72,.02);color:transparent;font:inherit;cursor:pointer;}',
      '.nex-panel .nex-switch[aria-checked="true"]{border-color:var(--colorPrimary1,#ff3a4a);background:var(--colorPrimary1,#ff3a4a);color:#fff;}',
      '.nex-panel .nex-switch:disabled{cursor:default;}',
      '.nex-panel .nex-switch::after{content:"✓";font-size:13px;line-height:18px;}',
      '.nex-panel .nex-select{box-sizing:border-box;width:190px;height:30px;border:1px solid rgba(40,50,72,.06);border-radius:24px;background:rgba(40,50,72,.03);color:rgba(40,50,72,.9);font:inherit;font-size:13px;line-height:28px;padding:0 28px 0 12px;outline:0;cursor:pointer;white-space:nowrap;text-overflow:ellipsis;overflow:hidden;}',
      '.nex-panel .nex-select:hover,.nex-panel .nex-select:focus{border-color:var(--colorPrimary1,#ff3a4a);box-shadow:0 0 0 2px rgba(255,58,58,.08);background:#fff;}',
      '.nex-panel .nex-select:disabled{cursor:default;}',
      '.nex-settings-title-host{position:relative;}',
      '#nex-settings-launcher{position:absolute;top:50px;right:0;display:inline-flex;align-items:center;justify-content:center;height:28px;padding:0 12px;border:1px solid rgba(40,50,72,.1);border-radius:14px;background:rgba(40,50,72,.03);color:rgba(40,50,72,.9);font:inherit;font-size:14px;line-height:18px;white-space:nowrap;cursor:pointer;}',
      '#nex-settings-launcher:hover,#nex-settings-launcher:focus-visible{border-color:rgba(255,58,58,.55);color:#d73535;background:rgba(255,255,255,.65);outline:0;}',
      '#nex-settings-launcher[data-open="true"]{display:none;}',
      '#nex-settings-module{box-sizing:border-box;display:none;position:fixed;top:50%;left:50%;right:auto;bottom:auto;z-index:1200;width:min(560px,calc(100vw - 32px));height:min(680px,calc(100vh - 96px));min-width:min(360px,calc(100vw - 24px));min-height:min(420px,calc(100vh - 24px));max-width:calc(100vw - 16px);max-height:calc(100vh - 16px);transform:translate(-50%,-50%);overflow:hidden;padding:22px 26px 30px;background:rgba(247,249,252,.98);border:1px solid rgba(40,50,72,.1);border-radius:8px;box-shadow:-8px 0 24px rgba(40,50,72,.12);}',
      '#nex-settings-module[data-open="true"]{display:flex;flex-direction:column;}',
      '#nex-settings-module .nex-module-head{display:flex;align-items:center;justify-content:space-between;margin:0 0 18px;padding-bottom:14px;border-bottom:1px solid rgba(40,50,72,.08);cursor:move;user-select:none;}',
      '#nex-settings-module .nex-module-title{margin:0;font-size:16px;line-height:22px;font-weight:700;color:rgba(40,50,72,.92);}',
      '#nex-settings-module .nex-module-content{min-width:0;min-height:0;flex:1;overflow:auto;}',
      '#nex-settings-module .nex-module-foot{position:relative;flex:0 0 16px;height:16px;margin:14px -26px -30px;border-top:1px solid rgba(40,50,72,.1);background:rgba(40,50,72,.025);}',
      '#nex-settings-module .nex-module-foot::after{content:"";position:absolute;right:8px;bottom:4px;width:12px;height:8px;background:linear-gradient(135deg,transparent 35%,rgba(40,50,72,.28) 36%,rgba(40,50,72,.28) 46%,transparent 47%,transparent 58%,rgba(40,50,72,.2) 59%,rgba(40,50,72,.2) 69%,transparent 70%);}',
      '#nex-settings-module .nex-module-close{display:inline-flex;align-items:center;justify-content:center;width:32px;height:32px;padding:0;border:0;border-radius:50%;background:transparent;color:#8a8f99;font:inherit;font-size:22px;line-height:1;cursor:pointer;}',
      '#nex-settings-module .nex-module-close:hover{background:rgba(40,50,72,.05);color:rgba(40,50,72,.9);}',
      '#nex-settings-module .nex-resize-handle{position:absolute;z-index:4;width:16px;height:16px;background:transparent;}',
      '#nex-settings-module .nex-resize-nw{top:0;left:0;cursor:nwse-resize;}',
      '#nex-settings-module .nex-resize-ne{top:0;right:0;cursor:nesw-resize;}',
      '#nex-settings-module .nex-resize-sw{bottom:0;left:0;cursor:nesw-resize;}',
      '#nex-settings-module .nex-resize-se{right:0;bottom:0;cursor:nwse-resize;}',
      '.nex-panel .nex-section{display:grid;grid-template-columns:84px minmax(0,1fr);column-gap:30px;margin:22px 0 0;}',
      '.nex-panel .nex-section-title{grid-column:1;grid-row:1;margin:0;font-size:14px;line-height:18px;font-weight:700;color:rgba(40,50,72,.9);}',
      '.nex-panel .nex-section .nex-item{grid-column:2;}',
      '@media (max-width:760px){#nex-settings-module{width:calc(100vw - 18px);height:calc(100vh - 48px);min-width:0;min-height:0;padding:18px 18px 24px;}#nex-settings-module .nex-module-foot{margin-left:-18px;margin-right:-18px;margin-bottom:-24px;}.nex-panel .nex-section{grid-template-columns:72px minmax(0,1fr);column-gap:18px;}.nex-panel .nex-item{grid-template-columns:minmax(0,1fr) 142px;align-items:start;gap:10px;}.nex-panel .nex-control{width:142px;}.nex-panel .nex-select{width:136px;}}'
    ].join('');
    var style = document.createElement('style');
    style.id = 'nex-panel-style';
    style.textContent = css;
    document.head.appendChild(style);
  }

  function currentConfig() {
    return Object.assign({}, state, pendingConfig || {});
  }

  function readValue(field) {
    var values = currentConfig();
    return values[field.key];
  }

  function strictHiFi() {
    return Number(currentConfig().hifi) !== 0;
  }

  function fieldDisabled(field) {
    if (!connected) return true;
    if (field.key === 'enable' || field.key === 'hifi' || field.key === 'audiohost') return false;
    if (field.key === 'endpoint') return false;
    if (!strictHiFi()) return false;
    return ['rate', 'compat', 'src_prefer', 'dither', 'fade_ms']
      .indexOf(field.key) >= 0;
  }

  function writeValue(field, raw) {
    var next = Object.assign({}, currentConfig());
    var value = typeof raw === 'string' ? parseInt(raw, 10) : raw;
    next[field.key] = value;
    pendingConfig = next;
    applyVisualState();
  }

  function makeSwitch(field) {
    var button = document.createElement('button');
    button.className = 'nex-switch';
    button.type = 'button';
      button.setAttribute('role', 'checkbox');
    button.setAttribute('aria-label', field.label);
    button.addEventListener('click', function () {
      if (button.disabled) return;
      writeValue(field, button.getAttribute('aria-checked') !== 'true' ? 1 : 0);
    });
    return { root: button, input: button, kind: 'switch' };
  }

  function makeSelect(field) {
    var select = document.createElement('select');
    select.className = 'nex-select';
    select.setAttribute('aria-label', field.label);
    field.options.forEach(function (option) {
      var item = document.createElement('option');
      item.value = String(option[0]);
      item.textContent = option[1];
      select.appendChild(item);
    });
    function updateTitle() {
      var option = select.options[select.selectedIndex];
      select.title = option ? option.textContent : field.label;
    }
    select.addEventListener('change', function () {
      updateTitle();
      writeValue(field, select.value);
    });
    updateTitle();
    return { root: select, input: select, kind: 'select' };
  }

  /* The output device is a string id with a live option list, so it is
     saved on its own (POST /api/endpoint) rather than through the numeric
     hook.cfg form and its save buttons. */
  var endpointState = { current: '', endpoints: [], currentAvailable: true };

  function fillEndpointSelect(select) {
    while (select.firstChild) select.removeChild(select.firstChild);
    var follow = document.createElement('option');
    follow.value = '';
    follow.textContent = '跟随网易云';
    select.appendChild(follow);
    endpointState.endpoints.forEach(function (ep) {
      var item = document.createElement('option');
      item.value = ep.id;
      item.textContent = ep.name + (ep.default ? '（系统默认）' : '');
      select.appendChild(item);
    });
    if (endpointState.current && !endpointState.currentAvailable) {
      var gone = document.createElement('option');
      gone.value = endpointState.current;
      gone.textContent = '已选设备（当前不可用）';
      select.appendChild(gone);
    }
    select.value = endpointState.current;
    var option = select.options[select.selectedIndex];
    select.title = option ? option.textContent : '独占输出设备';
  }

  function renderEndpoints() {
    var entry = controls.endpoint;
    if (entry && entry.widget) fillEndpointSelect(entry.widget.input);
  }

  function refreshEndpoints() {
    fetch(ENDPOINTS_API, { method: 'GET' })
      .then(function (response) { return response.ok ? response.json() : Promise.reject(response.status); })
      .then(function (list) {
        endpointState = list;
        renderEndpoints();
      })
      .catch(function () {});
  }

  function makeEndpointSelect(field) {
    var select = document.createElement('select');
    select.className = 'nex-select';
    select.setAttribute('aria-label', field.label);
    fillEndpointSelect(select);
    select.addEventListener('change', function () {
      var id = select.value;
      fetch(ENDPOINT_API, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ id: id })
      })
        .then(function (response) { return response.ok ? response.json() : Promise.reject(response.status); })
        .then(function (list) { endpointState = list; renderEndpoints(); })
        .catch(function () { refreshEndpoints(); });
    });
    return { root: select, input: select, kind: 'endpoint' };
  }

  function makeRow(field) {
    var row = document.createElement('div');
    row.className = 'nex-item';
    row.setAttribute('data-nex-key', field.key);

    var copy = document.createElement('div');
    copy.className = 'nex-copy';
    var label = document.createElement('div');
    label.className = 'nex-label';
    label.textContent = field.label;
    copy.appendChild(label);
    if (field.description) {
      var description = document.createElement('div');
      description.className = 'nex-description';
      description.textContent = field.description;
      copy.appendChild(description);
    }
    row.appendChild(copy);

    var control = document.createElement('div');
    control.className = 'nex-control';
    var widget = field.type === 'switch' ? makeSwitch(field)
      : field.type === 'endpoint' ? makeEndpointSelect(field) : makeSelect(field);
    control.appendChild(widget.root);
    row.appendChild(control);
    controls[field.key] = { row: row, widget: widget };
    return row;
  }

  function makeSection(section) {
    var element = document.createElement('section');
    element.className = 'nex-section';
    element.setAttribute('data-nex-section', section.key);
    var title = document.createElement('h4');
    title.className = 'nex-section-title';
    title.textContent = section.title;
    element.appendChild(title);
    section.fields.forEach(function (field) { element.appendChild(makeRow(field)); });
    return element;
  }

  function build(id) {
    styleOnce();
    controls = {};
    var panel = document.createElement('section');
    panel.id = id || PANEL_ID;
    panel.className = 'nex-panel';
    panel.setAttribute('data-nex-section', 'exclusive-output');

    var summary = document.createElement('div');
    summary.className = 'nex-summary';
    summary.setAttribute('data-nex-section', 'status');
    var summaryHead = document.createElement('div');
    summaryHead.className = 'nex-summary-head';
    var title = document.createElement('h4');
    title.className = 'nex-summary-title';
    title.textContent = '独占输出';
    var badge = document.createElement('span');
    badge.className = 'nex-badge';
    badge.id = 'nex-badge';
    summaryHead.appendChild(title);
    summaryHead.appendChild(badge);
    summary.appendChild(summaryHead);

    var status = document.createElement('div');
    status.className = 'nex-summary-line';
    status.id = 'nex-status';
    status.textContent = '等待音频模块上报状态';
    summary.appendChild(status);
    var endpoint = document.createElement('div');
    endpoint.className = 'nex-summary-line';
    endpoint.id = 'nex-endpoint';
    summary.appendChild(endpoint);
    var recovery = document.createElement('button');
    recovery.id = 'nex-recovery';
    recovery.className = 'nex-recovery';
    recovery.type = 'button';
    recovery.textContent = '切换到原率优先';
    recovery.addEventListener('click', switchToSourcePreferred);
    summary.appendChild(recovery);
    var volume = document.createElement('div');
    volume.className = 'nex-summary-line';
    volume.id = 'nex-volume';
    summary.appendChild(volume);
    panel.appendChild(summary);

    var formatBlock = document.createElement('div');
    formatBlock.className = 'nex-format-block';
    formatBlock.setAttribute('data-nex-section', 'audio-specification');
    var formatTitle = document.createElement('h4');
    formatTitle.className = 'nex-format-title';
    formatTitle.textContent = '音频规格';
    var sourceSummary = document.createElement('div');
    sourceSummary.id = 'nex-source-format-summary';
    sourceSummary.className = 'nex-format-summary nex-source-format-summary';
    sourceSummary.textContent = '源规格：未取得';
    var formatSummary = document.createElement('div');
    formatSummary.id = 'nex-format-summary';
    formatSummary.className = 'nex-format-summary';
    formatSummary.textContent = '等待音频协商';
    formatBlock.appendChild(formatTitle);
    formatBlock.appendChild(sourceSummary);
    formatBlock.appendChild(formatSummary);
    panel.appendChild(formatBlock);

    SECTIONS.forEach(function (section) { panel.appendChild(makeSection(section)); });
    var actions = document.createElement('div');
    actions.className = 'nex-config-actions';
    actions.setAttribute('data-nex-section', 'config-actions');
    var saveButton = document.createElement('button');
    saveButton.id = 'nex-config-save';
    saveButton.className = 'nex-config-action';
    saveButton.type = 'button';
    saveButton.textContent = '保存（下一首应用）';
    saveButton.addEventListener('click', function () { save(false); });
    var restartButton = document.createElement('button');
    restartButton.id = 'nex-config-restart';
    restartButton.className = 'nex-config-action';
    restartButton.type = 'button';
    restartButton.textContent = '保存（当前曲目重开应用）';
    restartButton.addEventListener('click', function () { save(true); });
    actions.appendChild(saveButton);
    actions.appendChild(restartButton);
    panel.appendChild(actions);
    applyVisualState(panel);
    return panel;
  }

  function applyVisualState(root) {
    var target = root || document.getElementById(SETTINGS_PANEL_ID)
      || document.getElementById(PANEL_ID);
    if (!target) return;
    allFields().forEach(function (field) {
      var entry = controls[field.key];
      if (!entry || !entry.widget) return;
      var disabled = fieldDisabled(field);
      var input = entry.widget.input;
      input.disabled = disabled;
      entry.row.setAttribute('aria-disabled', disabled ? 'true' : 'false');
      if (entry.widget.kind === 'endpoint') {
        /* filled by renderEndpoints(); only the enabled state is shared */
      } else if (entry.widget.kind === 'switch') {
        input.setAttribute('aria-checked', readValue(field) ? 'true' : 'false');
      } else {
        input.value = String(readValue(field));
        var selected = input.options && input.options[input.selectedIndex];
        input.title = selected ? selected.textContent : field.label;
      }
    });
    var badge = target.querySelector('#nex-badge');
    if (badge) {
      badge.textContent = connected ? '已连接' : '未连接';
      badge.classList.toggle('ok', connected);
    }
    var saveButton = target.querySelector('#nex-config-save');
    var restartButton = target.querySelector('#nex-config-restart');
    if (saveButton) saveButton.disabled = !connected || !pendingConfig || saveInFlight;
    if (restartButton) restartButton.disabled = !connected || saveInFlight;
  }

  function mountPlayback() {
    var play = document.querySelector('#play .play');
    if (!play) return false;
    var settingsPanel = document.getElementById(SETTINGS_PANEL_ID);
    if (settingsPanel) settingsPanel.remove();
    var existing = document.getElementById(PANEL_ID);
    if (existing && existing.isConnected) {
      applyVisualState(existing);
      return true;
    }
    if (existing) existing.remove();
    var anchor = null;
    var headings = play.querySelectorAll('h4');
    for (var i = 0; i < headings.length; i++) {
      if (headings[i].textContent.trim() !== '输出设备') continue;
      anchor = headings[i].closest('section.item, section');
      break;
    }
    var panel = build(PANEL_ID);
    if (anchor && anchor.parentElement === play) play.insertBefore(panel, anchor.nextSibling);
    else play.appendChild(panel);
    return true;
  }

  function visible(element) {
    if (!element) return false;
    var style = window.getComputedStyle(element);
    if (style.display === 'none' || style.visibility === 'hidden') return false;
    var box = element.getBoundingClientRect();
    return box.width > 0 && box.height > 0;
  }

  function settingsRoot() {
    var root = document.querySelector('#page_pc_setting, #setting, #settings, [data-page="setting"]');
    return visible(root) ? root : null;
  }

  function setSettingsOpen(open) {
    var module = document.getElementById('nex-settings-module');
    if (!module) return;
    var value = open ? 'true' : 'false';
    module.setAttribute('data-open', value);
    var launcher = document.getElementById(SETTINGS_LAUNCHER_ID);
    if (launcher) {
      launcher.setAttribute('data-open', value);
      launcher.setAttribute('aria-expanded', value);
    }
    if (open) syncSourcePlayId();
    if (open) reloadConfigIfClean();
    if (open) refreshEndpoints();
  }

  /* hook.cfg can change outside this page (a hand edit, another client
     window). Re-read it when the settings window opens, but never over
     changes the user has not saved yet. */
  function reloadConfigIfClean() {
    if (pendingConfig || saveInFlight) return;
    fetch(API, { method: 'GET' })
      .then(function (response) { return response.ok ? response.json() : Promise.reject(response.status); })
      .then(function (cfg) {
        if (pendingConfig || saveInFlight) return;
        state = Object.assign({}, DEFAULTS, cfg);
        setBadge(true);
        renderCurrentPanel();
      })
      .catch(function () {});
  }

  function makeModuleDraggable(module) {
    if (!module || module.getAttribute('data-drag-ready') === 'true') return;
    var head = module.querySelector('.nex-module-head');
    if (!head) return;
    module.setAttribute('data-drag-ready', 'true');
    head.addEventListener('mousedown', function (event) {
      if (event.button !== 0 || (event.target.closest && event.target.closest('.nex-module-close'))) return;
      var rect = module.getBoundingClientRect();
      var startX = event.clientX;
      var startY = event.clientY;
      var startLeft = rect.left;
      var startTop = rect.top;
      module.style.left = startLeft + 'px';
      module.style.top = startTop + 'px';
      module.style.right = 'auto';
      module.style.bottom = 'auto';
      module.style.transform = 'none';
      module.setAttribute('data-positioned', 'true');
      function move(moveEvent) {
        var width = module.offsetWidth;
        var height = module.offsetHeight;
        var left = Math.max(8, Math.min(window.innerWidth - width - 8, startLeft + moveEvent.clientX - startX));
        var top = Math.max(8, Math.min(window.innerHeight - height - 8, startTop + moveEvent.clientY - startY));
        module.style.left = left + 'px';
        module.style.top = top + 'px';
      }
      function stop() {
        document.removeEventListener('mousemove', move);
        document.removeEventListener('mouseup', stop);
      }
      document.addEventListener('mousemove', move);
      document.addEventListener('mouseup', stop);
      event.preventDefault();
    });
  }

  function makeModuleResizable(module) {
    if (!module || module.getAttribute('data-resize-ready') === 'true') return;
    module.setAttribute('data-resize-ready', 'true');
    ['nw', 'ne', 'sw', 'se'].forEach(function (edge) {
      var handle = document.createElement('span');
      handle.className = 'nex-resize-handle nex-resize-' + edge;
      handle.setAttribute('aria-hidden', 'true');
      handle.addEventListener('mousedown', function (event) {
        if (event.button !== 0) return;
        var rect = module.getBoundingClientRect();
        var startX = event.clientX;
        var startY = event.clientY;
        var startLeft = rect.left;
        var startTop = rect.top;
        var startWidth = rect.width;
        var startHeight = rect.height;
        var minWidth = Math.min(360, Math.max(240, window.innerWidth - 24));
        var minHeight = Math.min(420, Math.max(240, window.innerHeight - 24));
        var maxWidth = Math.max(minWidth, window.innerWidth - 16);
        var maxHeight = Math.max(minHeight, window.innerHeight - 16);
        module.style.left = startLeft + 'px';
        module.style.top = startTop + 'px';
        module.style.right = 'auto';
        module.style.bottom = 'auto';
        module.style.transform = 'none';
        module.setAttribute('data-positioned', 'true');

        function clamp(value, low, high) { return Math.max(low, Math.min(high, value)); }
        function move(moveEvent) {
          var dx = moveEvent.clientX - startX;
          var dy = moveEvent.clientY - startY;
          var width = startWidth;
          var height = startHeight;
          var left = startLeft;
          var top = startTop;
          if (edge.indexOf('e') >= 0) width = clamp(startWidth + dx, minWidth, maxWidth);
          if (edge.indexOf('w') >= 0) {
            width = clamp(startWidth - dx, minWidth, maxWidth);
            left = startLeft + startWidth - width;
          }
          if (edge.indexOf('s') >= 0) height = clamp(startHeight + dy, minHeight, maxHeight);
          if (edge.indexOf('n') >= 0) {
            height = clamp(startHeight - dy, minHeight, maxHeight);
            top = startTop + startHeight - height;
          }
          left = clamp(left, 8, Math.max(8, window.innerWidth - width - 8));
          top = clamp(top, 8, Math.max(8, window.innerHeight - height - 8));
          module.style.width = width + 'px';
          module.style.height = height + 'px';
          module.style.left = left + 'px';
          module.style.top = top + 'px';
        }
        function stop() {
          document.removeEventListener('mousemove', move);
          document.removeEventListener('mouseup', stop);
          document.body.style.userSelect = '';
        }
        document.body.style.userSelect = 'none';
        document.addEventListener('mousemove', move);
        document.addEventListener('mouseup', stop);
        event.preventDefault();
        event.stopPropagation();
      });
      module.appendChild(handle);
    });
  }

  function fitModuleToViewport(module) {
    if (!module || !module.isConnected) return;
    var rect = module.getBoundingClientRect();
    if (!module.style.left || !module.style.top || module.style.transform) return;
    var left = Math.max(8, Math.min(window.innerWidth - rect.width - 8, rect.left));
    var top = Math.max(8, Math.min(window.innerHeight - rect.height - 8, rect.top));
    module.style.left = left + 'px';
    module.style.top = top + 'px';
  }

  function createSettingsModule(parent) {
    var module = document.getElementById('nex-settings-module');
    if (!module) {
      module = document.createElement('section');
      module.id = 'nex-settings-module';
      module.setAttribute('data-nex-section', 'nexexclusive');
      if (parent) parent.appendChild(module);
    }
    var head = module.querySelector('.nex-module-head');
    if (!head) {
      var legacyTitle = null;
      Array.prototype.slice.call(module.children).forEach(function (child) {
        if (child.classList && child.classList.contains('nex-module-title')) legacyTitle = child;
      });
      if (legacyTitle) legacyTitle.remove();
      head = document.createElement('div');
      head.className = 'nex-module-head';
      var title = document.createElement('h4');
      title.className = 'nex-module-title';
      title.textContent = 'NexExclusive';
      head.appendChild(title);
      var close = document.createElement('button');
      close.className = 'nex-module-close';
      close.type = 'button';
      close.textContent = '×';
      close.setAttribute('aria-label', '关闭 NexExclusive 设置');
      close.addEventListener('click', function () { setSettingsOpen(false); });
      head.appendChild(close);
      module.insertBefore(head, module.firstChild);
    }
    var content = module.querySelector('.nex-module-content');
    if (!content) {
      content = document.createElement('div');
      content.className = 'nex-module-content';
      Array.prototype.slice.call(module.children).forEach(function (child) {
        if (child !== head) content.appendChild(child);
      });
      module.appendChild(content);
    }
    if (!module.querySelector('.nex-module-foot')) {
      var foot = document.createElement('div');
      foot.className = 'nex-module-foot';
      foot.setAttribute('aria-hidden', 'true');
      module.appendChild(foot);
    }
    makeModuleDraggable(module);
    makeModuleResizable(module);
    if (module.getAttribute('data-viewport-ready') !== 'true') {
      module.setAttribute('data-viewport-ready', 'true');
      window.addEventListener('resize', function () { fitModuleToViewport(module); });
    }
    fitModuleToViewport(module);
    return module;
  }

  function openNexSettings() {
    var root = settingsRoot();
    var module = createSettingsModule(root || document.body);
    var panel = document.getElementById(SETTINGS_PANEL_ID);
    if (!panel) {
      panel = build(SETTINGS_PANEL_ID);
      panel.setAttribute('data-nex-page', root ? 'settings' : 'header');
      var content = module.querySelector('.nex-module-content') || module;
      content.appendChild(panel);
    }
    setSettingsOpen(true);
    applyVisualState(panel);
    refreshStatus();
    return true;
  }

  function mountSettingsLauncher(root) {
    var title = root && document.querySelector('h1');
    if (!title || !visible(title) || !title.parentElement) return false;
    var host = title.parentElement;
    host.classList.add('nex-settings-title-host');
    var launcher = document.getElementById(SETTINGS_LAUNCHER_ID);
    if (launcher && launcher.parentElement !== host) {
      launcher.remove();
      launcher = null;
    }
    if (!launcher) {
      launcher = document.createElement('button');
      launcher.id = SETTINGS_LAUNCHER_ID;
      launcher.type = 'button';
      launcher.textContent = 'NexExclusive';
      launcher.setAttribute('aria-label', '打开 NexExclusive 设置');
      launcher.setAttribute('aria-expanded', 'false');
      launcher.addEventListener('click', function (event) {
        event.preventDefault();
        event.stopPropagation();
        var module = document.getElementById('nex-settings-module');
        if (module) setSettingsOpen(module.getAttribute('data-open') !== 'true');
      });
      host.appendChild(launcher);
    }
    var module = document.getElementById('nex-settings-module');
    if (module) {
      var open = module.getAttribute('data-open') === 'true';
      launcher.setAttribute('data-open', open ? 'true' : 'false');
      launcher.setAttribute('aria-expanded', open ? 'true' : 'false');
    }
    return true;
  }

  function mountSettings() {
    var root = settingsRoot();
    if (!root) return false;
    mountSettingsLauncher(root);
    var module = document.getElementById('nex-settings-module');
    var existing = document.getElementById(SETTINGS_PANEL_ID);
    if (module && module.isConnected && existing && existing.isConnected) {
      createSettingsModule(root);
      applyVisualState(existing);
      return true;
    }
    var playbackPanel = document.getElementById(PANEL_ID);
    if (playbackPanel) playbackPanel.remove();
    var panel = build(SETTINGS_PANEL_ID);
    panel.setAttribute('data-nex-page', 'settings');
    module = createSettingsModule(root);
    var content = module.querySelector('.nex-module-content') || module;
    content.appendChild(panel);
    return true;
  }

  function mount() {
    return mountSettings() || mountPlayback();
  }

  window.__nexOpenNexSettings = openNexSettings;

  function renderCurrentPanel() {
    var panel = document.getElementById(SETTINGS_PANEL_ID)
      || document.getElementById(PANEL_ID);
    if (panel && panel.parentNode) panel.parentNode.replaceChild(build(panel.id), panel);
  }

  function setBadge(ok) {
    connected = ok;
    applyVisualState();
  }

  function restartCurrentTrack() {
    if (!connected || saveInFlight) return;
    fetch(RESTART_API, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: '{}'
    }).then(function (response) {
      return response.ok ? response.json() : Promise.reject(response.status);
    }).then(function () {
      refreshStatus();
    }).catch(function () {
      var status = document.getElementById('nex-status');
      if (status) status.textContent = '当前曲目无法重开，请手动切换后再播放';
    });
  }

  function save(restart) {
    if (!connected) {
      return;
    }
    if (saveInFlight) return;
    if (!pendingConfig) {
      if (restart) restartCurrentTrack();
      return;
    }
    saveInFlight = true;
    var sent = pendingConfig;
    applyVisualState();
    fetch(API, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(sent)
    })
      .then(function (response) { return response.ok ? response.json() : Promise.reject(response.status); })
      .then(function (cfg) {
        state = Object.assign({}, DEFAULTS, cfg);
        if (pendingConfig === sent) pendingConfig = null;
        setBadge(true);
        saveInFlight = false;
        if (pendingConfig) save(restart);
        else {
          renderCurrentPanel();
          if (restart) restartCurrentTrack();
          else refreshStatus();
        }
      })
      .catch(function () {
        saveInFlight = false;
        setBadge(false);
        applyVisualState();
      });
  }

  function formatRate(hz) {
    if (!hz) return '';
    return (hz % 1000 === 0 ? (hz / 1000).toFixed(1) : String(hz / 1000)) + ' kHz';
  }

  function postSourceState(payload) {
    if (typeof fetch !== 'function') return Promise.resolve(false);
    return fetch(SOURCE_API, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    }).then(function (response) { return response.ok; }).catch(function () { return false; });
  }

  // 取网易云自己的 webpack require，只复用它已经实例化的 AudioPlayer。
  // 不能自己重新执行模块工厂：那会造出第二个 AudioPlayer，它向原生层注册的
  // 回调会顶掉网易云自己的，播放栏进度随即停住。
  var appRequire = null;
  function sourceAppRequire() {
    if (appRequire) return appRequire;
    var chunks = window.webpackJsonp;
    if (!chunks || typeof chunks.push !== 'function') return null;
    var id = 'nex-source-require-' + Date.now();
    var modules = {};
    modules[id] = function (module, exports, req) { appRequire = req; };
    try { chunks.push([[id], modules, [[id]]]); } catch (_) {}
    return appRequire && appRequire.c ? appRequire : null;
  }

  function sourceWebpackPlayer() {
    var req = sourceAppRequire();
    if (!req) return null;
    var cache = req.c;
    var ids = Object.prototype.hasOwnProperty.call(cache, '4') ? ['4'] : [];
    Object.keys(cache).forEach(function (key) { if (key !== '4') ids.push(key); });
    for (var c = 0; c < ids.length; c++) {
      var mod = cache[ids[c]];
      var exports = mod && mod.exports;
      if (!exports) continue;
      try {
        var player = exports.AudioPlayer || (exports.default && exports.default.AudioPlayer);
        if (player && typeof player.getPlaybackInfo === 'function'
            && typeof player.subscribePlayStatus === 'function') return player;
      } catch (_) {}
    }
    return null;
  }

  function sourceQualityTierHint() {
    var nodes = document.querySelectorAll(
      '[data-testid="tid_playbar_soundquality"], [data-testid*="soundquality"]');
    var text = '';
    for (var i = 0; i < nodes.length; i++) {
      var rect = nodes[i].getBoundingClientRect && nodes[i].getBoundingClientRect();
      if (rect && (rect.width === 0 || rect.height === 0)) continue;
      text += ' ' + (nodes[i].textContent || nodes[i].getAttribute('aria-label') || '');
    }
    if (/vivid|臻彩/i.test(text)) return 6999;
    if (/sky|天空/i.test(text)) return 5999;
    if (/jymaster|母带|至臻|master/i.test(text)) return 4999;
    if (/jyeffect|臻效/i.test(text)) return 3999;
    if (/dolby|杜比/i.test(text)) return 2999;
    if (/hi[\s-]?res|hires|高解析/i.test(text)) return 1999;
    if (/无损|lossless|\bSQ\b/i.test(text)) return 999;
    if (/极高|exhigh|高品质|high[\s-]?quality|\bHQ\b|320\s*k/i.test(text)) return 320;
    if (/较高|higher|192\s*k/i.test(text)) return 192;
    if (/标准|standard|128\s*k/i.test(text)) return 128;
    if (/64aac|64\s*k/i.test(text)) return 64;
    return 0;
  }

  // 只把当前 playId 和当前质量控件提示告诉宿主；源规格由宿主读网易云 .uc 缓存头取得。
  // 宿主对同一 playId 去重，并在换歌后的确认窗口内自己重试，
  // 所以这里不调用 getPlaybackInfo，也不在播放中做任何播放器查询。
  function reportSourcePlayId(playId) {
    if (!playId) return;
    var qualityTierHint = sourceQualityTierHint();
    postSourceState({ source_status: 'probe', source_play_id: playId,
      source_quality_tier_hint: qualityTierHint }).then(function (ok) {
      if (ok) {
        sourceReportedPlayId = playId;
        sourceReportedQualityTierHint = qualityTierHint;
      }
    });
  }

  function ensureSourceBridge() {
    if (sourceBridge && sourceBridge.player) return Promise.resolve(sourceBridge);
    if (sourceBridgePromise) return sourceBridgePromise;
    sourceBridgePromise = new Promise(function (resolve) {
      function attempt() {
        var player = sourceWebpackPlayer();
        if (!player) { setTimeout(attempt, 1000); return; }
        var callback = function (event) {
          if (!event || typeof event.playId !== 'string' || !event.playId) return;
          sourcePlayId = event.playId;
          if (sourcePlayId !== sourceReportedPlayId ||
              sourceQualityTierHint() !== sourceReportedQualityTierHint) reportSourcePlayId(sourcePlayId);
        };
        sourceBridge = { player: player, callback: callback };
        player.subscribePlayStatus({ type: 'playprogress', callback: callback }).catch(function () {});
        window.addEventListener('beforeunload', function () {
          try { if (player.unsubscribePlayStatus) player.unsubscribePlayStatus({ type: 'playprogress', callback: callback }); } catch (_) {}
          sourceBridge = null;
          sourceBridgePromise = null;
        }, { once: true });
        sourceBridgePromise = null;
        resolve(sourceBridge);
      }
      attempt();
    });
    return sourceBridgePromise;
  }

  function snapshotField(snapshot, camel, snake) {
    if (!snapshot) return undefined;
    if (snapshot[camel] !== undefined && snapshot[camel] !== null) return snapshot[camel];
    if (snapshot[snake] !== undefined && snapshot[snake] !== null) return snapshot[snake];
    return undefined;
  }

  function formatSourceAudioSpec(snapshot) {
    var sourceStatus = snapshotField(snapshot, 'sourceStatus', 'source_status');
    var rate = Number(snapshotField(snapshot, 'sourceRate', 'source_rate'));
    var valid = Number(snapshotField(snapshot, 'sourceValidBits', 'source_valid_bits'));
    var container = Number(snapshotField(snapshot, 'sourceContainerBits', 'source_container_bits'));
    var encoding = snapshotField(snapshot, 'sourceEncoding', 'source_encoding') || 'PCM';
    if (sourceStatus === 'pending') return '源规格：查询中';
    if (sourceStatus !== 'observed' || !rate) return '源规格：未取得';
    if (!container) container = valid;
    if (encoding === 'float' || encoding === 'float32') encoding = 'float32';
    else if (encoding === 'integer' || encoding === 'int' || encoding === 'pcm') encoding = '整数 PCM';
    var kbps = Number(snapshotField(snapshot, 'sourceBitrateKbps', 'source_bitrate_kbps'));
    if (encoding === '整数 PCM' && valid <= 0) return '源规格：未取得';
    if (encoding === 'FLAC' || encoding === 'MP3' || encoding === 'AAC' || encoding === 'ALAC' || encoding === 'APE' || encoding === 'OGG') {
      var details = [];
      if (valid > 0) details.push(valid + ' bit');
      if (kbps > 0) details.push(kbps + ' kbps');
      return '源规格　' + formatRate(rate) + ' / ' + (details.length ? details.join(' · ') + ' ' : '') + encoding;
    }
    if (valid <= 0) return '源规格：未取得';
    return '源规格　' + formatRate(rate) + ' / ' + valid + ' bit ' + encoding
      + '（' + container + ' bit 容器）';
  }

  function engagedSummary(text) {
    var match = /out=rate:(\d+)\/cont:(\d+)\.(\d+)/.exec(text || '');
    if (!match) return text ? text.slice(0, 80) : '等待开始播放';
    var container = Number(match[2]);
    var valid = Number(match[3]);
    var mode = /\[converted\]/.test(text) ? '独占转换'
      : /\[depth only\]/.test(text) ? '仅位深转换'
        : /\[repacked/.test(text) ? '原样数值（容器改变）' + (/\+fade\]/.test(text) ? '，开头淡入' : '')
          : /\[native\+fade\]/.test(text) ? '原样输出，开头淡入'
            : /\[native\]/.test(text) ? '原样输出' : '';
    return formatRate(Number(match[1])) + ' / ' + valid + ' bit'
      + (container !== valid ? '（' + container + ' bit 容器）' : '')
      + (mode ? ' · ' + mode : '');
  }

  function humanFault(text) {
    if (!text) return '';
    if (/0x8889000A/i.test(text)) return '独占端点正被上一条音频流占用，播放器正在重建';
    if (/strict native rejected/i.test(text) || /0x88890008/i.test(text)) {
      return strictHiFi()
        ? '严格原样未支持，切换到原率优先后可继续播放'
        : '设备不接受当前格式，未找到可用的独占输出格式';
    }
    if (/0x8889000F/i.test(text)) return '音频端点创建失败，设备或驱动暂时没有响应';
    return text.slice(0, 80);
  }

  function formatAudioSpec(snapshot) {
    var text = snapshot && (snapshot.lastEngaged || snapshot.lastLine || '');
    var input = /in=rate:(\d+)\/cont:(\d+)\/valid:(\d+)/.exec(text);
    var output = /out=rate:(\d+)\/cont:(\d+)\.(\d+)/.exec(text);
    var inputEncoding = /sub:0x00000003/i.test(text) ? 'float32'
      : /sub:0x00000001/i.test(text) ? '整数 PCM' : 'PCM';
    var lines = [formatSourceAudioSpec(snapshot)];
    if (input) lines.push('客户端 PCM　' + formatRate(Number(input[1])) + ' / ' + input[3] + ' bit ' + inputEncoding + '（' + input[2] + ' bit 容器）');
    if (output) lines.push('独占端点　' + formatRate(Number(output[1])) + ' / ' + output[3] + ' bit（' + output[2] + ' bit 容器）');
    if (/\[converted\]/.test(text)) lines.push('状态　独占转换');
    else if (/\[depth only\]/.test(text)) lines.push('状态　仅位深转换，未标记原样');
    else if (/\[repacked/.test(text)) lines.push('状态　原样数值，仅容器宽度改变'
      + (/\+fade\]/.test(text) ? '；开头淡入会改动前几毫秒' : ''));
    else if (/\[native\+fade\]/.test(text)) lines.push('状态　原样输出；开头淡入会改动前几毫秒');
    else if (/\[native\]/.test(text)) lines.push('状态　原样输出');
    else if (!input && !output) lines.push('等待新的音频协商');
    if (snapshot && snapshot.verification && snapshot.verification.line) {
      var verification = snapshot.verification;
      var evidence = verification.clientToEndpoint === 'bit-perfect'
        ? '客户端 PCM → 独占端点　逐样本一致'
        : verification.clientToEndpoint === 'converted'
          ? '客户端 PCM → 独占端点　已转换，未标记原样'
          : '客户端 PCM → 独占端点　暂无完整校验';
      lines.push('校验　' + evidence);
    } else if (input || output) {
      lines.push('校验　本次流尚未生成逐样本校验记录');
    }
    if (snapshot && snapshot.endpoint) lines.push('设备　' + snapshot.endpoint);
    return lines.join('\n');
  }

  function formatAudioSpecSummary(snapshot) {
    var text = snapshot && (snapshot.lastEngaged || snapshot.lastLine || '');
    var input = /in=rate:(\d+)\/cont:(\d+)\/valid:(\d+)/.exec(text);
    var output = /out=rate:(\d+)\/cont:(\d+)\.(\d+)/.exec(text);
    if (!input || !output) return '等待音频协商';
    var converted = /\[converted\]|\[depth only\]/.test(text);
    var mode = converted ? '已转换格式'
      : /\+fade\]/.test(text) ? '开头淡入，其余原样'
        : 'Bit-perfect 原样输出';
    return input[3] + ' bit / ' + formatRate(Number(input[1])) + ' → '
      + output[3] + ' bit / ' + formatRate(Number(output[1])) + ' · ' + mode;
  }

  function formatSettingsAudioSpecSummary(snapshot) {
    var summary = formatAudioSpecSummary(snapshot);
    if (summary === '等待音频协商') return summary;
    return '客户端 PCM ' + summary.replace(' → ', ' → 独占端点 ');
  }

  function isPlaybackPaused() {
    var button = document.querySelector('[data-testid="tid_playbar_play_btn"]');
    return !!(button && button.querySelector('[aria-label="play"]'));
  }

  // 设置窗口打开时补报一次：宿主重启过、或页面刚注入还没收到 playprogress。
  // 只有暂停且还不知道 playId 时才问一次 getPlaybackInfo({}) 来发现它。
  function syncSourcePlayId() {
    if (!sourceBridge || !sourceBridge.player) {
      ensureSourceBridge().then(function () { syncSourcePlayId(); });
      return;
    }
    if (sourcePlayId) { reportSourcePlayId(sourcePlayId); return; }
    if (!isPlaybackPaused()) return;
    Promise.race([
      sourceBridge.player.getPlaybackInfo({}),
      new Promise(function (resolve) { setTimeout(function () { resolve(null); }, 2500); })
    ]).catch(function () { return null; }).then(function (info) {
      if (!sourcePlayId && info && typeof info.playId === 'string' && info.playId) {
        sourcePlayId = info.playId;
      }
      reportSourcePlayId(sourcePlayId);
    });
  }

  function renderAudioSpecStatus(snapshot) {
    if (snapshot && snapshot.source_play_id && sourcePlayId && snapshot.source_play_id !== sourcePlayId) {
      snapshot = Object.assign({}, snapshot, { source_status: 'unavailable' });
    }
    var staleQualityCard = document.getElementById('nex-quality-card');
    if (staleQualityCard) staleQualityCard.remove();
    var sourceSummary = document.getElementById('nex-source-format-summary');
    if (sourceSummary) sourceSummary.textContent = formatSourceAudioSpec(snapshot || {});
    var formatSummary = document.getElementById('nex-format-summary');
    if (formatSummary) formatSummary.textContent = formatSettingsAudioSpecSummary(snapshot || {});
  }

  function switchToSourcePreferred() {
    pendingConfig = Object.assign({}, currentConfig(), {
      hifi: 0,
      compat: 1,
      audiohost: 0,
      shared_fallback: 0
    });
    applyVisualState();
  }

  function refreshStatus() {
    var status = document.getElementById('nex-status');
    if (!status || !connected) return;
    fetch(API.replace('/cfg', '/status'), { method: 'GET' })
      .then(function (response) { return response.ok ? response.json() : Promise.reject(response.status); })
      .then(function (snapshot) {
        status.classList.remove('warn');
        status.textContent = snapshot.fault && !snapshot.lastEngaged
          ? '当前输出　独占未接管，播放继续由客户端当前路径提供'
          : '当前输出　' + engagedSummary(snapshot.lastEngaged);
        var endpoint = document.getElementById('nex-endpoint');
        var recovery = document.getElementById('nex-recovery');
        var nativeRejected = strictHiFi() && /strict native rejected|0x88890008/i.test(snapshot.fault || '');
        if (recovery) recovery.style.display = nativeRejected ? 'inline-flex' : 'none';
        renderAudioSpecStatus(snapshot);
        if (endpoint) {
          var endpointText = snapshot.endpoint ? '播放设备　' + snapshot.endpoint : '';
          if (snapshot.fault) endpointText += (endpointText ? '　' : '') + '最近状态　' + humanFault(snapshot.fault);
          endpoint.textContent = endpointText;
          endpoint.classList.toggle('warn', !!snapshot.fault);
        }
        if (state.dither === 2 && !strictHiFi()) {
          status.textContent += '　自动抖动仅在降位深时启用';
        }
        var volume = document.getElementById('nex-volume');
        if (volume) {
          if (!snapshot.endpoint || snapshot.hwVolume === null || snapshot.hwVolume === undefined) {
            volume.textContent = '';
          } else if (snapshot.hwVolume === true) {
            volume.textContent = '硬件音量可用，独占时系统音量条仍有效';
          } else {
            volume.textContent = '当前设备没有硬件音量，独占时请使用设备旋钮';
          }
        }
      })
      .catch(function () {});
  }

  function refreshQualityOnly() {
    fetch(API.replace('/cfg', '/status'), { method: 'GET' })
      .then(function (response) { return response.ok ? response.json() : Promise.reject(response.status); })
      .then(renderAudioSpecStatus)
      .catch(function () {});
  }

  function load() {
    fetch(API, { method: 'GET' })
      .then(function (response) { return response.ok ? response.json() : Promise.reject(response.status); })
      .then(function (cfg) {
        state = Object.assign({}, DEFAULTS, cfg);
        pendingConfig = null;
        setBadge(true);
        mount();
        refreshStatus();
        refreshEndpoints();
      })
      .catch(function () {
        setBadge(false);
        mount();
      });
  }

  function watch() {
    mount();
    var pending = null;
    var observer = new MutationObserver(function () {
      if (pending) return;
      pending = setTimeout(function () {
        pending = null;
        if (!document.getElementById(PANEL_ID)) mount();
      }, 250);
    });
    if (document.body) observer.observe(document.body, { childList: true, subtree: true });
    setInterval(function () {
      mount();
      if (connected) refreshStatus(); else load();
      refreshQualityOnly();
      var module = document.getElementById('nex-settings-module');
      if (module && module.getAttribute('data-open') === 'true') syncSourcePlayId();
    }, 5000);
    ensureSourceBridge();
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', watch);
  else watch();
  load();
})();
