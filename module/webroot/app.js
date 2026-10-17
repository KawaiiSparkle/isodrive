/* ISODrive+ WebUI — single page app.
 * ES5-safe on purpose (no async/await, no template literals) so old Android
 * WebViews work: the panel has to be usable on Android 6+ devices.
 */
(function () {
  'use strict';

  var B = window.ISODriveBridge;

  var MODES = [
    { id: 'auto', label: '自动' },
    { id: 'cdrom', label: 'CD-ROM' },
    { id: 'hdd', label: '硬盘' },
    { id: 'windows', label: 'Windows' },
    { id: 'ventoy', label: 'Ventoy' }
  ];

  var KIND_LABELS = {
    mount: '挂载镜像',
    convert: '转换镜像',
    'ventoy-init': '创建 Ventoy 启动盘',
    'ventoy-add': '添加 ISO 到 Ventoy',
    blank: '创建空白镜像',
    restore: '恢复 USB'
  };

  var S = {
    mode: 'none',
    connInfo: null,
    tab: 'home',
    env: null,
    status: null,
    cfg: null,
    images: null,
    imageFilter: '',
    newDir: '',
    path: '',
    mountMode: 'auto',
    rw: false,
    usb3: false,
    job: null,
    jobTimer: null,
    jobStartMs: 0,
    logText: '',
    diagText: '',
    jobs: null,
    tools: {
      vout: '/data/adb/isodriveplus/images/ventoy.img',
      vgb: '16',
      vaddImg: '',
      vaddIso: '',
      convIso: '',
      convOut: '',
      blankName: 'blank.img',
      blankMb: '1024'
    },
    confirmResolve: null,
    srvStopped: false
  };

  function $(id) { return document.getElementById(id); }

  function esc(v) {
    return String(v === null || v === undefined ? '' : v)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function fmtSize(n) {
    var v = Number(n);
    if (!v || v < 0 || isNaN(v)) return '—';
    var units = ['B', 'KB', 'MB', 'GB', 'TB'];
    var i = 0;
    while (v >= 1024 && i < units.length - 1) { v = v / 1024; i++; }
    return (i === 0 ? Math.round(v) : Math.round(v * 10) / 10) + ' ' + units[i];
  }

  function fmtDur(ms) {
    var t = Math.max(0, Math.floor(ms / 1000));
    var m = Math.floor(t / 60);
    var s = t % 60;
    return (m < 10 ? '0' + m : String(m)) + ':' + (s < 10 ? '0' + s : String(s));
  }

  function fmtDate(epochSec) {
    var t = Number(epochSec);
    if (!t) return '';
    var d = new Date(t * 1000);
    return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate());
  }

  function pad2(n) { return n < 10 ? '0' + n : String(n); }

  function toast(msg, kind) {
    var root = $('toast-root');
    if (!root) return;
    var d = document.createElement('div');
    d.className = 'toast' + (kind ? ' ' + kind : '');
    d.textContent = String(msg);
    root.appendChild(d);
    setTimeout(function () {
      if (d.parentNode) d.parentNode.removeChild(d);
    }, kind === 'bad' ? 6500 : 4000);
  }

  function errMsg(e) {
    if (!e) return '未知错误';
    return e.message ? String(e.message) : String(e);
  }

  function callApi(args) {
    return B.ui(args).then(function (j) {
      if (j && j.ok === false) throw new Error(j.error || '操作失败');
      return j;
    });
  }

  function api(args) {
    if (S.mode === 'none') {
      return Promise.reject(new Error('界面未连接到执行端，请按顶部提示重新打开'));
    }
    return callApi(args);
  }

  function run(btn, thunk, okMsg) {
    var el = btn || null;
    if (el) {
      el.disabled = true;
      if (el.className.indexOf('busy') < 0) el.className += ' busy';
    }
    function done() {
      if (!el) return;
      el.disabled = false;
      el.className = el.className.replace(/ busy/g, '');
    }
    var p;
    try {
      p = thunk();
    } catch (e) {
      done();
      toast(errMsg(e), 'bad');
      return Promise.resolve(null);
    }
    return p.then(function (res) {
      done();
      if (okMsg) toast(okMsg, 'ok');
      return res;
    }, function (err) {
      done();
      toast(errMsg(err), 'bad');
      return null;
    });
  }

  function closestAct(node) {
    var el = node;
    while (el && el.getAttribute) {
      if (el.getAttribute('data-act')) return el;
      el = el.parentNode;
    }
    return null;
  }

  /* ---------------------------------------------------------------- chrome */

  function connLabel() {
    if (S.mode === 'ksu') return 'KernelSU 桥';
    if (S.mode === 'http') return '本地服务';
    return '未连接';
  }

  function selinuxText() {
    var v = (S.env && S.env.selinux) || (S.status && S.status.selinux) || '';
    if (v === 'Enforcing') return 'SELinux 强制';
    if (v === 'Permissive') return 'SELinux 宽容';
    return 'SELinux ' + (v || '未知');
  }

  function selinuxClass() {
    var v = (S.env && S.env.selinux) || (S.status && S.status.selinux) || '';
    if (v === 'Enforcing') return 'ok';
    if (v === 'Permissive') return 'warn';
    return '';
  }

  function renderConn() {
    var conn = $('pill-conn');
    var se = $('pill-selinux');
    if (conn) {
      conn.textContent = connLabel();
      conn.className = 'pill ' + (S.mode === 'none' ? 'bad' : 'acc');
    }
    if (se) {
      se.textContent = selinuxText();
      se.className = 'pill ' + selinuxClass();
    }
  }

  function renderBanner(info) {
    var b = $('banner-none');
    if (!b) return;
    if (S.mode !== 'none') {
      b.className = 'banner hidden';
      b.innerHTML = '';
      return;
    }
    var msg;
    if (S.srvStopped) {
      msg = '<b>界面服务已停止</b><p>可以关闭此页面。需要再次使用时，点模块卡片的“操作”按钮，或在终端执行 <code>su -c "isodrive ui start"</code>。</p>';
    } else if (info && info.error === 'missing-token') {
      msg = '<b>访问令牌缺失</b><p>请回到模块页重新打开界面。也可以执行 <code>su -c "isodrive ui start"</code> 获取带令牌的新链接。</p>';
    } else if (info && info.error === 'token') {
      msg = '<b>访问令牌已失效</b><p>界面服务可能已重启或令牌被更新。请回到模块页重新点“操作”按钮打开新链接，或在终端执行 <code>su -c "isodrive ui start"</code> 后打开它输出的地址。</p>';
    } else if (info && info.error === 'server') {
      msg = '<b>本地界面服务没有响应</b><p>请在终端执行 <code>su -c "isodrive ui start"</code>（或在 Magisk 点模块“操作”按钮）重新启动，然后打开它输出的链接。</p>';
    } else {
      msg = '<b>当前页面无法执行命令</b><p>请用以下任一方式打开：</p>' +
        '<p>• KernelSU / APatch / MMRL：从模块管理页进入 WebUI<br>' +
        '• 纯 Magisk：点模块卡片的“操作”(Action) 按钮，会自动启动界面并打开浏览器<br>' +
        '• 终端：<code>su -c "isodrive ui start"</code><br>' +
        '• 或安装 KsuWebUIStandalone，用它加载本模块的 WebUI</p>';
    }
    b.innerHTML = msg;
    b.className = 'banner';
  }

  /* ------------------------------------------------------------------ data */

  function refreshAll() {
    var p1 = api(['env']).then(function (j) { S.env = j; }, function () { return null; });
    var p2 = api(['status']).then(function (j) { S.status = j; }, function () { return null; });
    var p3 = api(['cfg']).then(function (j) { S.cfg = j; }, function () { return null; });
    return Promise.all([p1, p2, p3]).then(function () {
      renderConn();
      render();
    });
  }

  function loadImages(silent) {
    return api(['images', '400']).then(function (j) {
      S.images = j;
      render();
      if (!silent) toast('找到 ' + j.count + ' 个镜像文件', 'ok');
      return j;
    }, function (e) {
      if (!silent) toast(errMsg(e), 'bad');
      return null;
    });
  }

  function loadJobs() {
    return api(['jobs']).then(function (j) {
      S.jobs = j.jobs || [];
      if (S.tab === 'tools') render();
      return j;
    }, function () { return null; });
  }

  /* --------------------------------------------------------------- render */

  function switchTab(tab) {
    S.tab = tab;
    render();
    if (tab === 'images' && !S.images && S.mode !== 'none') loadImages(true);
    if (tab === 'tools' && S.mode !== 'none') loadJobs();
    window.scrollTo(0, 0);
  }

  function render() {
    var v = $('view');
    if (!v) return;
    var html;
    if (S.tab === 'home') html = viewHome();
    else if (S.tab === 'images') html = viewImages();
    else if (S.tab === 'tools') html = viewTools();
    else html = viewSettings();
    v.innerHTML = html;
    var tabs = document.querySelectorAll('#tabbar .tab');
    for (var i = 0; i < tabs.length; i++) {
      var t = tabs[i];
      if (t.getAttribute('data-tab') === S.tab) t.className = 'tab active';
      else t.className = 'tab';
    }
  }

  function modeButtons() {
    var h = '<div class="seg">';
    for (var i = 0; i < MODES.length; i++) {
      var m = MODES[i];
      h += '<button type="button" data-act="mode" data-mode="' + esc(m.id) + '" class="' +
        (S.mountMode === m.id ? 'on' : '') + '">' + esc(m.label) + '</button>';
    }
    return h + '</div>';
  }

  function kv(h, k, v) {
    var text = (v === null || v === undefined || v === '') ? '—' : String(v);
    h.push('<dt>' + esc(k) + '</dt><dd>' + esc(text) + '</dd>');
  }

  function statusModeText(st) {
    if (!st.mounted) return '未挂载';
    if (st.cdrom) return 'CD-ROM（只读）';
    if (st.rw) return '硬盘（可写）';
    return '硬盘（只读）';
  }

  function viewHome() {
    var st = S.status || {};
    var env = S.env || {};
    var h = [];

    h.push('<div class="card">');
    h.push('<div class="card-hd"><h2>当前状态</h2><span class="hint">' + esc(connLabel()) + '</span></div>');
    h.push('<div class="card-bd">');
    if (st.mounted) {
      h.push('<div class="status-hero"><div class="status-dot">●</div><div><p class="status-title">已挂载：' + esc(statusModeText(st)) + '</p>');
      h.push('<p class="status-path">' + esc(st.path || st.src || '') + '</p></div></div>');
    } else {
      h.push('<div class="status-hero"><div class="status-dot idle">○</div><div><p class="status-title">当前未挂载</p>');
      h.push('<p class="status-path">' + (st.bound ? 'USB 已连接，等待选择镜像' : '选择镜像后点“挂载并连接电脑”') + '</p></div></div>');
    }
    h.push('<dl class="kv">');
    kv(h, 'SELinux', (st.selinux || env.selinux || '') + ((st.selinux === 'Enforcing' || env.selinux === 'Enforcing') ? '（正常）' : ''));
    kv(h, '后端', st.backend || (st.mounted ? 'shell' : ''));
    kv(h, 'Gadget', st.gadget || '');
    kv(h, 'UDC', st.udc || '');
    kv(h, '引擎', st.engine || env.engine || '未找到 isodrive.bin');
    if (env.webroot) kv(h, 'WebUI', env.webroot);
    if (st.server && st.server.running) kv(h, '界面服务', st.server.url);
    h.push('</dl>');
    h.push('<div class="row" style="margin-top:12px">');
    h.push('<button type="button" class="btn ghost small" data-act="refresh">刷新状态</button>');
    h.push('<button type="button" class="btn danger small" data-act="unmount"' + (st.mounted ? '' : ' disabled') + '>卸载并恢复 USB</button>');
    h.push('</div>');
    h.push('</div></div>');

    h.push('<div class="card">');
    h.push('<div class="card-hd"><h2>挂载镜像</h2><span class="hint">.iso / .img / .bin</span></div>');
    h.push('<div class="card-bd">');
    h.push('<input id="mount-path" class="input mono" type="text" autocapitalize="off" autocorrect="off" spellcheck="false" placeholder="/sdcard/Download/ubuntu.iso" value="' + esc(S.path) + '">');
    h.push('<div class="row" style="margin-top:8px">');
    h.push('<button type="button" class="btn ghost small" data-act="goto-images">从镜像库选择</button>');
    h.push('<button type="button" class="btn ghost small" data-act="clear-path">清空路径</button>');
    h.push('</div>');
    h.push('<p class="hint" style="margin-bottom:8px">设备模式</p>');
    h.push(modeButtons());
    h.push('<div class="switch-row" style="margin-top:12px">');
    h.push('<div><div class="label">可写模式</div><div class="desc">默认只读；可写会把镜像当作硬盘暴露</div></div>');
    h.push('<label class="switch"><input type="checkbox" id="opt-rw"' + (S.rw ? ' checked' : '') + '><span class="track"></span></label>');
    h.push('</div>');
    h.push('<div class="switch-row">');
    h.push('<div><div class="label">USB 3.0 描述符</div><div class="desc">仅在老电脑（USB 2.0）不识别时尝试</div></div>');
    h.push('<label class="switch"><input type="checkbox" id="opt-usb3"' + (S.usb3 ? ' checked' : '') + '><span class="track"></span></label>');
    h.push('</div>');
    h.push('<div class="row" style="margin-top:12px">');
    h.push('<button type="button" class="btn primary grow" data-act="mount">挂载并连接电脑</button>');
    h.push('</div>');
    h.push('<p class="hint">挂载后手机自身的 MTP 文件传输会断开，电脑上应出现新的磁盘或 CD-ROM 设备。使用完点“卸载并恢复 USB”。</p>');
    h.push('</div></div>');

    if (S.jobs && S.jobs.length) {
      h.push('<div class="card"><div class="card-hd"><h2>最近任务</h2><span class="hint">' + S.jobs.length + ' 条</span></div><div class="card-bd tight">');
      h.push(jobsListHtml());
      h.push('</div></div>');
    }
    return h.join('');
  }

  function jobsListHtml() {
    var h = ['<ul class="image-list">'];
    for (var i = 0; i < S.jobs.length; i++) {
      var j = S.jobs[i];
      var label = KIND_LABELS[j.kind] || j.kind;
      var state = j.running ? '执行中…' : (j.rc === 0 ? '已完成' : '失败 (rc=' + j.rc + ')');
      h.push('<li class="image-item"><div class="ext-badge">' + (j.running ? '…' : (j.rc === 0 ? '✓' : '✗')) + '</div>');
      h.push('<div class="image-meta"><div class="image-name">' + esc(label) + '</div>');
      h.push('<div class="image-sub">' + esc(state) + ' · ' + esc(j.id) + '</div></div>');
      h.push('<button type="button" class="btn ghost small" data-act="job-open" data-job="' + esc(j.id) + '" data-kind="' + esc(j.kind) + '">查看</button>');
      h.push('</li>');
    }
    return h.join('') + '</ul>';
  }

  function viewImages() {
    var h = [];
    var img = S.images || { count: 0, files: [], dirs: [] };
    var dirs = img.dirs || [];
    h.push('<div class="card">');
    h.push('<div class="card-hd"><h2>扫描目录</h2><button type="button" class="btn ghost small" data-act="scan">重新扫描</button></div>');
    h.push('<div class="card-bd">');
    h.push('<div class="chips">');
    if (!dirs.length) h.push('<span class="hint">暂无扫描目录</span>');
    for (var i = 0; i < dirs.length; i++) {
      var d = dirs[i];
      h.push('<span class="chip' + (d.exists ? '' : ' missing') + '"><span>' + esc(d.path) + '</span>' +
        '<button type="button" title="移除" data-act="path-del" data-path="' + esc(d.path) + '">×</button></span>');
    }
    h.push('</div>');
    h.push('<div class="row" style="margin-top:10px">');
    h.push('<input id="new-dir" class="input mono grow" type="text" placeholder="/sdcard/Download/ISO" value="' + esc(S.newDir) + '">');
    h.push('<button type="button" class="btn ghost" data-act="path-add">添加</button>');
    h.push('</div>');
    h.push('<p class="hint">支持 .iso / .img / .bin；扫描深度 3 层。</p>');
    h.push('</div></div>');

    h.push('<div class="card">');
    h.push('<div class="card-hd"><h2>镜像文件</h2><span class="hint">共 ' + (img.count || 0) + ' 个</span></div>');
    h.push('<div class="card-bd tight">');
    h.push('<input id="img-filter" class="input" type="text" placeholder="搜索文件名…" value="' + esc(S.imageFilter) + '">');
    var files = img.files || [];
    var q = S.imageFilter.toLowerCase();
    var shown = 0;
    var list = [];
    for (var k = 0; k < files.length; k++) {
      var f = files[k];
      if (q && String(f.name || '').toLowerCase().indexOf(q) < 0) continue;
      shown++;
      list.push('<li class="image-item' + (f.path === S.path ? ' active' : '') + '" data-act="pick-image" data-path="' + esc(f.path) + '">');
      list.push('<div class="ext-badge ' + esc(f.ext || '') + '">' + esc(f.ext || '?') + '</div>');
      list.push('<div class="image-meta"><div class="image-name">' + esc(f.name) + (f.mounted ? '<span class="badge-mount">已挂载</span>' : '') + '</div>');
      list.push('<div class="image-sub">' + esc(fmtSize(f.size)) + ' · ' + esc(fmtDate(f.mtime)) + ' · ' + esc(f.dir) + '</div></div>');
      list.push('<button type="button" class="btn ghost small" data-act="mount-image" data-path="' + esc(f.path) + '">挂载</button>');
      list.push('</li>');
    }
    if (!files.length) {
      h.push('<div class="empty">没有找到镜像文件。<br>把 ISO/IMG 放进上面任一目录，或添加新的扫描目录后点“重新扫描”。</div>');
    } else if (!shown) {
      h.push('<div class="empty">没有匹配“' + esc(S.imageFilter) + '”的文件。</div>');
    } else {
      h.push('<ul class="image-list">' + list.join('') + '</ul>');
    }
    h.push('</div></div>');
    return h.join('');
  }

  function toolInput(id, label, value, placeholder, hint) {
    var h = [];
    h.push('<label class="hint" for="' + id + '" style="display:block;margin:10px 0 5px">' + esc(label) + '</label>');
    h.push('<input id="' + id + '" class="input mono" type="text" autocapitalize="off" autocorrect="off" spellcheck="false" placeholder="' +
      esc(placeholder || '') + '" value="' + esc(value) + '">');
    if (hint) h.push('<p class="hint" style="margin-top:4px">' + esc(hint) + '</p>');
    return h.join('');
  }

  function viewTools() {
    var T = S.tools;
    var h = [];
    h.push('<div class="card tool-card"><div class="card-hd"><h2><span class="tool-ico">◈</span>创建 Ventoy 启动盘</h2></div><div class="card-bd">');
    h.push('<p class="hint">创建可多 ISO 引导的 Ventoy 磁盘镜像（需要网络下载 Ventoy，耗时数分钟）。</p>');
    h.push(toolInput('t-ventoy-out', '输出镜像路径', T.vout, '/data/adb/isodriveplus/images/ventoy.img'));
    h.push(toolInput('t-ventoy-gb', '容量 (GB)', T.vgb, '16'));
    h.push('<div class="row" style="margin-top:12px"><button type="button" class="btn primary grow" data-act="tools-ventoy-init">开始创建</button></div>');
    h.push('</div></div>');

    h.push('<div class="card tool-card"><div class="card-hd"><h2><span class="tool-ico">+</span>向 Ventoy 添加 ISO</h2></div><div class="card-bd">');
    h.push(toolInput('t-vadd-img', 'Ventoy 镜像路径', T.vaddImg, '/data/adb/isodriveplus/images/ventoy.img'));
    h.push(toolInput('t-vadd-iso', '要添加的 ISO 路径', T.vaddIso, '/sdcard/Download/ubuntu.iso'));
    h.push('<div class="row" style="margin-top:12px"><button type="button" class="btn primary grow" data-act="tools-ventoy-add">添加 ISO</button></div>');
    h.push('</div></div>');

    h.push('<div class="card tool-card"><div class="card-hd"><h2><span class="tool-ico">→</span>ISO 转直写镜像</h2></div><div class="card-bd">');
    h.push('<p class="hint">把 Windows/官方 ISO 解包成 FAT32/exFAT 磁盘镜像。超过 4 GiB 的 install.wim 需要 exFAT。</p>');
    h.push(toolInput('t-conv-iso', '源 ISO 路径', T.convIso, '/sdcard/Download/win11.iso'));
    h.push(toolInput('t-conv-out', '输出路径（可留空）', T.convOut, '/data/adb/isodriveplus/images/win11.img'));
    h.push('<div class="row" style="margin-top:12px"><button type="button" class="btn primary grow" data-act="tools-convert">开始转换</button></div>');
    h.push('</div></div>');

    h.push('<div class="card tool-card"><div class="card-hd"><h2><span class="tool-ico">▤</span>创建空白镜像</h2></div><div class="card-bd">');
    h.push(toolInput('t-blank-name', '文件名', T.blankName, 'blank.img'));
    h.push(toolInput('t-blank-mb', '容量 (MB)', T.blankMb, '1024'));
    h.push('<div class="row" style="margin-top:12px"><button type="button" class="btn primary grow" data-act="tools-blank">创建空白镜像</button></div>');
    h.push('</div></div>');

    h.push('<div class="card"><div class="card-hd"><h2>任务记录</h2><button type="button" class="btn ghost small" data-act="jobs-refresh">刷新</button></div><div class="card-bd tight">');
    if (S.jobs && S.jobs.length) h.push(jobsListHtml());
    else h.push('<div class="empty">暂无后台任务。</div>');
    h.push('</div></div>');
    return h.join('');
  }

  function viewSettings() {
    var env = S.env || {};
    var st = S.status || {};
    var cfg = S.cfg || {};
    var h = [];
    h.push('<div class="card"><div class="card-hd"><h2>启动与恢复</h2></div><div class="card-bd">');
    h.push('<div class="switch-row"><div><div class="label">开机自动挂载</div><div class="desc">重启后自动挂载上次使用的镜像</div></div>');
    h.push('<label class="switch"><input type="checkbox" id="cfg-persist"' + (String(cfg.persist) === '1' ? ' checked' : '') + '><span class="track"></span></label></div>');
    h.push('<div class="switch-row"><div><div class="label">掉线自动恢复</div><div class="desc">USB 重新连接后自动恢复挂载</div></div>');
    h.push('<label class="switch"><input type="checkbox" id="cfg-autorestore"' + (String(cfg.autorestore) === '1' ? ' checked' : '') + '><span class="track"></span></label></div>');
    h.push('<p class="hint">设置会立即写入 /data/adb/isodriveplus/config。</p>');
    h.push('</div></div>');

    h.push('<div class="card"><div class="card-hd"><h2>界面连接</h2></div><div class="card-bd">');
    h.push('<dl class="kv">');
    kv(h, '传输', S.mode === 'ksu' ? 'KernelSU / APatch WebUI 桥' : (S.mode === 'http' ? '本地回环 HTTP 服务' : '未连接'));
    kv(h, '地址', S.mode === 'http' ? (B.server() || location.origin) : '');
    kv(h, '版本', st.version || env.version || '');
    kv(h, '引擎', st.engine || env.engine || '');
    kv(h, '模块目录', env.module_dir || '');
    kv(h, 'WebUI 目录', env.webroot || '');
    h.push('</dl>');
    if (S.mode === 'http') {
      h.push('<div class="row" style="margin-top:12px"><button type="button" class="btn danger small" data-act="stop-server">停止本地服务</button></div>');
      h.push('<p class="hint">停止后本页面将无法再执行命令；下次用模块“操作”按钮或 <code>su -c "isodrive ui start"</code> 重新打开。</p>');
    } else if (S.mode === 'ksu') {
      h.push('<p class="hint">当前由模块管理器注入执行桥，不需要本地服务。纯 Magisk 环境请点模块“操作”按钮打开浏览器界面。</p>');
    }
    h.push('</div></div>');

    h.push('<div class="card"><div class="card-hd"><h2>诊断</h2></div><div class="card-bd">');
    h.push('<div class="row"><button type="button" class="btn ghost small" data-act="diag">环境诊断</button>');
    h.push('<button type="button" class="btn ghost small" data-act="log">查看日志</button></div>');
    h.push('<pre class="out" id="diag-out">' + esc(S.diagText) + '</pre>');
    h.push('</div></div>');

    h.push('<div class="card"><div class="card-hd"><h2>命令行速查</h2></div><div class="card-bd">');
    h.push('<div class="cli-help">su -c isodrive probe              # 环境自检\n' +
      'su -c "isodrive /sdcard/Download/ubuntu.iso"\n' +
      'su -c "isodrive /sdcard/Win11.iso -windows"\n' +
      'su -c isodrive list               # 扫描镜像\n' +
      'su -c isodrive restore            # 卸载并恢复 MTP\n' +
      'su -c "isodrive ui start"         # 启动本地界面\n' +
      'su -c "isodrive ui stop"          # 停止本地界面</div>');
    h.push('</div></div>');
    return h.join('');
  }

  /* ------------------------------------------------------------- job modal */

  function openJob(jobId, kind, path) {
    S.job = { id: jobId, kind: kind, path: path || '' };
    S.jobStartMs = Date.now();
    var root = $('modal-root');
    var title = KIND_LABELS[kind] || kind || '任务';
    var h = [];
    h.push('<div class="modal-backdrop" data-act="backdrop-close">');
    h.push('<div class="modal" role="dialog">');
    h.push('<div class="modal-hd"><div><h3>' + esc(title) + '</h3><p class="modal-sub">' + esc(jobId) + '</p></div>');
    h.push('<button type="button" class="btn ghost small" data-act="job-close">关闭</button></div>');
    h.push('<div class="modal-bd">');
    h.push('<div class="job-meta"><span class="spinner" id="job-spin"></span><span id="job-status">执行中…</span><span id="job-elapsed">00:00</span></div>');
    h.push('<pre class="out" id="job-log"></pre>');
    h.push('<p class="hint" style="margin-top:10px">任务在后台执行，关闭窗口不会中断；可随时从“工具 → 任务记录”重新查看。</p>');
    h.push('</div>');
    h.push('<div class="modal-ft"><button type="button" class="btn danger grow" id="job-cancel" data-act="job-cancel">取消任务</button>');
    h.push('<button type="button" class="btn ghost grow" data-act="job-close">后台运行</button></div>');
    h.push('</div></div>');
    root.innerHTML = h.join('');
    startJobPoll();
  }

  function startJobPoll() {
    stopJobPoll();
    pollJob();
    S.jobTimer = setInterval(pollJob, 1200);
  }

  function stopJobPoll() {
    if (S.jobTimer) { clearInterval(S.jobTimer); S.jobTimer = null; }
  }

  function pollJob() {
    if (!S.job) { stopJobPoll(); return; }
    var id = S.job.id;
    api(['job-poll', id]).then(function (j) {
      if (!j || j.ok === false) return;
      if (!S.job || S.job.id !== id) return;
      var log = $('job-log');
      var elapsed = $('job-elapsed');
      if (log) {
        log.textContent = j.tail || '';
        log.scrollTop = log.scrollHeight;
      }
      if (elapsed) elapsed.textContent = fmtDur(Date.now() - S.jobStartMs);
      if (!j.running) finishJobModal(j);
    }, function () { /* keep polling through transient errors */ });
  }

  function finishJobModal(j) {
    stopJobPoll();
    var spin = $('job-spin');
    var status = $('job-status');
    var cancel = $('job-cancel');
    if (spin) spin.className = 'hidden';
    if (cancel) cancel.className = 'hidden';
    if (status) {
      if (j.rc === 0) {
        status.className = 'job-done-ok';
        status.textContent = '已完成';
      } else {
        status.className = 'job-done-bad';
        status.textContent = '失败 (rc=' + j.rc + ')';
      }
    }
    if (j.rc === 0) {
      toast((KIND_LABELS[j.kind] || j.kind) + '：已完成', 'ok');
      refreshAll();
      loadImages(true);
      loadJobs();
    } else {
      toast('任务失败，详情见任务输出', 'bad');
    }
  }

  function closeModal() {
    stopJobPoll();
    S.job = null;
    var root = $('modal-root');
    if (root) root.innerHTML = '';
  }

  function confirmBox(title, text, okLabel) {
    return new Promise(function (resolve) {
      S.confirmResolve = resolve;
      var h = [];
      h.push('<div class="modal-backdrop" data-act="backdrop-close">');
      h.push('<div class="modal" role="dialog">');
      h.push('<div class="modal-hd"><div><h3>' + esc(title) + '</h3></div></div>');
      h.push('<div class="modal-bd"><p class="hint">' + esc(text) + '</p></div>');
      h.push('<div class="modal-ft"><button type="button" class="btn ghost grow" data-act="confirm-no">取消</button>');
      h.push('<button type="button" class="btn danger grow" data-act="confirm-yes">' + esc(okLabel || '确定') + '</button></div>');
      h.push('</div></div>');
      $('modal-root').innerHTML = h.join('');
    });
  }

  function resolveConfirm(value) {
    var fn = S.confirmResolve;
    S.confirmResolve = null;
    closeModal();
    if (fn) fn(value);
  }

  /* ---------------------------------------------------------------- actions */

  function doMount(path, btn) {
    var p = path || S.path;
    if (!p) {
      toast('请先选择镜像文件', 'bad');
      return Promise.resolve(null);
    }
    var args = ['mount', p];
    if (S.mountMode === 'cdrom') args.push('-cdrom');
    else if (S.mountMode === 'hdd') args.push('-hdd');
    else if (S.mountMode === 'windows') args.push('-windows');
    else if (S.mountMode === 'ventoy') args.push('-hdd');
    if (S.rw || S.mountMode === 'ventoy') args.push('-rw');
    if (S.usb3) args.push('-usb3');
    return run(btn, function () { return api(args); }).then(function (res) {
      if (!res) return null;
      S.path = p;
      openJob(res.job, res.kind || 'mount', p);
      return res;
    });
  }

  function onClick(ev) {
    var el = closestAct(ev.target);
    if (!el) return;
    var act = el.getAttribute('data-act');

    if (act === 'tab') {
      switchTab(el.getAttribute('data-tab'));
      return;
    }
    if (act === 'backdrop-close') {
      if (ev.target !== el) return;
      if (S.confirmResolve) resolveConfirm(false);
      else closeModal();
      return;
    }
    if (act === 'confirm-yes') { resolveConfirm(true); return; }
    if (act === 'confirm-no') { resolveConfirm(false); return; }
    if (act === 'job-close') { closeModal(); return; }
    if (act === 'job-cancel') {
      if (!S.job) return;
      var id = S.job.id;
      run(el, function () { return api(['job-cancel', id]); }, '已请求取消');
      setTimeout(pollJob, 600);
      return;
    }
    if (act === 'job-open') {
      openJob(el.getAttribute('data-job'), el.getAttribute('data-kind'));
      return;
    }

    if (act === 'refresh') { run(el, function () { return refreshAll(); }, '已刷新'); return; }
    if (act === 'unmount') {
      confirmBox('卸载并恢复 USB？', '将断开电脑上的虚拟磁盘，并恢复手机 MTP 文件传输。', '卸载').then(function (ok) {
        if (!ok) return;
        run(el, function () { return api(['unmount']); }, '已卸载').then(function () { refreshAll(); });
      });
      return;
    }
    if (act === 'mount') { doMount(); return; }
    if (act === 'mode') {
      S.mountMode = el.getAttribute('data-mode');
      if (S.mountMode === 'ventoy') S.rw = true;
      render();
      return;
    }
    if (act === 'clear-path') { S.path = ''; render(); return; }
    if (act === 'goto-images') {
      switchTab('images');
      if (!S.images) loadImages(true);
      return;
    }
    if (act === 'pick-image') {
      S.path = el.getAttribute('data-path');
      S.tab = 'home';
      render();
      toast('已选择：' + S.path.split('/').pop(), 'ok');
      return;
    }
    if (act === 'mount-image') {
      S.path = el.getAttribute('data-path');
      doMount(S.path, el);
      return;
    }
    if (act === 'scan') { run(el, function () { return loadImages(false); }); return; }
    if (act === 'path-add') {
      var nd = $('new-dir');
      var dir = nd ? nd.value.replace(/^\s+|\s+$/g, '') : S.newDir;
      if (!dir) { toast('请输入目录路径', 'bad'); return; }
      run(el, function () { return api(['path-add', dir]); }, '目录已添加').then(function (res) {
        if (!res) return;
        S.newDir = '';
        loadImages(true);
      });
      return;
    }
    if (act === 'path-del') {
      var dp = el.getAttribute('data-path');
      run(el, function () { return api(['path-del', dp]); }, '目录已移除').then(function (res) {
        if (!res) return;
        loadImages(true);
      });
      return;
    }
    if (act === 'jobs-refresh') { run(el, function () { return loadJobs(); }, '已刷新'); return; }

    if (act === 'tools-ventoy-init') {
      var out = val('t-ventoy-out', S.tools.vout);
      var gb = val('t-ventoy-gb', S.tools.vgb);
      if (!out) { toast('请填写输出路径', 'bad'); return; }
      run(el, function () { return api(['ventoy-init', out, gb || '16']); }).then(function (res) {
        if (res) openJob(res.job, res.kind || 'ventoy-init', out);
      });
      return;
    }
    if (act === 'tools-ventoy-add') {
      var vimg = val('t-vadd-img', S.tools.vaddImg);
      var viso = val('t-vadd-iso', S.tools.vaddIso);
      if (!vimg || !viso) { toast('请填写 Ventoy 镜像和 ISO 路径', 'bad'); return; }
      run(el, function () { return api(['ventoy-add', vimg, viso]); }).then(function (res) {
        if (res) openJob(res.job, res.kind || 'ventoy-add', viso);
      });
      return;
    }
    if (act === 'tools-convert') {
      var ciso = val('t-conv-iso', S.tools.convIso);
      var cout = val('t-conv-out', S.tools.convOut);
      if (!ciso) { toast('请填写源 ISO 路径', 'bad'); return; }
      var cargs = ['convert', ciso];
      if (cout) cargs.push(cout);
      run(el, function () { return api(cargs); }).then(function (res) {
        if (res) openJob(res.job, res.kind || 'convert', ciso);
      });
      return;
    }
    if (act === 'tools-blank') {
      var bname = val('t-blank-name', S.tools.blankName);
      var bmb = val('t-blank-mb', S.tools.blankMb);
      if (!bname) { toast('请填写文件名', 'bad'); return; }
      run(el, function () { return api(['blank', bname, bmb || '1024']); }).then(function (res) {
        if (res) openJob(res.job, res.kind || 'blank', bname);
      });
      return;
    }

    if (act === 'diag') {
      run(el, function () {
        return api(['diag']).then(function (j) { S.diagText = j.text || ''; render(); return j; });
      }, '诊断完成');
      return;
    }
    if (act === 'log') {
      run(el, function () {
        return api(['log', '200']).then(function (j) { S.diagText = j.text || ''; render(); return j; });
      });
      return;
    }
    if (act === 'stop-server') {
      confirmBox('停止本地界面服务？', '停止后本页面无法继续操作。下次可从模块“操作”按钮重新打开。', '停止服务').then(function (ok) {
        if (!ok) return;
        run(el, function () { return api(['stop']); }).then(function (res) {
          if (!res) return;
          S.srvStopped = true;
          S.mode = 'none';
          S.connInfo = { mode: 'none', error: 'server' };
          renderConn();
          renderBanner(S.connInfo);
          render();
          toast('本地服务已停止', 'ok');
        });
      });
      return;
    }
  }

  function val(id, fallback) {
    var el = $(id);
    if (!el) return fallback || '';
    return String(el.value || '').replace(/^\s+|\s+$/g, '') || fallback || '';
  }

  function onChange(ev) {
    var t = ev.target;
    if (!t || !t.id) return;
    if (t.id === 'opt-rw') { S.rw = !!t.checked; return; }
    if (t.id === 'opt-usb3') { S.usb3 = !!t.checked; return; }
    if (t.id === 'cfg-persist') {
      var v1 = t.checked ? '1' : '0';
      run(null, function () { return api(['cfg-set', 'persist', v1]); }, '已保存').then(function (res) {
        if (res) S.cfg = res;
      });
      return;
    }
    if (t.id === 'cfg-autorestore') {
      var v2 = t.checked ? '1' : '0';
      run(null, function () { return api(['cfg-set', 'autorestore', v2]); }, '已保存').then(function (res) {
        if (res) S.cfg = res;
      });
      return;
    }
  }

  function storeToolValue(id, value) {
    var map = {
      't-ventoy-out': 'vout',
      't-ventoy-gb': 'vgb',
      't-vadd-img': 'vaddImg',
      't-vadd-iso': 'vaddIso',
      't-conv-iso': 'convIso',
      't-conv-out': 'convOut',
      't-blank-name': 'blankName',
      't-blank-mb': 'blankMb'
    };
    if (map[id]) S.tools[map[id]] = value;
  }

  function onInput(ev) {
    var t = ev.target;
    if (!t || !t.id) return;
    if (t.id === 'mount-path') { S.path = t.value; return; }
    if (t.id === 'new-dir') { S.newDir = t.value; return; }
    if (t.id === 'img-filter') {
      S.imageFilter = t.value;
      render();
      return;
    }
    storeToolValue(t.id, t.value);
  }

  /* ------------------------------------------------------------------ boot */

  function bind() {
    var view = $('view');
    var tabbar = $('tabbar');
    if (view) {
      view.addEventListener('click', onClick, false);
      view.addEventListener('change', onChange, false);
      view.addEventListener('input', onInput, false);
    }
    if (tabbar) {
      tabbar.addEventListener('click', function (ev) {
        var el = closestAct(ev.target);
        if (el && el.getAttribute('data-act') === 'tab') switchTab(el.getAttribute('data-tab'));
      }, false);
    }
    var modal = $('modal-root');
    if (modal) {
      modal.addEventListener('click', onClick, false);
      modal.addEventListener('change', onChange, false);
    }
  }

  function transportFailure(err) {
    var text = errMsg(err);
    S.mode = 'none';
    S.connInfo = { error: text.indexOf('403') >= 0 ? 'token' : 'server' };
    renderConn();
    renderBanner(S.connInfo);
    render();
  }

  function startStatusPoll() {
    setInterval(function () {
      if (S.mode === 'none') return;
      if (document.visibilityState === 'hidden') return;
      if (S.tab !== 'home') return;
      api(['status']).then(function (j) {
        S.status = j;
        if (S.tab === 'home' && !S.job) render();
        renderConn();
      }, function () { return null; });
    }, 15000);
  }

  function boot() {
    if (!B) {
      var v = $('view');
      if (v) v.innerHTML = '<div class="card"><div class="card-bd"><p class="hint">bridge.js 加载失败，请重新安装模块。</p></div></div>';
      return;
    }
    renderConn();
    bind();
    B.init().then(function (info) {
      S.mode = info.mode;
      S.connInfo = info;
      renderConn();
      renderBanner(info);
      if (info.mode === 'none') {
        render();
        return;
      }
      // The ping endpoint is public. An authenticated call proves the token is still valid
      // (the service may have restarted with a fresh token), otherwise show why instead of
      // presenting an empty panel that looks healthy.
      B.ui(['env']).then(function (env) {
        S.env = env;
        refreshAll().then(function () {
          loadImages(true);
          loadJobs();
        });
        startStatusPoll();
      }, function (err) {
        transportFailure(err);
      });
    }, function (err) {
      S.mode = 'none';
      S.connInfo = { error: 'server' };
      renderConn();
      renderBanner(S.connInfo);
      render();
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot, false);
  } else {
    boot();
  }
})();
