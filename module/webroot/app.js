/* ISODrive+ WebUI */
(function () {
  const $ = (id) => document.getElementById(id);

  function execRaw(cmd) {
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error("exec timeout")), 300000);
      function done(code, stdout, stderr) {
        clearTimeout(timeout);
        resolve({
          code: Number(code),
          stdout: stdout == null ? "" : String(stdout),
          stderr: stderr == null ? "" : String(stderr),
        });
      }
      try {
        if (typeof ksu !== "undefined" && typeof ksu.exec === "function") {
          const cb = "_cb_" + Math.random().toString(36).slice(2);
          window[cb] = function (code, stdout, stderr) {
            try { delete window[cb]; } catch (e) {}
            done(code, stdout, stderr);
          };
          ksu.exec(cmd, "{}", cb);
          return;
        }
      } catch (e) {}
      clearTimeout(timeout);
      reject(new Error("没有 WebUI 执行接口。请用 KSU / MMRL 打开。"));
    });
  }

  async function sh(cmd) {
    const full = "export PATH=/data/adb/modules/isodriveplus/system/bin:/system/bin:$PATH; " + cmd;
    try { return await execRaw(full); }
    catch (e) { return { code: 1, stdout: "", stderr: String(e.message || e) }; }
  }

  function out(el, r) {
    const text = [r.stdout, r.stderr].filter(Boolean).join("\n").trim() || "(no output)";
    el.textContent = text;
    el.className = "out " + (r.code === 0 ? "ok" : "bad");
  }

  function q(s) { return "'" + String(s).replace(/'/g, "'\\''") + "'"; }

  async function probe() {
    const r = await sh("isodrive probe");
    $("probe").textContent = (r.stdout || r.stderr || "failed").trim();
    const se = /selinux=(\S+)/.exec(r.stdout || "");
    if (se) {
      $("selinux").textContent = "SELinux " + se[1];
      $("selinux").className = "pill " + se[1].toLowerCase();
    }
    const hint = $("cap-hint");
    if ((r.stdout || "").indexOf("mass_storage=no") >= 0 || (r.stdout || "").indexOf("mass_storage_create=no") >= 0) {
      hint.textContent = "内核没有 mass_storage gadget。换 ROM 或用外接 U 盘。";
      hint.className = "hint bad";
    } else {
      hint.textContent = "多 ISO 请用 Ventoy 盘镜像（-hdd -rw）。单张官方 Windows ISO 用 CD-ROM。不要 UDF 刻录。";
      hint.className = "hint";
    }
  }

  function flags() {
    const mode = (document.querySelector("input[name=mode]:checked") || {}).value || "auto";
    let f = "";
    if (mode === "cdrom") f += " -cdrom";
    if (mode === "hdd" || mode === "ventoy") f += " -hdd";
    if (mode === "windows") f += " -windows";
    if (mode === "ventoy" || $("rw").checked) f += " -rw";
    if ($("usb3").checked) f += " -usb3";
    return f;
  }

  $("btn-probe").onclick = probe;
  $("btn-restore").onclick = async () => { out($("out"), await sh("isodrive restore")); probe(); };
  $("btn-status").onclick = async () => { out($("out"), await sh("isodrive status")); };
  $("btn-diag").onclick = async () => { out($("out"), await sh("isodrive diag")); };
  $("btn-mount").onclick = async () => {
    const p = $("path").value.trim();
    if (!p) { $("out").textContent = "请填写路径"; return; }
    $("out").textContent = "挂载中…";
    out($("out"), await sh("isodrive " + q(p) + flags()));
    probe();
  };
  $("btn-list").onclick = async () => {
    const r = await sh("isodrive list");
    const ul = $("files");
    ul.innerHTML = "";
    (r.stdout || "").split("\n").forEach((line) => {
      line = line.trim();
      if (!line) return;
      if (line.charAt(0) === "#") return;
      const li = document.createElement("li");
      li.textContent = line;
      li.onclick = () => { $("path").value = line; };
      ul.appendChild(li);
    });
    if (!ul.childNodes.length) ul.innerHTML = "<li>没找到文件，先添加扫描路径</li>";
    $("paths").textContent = (r.stdout || "").split("\n").filter((l) => l.indexOf("# /") === 0 || l.indexOf("# /data") === 0).join("\n");
  };
  $("btn-path-add").onclick = async () => {
    const p = $("new-path").value.trim();
    if (!p) return;
    out($("paths"), await sh("isodrive paths-add " + q(p)));
  };
  $("btn-ventoy").onclick = async () => {
    $("out").textContent = "创建 Ventoy 盘（需联网下载官方包，可能较久）…";
    const o = $("ventoy-out").value.trim();
    const g = $("ventoy-gb").value || "16";
    const r = await sh("isodrive ventoy-init " + q(o) + " " + g);
    out($("out"), r);
    if (r.code === 0) {
      $("path").value = o;
      document.querySelector('input[name=mode][value=ventoy]').checked = true;
    }
  };
  $("btn-convert").onclick = async () => {
    const iso = $("path").value.trim();
    if (!iso) { $("out").textContent = "先填 ISO 路径"; return; }
    $("out").textContent = "正在解包 UDF 并写入 FAT/exFAT（大 ISO 会很久）…";
    const r = await sh("isodrive convert " + q(iso));
    out($("out"), r);
    const lines = (r.stdout || "").trim().split("\n");
    const last = lines[lines.length - 1] || "";
    if (r.code === 0 && last.indexOf("/") === 0) {
      $("path").value = last;
      document.querySelector('input[name=mode][value=hdd]').checked = true;
      $("rw").checked = true;
    }
  };
  $("btn-ventoy-add").onclick = async () => {
    const img = $("ventoy-out").value.trim();
    const iso = $("ventoy-iso").value.trim() || $("path").value.trim();
    $("out").textContent = "拷贝中…";
    out($("out"), await sh("isodrive ventoy-add " + q(img) + " " + q(iso)));
  };
  $("btn-cfg-save").onclick = async () => {
    await sh("isodrive cfg persist " + ($("persist").checked ? "1" : "0"));
    out($("out"), await sh("isodrive cfg autorestore " + ($("autorestore").checked ? "1" : "0")));
  };

  (async () => {
    const c = await sh("isodrive cfg");
    if (/PERSIST_MOUNT=1/.test(c.stdout)) $("persist").checked = true;
    if (/AUTO_RESTORE=0/.test(c.stdout)) $("autorestore").checked = false;
    probe();
  })();
})();
