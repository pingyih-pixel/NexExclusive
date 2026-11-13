// NexExclusive interface editor. The client keeps its layout; this script adds
// one native-style entry point and a modal for optional visibility rules.
(function () {
  'use strict';
  var SCRIPT_VERSION = 'header-modal-v14';
  if (window.__nexSimpleLoaded === SCRIPT_VERSION) return;
  window.__nexSimpleLoaded = SCRIPT_VERSION;

  var API = 'http://127.0.0.1:9334/api/ui';
  var PANEL_ID = 'nex-simple-panel';
  var TOOLBAR_ID = 'nex-simple-toolbar';
  var HEADER_ENTRY_ID = 'nex-simple-header-entry';
  var NEX_BUTTON_ID = 'nex-simple-nex-button';
  var EDIT_BUTTON_ID = 'nex-simple-edit-toggle';
  var MODAL_ID = 'nex-simple-modal';
  var STORE = 'nexsink.ui.mirror';

  var staleToolbar = document.getElementById(TOOLBAR_ID);
  if (staleToolbar) staleToolbar.remove();
  var staleHeader = document.getElementById(HEADER_ENTRY_ID);
  if (staleHeader) staleHeader.remove();
  var stalePanel = document.getElementById(PANEL_ID);
  if (stalePanel) stalePanel.remove();
  var staleStyle = document.getElementById('nex-simple-style');
  if (staleStyle) staleStyle.remove();

  function testId(id) { return document.querySelector('[data-testid="' + id + '"]'); }
  function aria(label) { return document.querySelector('[aria-label="' + label + '"]'); }
  function home() { return document.getElementById('page_pc_home'); }
  function grid() { return home(); }

  function playlistNode(node) {
    return !node || !!node.closest('[data-testid="tid_createplaylist_section"], [data-testid="tid_collectingplaylist_title"]');
  }

  function blockByText(text, suffix) {
    var root = grid();
    if (!root) return null;
    var nodes = root.querySelectorAll('.exposure-bar, .block-title');
    for (var i = 0; i < nodes.length; i++) {
      var value = (nodes[i].textContent || '').replace(/Loading\.\.\./g, '').trim();
      var match = suffix ? value.slice(-text.length) === text : value.indexOf(text) === 0;
      if (!match) continue;
      var node = nodes[i];
      while (node && node.parentElement !== root) node = node.parentElement;
      if (node && !playlistNode(node)) return node;
    }
    return null;
  }

  function iconSlot(element) {
    if (!element) return null;
    var bar = element.closest('[class*="MiniModeIconBar"], [class*="IconBar"]');
    if (!bar) return element;
    var node = element;
    while (node && node.parentElement !== bar) node = node.parentElement;
    return node || element;
  }

  var GROUPS = [
    {
      key: 'sidebar', title: '左侧导航', items: [
        { key: 'sidebar_recommend', label: '推荐', target: function () { return testId('tid_navitem_recommend'); } },
        { key: 'sidebar_featured', label: '精选', target: function () { return testId('tid_navitem_choice'); } },
        { key: 'sidebar_podcast', label: '播客', target: function () { return testId('tid_navitem_audio'); } },
        { key: 'sidebar_fm', label: '私人漫游', target: function () { return testId('tid_navitem_privateFm'); } },
        { key: 'sidebar_community', label: '社区', target: function () { return testId('tid_navitem_community'); } },
        { key: 'sidebar_heartbeat', label: '关注', target: function () { return testId('tid_sidebar_heartbeat_btn'); } },
        { key: 'sidebar_more', label: '更多', target: function () { return testId('tid_nav_more_btn'); } }
      ]
    },
    {
      key: 'main', title: '右侧大页', items: [
        { key: 'main_recommend', label: '推荐歌单', target: function () { return blockByText('推荐歌单'); } },
        { key: 'main_featured', label: '精选活动', target: function () { return blockByText('精选活动'); } },
        { key: 'main_charts', label: '榜单精选', target: function () { return blockByText('榜单精选'); } },
        { key: 'main_recent', label: '最近常听', target: function () { return blockByText('最近常听'); } },
        { key: 'main_liked', label: '红心歌曲与相似推荐', target: function () { return blockByText('你的红心歌曲和相似推荐播放'); } },
        { key: 'main_radar', label: '雷达歌单', target: function () { return blockByText('的雷达歌单', true); } },
        { key: 'main_blackvinyl', label: '黑胶 VIP 专享', target: function () { return blockByText('黑胶VIP'); } },
        { key: 'main_context', label: '根据当前歌曲推荐', target: function () { return blockByText('这些说唱好带感播放'); } },
        { key: 'main_daily', label: '每日推荐音乐', target: function () { return blockByText('每天听点儿好音乐'); } },
        { key: 'main_podcast', label: '专属播客', target: function () { return blockByText('你的专属播客'); } },
        { key: 'main_books', label: '有声书', target: function () { return blockByText('你的宝藏有声书'); } }
      ]
    },
    {
      key: 'header', title: '顶部按钮', items: [
        { key: 'header_message', label: '消息中心', target: function () { return iconSlot(testId('tid_header_msg_btn')); } },
        { key: 'header_soundbound', label: '听歌识曲', target: function () { return iconSlot(aria('soundbound')); } },
        { key: 'header_skin', label: '换肤', target: function () { return iconSlot(testId('tid_header_skin_btn')); } },
        { key: 'header_pet', label: '宠物模式', target: function () { return iconSlot(aria('mini-pet')); } },
        { key: 'header_mini', label: 'mini 模式', target: function () { return iconSlot(aria('mini')); } },
        { key: 'header_setting', label: '设置', target: function () { return iconSlot(testId('tid_header_setting_btn')); } }
      ]
    }
  ];

  var ITEMS = GROUPS.reduce(function (all, group) { return all.concat(group.items); }, []);
  var KEYS = ITEMS.map(function (item) { return item.key; });
  var state = {};
  ITEMS.forEach(function (item) { state[item.key] = 0; });
  var hidden = new Map();
  var activeGroup = 'sidebar';
  var draft = null;

  function styleOnce() {
    if (document.getElementById('nex-simple-style')) return;
    var css = [
      '#nex-simple-panel{max-width:620px;margin:0 0 30px;font-family:inherit;}',
      '#nex-simple-panel .nex-head{display:flex;align-items:center;min-height:22px;margin:0 0 4px;}',
      '#nex-simple-panel .nex-head h4{margin:0;font-size:14px;font-weight:700;line-height:20px;color:rgba(40,50,72,.92);}',
      '#nex-simple-panel .nex-note{font-size:12px;line-height:18px;color:rgba(40,50,72,.48);margin:0 0 10px;}',
      '#nex-simple-panel .nex-open{height:28px;padding:0 12px;border:1px solid rgba(40,50,72,.1);border-radius:14px;background:transparent;color:rgba(40,50,72,.9);font:inherit;font-size:14px;line-height:18px;cursor:pointer;}',
      '#nex-simple-panel .nex-open:hover{border-color:rgba(255,58,58,.55);background:rgba(255,255,255,.65);color:#d73535;}',
      '#' + TOOLBAR_ID + '{display:none;}',
      '#' + HEADER_ENTRY_ID + '{display:inline-flex;align-items:center;gap:6px;flex:0 0 auto;width:auto;height:36px;margin-right:8px;}',
      '#' + NEX_BUTTON_ID + ',#' + EDIT_BUTTON_ID + '{display:inline-flex;align-items:center;justify-content:center;gap:5px;width:auto;height:28px;padding:0 12px;border:1px solid rgba(40,50,72,.1);border-radius:14px;background:rgba(40,50,72,.03);color:rgba(40,50,72,.9);font:inherit;font-size:14px;line-height:18px;white-space:nowrap;cursor:pointer;}',
      '#' + NEX_BUTTON_ID + ':hover,#' + NEX_BUTTON_ID + ':focus-visible,#' + EDIT_BUTTON_ID + ':hover,#' + EDIT_BUTTON_ID + ':focus-visible{border-color:rgba(255,58,58,.55);background:rgba(255,255,255,.65);color:#d73535;outline:0;}',
      '#' + EDIT_BUTTON_ID + '::before{content:"✎";font-size:14px;line-height:1;}',
      '#' + MODAL_ID + '{position:fixed;inset:0;z-index:2147483000;display:flex;align-items:center;justify-content:center;background:rgba(40,50,72,.18);}',
      '#' + MODAL_ID + ' .nex-dialog{width:min(560px,calc(100vw - 36px));max-height:min(680px,calc(100vh - 48px));display:flex;flex-direction:column;background:rgb(247,249,252);border:1px solid rgba(40,50,72,.08);border-radius:6px;box-shadow:0 12px 32px rgba(40,50,72,.16);overflow:hidden;color:rgba(40,50,72,.9);}',
      '#' + MODAL_ID + ' .nex-dialog-head{display:flex;align-items:center;justify-content:space-between;height:54px;padding:0 20px;border-bottom:1px solid rgba(40,50,72,.08);background:rgba(255,255,255,.35);}',
      '#' + MODAL_ID + ' .nex-dialog-title{margin:0;font-size:16px;line-height:22px;font-weight:700;}',
      '#' + MODAL_ID + ' .nex-close{width:28px;height:28px;border:0;border-radius:50%;background:transparent;color:rgba(40,50,72,.52);font-size:20px;line-height:26px;cursor:pointer;}',
      '#' + MODAL_ID + ' .nex-close:hover{background:rgba(40,50,72,.08);color:rgba(40,50,72,.9);}',
      '#' + MODAL_ID + ' .nex-dialog-body{display:flex;min-height:0;flex:1;}',
      '#' + MODAL_ID + ' .nex-tabs{width:132px;padding:12px 8px;background:rgba(40,50,72,.03);border-right:1px solid rgba(40,50,72,.08);}',
      '#' + MODAL_ID + ' .nex-tab{display:flex;width:100%;height:34px;align-items:center;padding:0 12px;border:0;border-radius:4px;background:transparent;color:rgba(40,50,72,.62);font:inherit;font-size:13px;text-align:left;cursor:pointer;}',
      '#' + MODAL_ID + ' .nex-tab:hover{background:rgba(40,50,72,.06);}',
      '#' + MODAL_ID + ' .nex-tab[data-active="true"]{background:rgba(255,255,255,.78);color:#d73535;font-weight:700;box-shadow:0 1px 3px rgba(40,50,72,.06);}',
      '#' + MODAL_ID + ' .nex-content{min-width:0;flex:1;overflow:auto;padding:16px 20px 20px;}',
      '#' + MODAL_ID + ' .nex-hint{margin:0 0 12px;color:rgba(40,50,72,.5);font-size:12px;line-height:18px;}',
      '#' + MODAL_ID + ' .nex-group{margin:0 0 8px;border:1px solid rgba(40,50,72,.08);border-radius:6px;background:rgba(255,255,255,.5);overflow:hidden;}',
      '#' + MODAL_ID + ' .nex-group > summary{display:flex;align-items:center;justify-content:space-between;height:40px;padding:0 12px;list-style:none;font-size:13px;font-weight:700;cursor:pointer;background:rgba(255,255,255,.35);}',
      '#' + MODAL_ID + ' .nex-group > summary::-webkit-details-marker{display:none;}',
      '#' + MODAL_ID + ' .nex-group > summary::after{content:"";width:8px;height:8px;margin-right:4px;border-right:1.5px solid #8a8f99;border-bottom:1.5px solid #8a8f99;transform:translateY(-2px) rotate(45deg);transition:transform .15s;}',
      '#' + MODAL_ID + ' .nex-group[open] > summary::after{transform:translateY(2px) rotate(225deg);}',
      '#' + MODAL_ID + ' .nex-row{position:relative;display:flex;align-items:center;justify-content:space-between;min-height:44px;padding:0 16px;font-size:13px;}',
      '#' + MODAL_ID + ' .nex-row + .nex-row::before{content:"";position:absolute;left:16px;right:16px;top:0;border-top:1px solid #edf0f4;}',
      '#' + MODAL_ID + ' .nex-row[data-missing="true"]{color:rgba(40,50,72,.38);}',
      '#' + MODAL_ID + ' .nex-visibility{position:relative;display:inline-flex;align-items:center;width:36px;height:20px;padding:0;border:0;border-radius:10px;background:#c8cdd5;box-shadow:inset 0 0 0 1px rgba(40,50,72,.08);cursor:pointer;transition:background .15s ease,box-shadow .15s ease;}',
      '#' + MODAL_ID + ' .nex-visibility::after{content:"";position:absolute;top:3px;left:3px;width:14px;height:14px;border-radius:50%;background:#fff;box-shadow:0 1px 3px rgba(24,31,44,.22);transition:transform .15s ease;}',
      '#' + MODAL_ID + ' .nex-visibility[data-hidden="false"]{background:#ff3a4a;box-shadow:inset 0 0 0 1px rgba(198,33,48,.2);}',
      '#' + MODAL_ID + ' .nex-visibility[data-hidden="false"]::after{transform:translateX(16px);}',
      '#' + MODAL_ID + ' .nex-visibility[data-hidden="true"]{background:#c8cdd5;}',
      '#' + MODAL_ID + ' .nex-visibility:hover{filter:brightness(.97);}',
      '#' + MODAL_ID + ' .nex-visibility:focus-visible{outline:2px solid rgba(232,59,69,.38);outline-offset:2px;}',
      '#' + MODAL_ID + ' .nex-dialog-foot{display:flex;justify-content:flex-end;gap:8px;padding:12px 20px;border-top:1px solid rgba(40,50,72,.08);background:rgba(255,255,255,.28);}',
      '#' + MODAL_ID + ' .nex-editor-cancel,#' + MODAL_ID + ' .nex-editor-save{height:34px;padding:0 14px;border-radius:4px;font:inherit;font-size:13px;cursor:pointer;}',
      '#' + MODAL_ID + ' .nex-editor-cancel{border:1px solid rgba(40,50,72,.1);border-radius:14px;background:transparent;color:rgba(40,50,72,.78);}',
      '#' + MODAL_ID + ' .nex-editor-save{border:1px solid #ff3a4a;border-radius:14px;background:#ff3a4a;color:#fff;}',
      '#' + MODAL_ID + ' .nex-editor-cancel:hover{background:rgba(40,50,72,.04);}',
      '#' + MODAL_ID + ' .nex-editor-save:hover{background:#f23847;}',
      '@media (max-width:520px){#' + MODAL_ID + ' .nex-tabs{width:108px;padding:10px 6px;}#' + MODAL_ID + ' .nex-content{padding:14px 12px 18px;}}'
    ].join('');
    var style = document.createElement('style');
    style.id = 'nex-simple-style';
    style.textContent = css;
    document.head.appendChild(style);
  }

  function targets(item) {
    var result = item.target ? item.target() : null;
    if (!result) return [];
    return Array.isArray(result) ? result.filter(Boolean) : [result];
  }

  function hide(element, key) {
    if (!element || playlistNode(element)) return;
    if (!hidden.has(element)) hidden.set(element, element.style.display);
    element.style.display = 'none';
    element.setAttribute('data-nex-hidden', key);
  }

  function unhide(key) {
    hidden.forEach(function (original, element) {
      if (element.getAttribute('data-nex-hidden') !== key) return;
      element.style.display = original || '';
      element.removeAttribute('data-nex-hidden');
      hidden.delete(element);
    });
  }

  function apply() {
    ITEMS.forEach(function (item) {
      if (state[item.key]) targets(item).forEach(function (element) { hide(element, item.key); });
      else unhide(item.key);
    });
  }

  function save(key, value) {
    state[key] = value ? 1 : 0;
    try { localStorage.setItem(STORE, JSON.stringify(state)); } catch (e) {}
    var patch = {};
    patch[key] = state[key];
    fetch(API, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(patch) }).catch(function () {});
  }

  function closeModal() {
    var modal = document.getElementById(MODAL_ID);
    if (modal) modal.remove();
    draft = null;
  }

  function saveDraft() {
    if (!draft) return;
    state = Object.assign({}, state, draft);
    try { localStorage.setItem(STORE, JSON.stringify(state)); } catch (e) {}
    fetch(API, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(state)
    }).then(function (response) {
      return response.ok ? response.json() : Promise.reject(response.status);
    }).then(function (ui) {
      KEYS.forEach(function (key) { state[key] = ui[key] ? 1 : 0; });
      apply();
      closeModal();
    }).catch(function () {});
  }

  function renderModal() {
    var modal = document.getElementById(MODAL_ID);
    if (!modal) return;
    modal.querySelectorAll('.nex-tab').forEach(function (tab) {
      tab.setAttribute('data-active', tab.getAttribute('data-group') === activeGroup ? 'true' : 'false');
    });
    var content = modal.querySelector('.nex-content');
    if (!content) return;
    content.innerHTML = '';
    var group = GROUPS.filter(function (entry) { return entry.key === activeGroup; })[0] || GROUPS[0];
    var values = draft || state;
    var details = document.createElement('details');
    details.className = 'nex-group';
    details.open = true;
    var summary = document.createElement('summary');
    summary.textContent = group.title;
    details.appendChild(summary);
    group.items.forEach(function (item) {
      var row = document.createElement('div');
      row.className = 'nex-row';
      var present = targets(item).length > 0;
      if (!present) row.setAttribute('data-missing', 'true');
      var label = document.createElement('span');
      label.textContent = item.label + (present ? '' : '（当前页面没有）');
      row.appendChild(label);
      var button = document.createElement('button');
      button.className = 'nex-visibility';
      button.type = 'button';
      button.innerHTML = '';
      button.setAttribute('data-hidden', values[item.key] ? 'true' : 'false');
      button.setAttribute('aria-pressed', values[item.key] ? 'true' : 'false');
      button.setAttribute('aria-label', values[item.key] ? '显示' + item.label : '隐藏' + item.label);
      button.addEventListener('click', function () {
        draft[item.key] = draft[item.key] ? 0 : 1;
        renderModal();
      });
      row.appendChild(button);
      details.appendChild(row);
    });
    content.appendChild(details);
  }

  function openModal() {
    styleOnce();
    closeModal();
    draft = Object.assign({}, state);
    var modal = document.createElement('div');
    modal.id = MODAL_ID;
    modal.setAttribute('role', 'dialog');
    modal.setAttribute('aria-modal', 'true');
    modal.addEventListener('click', function (event) { if (event.target === modal) closeModal(); });
    var dialog = document.createElement('div');
    dialog.className = 'nex-dialog';
    var head = document.createElement('div');
    head.className = 'nex-dialog-head';
    var title = document.createElement('h2');
    title.className = 'nex-dialog-title';
    title.textContent = '编辑界面';
    head.appendChild(title);
    var close = document.createElement('button');
    close.className = 'nex-close';
    close.type = 'button';
    close.textContent = '×';
    close.setAttribute('aria-label', '关闭编辑界面');
    close.addEventListener('click', closeModal);
    head.appendChild(close);
    dialog.appendChild(head);
    var body = document.createElement('div');
    body.className = 'nex-dialog-body';
    var tabs = document.createElement('nav');
    tabs.className = 'nex-tabs';
    GROUPS.forEach(function (group) {
      var tab = document.createElement('button');
      tab.className = 'nex-tab';
      tab.type = 'button';
      tab.textContent = group.title;
      tab.setAttribute('data-group', group.key);
      tab.addEventListener('click', function () { activeGroup = group.key; renderModal(); });
      tabs.appendChild(tab);
    });
    body.appendChild(tabs);
    var content = document.createElement('div');
    content.className = 'nex-content';
    body.appendChild(content);
    dialog.appendChild(body);
    var foot = document.createElement('div');
    foot.className = 'nex-dialog-foot';
    var cancel = document.createElement('button');
    cancel.className = 'nex-editor-cancel';
    cancel.type = 'button';
    cancel.textContent = '取消';
    cancel.addEventListener('click', closeModal);
    var saveButton = document.createElement('button');
    saveButton.className = 'nex-editor-save';
    saveButton.type = 'button';
    saveButton.textContent = '保存';
    saveButton.addEventListener('click', saveDraft);
    foot.appendChild(cancel);
    foot.appendChild(saveButton);
    dialog.appendChild(foot);
    modal.appendChild(dialog);
    document.body.appendChild(modal);
    renderModal();
  }

  function openNexSettings() {
    if (typeof window.__nexOpenNexSettings === 'function' && window.__nexOpenNexSettings()) return;
    var launcher = document.getElementById('nex-settings-launcher');
    if (launcher) {
      launcher.click();
      return;
    }
    var setting = testId('tid_header_setting_btn');
    if (!setting) return;
    setting.click();
    var attempts = 0;
    var retry = setInterval(function () {
      attempts += 1;
      var next = document.getElementById('nex-settings-launcher');
      if (next) {
        clearInterval(retry);
        next.click();
      } else if (attempts >= 12) {
        clearInterval(retry);
      }
    }, 250);
  }

  function mountEditorButton() {
    var anchor = testId('tid_useravatar_click') || testId('tid_pc_nav_bar_account');
    if (!anchor) return false;
    var oldToolbar = document.getElementById(TOOLBAR_ID);
    if (oldToolbar) oldToolbar.remove();
    if (document.getElementById(HEADER_ENTRY_ID)) return true;
    styleOnce();
    var entry = document.createElement('div');
    entry.id = HEADER_ENTRY_ID;
    var nexButton = document.createElement('button');
    nexButton.id = NEX_BUTTON_ID;
    nexButton.type = 'button';
    nexButton.textContent = 'NexExclusive';
    nexButton.setAttribute('aria-label', '打开 NexExclusive 设置');
    nexButton.title = 'NexExclusive';
    nexButton.addEventListener('click', function (event) { event.preventDefault(); event.stopPropagation(); openNexSettings(); });
    entry.appendChild(nexButton);
    var button = document.createElement('button');
    button.id = EDIT_BUTTON_ID;
    button.type = 'button';
    button.textContent = '编辑界面';
    button.setAttribute('aria-label', '编辑界面');
    button.title = '编辑界面';
    button.addEventListener('click', function (event) { event.preventDefault(); event.stopPropagation(); openModal(); });
    entry.appendChild(button);
    var avatarContainer = anchor.parentElement;
    var insertionParent = avatarContainer && avatarContainer.parentElement;
    if (insertionParent && avatarContainer) insertionParent.insertBefore(entry, avatarContainer);
    else if (anchor.parentElement) anchor.parentElement.insertBefore(entry, anchor);
    return true;
  }

  function mountSettingsPanel() {
    var play = document.querySelector('#play .play');
    if (!play) return false;
    var module = document.getElementById('nex-settings-module');
    if (!module) {
      module = document.createElement('section');
      module.id = 'nex-settings-module';
      module.setAttribute('data-nex-section', 'nexexclusive');
      var moduleTitle = document.createElement('h3');
      moduleTitle.className = 'nex-module-title';
      moduleTitle.textContent = 'NexExclusive';
      module.appendChild(moduleTitle);
      play.appendChild(module);
    }
    if (document.getElementById(PANEL_ID)) return true;
    styleOnce();
    var panel = document.createElement('section');
    panel.id = PANEL_ID;
    panel.setAttribute('data-nex-section', 'interface-simplify');
    var head = document.createElement('div');
    head.className = 'nex-head';
    var title = document.createElement('h4');
    title.textContent = '界面精简';
    head.appendChild(title);
    panel.appendChild(head);
    var open = document.createElement('button');
    open.className = 'nex-open';
    open.type = 'button';
    open.textContent = '打开编辑界面';
    open.addEventListener('click', openModal);
    panel.appendChild(open);
    var content = module.querySelector('.nex-module-content') || module;
    content.appendChild(panel);
    return true;
  }

  function mount() { mountEditorButton(); mountSettingsPanel(); }

  function load() {
    try {
      var cached = localStorage.getItem(STORE);
      if (cached) state = Object.assign(state, JSON.parse(cached));
    } catch (e) {}
    fetch(API).then(function (response) { return response.ok ? response.json() : Promise.reject(response.status); })
      .then(function (ui) {
        KEYS.forEach(function (key) { state[key] = ui[key] ? 1 : 0; });
        try { localStorage.setItem(STORE, JSON.stringify(state)); } catch (e) {}
        apply();
        renderModal();
      }).catch(function () { apply(); });
  }

  function watch() {
    mount();
    apply();
    var pending = null;
    var observer = new MutationObserver(function () {
      if (pending) return;
      pending = setTimeout(function () { pending = null; mount(); apply(); }, 300);
    });
    if (document.body) observer.observe(document.body, { childList: true, subtree: true });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', watch);
  else watch();
  load();
})();
