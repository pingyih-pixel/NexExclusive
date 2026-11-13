// NexExclusive settings injected into the client's Playback or Settings page.
// The page owns navigation and typography; this script only adds native-style
// sections and talks to the local host API.
(function () {
  'use strict';

  var PANEL_VERSION = 'settings-v24';
  if (window.__nexPanelLoaded === PANEL_VERSION) return;
  window.__nexPanelLoaded = PANEL_VERSION;

  var API = 'http://127.0.0.1:9334/api/cfg';
  var PANEL_ID = 'nexsink-panel';
  var SETTINGS_PANEL_ID = 'nex-settings-panel';
  var SETTINGS_LAUNCHER_ID = 'nex-settings-launcher';
  var DEFAULTS = {
    enable: 1, bits: 24, rate: 0, buffer: 0,
    compat: 1, dither: 2, fade_ms: 5, shared_fallback: 0,
    src_prefer: 0, hifi: 0, audiohost: 0
  };

  /* Replace DOM and styles from an older injected script version. */
  ['nex-panel-style', 'nex-quality-override-style', PANEL_ID, SETTINGS_PANEL_ID, 'nex-settings-module', 'nex-settings-entry', SETTINGS_LAUNCHER_ID, 'nex-quality-card'].forEach(function (id) {
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
          key: 'audiohost', label: '独立音频宿主', type: 'switch',
          description: '把独占端点和实时提交移到独立进程；仅在严格原样模式下启用。'
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
    style.textContent = '#nex-quality-card{width:172px!important;min-width:156px!important;max-width:196px!important;height:28px!important;padding:0 11px!important;border-radius:14px!important;justify-content:center!important;border-color:rgba(40,50,72,.08)!important;background:rgba(247,249,252,.72)!important;color:rgba(40,50,72,.9)!important;}#nex-quality-card .nex-quality-title{font-size:13px!important;}#nex-quality-card .nex-quality-summary{flex:0 1 auto!important;max-width:calc(100% - 58px)!important;text-align:center!important;}#nex-quality-card:hover,#nex-quality-card:focus-visible,#nex-quality-card[data-open="true"]{border-color:rgba(255,58,58,.45)!important;background:rgba(255,255,255,.72)!important;}#nex-settings-module .nex-item{grid-template-columns:minmax(0,1fr) 210px!important;}#nex-settings-module .nex-control{width:210px!important;}#nex-settings-module .nex-select{width:190px!important;font-size:13px!important;}';
    document.head.appendChild(style);
  }

  function styleOnce() {
    qualityStyleOnce();
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
      '.nex-panel .nex-recovery{display:none;margin-top:8px;height:28px;padding:0 10px;border:1px solid rgba(255,58,58,.35);border-radius:4px;background:#fff7f7;color:#c42f2f;font:inherit;font-size:12px;cursor:pointer;}',
      '.nex-panel .nex-recovery:hover{border-color:#ff3a3a;background:#fff0f0;}',
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
      '#nex-settings-module{box-sizing:border-box;display:none;position:fixed;top:106px;right:0;bottom:78px;z-index:1200;width:min(560px,42vw);min-width:420px;overflow:auto;padding:22px 26px 30px;background:rgba(247,249,252,.98);border-left:1px solid rgba(40,50,72,.1);box-shadow:-8px 0 24px rgba(40,50,72,.08);}',
      '#nex-settings-module[data-open="true"]{display:block;}',
      '#nex-settings-module .nex-module-head{display:flex;align-items:center;justify-content:space-between;margin:0 0 18px;padding-bottom:14px;border-bottom:1px solid rgba(40,50,72,.08);cursor:move;user-select:none;}',
      '#nex-settings-module .nex-module-title{margin:0;font-size:16px;line-height:22px;font-weight:700;color:rgba(40,50,72,.92);}',
      '#nex-settings-module .nex-module-content{min-width:0;}',
      '#nex-settings-module .nex-module-close{display:inline-flex;align-items:center;justify-content:center;width:32px;height:32px;padding:0;border:0;border-radius:50%;background:transparent;color:#8a8f99;font:inherit;font-size:22px;line-height:1;cursor:pointer;}',
      '#nex-settings-module .nex-module-close:hover{background:rgba(40,50,72,.05);color:rgba(40,50,72,.9);}',
      '.nex-panel .nex-section{display:grid;grid-template-columns:84px minmax(0,1fr);column-gap:30px;margin:22px 0 0;}',
      '.nex-panel .nex-section-title{grid-column:1;grid-row:1;margin:0;font-size:14px;line-height:18px;font-weight:700;color:rgba(40,50,72,.9);}',
      '.nex-panel .nex-section .nex-item{grid-column:2;}',
      '.nex-quality-card{position:relative;display:inline-flex;flex:0 0 172px;flex-direction:row;align-items:center;justify-content:center;gap:5px;vertical-align:middle;box-sizing:border-box;width:172px;min-width:156px;max-width:196px;height:28px;margin:0 8px 0 0;padding:0 11px;border:1px solid rgba(40,50,72,.08);border-radius:14px;background:rgba(247,249,252,.72);color:rgba(40,50,72,.9);font:inherit;font-size:12px;line-height:26px;cursor:pointer;white-space:nowrap;}',
      '.nex-quality-card:hover,.nex-quality-card:focus-visible,.nex-quality-card[data-open="true"]{border-color:rgba(255,58,58,.45);background:rgba(255,255,255,.72);outline:0;}',
      '#nex-quality-card{border-color:rgba(40,50,72,.08);background:rgba(247,249,252,.72);color:rgba(40,50,72,.9);}',
      '#nex-quality-card:hover,#nex-quality-card:focus-visible,#nex-quality-card[data-open="true"]{border-color:rgba(255,58,58,.45);background:rgba(255,255,255,.72);}',
      '.nex-quality-title{flex:0 0 auto;font-size:13px;line-height:26px;color:rgba(40,50,72,.9);}',
      '.nex-quality-summary{min-width:0;flex:0 1 auto;max-width:calc(100% - 58px);overflow:hidden;text-overflow:ellipsis;font-size:11px;line-height:26px;color:rgba(40,50,72,.58);text-align:center;}',
      '.nex-quality-tooltip{position:absolute;right:0;bottom:calc(100% + 8px);z-index:1000;display:none;width:276px;padding:10px 12px;border:1px solid rgba(40,50,72,.12);border-radius:5px;background:#fff;color:rgba(40,50,72,.82);box-shadow:0 8px 24px rgba(40,50,72,.16);font-size:12px;line-height:18px;text-align:left;white-space:pre-line;pointer-events:none;}',
      '.nex-quality-card:hover .nex-quality-tooltip,.nex-quality-card:focus-visible .nex-quality-tooltip,.nex-quality-card[data-open="true"] .nex-quality-tooltip{display:block;}',
      '@media (max-width:760px){#nex-settings-module{top:74px;bottom:72px;width:min(560px,calc(100vw - 18px));min-width:0;padding:18px 18px 24px;}.nex-panel .nex-section{grid-template-columns:72px minmax(0,1fr);column-gap:18px;}.nex-panel .nex-item{grid-template-columns:minmax(0,1fr) 142px;align-items:start;gap:10px;}.nex-panel .nex-control{width:142px;}.nex-panel .nex-select{width:136px;}.nex-quality-card{min-width:0;max-width:196px;}.nex-quality-tooltip{right:0;}}'
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
    if (field.key === 'enable' || field.key === 'hifi') return false;
    if (field.key === 'audiohost') return !strictHiFi();
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
    save();
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
    var widget = field.type === 'switch' ? makeSwitch(field) : makeSelect(field);
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

    SECTIONS.forEach(function (section) { panel.appendChild(makeSection(section)); });
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
      if (entry.widget.kind === 'switch') {
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
    makeModuleDraggable(module);
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

  function save() {
    if (!connected) {
      pendingConfig = null;
      renderCurrentPanel();
      return;
    }
    if (saveInFlight || !pendingConfig) return;
    saveInFlight = true;
    var sent = pendingConfig;
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
        if (pendingConfig) save();
        else {
          renderCurrentPanel();
          refreshStatus();
        }
      })
      .catch(function () {
        saveInFlight = false;
        pendingConfig = null;
        setBadge(false);
        renderCurrentPanel();
      });
  }

  function formatRate(hz) {
    if (!hz) return '';
    return (hz % 1000 === 0 ? (hz / 1000).toFixed(1) : String(hz / 1000)) + ' kHz';
  }

  function engagedSummary(text) {
    var match = /out=rate:(\d+)\/cont:(\d+)\.(\d+)/.exec(text || '');
    if (!match) return text ? text.slice(0, 80) : '等待开始播放';
    var container = Number(match[2]);
    var valid = Number(match[3]);
    var mode = /\[converted\]/.test(text) ? '独占转换'
      : /\[depth only\]/.test(text) ? '仅位深转换'
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
    var lines = ['音频规格'];
    if (input) lines.push('客户端 PCM　' + formatRate(Number(input[1])) + ' / ' + input[3] + ' bit（' + input[2] + ' bit 容器）');
    if (output) lines.push('独占端点　' + formatRate(Number(output[1])) + ' / ' + output[3] + ' bit（' + output[2] + ' bit 容器）');
    if (/\[converted\]/.test(text)) lines.push('状态　独占转换');
    else if (/\[native\]/.test(text)) lines.push('状态　原样输出');
    else if (!input && !output) lines.push('等待新的音频协商');
    if (snapshot && snapshot.verification) {
      var verification = snapshot.verification;
      var evidence = verification.clientToEndpoint === 'bit-perfect'
        ? '客户端 PCM → 独占端点　逐样本一致'
        : verification.clientToEndpoint === 'converted'
          ? '客户端 PCM → 独占端点　已转换，未标记原样'
          : '客户端 PCM → 独占端点　暂无完整校验';
      lines.push('校验　' + evidence);
      lines.push('源文件解码前　未提供逐样本证据');
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
    var mode = converted ? '已转换格式' : 'Bit-perfect 原样输出';
    return input[2] + '-bit / ' + formatRate(Number(input[1])) + ' -> '
      + output[3] + '-bit / ' + formatRate(Number(output[1])) + ' · ' + mode;
  }

  function qualityAnchor() {
    var directs = document.querySelectorAll('[data-testid="tid_playbar_soundquality"]');
    for (var d = 0; d < directs.length; d++) {
      if (visible(directs[d])) return directs[d];
    }
    var nodes = document.querySelectorAll('[aria-label*="Tag:"], button, span, div');
    for (var i = 0; i < nodes.length; i++) {
      if ((nodes[i].textContent || '').trim() === '无损' && nodes[i].getBoundingClientRect().width > 0) return nodes[i];
    }
    return null;
  }

  function renderQualityBox(snapshot) {
    styleOnce();
    var anchor = qualityAnchor();
    var existing = document.getElementById('nex-quality-card');
    if (!anchor) {
      if (existing) existing.remove();
      return;
    }
    if (existing && (!existing.isConnected || existing.tagName !== 'BUTTON'
        || existing.parentElement !== anchor.parentElement)) {
      existing.remove();
      existing = null;
    }
    if (!existing) {
      existing = document.createElement('button');
      existing.id = 'nex-quality-card';
      existing.className = 'nex-quality-card';
      existing.type = 'button';
      existing.setAttribute('aria-label', '音频规格详情');
      existing.setAttribute('aria-expanded', 'false');
      var title = document.createElement('span');
      title.className = 'nex-quality-title';
      title.textContent = '音频规格';
      existing.appendChild(title);
      var summary = document.createElement('span');
      summary.className = 'nex-quality-summary';
      existing.appendChild(summary);
      var tooltip = document.createElement('span');
      tooltip.className = 'nex-quality-tooltip';
      tooltip.setAttribute('role', 'tooltip');
      existing.appendChild(tooltip);
      existing.addEventListener('click', function () {
        var open = existing.getAttribute('data-open') === 'true';
        existing.setAttribute('data-open', open ? 'false' : 'true');
        existing.setAttribute('aria-expanded', open ? 'false' : 'true');
      });
      existing.addEventListener('keydown', function (event) {
        if (event.key === 'Escape') {
          existing.setAttribute('data-open', 'false');
          existing.setAttribute('aria-expanded', 'false');
        }
      });
      if (anchor.parentElement) anchor.parentElement.insertBefore(existing, anchor);
    }
    var summary = existing.querySelector('.nex-quality-summary');
    if (summary) summary.textContent = formatAudioSpecSummary(snapshot || {});
    var tip = existing.querySelector('.nex-quality-tooltip');
    if (tip) tip.textContent = formatAudioSpec(snapshot || {});
  }

  function switchToSourcePreferred() {
    pendingConfig = Object.assign({}, currentConfig(), {
      hifi: 0,
      compat: 1,
      audiohost: 0,
      shared_fallback: 0
    });
    applyVisualState();
    save();
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
        renderQualityBox(snapshot);
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
      .then(renderQualityBox)
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
    }, 5000);
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', watch);
  else watch();
  load();
})();
