// 界面精简 - 隐藏首页与侧栏里的运营区块，以及顶栏的两个入口。
//
// 分三组：页面精简、侧栏精简、顶栏精简。每项默认关闭，状态存到
// 宿主的 /api/ui（落到 mod/ui.json），页面重绘后自动重新应用。
// 不修改客户端任何资源文件，关闭开关即刻恢复。
(function () {
  'use strict';
  if (window.__nexSimpleLoaded) return;
  window.__nexSimpleLoaded = true;

  var API = 'http://127.0.0.1:9334/api/ui';
  var PANEL_ID = 'nex-simple-panel';
  var STORE = 'nexsink.ui.mirror';

  var GROUPS = [
    {
      title: '页面精简',
      items: [
        { key: 'banner', label: '精选活动条' },
        { key: 'charts', label: '榜单精选' },
        { key: 'daily', label: '每日推荐' }
      ]
    },
    {
      title: '侧栏精简',
      items: [
        { key: 'podcast', label: '播客入口' },
        { key: 'choice', label: '侧栏精选' }
      ]
    },
    {
      title: '顶栏精简',
      items: [
        { key: 'skin', label: '皮肤按钮' },
        { key: 'pet', label: '宠物按钮' }
      ]
    }
  ];

  var OPTIONS = GROUPS.reduce(function (acc, g) { return acc.concat(g.items); }, []);
  var KEYS = OPTIONS.map(function (o) { return o.key; });

  var state = {};
  OPTIONS.forEach(function (o) { state[o.key] = 0; });

  var hidden = new Map();

  function styleOnce() {
    if (document.getElementById('nex-simple-style')) return;
    var css = [
      '#nex-simple-panel{max-width:460px;margin:0 0 30px;}',
      '#nex-simple-panel .nex-head{display:flex;align-items:center;gap:8px;margin:0 0 10px;}',
      '#nex-simple-panel .nex-head h4{margin:0;font-size:14px;font-weight:700;',
      'line-height:18px;color:rgba(40,50,72,0.9);}',
      '#nex-simple-panel .nex-note{font-size:12px;line-height:18px;',
      'color:rgba(40,50,72,0.45);margin:0 0 4px;}',
      '#nex-simple-panel .nex-group{font-size:12px;line-height:18px;margin:18px 0 8px;',
      'color:rgba(40,50,72,0.45);}',
      '#nex-simple-panel .nex-row{display:flex;flex-direction:column;align-items:flex-start;',
      'min-height:30px;margin:0 0 12px;}',
      '#nex-simple-panel .nex-label{font-size:14px;line-height:20px;margin:0 0 4px;',
      'color:rgba(40,50,72,0.9);}',
      '#nex-simple-panel .nex-switch{width:34px;height:20px;border-radius:10px;border:0;',
      'padding:0;cursor:pointer;background:rgba(40,50,72,0.18);position:relative;',
      'transition:background .15s;flex:none;}',
      '#nex-simple-panel .nex-switch[aria-checked="true"]{background:#FF3A3A;}',
      '#nex-simple-panel .nex-switch::after{content:"";position:absolute;top:2px;left:2px;',
      'width:16px;height:16px;border-radius:50%;background:#fff;transition:left .15s;}',
      '#nex-simple-panel .nex-switch[aria-checked="true"]::after{left:16px;}'
    ].join('');
    var s = document.createElement('style');
    s.id = 'nex-simple-style';
    s.textContent = css;
    document.head.appendChild(s);
  }

  function hide(el, tag) {
    if (!el) return;
    if (!hidden.has(el)) hidden.set(el, el.style.display);
    el.style.display = 'none';
    el.setAttribute('data-nex-hidden', tag);
  }

  function unhideTag(tag) {
    hidden.forEach(function (orig, el) {
      if (el.getAttribute('data-nex-hidden') !== tag) return;
      el.style.display = orig || '';
      el.removeAttribute('data-nex-hidden');
      hidden.delete(el);
    });
  }

  /* 旧版本用过 headerIcons 这个键，升级后把残留标记清掉 */
  function clearLegacy() {
    document.querySelectorAll('[data-nex-hidden]').forEach(function (el) {
      var tag = el.getAttribute('data-nex-hidden');
      if (KEYS.indexOf(tag) >= 0) return;
      el.style.display = '';
      el.removeAttribute('data-nex-hidden');
      hidden.delete(el);
    });
  }

  function grid() {
    var home = document.getElementById('page_pc_home');
    return home && home.firstElementChild ? home.firstElementChild : null;
  }

  function blockByTitle(title) {
    var g = grid();
    if (!g) return null;
    var titles = g.querySelectorAll('.exposure-bar, .block-title');
    for (var i = 0; i < titles.length; i++) {
      if (titles[i].textContent.trim().indexOf(title) !== 0) continue;
      var node = titles[i];
      while (node && node.parentElement !== g) node = node.parentElement;
      if (node) return node;
    }
    return null;
  }

  function cardByTitle(title) {
    var home = document.getElementById('page_pc_home');
    if (!home) return null;
    var rails = home.querySelectorAll('.rail-wrapper .item-main, .rail-wrapper .item-sub');
    for (var i = 0; i < rails.length; i++) {
      var kids = rails[i].children;
      for (var j = 0; j < kids.length; j++) {
        var k = kids[j];
        if (k.textContent.trim().indexOf(title) !== 0) continue;
        /* 卡片宽度远小于整排，避免误伤承载整排的容器 */
        if (k.getBoundingClientRect().width > 320) continue;
        return k;
      }
    }
    return null;
  }

  function byTestId(tid) {
    return document.querySelector('[data-testid="' + tid + '"]');
  }

  /* 顶栏图标外面套着徽标容器，只隐藏图标本身会把红点留在原位。
     向上找到图标栏的直接子节点，整格一起收掉。 */
  function headerSlot(icon) {
    if (!icon) return null;
    var bar = icon.closest('[class*="MiniModeIconBar"], [class*="IconBar"]');
    if (!bar) return icon;
    var node = icon;
    while (node && node.parentElement !== bar) node = node.parentElement;
    return node || icon;
  }

  var RULES = {
    banner: function () { hide(blockByTitle('精选活动'), 'banner'); },
    charts: function () { hide(blockByTitle('榜单精选'), 'charts'); },
    daily: function () { hide(cardByTitle('每日推荐'), 'daily'); },
    podcast: function () {
      hide(byTestId('tid_navitem_audio'), 'podcast');
      hide(byTestId('tid_navitem_myAudio'), 'podcast');
    },
    choice: function () { hide(byTestId('tid_navitem_choice'), 'choice'); },
    skin: function () { hide(headerSlot(byTestId('tid_header_skin_btn')), 'skin'); },
    pet: function () { hide(headerSlot(document.querySelector('[aria-label="mini-pet"]')), 'pet'); }
  };

  function apply() {
    clearLegacy();
    OPTIONS.forEach(function (o) {
      if (state[o.key]) RULES[o.key]();
      else unhideTag(o.key);
    });
  }

  function build() {
    styleOnce();
    var panel = document.createElement('section');
    panel.id = PANEL_ID;

    var head = document.createElement('div');
    head.className = 'nex-head';
    var h4 = document.createElement('h4');
    h4.textContent = '界面精简';
    head.appendChild(h4);
    panel.appendChild(head);

    var note = document.createElement('div');
    note.className = 'nex-note';
    note.textContent = '隐藏后随时可以关回来，不改动客户端任何文件。';
    panel.appendChild(note);

    GROUPS.forEach(function (group) {
      var title = document.createElement('div');
      title.className = 'nex-group';
      title.textContent = group.title;
      panel.appendChild(title);

      group.items.forEach(function (o) {
        var row = document.createElement('div');
        row.className = 'nex-row';

        var label = document.createElement('div');
        label.className = 'nex-label';
        label.textContent = o.label;
        row.appendChild(label);

        var sw = document.createElement('button');
        sw.className = 'nex-switch';
        sw.setAttribute('role', 'switch');
        sw.setAttribute('aria-label', o.label);
        sw.setAttribute('aria-checked', state[o.key] ? 'true' : 'false');
        sw.addEventListener('click', function () {
          var next = sw.getAttribute('aria-checked') !== 'true';
          sw.setAttribute('aria-checked', next ? 'true' : 'false');
          state[o.key] = next ? 1 : 0;
          save(o.key, state[o.key]);
          apply();
        });
        row.appendChild(sw);
        panel.appendChild(row);
      });
    });

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
    var anchor = document.getElementById('nexsink-panel');
    var panel = build();
    if (anchor && anchor.parentElement === play) play.insertBefore(panel, anchor.nextSibling);
    else play.appendChild(panel);
    return true;
  }

  /* 只提交变化的那一项：整份状态回写会让多个页面实例互相覆盖。 */
  function save(key, value) {
    try { localStorage.setItem(STORE, JSON.stringify(state)); } catch (e) {}
    var patch = {};
    patch[key] = value;
    fetch(API, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(patch)
    }).catch(function () {});
  }

  function load() {
    try {
      var cached = localStorage.getItem(STORE);
      if (cached) state = Object.assign(state, JSON.parse(cached));
    } catch (e) {}
    apply();
    fetch(API, { method: 'GET' })
      .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
      .then(function (ui) {
        KEYS.forEach(function (k) { state[k] = ui[k] ? 1 : 0; });
        try { localStorage.setItem(STORE, JSON.stringify(state)); } catch (e) {}
        apply();
        var play = document.querySelector('#play .play');
        var panel = document.getElementById(PANEL_ID);
        if (play && panel) play.replaceChild(build(), panel);
      })
      .catch(function () {});
  }

  function watch() {
    mount();
    apply();
    var pending = null;
    var obs = new MutationObserver(function () {
      if (pending) return;
      pending = setTimeout(function () {
        pending = null;
        if (!document.getElementById(PANEL_ID)) mount();
        apply();
      }, 300);
    });
    obs.observe(document.body, { childList: true, subtree: true });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', watch);
  else watch();
  load();
})();
