/* ISODrive+ WebUI — Magisk / KernelSU / MMRL / APatch */
(function () {
  const $ = (id) => document.getElementById(id);

  function execRaw(cmd) {
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error("exec timeout")), 120000);

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

      try {
        if (window.$isodriveplus && $isodriveplus.exec) {
          $isodriveplus.exec(cmd, done);
          return;
        }
      } catch (e) {}

      clearTimeout(timeout);
      reject(new Error("没有 WebUI 执行接口。请用 KernelSU / Magisk(MMRL) 的模块 WebUI 打开，或 su -c isodrive"));
    });
  }

  async function sh(cmd) {
    const full = "export PATH=/data/adb/modules/isodriveplus/system/bin:/system/bin:$PATH; " + cmd;
    try {
      return await execRaw(full);
    } catch (e) {
      return { code: 1, stdout: "", stderr: String(e.message || e) };
    }
  }

  function out(el, r) {
    const text = [r.stdout, r.stderr].filter(Boolean).join("\n").trim() || "(no output)";
    el.textContent = text;
    el.className = "out " + (r.code === 0 ? "ok" : "bad");
  }

  async function probe() {
    const r = await sh("isodrive probe");
    $("probe").textContent = (r.stdout || r.stderr || "failed").trim();
    const se = /selinux=(\S+)/.exec(r.stdout || "");
    const pill = $("selinux");
    if (se) {
      pill.textContent = "SELinux " + se[1];
      pill.className = "pill " + se[1].toLowerCase();
    }
    const ms = /mass_storage=(\S+)/.exec(r.stdout || "");
    const hint = $("cap-hint");
    if (ms && ms[1] === "no") {
      hint.textContent = "内核没有 mass_storage gadget（CONFIG_USB_CONFIGFS_MASS_STORAGE=n）。换 ROM / 自编译内核，或用 EtchDroid 外接 U 盘。";
      hint.className = "hint bad";
    } else if ((r.stdout || "").indexOf("usb_gadget=no") >= 0 && (r.stdout || "").indexOf("android_usb=no") >= 0) {
      hint.textContent = "这台机没有 configfs usb_gadget，也没有旧版 android_usb。无法虚拟 USB。";
      hint.className = "hint bad";
    } else {
      hint.textContent = "探测通过即可在 enforcing 下挂载。镜像会自动映射到 /data/media 或 stage，避免 FUSE 导致 kernel AVC。";
      hint.className = "hint";
    }
  }

  function flags() {
    const mode = (document.querySelector("input[name=mode]:checked") || {}).value || "auto";
    let f = "";
    if (mode === "cdrom") f += " -cdrom";
    if (mode === "hdd") f += " -hdd";
    if (mode === "windows") f += " -windows";
    if ($("rw").checked) f += " -rw";
    if ($("usb3").checked) f += " -usb3";
    return f;
  }

  $("btn-probe").onclick = probe;
  $("btn-restore").onclick = async () => {
    out($("out"), await sh("isodrive restore"));
    probe();
  };
  $("btn-status").onclick = async () => {
    out($("out"), await sh("isodrive status"));
  };
  $("btn-mount").onclick = async () => {
    const p = $("path").value.trim();
    if (!p) {
      $("out").textContent = "请填写镜像路径";
      return;
    }
    $("out").textContent = "挂载中…";
    const r = await sh("isodrive '" + p.replace(/'/g, "'\\''") + "'" + flags());
    out($("out"), r);
    probe();
  };
  $("btn-list").onclick = async () => {
    const r = await sh("isodrive list");
    const ul = $("files");
    ul.innerHTML = "";
    (r.stdout || "").split("\n").forEach((line) => {
      line = line.trim();
      if (!line || line.charAt(0) === "#") return;
      const li = document.createElement("li");
      li.textContent = line;
      li.onclick = () => { $("path").value = line; };
      ul.appendChild(li);
    });
    if (!ul.childNodes.length) {
      ul.innerHTML = "<li>没找到 iso/img，放到 /sdcard/Download 或 /data/adb/isodriveplus/images</li>";
    }
  };
  $("btn-blank").onclick = async () => {
    const n = $("blank-name").value.trim() || "blank.img";
    const mb = $("blank-mb").value || "4096";
    $("out").textContent = "创建中…";
    const r = await sh("isodrive blank '" + n.replace(/'/g, "") + "' " + mb);
    out($("out"), r);
    if (r.stdout) $("path").value = r.stdout.trim().split("\n").pop();
  };

  probe();
})();
