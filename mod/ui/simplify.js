// NexExclusive interface editor. The client keeps its layout; this script adds
// one native-style entry point and a modal for optional visibility rules.
(function () {
  'use strict';
  var SCRIPT_VERSION = 'header-modal-v46';
  if (window.__nexSimpleLoaded === SCRIPT_VERSION) return;
  window.__nexSimpleLoaded = SCRIPT_VERSION;
  /* 重新注入时让上一代脚本停手，不然两代的观察器一起改动同一块页面，状态会互相踩。 */
  var GEN = (window.__nexSimpleGen = (window.__nexSimpleGen || 0) + 1);
  function alive() { return window.__nexSimpleGen === GEN; }

  var API = 'http://127.0.0.1:9334/api/ui';
  var PANEL_ID = 'nex-simple-panel';
  var TOOLBAR_ID = 'nex-simple-toolbar';
  var HEADER_ENTRY_ID = 'nex-simple-header-entry';
  var NEX_BUTTON_ID = 'nex-simple-nex-button';
  var EDIT_BUTTON_ID = 'nex-simple-edit-toggle';
  var MODAL_ID = 'nex-simple-modal';
  var STORE = 'nexsink.ui.mirror';
  var RULES_STORE = 'nexsink.ui.rules';
  var RULES_STYLE_ID = 'nex-simple-rules';
  var PICKER_ID = 'nex-simple-picker';
  var PICK_HINT_ID = 'nex-simple-pick-hint';

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

  /* 右侧大页的区块是同一个容器下的同级节点，往上找到「父级等于页面根」时
     会停在那个容器上，藏起来就是整页消失。这里取「只含一个标题组的最外层祖先」。 */
  function titleGroups(root) {
    var scope = root || grid();
    if (!scope) return [];
    var nodes = scope.querySelectorAll('.exposure-bar, .block-title');
    var groups = [];
    for (var i = 0; i < nodes.length; i++) {
      var nested = false;
      for (var j = 0; j < groups.length; j++) {
        if (groups[j].contains(nodes[i])) { nested = true; break; }
        if (nodes[i].contains(groups[j])) groups.splice(j, 1);
      }
      if (!nested) groups.push(nodes[i]);
    }
    return groups;
  }

  function sectionOf(group, groups, root) {
    var scope = root || grid();
    if (!scope || group === scope) return null;
    var node = group;
    var best = null;
    while (node && node !== scope) {
      var inside = 0;
      for (var i = 0; i < groups.length; i++) if (node.contains(groups[i])) inside += 1;
      if (inside !== 1) break;
      best = node;
      node = node.parentElement;
    }
    return best === scope ? null : best;
  }

  function containsMultipleSections(element) {
    var groups = titleGroups();
    var inside = 0;
    for (var i = 0; i < groups.length; i++) if (element.contains(groups[i])) inside += 1;
    return inside > 1;
  }

  function blockByText(text, suffix) {
    var groups = titleGroups();
    for (var i = 0; i < groups.length; i++) {
      var value = (groups[i].textContent || '').replace(/Loading\.\.\./g, '').trim();
      var match = suffix ? value.slice(-text.length) === text : value.indexOf(text) === 0;
      if (!match) continue;
      var node = sectionOf(groups[i], groups);
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
    if (!node) return element;
    /* 这一层里还装着别的图标时不能藏它：窗口的最小化、最大化、关闭就和 mini 图标同在一排。 */
    var icons = node.querySelectorAll('[class*="cmd-icon"], [aria-label]');
    if (icons.length > 1) return element;
    return node;
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
  var MAIN_ITEMS = GROUPS.filter(function (group) { return group.key === 'main'; })[0].items;
  var KEYS = ITEMS.map(function (item) { return item.key; });
  var state = {};
  ITEMS.forEach(function (item) { state[item.key] = 0; });
  var presetPaths = {};
  var activeGroup = 'sidebar';
  var draft = null;
  var picking = null;

  /* ---- 自定义条目：位置写进 CSS ------------------------------------------ */

  function loadRules() {
    try {
      var raw = localStorage.getItem(RULES_STORE);
      var list = raw ? JSON.parse(raw) : [];
      if (!list || !list.length) return [];
      return list.filter(function (entry) { return entry && typeof entry.path === 'string' && entry.path; });
    } catch (e) { return []; }
  }

  var rules = loadRules();

  function saveRules() {
    try { localStorage.setItem(RULES_STORE, JSON.stringify(rules)); } catch (e) {}
    applyRules();
  }

  function applyRules() {
    if (!document.head) return;
    var paths = rules.map(function (entry) { return entry.path; });
    Object.keys(presetPaths).forEach(function (key) { paths.push(presetPaths[key]); });
    var css = paths.map(function (path) { return path + '{display:none !important;}'; }).join('\n');
    var style = document.getElementById(RULES_STYLE_ID);
    if (!style) {
      style = document.createElement('style');
      style.id = RULES_STYLE_ID;
      document.head.appendChild(style);
    }
    if (style.textContent !== css) style.textContent = css;
  }

  /* 只认 id、data-testid 和层级位置。客户端类名带哈希，升级之后会换。 */
  function anchorOf(node) {
    if (node.id) return '#' + node.id;
    var testid = node.getAttribute ? node.getAttribute('data-testid') : '';
    return testid ? '[data-testid="' + testid + '"]' : '';
  }

  function pathOf(element) {
    var parts = [];
    var node = element;
    while (node && node.nodeType === 1 && node !== document.body) {
      var anchor = anchorOf(node);
      if (anchor) { parts.unshift(anchor); break; }
      var index = 1;
      var sib = node;
      while ((sib = sib.previousElementSibling)) if (sib.tagName === node.tagName) index += 1;
      var total = 0;
      var siblings = node.parentElement ? node.parentElement.children : [];
      for (var i = 0; i < siblings.length; i++) if (siblings[i].tagName === node.tagName) total += 1;
      var tag = node.tagName.toLowerCase();
      parts.unshift(total > 1 ? tag + ':nth-of-type(' + index + ')' : tag);
      node = node.parentElement;
    }
    if (!parts.length) return '';
    var first = parts[0];
    return first.charAt(0) === '#' || first.charAt(0) === '[' ? parts.join(' > ') : 'body > ' + parts.join(' > ');
  }

  function pageOf(node) {
    var page = node && node.closest ? node.closest('[id^="page_pc_"]') : null;
    return page && page.id ? page.id : '';
  }

  function labelOf(element) {
    var text = (element.textContent || '').replace(/\s+/g, ' ').trim();
    return text.slice(0, 16) || element.tagName.toLowerCase();
  }

  function chainOf(node) {
    var chain = [];
    var cur = node;
    while (cur && cur.nodeType === 1 && cur !== document.body) {
      chain.push(cur);
      cur = cur.parentElement;
    }
    return chain;
  }

  /* 整页容器记下来就等于把客户端清空。选区可以到区块，再往上就到这里为止。 */
  function wholePageTarget(node) {
    if (!node || node.nodeType !== 1) return true;
    if (node.id === 'root' || node.id === 'body') return true;
    if (node.id && node.id.indexOf('page_pc_') === 0) return true;
    if (node.querySelectorAll('[id^="page_pc_"]').length > 1) return true;
    return containsMultipleSections(node);
  }

  /* 右侧大页：表格不写死，按当前停着的那一页现列区块。
     同一时刻页面上可能挂着几个 page_pc_*，只认屏幕落点真正命中的那一个。 */
  function reservedPage(id) {
    return id === 'page_pc_main_tab' || id === 'page_pc_main_nav' || id === 'page_pc_mini_bar';
  }

  function visiblePage() {
    var w = window.innerWidth || 0;
    var h = window.innerHeight || 0;
    var hits = {};
    var order = [];
    var points = [[0.2, 0.25], [0.5, 0.25], [0.8, 0.25], [0.2, 0.55], [0.5, 0.55], [0.8, 0.55], [0.2, 0.85], [0.5, 0.85], [0.8, 0.85]];
    for (var i = 0; i < points.length; i++) {
      var node = document.elementFromPoint(Math.round(w * points[i][0]), Math.round(h * points[i][1]));
      if (!node || (node.closest && node.closest('#' + MODAL_ID))) continue;
      var page = node.closest ? node.closest('[id^="page_pc_"]') : null;
      if (!page || reservedPage(page.id)) continue;
      if (page.getBoundingClientRect().height < 80) continue;
      if (!hits[page.id]) { hits[page.id] = { page: page, count: 0 }; order.push(page.id); }
      hits[page.id].count += 1;
    }
    var best = null;
    for (var j = 0; j < order.length; j++) {
      var entry = hits[order[j]];
      if (!best) { best = entry; continue; }
      if (entry.count > best.count) best = entry;
      else if (entry.count === best.count && entry.page.getBoundingClientRect().height > best.page.getBoundingClientRect().height) best = entry;
    }
    if (best) return best.page;
    var pages = document.querySelectorAll('[id^="page_pc_"]');
    for (var k = 0; k < pages.length; k++) {
      if (reservedPage(pages[k].id)) continue;
      if (pages[k].getBoundingClientRect().height < 80) continue;
      return pages[k];
    }
    return null;
  }

  /* 区块标题在两套写法之间：首页那套是 .exposure-bar / .block-title，
     精选、播客这些页用的是类名里带 title 的小标题行。
     先只认正式的分块标题；一处都没有，才退到类名里带 title 的松体，免得把卡片标题当成区块。 */
  function titleCandidates(root) {
    var strong = root.querySelectorAll('.exposure-bar, .block-title, [class*="essential-title"], [class*="TitleLink"], [class*="sectionTitle"], [class*="blockTitle"]');
    var nodes = strong.length ? strong : root.querySelectorAll('[class*="title" i]');
    var limit = strong.length ? 120 : 64;
    var textLimit = strong.length ? 60 : 20;
    var picked = [];
    for (var i = 0; i < nodes.length; i++) {
      var node = nodes[i];
      var cls = (node.className || '').toString();
      if (!cls) continue;
      var height = node.getBoundingClientRect().height;
      if (height > 0 && (height < 12 || height > limit)) continue;
      var text = (node.textContent || '').replace(/\s+/g, '');
      if (!text || text.length > textLimit) continue;
      picked.push(node);
    }
    var groups = [];
    for (var m = 0; m < picked.length; m++) {
      var nested = false;
      for (var n = 0; n < groups.length; n++) {
        if (groups[n].contains(picked[m])) { nested = true; break; }
        if (picked[m].contains(groups[n])) groups.splice(n, 1);
      }
      if (!nested) groups.push(picked[m]);
    }
    return groups;
  }

  function labelOfGroup(group) {
    var first = group.firstElementChild;
    var raw = ((first && first.textContent) || group.textContent || '').replace(/\s+/g, '').trim();
    var cut = raw.split(/[，,｜|]/)[0].replace(/播放$/, '');
    if (cut.length > 8) cut = cut.slice(0, 8) + '…';
    return cut || '未命名区块';
  }

  /* 一页一类：有标题行就按标题取块，取不到就退到页面里的大块。 */
  var blocksCoarse = false;
  function unitLabel(node) {
    var hits = node.querySelectorAll('.title-wrap, [class*="title" i]');
    for (var i = 0; i < hits.length; i++) {
      var text = (hits[i].textContent || '').replace(/\s+/g, '');
      if (text && text.length <= 10 && hits[i].getBoundingClientRect().height > 0) return text;
    }
    return labelOfGroup(node);
  }

  function pageBlocks() {
    blocksCoarse = false;
    var page = visiblePage();
    if (!page) return [];
    var groups = titleCandidates(page);
    var blocks = [];
    var i;
    for (i = 0; i < groups.length; i++) {
      var node = sectionOf(groups[i], groups, page);
      if (!node || node === page) continue;
      if (playlistNode(node) || wholePageTarget(node)) continue;
      var path = pathOf(node);
      /* 被规则藏起来的块在页面上高度是 0，还得留在表里，不然关不回去。 */
      if (node.getBoundingClientRect().height < 80 && ruleIndex(path) < 0) continue;
      blocks.push({ node: node, path: path, page: page.id, label: labelOfGroup(groups[i]) });
      if (blocks.length >= 12) break;
    }
    if (blocks.length) {
      /* 同一层的兄弟里，标题认不出来的那些（首页第一块 banner 就是这种）也补进来，
         称号从块里最靠前的小标题取。 */
      var parent = blocks[0].node.parentElement;
      var same = !!parent && parent !== page;
      for (i = 0; same && i < blocks.length; i++) if (blocks[i].node.parentElement !== parent) same = false;
      if (same) {
        var mates = parent.children;
        if (mates.length > blocks.length && mates.length <= blocks.length + 4) {
          for (i = 0; i < mates.length; i++) {
            var mate = mates[i];
            var matePath = pathOf(mate);
            if (mate.getBoundingClientRect().height < 80 && ruleIndex(matePath) < 0) continue;
            if (wholePageTarget(mate) || playlistNode(mate)) continue;
            var listed = false;
            for (var b = 0; b < blocks.length; b++) if (blocks[b].node === mate) { listed = true; break; }
            if (listed) continue;
            blocks.push({ node: mate, path: matePath, page: page.id, label: unitLabel(mate) });
          }
        }
      }
      blocks.sort(function (a, b) {
        if (a.node === b.node) return 0;
        return a.node.compareDocumentPosition(b.node) & 4 ? -1 : 1;
      });
      return blocks.slice(0, 12);
    }
    blocksCoarse = true;
    var kids = page.children;
    for (i = 0; i < kids.length; i++) {
      var child = kids[i];
      if (child.getBoundingClientRect().height < 80) continue;
      if (wholePageTarget(child)) continue;
      blocks.push({ node: child, path: pathOf(child), page: page.id, label: labelOfGroup(child) });
      if (blocks.length >= 12) break;
    }
    return blocks;
  }

  function ruleIndex(path) {
    for (var i = 0; i < rules.length; i++) if (rules[i].path === path) return i;
    return -1;
  }

  function toggleRule(path, label, page) {
    var index = ruleIndex(path);
    if (index >= 0) rules.splice(index, 1);
    else rules.push({ path: path, page: page, label: label });
    saveRules();
    renderModal();
  }

  function pickTarget() {
    if (!picking || !picking.hovered) return null;
    var chain = chainOf(picking.hovered);
    return chain[Math.min(picking.up, chain.length - 1)] || null;
  }

  function paintPick() {
    var box = document.getElementById(PICKER_ID);
    var hint = document.getElementById(PICK_HINT_ID);
    var target = pickTarget();
    if (!box || !hint || !target) return;
    var rect = target.getBoundingClientRect();
    var blocked = wholePageTarget(target);
    box.style.left = rect.left + 'px';
    box.style.top = rect.top + 'px';
    box.style.width = rect.width + 'px';
    box.style.height = rect.height + 'px';
    box.setAttribute('data-blocked', blocked ? 'true' : 'false');
    hint.textContent = target.tagName.toLowerCase() + ' · 高 ' + Math.round(rect.height) +
      (picking.up ? ' · 已上移 ' + picking.up + ' 层' : '') +
      (blocked ? ' · 这一层是整页容器，记不下' : '') +
      '\n↑ / ↓ 换层级，回车记下这一块，Esc 退出';
  }

  function onPickMove(event) {
    if (!picking || !event.clientX && !event.clientY) return;
    var node = document.elementFromPoint(event.clientX, event.clientY);
    if (!node || node.id === PICKER_ID || node.id === PICK_HINT_ID) return;
    if (node !== picking.hovered) { picking.hovered = node; picking.up = 0; }
    paintPick();
  }

  function onPickKey(event) {
    if (!picking) return;
    var target = pickTarget();
    var chain = picking.hovered ? chainOf(picking.hovered) : [];
    if (event.key === 'ArrowUp') {
      var outer = chain[picking.up + 1];
      if (outer && !wholePageTarget(outer)) picking.up += 1;
    } else if (event.key === 'ArrowDown') {
      if (picking.up > 0) picking.up -= 1;
    } else if (event.key === 'Enter') {
      commitPick();
      event.preventDefault();
      return;
    } else if (event.key === 'Escape') {
      stopPick();
      renderModal();
      event.preventDefault();
      return;
    } else {
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    if (!target) return;
    paintPick();
  }

  function startPick() {
    if (picking) return;
    styleOnce();
    stopPick();
    var modal = document.getElementById(MODAL_ID);
    if (modal) modal.style.display = 'none';
    var box = document.createElement('div');
    box.id = PICKER_ID;
    document.body.appendChild(box);
    var hint = document.createElement('div');
    hint.id = PICK_HINT_ID;
    hint.textContent = '移动鼠标选择区块，↑ / ↓ 换层级，回车记下这一块，Esc 退出';
    document.body.appendChild(hint);
    picking = { hovered: null, up: 0 };
    document.addEventListener('mousemove', onPickMove, true);
    document.addEventListener('keydown', onPickKey, true);
    document.body.style.cursor = 'crosshair';
  }

  function stopPick() {
    var box = document.getElementById(PICKER_ID);
    if (box) box.remove();
    var hint = document.getElementById(PICK_HINT_ID);
    if (hint) hint.remove();
    document.removeEventListener('mousemove', onPickMove, true);
    document.removeEventListener('keydown', onPickKey, true);
    if (picking) document.body.style.cursor = '';
    var modal = document.getElementById(MODAL_ID);
    if (modal) modal.style.display = '';
    picking = null;
  }

  function commitPick() {
    var target = pickTarget();
    var path = target && !wholePageTarget(target) ? pathOf(target) : '';
    if (path) {
      var exists = false;
      for (var i = 0; i < rules.length; i++) if (rules[i].path === path) exists = true;
      if (!exists) {
        rules.push({ path: path, page: pageOf(target), label: labelOf(target) });
        saveRules();
      }
    }
    stopPick();
    renderModal();
  }

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
      '#' + HOME_BUTTON_ID + '{display:inline-flex;align-items:center;justify-content:center;width:auto;height:28px;padding:0 12px;border:1px solid rgba(40,50,72,.1);border-radius:14px;background:rgba(40,50,72,.03);color:rgba(40,50,72,.9);font:inherit;font-size:14px;line-height:18px;white-space:nowrap;cursor:pointer;}',
      '#' + HOME_BUTTON_ID + ':hover,#' + HOME_BUTTON_ID + ':focus-visible{border-color:rgba(255,58,58,.55);background:rgba(255,255,255,.65);color:#d73535;outline:0;}',
      '#' + HOME_VIEW_ID + '{position:fixed;right:0;bottom:0;z-index:2147483100;display:none;overflow:auto;background:rgb(252,253,255);color:rgba(40,50,72,.9);}',
      '#' + HOME_VIEW_ID + ' .nex-home-flow{display:flex;flex-direction:column;gap:26px;padding:22px 40px 64px;}',
      '#' + HOME_VIEW_ID + ' .nex-home-unit{position:relative;}',
      /* 块头那一行在页面里靠祖上的选择器才拿到 flex，搬到首页之后落到基础规则 block，
         标题和右侧按钮就上下错开。把这一层显式摆回横排。 */
      '#' + HOME_VIEW_ID + ' .nex-home-unit .exposure-bar{display:flex;align-items:flex-start;}',
      '#' + HOME_VIEW_ID + ' .nex-home-unit-head{display:flex;align-items:center;gap:8px;margin-bottom:4px;font-size:11px;color:rgba(40,50,72,.4);}',
      '#' + HOME_VIEW_ID + ' .nex-home-unit-remove{margin-left:auto;width:20px;height:20px;border:0;border-radius:50%;background:transparent;color:rgba(40,50,72,.38);font-size:13px;line-height:18px;cursor:pointer;}',
      '#' + HOME_VIEW_ID + ' .nex-home-unit-remove:hover{background:rgba(198,33,48,.1);color:#d73535;}',
      '#' + HOME_VIEW_ID + ' .nex-home-empty{margin:0;font-size:13px;line-height:1.8;color:rgba(40,50,72,.5);}',
      '#' + FOCUS_EXIT_ID + '{position:fixed;left:50%;bottom:26px;transform:translateX(-50%);z-index:2147483060;height:32px;padding:0 16px;border:1px solid rgba(40,50,72,.12);border-radius:16px;background:rgb(252,253,255);color:rgba(40,50,72,.8);font:inherit;font-size:12px;box-shadow:0 6px 18px rgba(40,50,72,.16);cursor:pointer;}',
      '#' + FOCUS_EXIT_ID + ':hover{border-color:rgba(255,58,58,.5);color:#d73535;}',
      '#' + MODAL_ID + ' .nex-home-add{flex:0 0 auto;height:24px;margin-left:8px;padding:0 9px;border:1px solid rgba(40,50,72,.12);border-radius:12px;background:transparent;color:rgba(40,50,72,.62);font:inherit;font-size:11px;cursor:pointer;}',
      '#' + MODAL_ID + ' .nex-home-add:hover{border-color:rgba(255,58,58,.45);color:#d73535;}',
      '#' + MODAL_ID + ' .nex-home-add[data-on="true"]{border-color:rgba(255,58,58,.45);background:rgba(255,58,58,.08);color:#d73535;}',
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
      '#' + PICKER_ID + '{position:fixed;z-index:2147483600;pointer-events:none;box-sizing:border-box;border:2px solid #ff3a4a;border-radius:4px;background:rgba(255,58,74,.10);}',
      '#' + PICKER_ID + '[data-blocked="true"]{border-color:#8a8f99;background:rgba(138,143,153,.14);}',
      '#' + PICK_HINT_ID + '{position:fixed;left:50%;bottom:28px;z-index:2147483601;max-width:calc(100vw - 48px);padding:8px 14px;border-radius:6px;background:rgba(24,31,44,.86);color:#fff;font-size:12px;line-height:18px;white-space:pre;pointer-events:none;transform:translateX(-50%);}',
      '#' + MODAL_ID + ' .nex-rule-list{margin:10px 0 0;}',
      '#' + MODAL_ID + ' .nex-rule{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:8px 0;border-bottom:1px solid #edf0f4;}',
      '#' + MODAL_ID + ' .nex-rule-text{display:flex;flex-direction:column;gap:2px;min-width:0;}',
      '#' + MODAL_ID + ' .nex-rule-name{font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:rgba(40,50,72,.9);}',
      '#' + MODAL_ID + ' .nex-rule-path{font-size:11px;line-height:15px;color:rgba(40,50,72,.42);word-break:break-all;}',
      '#' + MODAL_ID + ' .nex-rule-drop{flex:0 0 auto;width:24px;height:24px;padding:0;border:0;border-radius:50%;background:transparent;color:rgba(40,50,72,.5);font-size:16px;line-height:24px;cursor:pointer;}',
      '#' + MODAL_ID + ' .nex-rule-clear{display:inline-flex;align-items:center;height:30px;padding:0 12px;margin:0 0 12px;border:1px solid rgba(40,50,72,.12);border-radius:15px;background:rgba(40,50,72,.04);color:rgba(40,50,72,.62);font-size:12px;cursor:pointer;}',
      '#' + MODAL_ID + ' .nex-rule-drop:hover{background:rgba(40,50,72,.08);color:#d73535;}',
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

  /* 老版本把 display:none 写在节点上，会被 React 重新渲染冲掉。这里顺手清掉残留。 */
  var legacyCleared = false;
  function clearLegacyInline() {
    if (legacyCleared || !document.querySelectorAll) return;
    legacyCleared = true;
    var nodes = document.querySelectorAll('[data-nex-hidden]');
    for (var i = 0; i < nodes.length; i++) {
      nodes[i].style.display = '';
      nodes[i].removeAttribute('data-nex-hidden');
    }
  }

  function usableTarget(element) {
    return element && !playlistNode(element) && !containsMultipleSections(element) && !wholePageTarget(element);
  }

  /* 预设开关也不再往节点上写内联样式：解析出节点，换算成路径，写进 CSS。
     节点被重新渲染之后 CSS 仍然命中，不需要每一轮都去补样式。 */
  function apply() {
    var changed = false;
    ITEMS.forEach(function (item) {
      var path = '';
      if (state[item.key]) {
        var list = targets(item).filter(usableTarget);
        path = list.length ? pathOf(list[0]) : '';
      }
      if ((presetPaths[item.key] || '') !== path) {
        if (path) presetPaths[item.key] = path;
        else delete presetPaths[item.key];
        changed = true;
      }
    });
    clearLegacyInline();
    if (changed) applyRules();
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

  function renderRules(content) {
    var hint = document.createElement('p');
    hint.className = 'nex-hint';
    hint.textContent = '「选区添加」之后在页面上移动鼠标：↑ 放大到上一层，↓ 收回到下一层，回车记下这一块。记下的位置写成 CSS，页面重新渲染后依然生效。';
    content.appendChild(hint);
    var start = document.createElement('button');
    start.className = 'nex-open';
    start.type = 'button';
    start.textContent = '选区添加';
    start.addEventListener('click', startPick);
    content.appendChild(start);
    if (rules.length) {
      var clear = document.createElement('button');
      clear.className = 'nex-rule-clear';
      clear.type = 'button';
      clear.textContent = '全部清除';
      clear.addEventListener('click', function () {
        rules.length = 0;
        saveRules();
        renderModal();
      });
      content.appendChild(clear);
    }
    var list = document.createElement('div');
    list.className = 'nex-rule-list';
    if (!rules.length) {
      var empty = document.createElement('p');
      empty.className = 'nex-hint';
      empty.textContent = '还没有自定义条目。';
      list.appendChild(empty);
    }
    rules.forEach(function (entry, index) {
      var row = document.createElement('div');
      row.className = 'nex-rule';
      var text = document.createElement('div');
      text.className = 'nex-rule-text';
      var name = document.createElement('span');
      name.className = 'nex-rule-name';
      name.textContent = (entry.page ? entry.page + ' · ' : '') + (entry.label || entry.path);
      var path = document.createElement('span');
      path.className = 'nex-rule-path';
      path.textContent = entry.path;
      text.appendChild(name);
      text.appendChild(path);
      row.appendChild(text);
      var drop = document.createElement('button');
      drop.className = 'nex-rule-drop';
      drop.type = 'button';
      drop.textContent = '×';
      drop.setAttribute('aria-label', '删除 ' + (entry.label || entry.path));
      drop.addEventListener('click', function () {
        rules.splice(index, 1);
        saveRules();
        renderModal();
      });
      row.appendChild(drop);
      list.appendChild(row);
    });
    content.appendChild(list);
  }

  /* 按当前大页现列区块：一行一块，开关写进同一份规则。 */
  function renderBlockRows(content) {
    var blocks = pageBlocks();
    var hint = document.createElement('p');
    hint.className = 'nex-hint';
    hint.textContent = !blocks.length
      ? '这一页没找到能列出来的区块，用「自定义」里的选区自己挑。'
      : (blocksCoarse
        ? '这一页没有认得出的分块标题，下面按页面里的大块来列；要更细的块，用「自定义」里的选区。'
        : '下表跟当前停着的大页走：一行是一块，开关记的就是这一页的这一块。换一页，表也跟着换。');
    content.appendChild(hint);
    if (!blocks.length) return;
    var list = document.createElement('div');
    list.className = 'nex-group';
    var seen = {};
    blocks.forEach(function (block) {
      if (seen[block.path]) return;
      seen[block.path] = true;
      var row = document.createElement('div');
      row.className = 'nex-row';
      var label = document.createElement('span');
      label.textContent = block.label;
      row.appendChild(label);
      var button = document.createElement('button');
      button.className = 'nex-visibility';
      button.type = 'button';
      button.innerHTML = '';
      var stored = ruleIndex(block.path) >= 0;
      button.setAttribute('data-hidden', stored ? 'true' : 'false');
      button.setAttribute('aria-pressed', stored ? 'true' : 'false');
      button.setAttribute('aria-label', (stored ? '显示' : '隐藏') + block.label);
      button.addEventListener('click', function () { toggleRule(block.path, block.label, block.page); });
      row.appendChild(button);
      var pin = document.createElement('button');
      pin.className = 'nex-home-add';
      pin.type = 'button';
      var collected = inHome(block.path);
      pin.textContent = collected ? '已收' : '＋首页';
      pin.setAttribute('data-on', collected ? 'true' : 'false');
      pin.setAttribute('aria-label', (collected ? '移出首页：' : '收进首页：') + block.label);
      pin.addEventListener('click', function () { collectBlock(block); });
      row.appendChild(pin);
      list.appendChild(row);
    });
    content.appendChild(list);
  }

  /* 早先那 10 个 main_* 键靠标题文案定位，文案一改就指空。把它们换成位置规则。
     只认 /api/ui 读回来的值：localStorage 里可能还留着更早的一份，照它迁移会把旧设置复活。 */
  var mainMigrated = false;
  var stateReady = false;
  function migrateMainKeys() {
    if (!stateReady || mainMigrated || !grid()) return false;
    mainMigrated = true;
    var patch = {};
    var moved = false;
    MAIN_ITEMS.forEach(function (item) {
      if (!state[item.key]) return;
      var node = targets(item).filter(usableTarget)[0];
      var path = node ? pathOf(node) : '';
      if (path && ruleIndex(path) < 0) rules.push({ path: path, page: pageOf(node), label: item.label });
      state[item.key] = 0;
      patch[item.key] = 0;
      moved = true;
    });
    if (!moved) return false;
    try { localStorage.setItem(RULES_STORE, JSON.stringify(rules)); } catch (e) {}
    applyRules();
    fetch(API, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(patch) }).catch(function () {});
    return true;
  }

  /* 切了大页之后，「右侧大页」那张表要跟着换。 */
  var lastPageId = '';
  function refreshPageTab() {
    var page = visiblePage();
    var id = page ? page.id : '';
    if (id === lastPageId) return;
    lastPageId = id;
    if (activeGroup === 'main' && document.getElementById(MODAL_ID)) renderModal();
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
    if (activeGroup === 'custom') {
      renderRules(content);
      return;
    }
    var group = GROUPS.filter(function (entry) { return entry.key === activeGroup; })[0] || GROUPS[0];
    if (group.key === 'main') {
      renderBlockRows(content);
      return;
    }
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
    /* 首页把各页藏掉、把块搬进自己的容器里；这时按页面去列块会读到一片空，也会把
       已经搬走的节点再走一遍。先退回真实页面，再开弹窗。 */
    if (homeActive) {
      deactivateHome();
      setTimeout(openModal, 320);
      return;
    }
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
    var ruleTab = document.createElement('button');
    ruleTab.className = 'nex-tab';
    ruleTab.type = 'button';
    ruleTab.textContent = '自定义';
    ruleTab.setAttribute('data-group', 'custom');
    ruleTab.addEventListener('click', function () { activeGroup = 'custom'; renderModal(); });
    tabs.appendChild(ruleTab);
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

  function waitForSettingsLauncher() {
    var attempts = 0;
    var retry = setInterval(function () {
      attempts += 1;
      var module = document.getElementById('nex-settings-module');
      var launcher = document.getElementById('nex-settings-launcher');
      var opener = window.__nexOpenNexSettings;
      if (module && typeof opener === 'function') {
        clearInterval(retry);
        opener();
        return;
      }
      if (module && launcher) {
        clearInterval(retry);
        launcher.click();
        return;
      }
      if (attempts >= 40) clearInterval(retry);
    }, 250);
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
    waitForSettingsLauncher();
  }

  /* ---------- 我的首页：左栏一级页，右边内容由收进来的真实区块拼成 ---------- */
  var HOME_STORE = 'nexsink.ui.home';
  var HOME_BUTTON_ID = 'nex-simple-home-toggle';
  var HOME_VIEW_ID = 'nex-home-view';
  var HOME_NAV_ID = 'nex-home-navitem';
  var HOME_HIDE_ID = 'nex-home-page-hide';
  var FOCUS_STYLE_ID = 'nex-simple-focus';
  var FOCUS_EXIT_ID = 'nex-simple-focus-exit';
  var focusPath = '';
  var homeActive = false;
  var homePageSnapshot = '';
  var heldBlocks = [];
  var PAGE_NAMES = {
    page_pc_home: '推荐', page_pc_choice: '精选', page_pc_music_rcmd: '精选',
    page_pc_music_rcmd_subpage: '精选',
    page_pc_music_rcmd_subpage: '精选', page_pc_voice: '播客',
    page_pc_privateFm: '漫游', page_pc_friend: '关注'
  };
  var PAGE_NAV = {
    page_pc_home: 'tid_navitem_recommend', page_pc_choice: 'tid_navitem_choice',
    page_pc_music_rcmd: 'tid_navitem_choice', page_pc_music_rcmd_subpage: 'tid_navitem_choice', page_pc_voice: 'tid_navitem_audio',
    page_pc_privateFm: 'tid_navitem_privateFm', page_pc_friend: 'tid_navitem_community'
  };

  function pageName(id) { return PAGE_NAMES[id] || String(id || '').replace('page_pc_', ''); }

  function loadHome() {
    try {
      var raw = localStorage.getItem(HOME_STORE);
      var list = raw ? JSON.parse(raw) : [];
      if (!list || !list.length) return [];
      return list.filter(function (entry) { return entry && typeof entry.path === 'string' && entry.path; });
    } catch (e) { return []; }
  }

  var homeItems = loadHome();

  function saveHome() {
    try { localStorage.setItem(HOME_STORE, JSON.stringify(homeItems)); } catch (e) {}
  }

  function inHome(path) {
    for (var i = 0; i < homeItems.length; i++) if (homeItems[i].path === path) return true;
    return false;
  }

  function collectBlock(block) {
    if (!block || !block.path) return;
    if (inHome(block.path)) homeItems = homeItems.filter(function (entry) { return entry.path !== block.path; });
    else homeItems.push({ path: block.path, page: block.page, label: block.label });
    saveHome();
    refreshHomeView();
    renderModal();
  }

  function clearFocus() {
    focusPath = '';
    var style = document.getElementById(FOCUS_STYLE_ID);
    if (style) style.remove();
    var exit = document.getElementById(FOCUS_EXIT_ID);
    if (exit) exit.remove();
  }

  /* 只留一块：把目标往上每一层的兄弟节点藏掉，页面里只留通向那一块的那一支。 */
  function focusOn(target, label) {
    clearFocus();
    if (!target) return false;
    var page = target.closest ? target.closest('[id^="page_pc_"]') : null;
    if (!page) return false;
    var css = [];
    var cur = target;
    while (cur && cur.parentElement && cur !== page) {
      var parent = cur.parentElement;
      var kids = parent.children;
      for (var i = 0; i < kids.length; i++) {
        if (kids[i] !== cur) css.push(pathOf(kids[i]) + '{display:none !important;}');
      }
      if (parent === page) break;
      cur = parent;
    }
    var style = document.createElement('style');
    style.id = FOCUS_STYLE_ID;
    style.textContent = css.join('\n');
    document.head.appendChild(style);
    focusPath = pathOf(target);
    var exit = document.createElement('button');
    exit.id = FOCUS_EXIT_ID;
    exit.type = 'button';
    exit.textContent = '只看：' + (label || '这一块') + '   × 退出';
    exit.addEventListener('click', function (event) { event.preventDefault(); event.stopPropagation(); clearFocus(); });
    document.body.appendChild(exit);
    return true;
  }

  /* 内容区从顶部导航的下沿、侧栏的右边开始，不要盖住侧栏。 */
  function contentRect() {
    var nav = document.getElementById('page_pc_main_nav');
    var left = 0;
    var top = 0;
    if (nav) {
      var navRect = nav.getBoundingClientRect();
      left = navRect.left;
      top = navRect.bottom;
    }
    return { left: left, top: top };
  }

  function ensureHomeView() {
    var view = document.getElementById(HOME_VIEW_ID);
    if (view) return view;
    view = document.createElement('div');
    view.id = HOME_VIEW_ID;
    var rect = contentRect();
    view.style.left = Math.round(rect.left) + 'px';
    view.style.top = Math.round(rect.top) + 'px';
    var flow = document.createElement('div');
    flow.className = 'nex-home-flow';
    view.appendChild(flow);
    document.body.appendChild(view);
    return view;
  }

  function homeFlow() {
    var view = ensureHomeView();
    var flow = view.querySelector('.nex-home-flow');
    if (!flow) {
      flow = document.createElement('div');
      flow.className = 'nex-home-flow';
      view.appendChild(flow);
    }
    return flow;
  }

  /* 把收回来的块从原页搬进来；原位置留一个注释占位，退出时再放回去。
     原页被客户端卸载、槽位没了，节点就留在首页里由我们保管，下次还用它。 */
  var orphanBlocks = {};
  var lastHomeRefresh = 0;
  orphanBlocks = {};

  function moveBlocksIn() {
    var flow = homeFlow();
    homeItems.forEach(function (item) {
      var live = null;
      try { live = document.querySelector(item.path); } catch (e) { live = null; }
      var node = live;
      if (live && orphanBlocks[item.path]) delete orphanBlocks[item.path];
      if (!live) {
        /* 只有文字对得上才复用留存的节点，免得位置漂了把别的块顶上来。 */
        var kept = orphanBlocks[item.path];
        var stamp = String(item.label || '').slice(0, 4);
        if (kept && stamp && (kept.textContent || '').replace(/\s+/g, '').indexOf(stamp) >= 0) node = kept;
        else if (kept) delete orphanBlocks[item.path];
      }
      var unit = document.createElement('div');
      unit.className = 'nex-home-unit';
      var head = document.createElement('div');
      head.className = 'nex-home-unit-head';
      var from = document.createElement('span');
      from.textContent = '来自' + pageName(item.page);
      head.appendChild(from);
      var remove = document.createElement('button');
      remove.className = 'nex-home-unit-remove';
      remove.type = 'button';
      remove.textContent = '×';
      remove.title = '从首页移出';
      remove.addEventListener('click', function (event) {
        event.preventDefault();
        event.stopPropagation();
        homeItems = homeItems.filter(function (entry) { return entry.path !== item.path; });
        if (orphanBlocks[item.path]) delete orphanBlocks[item.path];
        saveHome();
        refreshHomeView();
        renderModal();
      });
      head.appendChild(remove);
      unit.appendChild(head);
      if (node) {
        var slot = null;
        if (node.parentElement) {
          /* 占位用同标签的隐藏元素，不用注释：nth-of-type 只数元素，
             换成注释会让后面兄弟的序号整体前移，别的位置规则就指错了。 */
          slot = document.createElement(node.tagName);
          slot.setAttribute('data-nex-home-slot', 'true');
          slot.style.display = 'none';
          node.parentElement.insertBefore(slot, node);
        }
        unit.appendChild(node);
        heldBlocks.push({ path: item.path, node: node, slot: slot });
      } else {
        var missing = document.createElement('p');
        missing.className = 'nex-home-empty';
        missing.setAttribute('data-missing', 'true');
        missing.textContent = item.label + '：这一页现在没渲染出来，回到' + pageName(item.page) + '页再看。';
        unit.appendChild(missing);
      }
      flow.appendChild(unit);
    });
  }

  function moveBlocksOut() {
    heldBlocks.forEach(function (entry) {
      if (entry.slot && entry.slot.parentNode) {
        entry.slot.parentNode.insertBefore(entry.node, entry.slot);
        entry.slot.parentNode.removeChild(entry.slot);
        return;
      }
      if (entry.node.parentElement) entry.node.parentElement.removeChild(entry.node);
      orphanBlocks[entry.path] = entry.node;
    });
    heldBlocks = [];
  }

  function refreshHomeView() {
    lastHomeRefresh = Date.now();
    var flow = homeFlow();
    if (!homeActive) {
      flow.innerHTML = '';
      return;
    }
    moveBlocksOut();
    flow.innerHTML = '';
    if (!homeItems.length) {
      var empty = document.createElement('p');
      empty.className = 'nex-home-empty';
      empty.textContent = '首页还是空的。在「编辑界面」→「右侧大页」里给想要的块点「＋首页」，这一页就会把它们摆出来。';
      flow.appendChild(empty);
      return;
    }
    moveBlocksIn();
  }

  function ensureHomeNav() {
    var recommend = testId('tid_navitem_recommend');
    var container = recommend ? recommend.parentElement : null;
    if (!container) return false;
    function onHomeClick(event) {
      if (!alive()) return;
      event.preventDefault();
      event.stopPropagation();
      if (homeActive) deactivateHome();
      else activateHome();
    }
    var existing = document.getElementById(HOME_NAV_ID);
    if (existing && existing.parentElement !== container) container.insertBefore(existing, recommend);
    var item = existing;
    if (!item) {
      item = recommend.cloneNode(true);
      item.id = HOME_NAV_ID;
      item.removeAttribute('data-testid');
      if (item.classList) item.classList.remove('selected');
      var texts = item.querySelectorAll('*');
      for (var i = 0; i < texts.length; i++) {
        if (texts[i].children.length === 0 && (texts[i].textContent || '').trim() === '推荐') {
          texts[i].textContent = '首页';
          break;
        }
      }
      container.insertBefore(item, recommend);
    }
    /* 重新注入时，页面上这一项还带着上一代的点击处理，这里换成当前这一代的。 */
    /* data-log 里写着「推荐」那条路由，留着它的话客户端会抢在我们前面把页面切走。 */
    item.removeAttribute('data-log');
    if (item.__nexHomeGen !== GEN) {
      if (item.__nexHomeClick) item.removeEventListener('click', item.__nexHomeClick, true);
      item.__nexHomeClick = onHomeClick;
      item.__nexHomeGen = GEN;
      item.addEventListener('click', onHomeClick, true);
    }
    return true;
  }

  function selectHomeNav(on) {
    var item = document.getElementById(HOME_NAV_ID);
    if (!item || !item.classList) return;
    if (on) {
      item.classList.add('selected');
      var sibs = item.parentElement ? item.parentElement.children : [];
      for (var i = 0; i < sibs.length; i++) {
        if (sibs[i] !== item && sibs[i].classList) {
          sibs[i].classList.remove('selected');
        }
      }
    } else {
      item.classList.remove('selected');
    }
  }

  function pageIdList() {
    var list = [];
    var pages = document.querySelectorAll('[id^="page_pc_"]');
    for (var i = 0; i < pages.length; i++) list.push(pages[i].id);
    return list.join(',');
  }

  var homeReturnNav = '';
  var homeJumpTries = 0;
  function activateHome() {
    clearFocus();
    /* 首页摆的是别的页里的真实块。来源页都没渲染时，先让客户端把那一页挂上，
       不然搬进来的是空位。退出时再回到点首页之前那一项。 */
    if (homeItems.length && !homeActive) {
      var unmounted = homeItems.every(function (entry) { return !document.getElementById(entry.page); });
      if (unmounted && homeJumpTries < 2) {
        var jump = testId(PAGE_NAV[homeItems[0].page] || '');
        if (jump) {
          var litNow = document.querySelector('.ItemContainer_ijv59hq.selected[data-testid^="tid_navitem_"]');
          if (litNow && litNow.id !== HOME_NAV_ID && !homeReturnNav) homeReturnNav = litNow.getAttribute('data-testid');
          homeJumpTries += 1;
          jump.click();
          setTimeout(function () {
            if (!alive()) return;
            activateHome();
          }, 900);
          return;
        }
      }
    }
    homeJumpTries = 0;
    var view = ensureHomeView();
    var rect = contentRect();
    view.style.left = Math.round(rect.left) + 'px';
    view.style.top = Math.round(rect.top) + 'px';
    var current = visiblePage();
    preHomeNav = homeReturnNav || PAGE_NAV[current && current.id] || '';
    if (!preHomeNav) {
      var lit = document.querySelector('.ItemContainer_ijv59hq.selected[data-testid^="tid_navitem_"]');
      preHomeNav = lit && lit.id !== HOME_NAV_ID ? lit.getAttribute('data-testid') : 'tid_navitem_recommend';
    }
    homeActive = true;
    homePageSnapshot = pageIdList();
    var hide = document.getElementById(HOME_HIDE_ID);
    if (!hide) {
      hide = document.createElement('style');
      hide.id = HOME_HIDE_ID;
      hide.textContent = '[id^="page_pc_"]:not(#page_pc_main_tab):not(#page_pc_main_nav):not(#page_pc_mini_bar){display:none !important;}';
      document.head.appendChild(hide);
    }
    view.style.display = 'flex';
    selectHomeNav(true);
    refreshHomeView();
  }

  function deactivateHome() {
    if (!homeActive) return;
    homeActive = false;
    moveBlocksOut();
    var view = document.getElementById(HOME_VIEW_ID);
    if (view) {
      view.style.display = 'none';
      var flow = view.querySelector('.nex-home-flow');
      if (flow) flow.innerHTML = '';
    }
    var hide = document.getElementById(HOME_HIDE_ID);
    if (hide) hide.remove();
    selectHomeNav(false);
    /* 藏页样式撤掉之后再采样一次，回到哪一页就把高亮放回那一页的导航项。 */
    var now = visiblePage();
    var key = (now && PAGE_NAV[now.id]) || preHomeNav || 'tid_navitem_recommend';
    var back = key ? testId(key) : null;
    /* 取块时被跳到的那一页不是用户原来停的页；这里回到原来那一项。 */
    var origin = homeReturnNav;
    homeReturnNav = '';
    if (origin && testId(origin)) {
      back = testId(origin);
    }
    if (back) {
      markSelected(back);
      if (origin) {
        setTimeout(function () {
          if (!alive() || homeActive) return;
          var target = testId(origin);
          if (target) target.click();
        }, 80);
        return;
      }
      /* 客户端处理这次点击时会再刷一遍左栏，隔一会儿补一次，避免高亮被它擦掉。 */
      setTimeout(function () {
        if (homeActive) return;
        var host = back.parentElement;
        if (host && !host.querySelectorAll('.selected').length) markSelected(back);
      }, 420);
    }
  }

  function closeHomePage() { deactivateHome(); }

  function openHomePage() {
    styleOnce();
    if (homeActive) deactivateHome();
    else activateHome();
  }

  function mountEditorButton() {
    var anchor = testId('tid_useravatar_click') || testId('tid_pc_nav_bar_account');
    if (!anchor) return false;
    var oldToolbar = document.getElementById(TOOLBAR_ID);
    if (oldToolbar) oldToolbar.remove();
    /* 顶部「我的首页」已经撤掉，旧页面上留下的那个按钮也一并清掉。 */
    var staleHome = document.getElementById(HOME_BUTTON_ID);
    if (staleHome && staleHome.parentElement) staleHome.parentElement.removeChild(staleHome);
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
        stateReady = true;
        try { localStorage.setItem(STORE, JSON.stringify(state)); } catch (e) {}
        apply();
        migrateMainKeys();
        applyRules();
        renderModal();
      }).catch(function () { stateReady = true; apply(); applyRules(); });
  }

  /* 跳到别的页之后，聚光那层和「退出」条要跟着撤掉，不然会留在新页面上。 */
  var navWatchBound = false;
  var preHomeNav = '';
  function markSelected(item) {
    if (!item || !item.classList) return;
    var sibs = item.parentElement ? item.parentElement.children : [];
    for (var i = 0; i < sibs.length; i++) {
      if (sibs[i] !== item && sibs[i].classList) sibs[i].classList.remove('selected');
    }
    item.classList.add('selected');
    syncNavColors();
  }

  /* 导航项的字色和图标色由客户端写成内联自定义属性：选中态是 Sidebar8（白），
     未选是 Black3/Black5。它只在真正的路由变化时重绘；我们摘掉它的选中态、
     或者页面被我们的样式藏起来时它不重绘，白字就留在浅底上。
     这里按「有没有选中」把那个属性换成对应的 token，跟客户端自己的取值一致。 */
  function colorVarOf(el) {
    if (!el || !el.style) return '';
    for (var i = 0; i < el.style.length; i++) {
      var name = el.style[i];
      if (name.indexOf('--') === 0 && /color/i.test(el.style.getPropertyValue(name))) return name;
    }
    return '';
  }

  function syncNavColors() {
    var items = document.querySelectorAll('.ItemContainer_ijv59hq');
    for (var i = 0; i < items.length; i++) {
      var item = items[i];
      if (!item.classList) continue;
      var on = item.classList.contains('selected');
      var label = item.querySelector('.title');
      var lv = colorVarOf(label);
      if (lv) {
        var lcur = label.style.getPropertyValue(lv) || '';
        if (!on && /Sidebar8/.test(lcur)) label.style.setProperty(lv, 'var(--colorBlack3)');
        else if (on && !/Sidebar8/.test(lcur)) label.style.setProperty(lv, 'var(--colorSidebar8)');
      }
      var icon = item.querySelector('.IconWrapper_i1ncivvx');
      var iv = colorVarOf(icon);
      if (iv) {
        var icur = icon.style.getPropertyValue(iv) || '';
        if (!on && /Sidebar8/.test(icur)) icon.style.setProperty(iv, 'var(--colorBlack5)');
        else if (on && !/Sidebar8/.test(icur)) icon.style.setProperty(iv, 'var(--colorSidebar8)');
      }
    }
  }

  function bindNavWatch() {
    if (navWatchBound) return;
    navWatchBound = true;
    document.addEventListener('click', function (event) {
      if (!alive()) return;
      var node = event.target && event.target.closest ? event.target.closest('[data-testid^="tid_navitem_"]') : null;
      if (!node || node.id === HOME_NAV_ID) return;
      if (homeActive) closeHomePage();
      else if (focusPath) clearFocus();
      /* 客户端的选中态被摘过之后它不一定重绘，高亮由这边补。 */
      markSelected(node);
      setTimeout(function () {
        var host = node.parentElement;
        if (host && !host.querySelectorAll('.selected').length) markSelected(node);
      }, 420);
    }, true);
  }

  function syncFocus() {
    if (!focusPath) return;
    var target = null;
    try { target = document.querySelector(focusPath); } catch (e) { target = null; }
    if (!target) clearFocus();
  }

  /* 在首页上点里面的卡片，客户端会自己换页（比如进到歌单）。这时要把拼出来的页面撤掉，
     把块放回原页，不然新页被藏住，界面看着是空的。 */
  function syncHome() {
    ensureHomeNav();
    /* 客户端重绘过左栏、或它停在别的页面时，选中项的颜色可能没跟上，
       每轮按当前选中态把颜色校一遍。 */
    syncNavColors();
    if (!homeActive) return;
    /* 客户端重绘左栏会把选中态还原成它自己的那一项，这里把首页那一项按住。 */
    selectHomeNav(true);
    syncNavColors();
    if (pageIdList() !== homePageSnapshot) { deactivateHome(); return; }
    /* 客户端刚起来时区块还没渲染，缺块就隔一会儿再试一次。 */
    var flow = homeFlow();
    if (flow.querySelector('[data-missing="true"]') && Date.now() - lastHomeRefresh > 1200) refreshHomeView();
  }

  function watch() {
    mount();
    apply();
    applyRules();
    bindNavWatch();
    ensureHomeNav();
    var pending = null;
    var observer = new MutationObserver(function () {
      if (!alive()) { observer.disconnect(); return; }
      if (pending) return;
      pending = setTimeout(function () {
        pending = null;
        mount();
        apply();
        migrateMainKeys();
        applyRules();
        refreshPageTab();
        syncFocus();
        syncHome();
      }, 300);
    });
    if (document.body) observer.observe(document.body, { childList: true, subtree: true });
    /* 页面静着的时候没有变动，客户端也就不会重绘；左栏那几项的颜色靠这个定时对一遍。 */
    setInterval(function () {
      if (!alive()) return;
      syncNavColors();
    }, 700);
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', watch);
  else watch();
  load();
})();
