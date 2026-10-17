/* ISODrive+ universal bridge.
 * One UI, two transports:
 *   - KernelSU / APatch / MMRL / KsuWebUIStandalone: window.ksu.exec()
 *   - pure Magisk browser: POST /api/exec on the local loopback server
 * No mock data: if no transport is available the UI says so.
 */
(function () {
  'use strict';

  var KSU_TIMEOUT_MS = 180000;
  var seq = 0;
  var state = { mode: 'none', token: '', server: '', version: '' };

  function ksuAvailable() {
    return typeof ksu !== 'undefined' && ksu && typeof ksu.exec === 'function';
  }

  function isHttp() {
    return location.protocol === 'http:' || location.protocol === 'https:';
  }

  function readToken() {
    var token = '';
    var hash = String(location.hash || '');
    if (hash.length > 1) {
      token = decodeURIComponent(hash.slice(1));
      if (token.indexOf('=') >= 0) {
        token = '';
      }
      try {
        history.replaceState(null, '', location.pathname + location.search);
      } catch (e) {
        location.hash = '';
      }
    }
    if (!token) {
      var q = String(location.search || '');
      if (q.length > 1) {
        var parts = q.slice(1).split('&');
        for (var i = 0; i < parts.length; i++) {
          var kv = parts[i].split('=');
          if (kv[0] === 'token' && kv.length > 1) token = decodeURIComponent(kv[1]);
        }
      }
    }
    if (token) {
      try { sessionStorage.setItem('isodrive-ui-token', token); } catch (e2) {}
    } else {
      try { token = sessionStorage.getItem('isodrive-ui-token') || ''; } catch (e3) {}
    }
    return token;
  }

  function ksuExec(command) {
    return new Promise(function (resolve, reject) {
      var name = '_isodrive_cb_' + (++seq) + '_' + Math.floor(Math.random() * 1e9);
      var done = false;
      var timer = setTimeout(function () {
        if (done) return;
        done = true;
        try { delete window[name]; } catch (e) {}
        reject(new Error('命令超时（' + Math.round(KSU_TIMEOUT_MS / 1000) + ' 秒）'));
      }, KSU_TIMEOUT_MS);
      window[name] = function (code, stdout, stderr) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        try { delete window[name]; } catch (e) {}
        resolve({ code: Number(code) || 0, stdout: String(stdout || ''), stderr: String(stderr || '') });
      };
      try {
        ksu.exec(command, '{}', name);
      } catch (err) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        reject(err);
      }
    });
  }

  function httpExec(args) {
    var headers = { 'Content-Type': 'application/json' };
    if (state.token) headers['X-ISODrive-Token'] = state.token;
    return fetch('/api/exec', {
      method: 'POST',
      cache: 'no-store',
      headers: headers,
      body: JSON.stringify({ args: args })
    }).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status + ' ' + (r.status === 403 ? '（令牌无效或服务已重启，请重新点开界面）' : ''));
      return r.json();
    }).then(function (j) {
      return { code: Number(j.code) || 0, stdout: String(j.stdout || ''), stderr: String(j.stderr || '') };
    });
  }

  function shQuote(s) {
    return "'" + String(s).replace(/'/g, "'\\''") + "'";
  }

  function parseResult(res) {
    var text = String(res.stdout || '');
    var lines = text.split('\n');
    var payload = '';
    for (var i = lines.length - 1; i >= 0; i--) {
      if (lines[i].replace(/\s+$/, '') !== '') { payload = lines[i]; break; }
    }
    if (!payload) {
      var errText = String(res.stderr || '').replace(/\s+$/, '');
      throw new Error(errText || ('命令没有返回数据 (code ' + res.code + ')'));
    }
    var parsed;
    try {
      parsed = JSON.parse(payload);
    } catch (e) {
      var msg = String(res.stderr || '').replace(/\s+$/, '') || payload;
      throw new Error(msg.slice(0, 400));
    }
    return parsed;
  }

  function ui(args) {
    var list = ['ui'].concat(args);
    if (state.mode === 'ksu') {
      var cmd = 'export PATH=/data/adb/modules/isodriveplus/system/bin:/system/bin:$PATH; isodrive ' + list.map(shQuote).join(' ');
      return ksuExec(cmd).then(function (res) {
        if (res.code !== 0 && !String(res.stdout || '').replace(/\s+$/, '')) {
          throw new Error(String(res.stderr || '').replace(/\s+$/, '') || ('命令失败 (code ' + res.code + ')'));
        }
        return parseResult(res);
      });
    }
    if (state.mode === 'http') {
      return httpExec(list).then(function (res) {
        if (res.code !== 0 && !String(res.stdout || '').replace(/\s+$/, '')) {
          throw new Error(String(res.stderr || '').replace(/\s+$/, '') || ('命令失败 (code ' + res.code + ')'));
        }
        return parseResult(res);
      });
    }
    return Promise.reject(new Error('没有可用的执行接口'));
  }

  function ping() {
    if (state.mode !== 'http') return Promise.resolve({ ok: true, mode: state.mode });
    return fetch('/api/ping', { cache: 'no-store' }).then(function (r) { return r.json(); });
  }

  function init() {
    state.token = readToken();
    if (ksuAvailable()) {
      state.mode = 'ksu';
      state.server = 'KernelSU / APatch WebUI';
      return Promise.resolve({ mode: 'ksu', server: state.server });
    }
    if (isHttp()) {
      if (!state.token) {
        state.mode = 'none';
        return Promise.resolve({ mode: 'none', error: 'missing-token' });
      }
      return fetch('/api/ping', { cache: 'no-store' }).then(function (r) {
        return r.json().then(function (j) {
          if (j && j.ok) {
            state.mode = 'http';
            state.server = location.origin;
            state.version = j.version || '';
            return { mode: 'http', server: state.server, version: state.version };
          }
          state.mode = 'none';
          return { mode: 'none', error: 'server' };
        });
      }).catch(function () {
        state.mode = 'none';
        return { mode: 'none', error: 'server' };
      });
    }
    state.mode = 'none';
    return Promise.resolve({ mode: 'none', error: 'no-bridge' });
  }

  function stopServer() {
    return ui(['stop']);
  }

  window.ISODriveBridge = {
    init: init,
    ui: ui,
    ping: ping,
    stopServer: stopServer,
    mode: function () { return state.mode; },
    token: function () { return state.token; },
    server: function () { return state.server; }
  };
})();
