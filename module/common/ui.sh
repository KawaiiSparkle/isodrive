#!/system/bin/sh
# ISODrive+ UI API - used by the WebUI (KernelSU / MMRL / standalone) and by the
# built-in loopback HTTP server (pure Magisk browser mode).
#
# Every "isodrive ui <command>" prints exactly ONE line of JSON on stdout.
# Sourced by system/bin/isodrive; keep this file POSIX sh (Android 6+ mksh/toybox).

UI_VERSION="1.5.0"
UI_DIR="/data/adb/isodriveplus/ui"
UI_SETTINGS="/data/adb/isodriveplus/ui.settings"
UI_JOBS="$UI_DIR/jobs"
UI_STATE="/data/adb/isodriveplus/ui.state"
UI_SERVER_LOG="$UI_DIR/server.log"
UI_IDLE="1800"

# --------------------------------------------------------------- json utils
_ui_esc() {
  # awk is missing from some old Android toybox builds; busybox/sed fallbacks.
  _esc_awk=""
  if command -v awk >/dev/null 2>&1; then
    _esc_awk="awk"
  elif [ -x /data/adb/magisk/busybox ]; then
    _esc_awk="/data/adb/magisk/busybox awk"
  elif command -v busybox >/dev/null 2>&1; then
    _esc_awk="busybox awk"
  fi
  if [ -n "$_esc_awk" ]; then
    # shellcheck disable=SC2086
    $_esc_awk 'BEGIN { ORS = ""; tab = sprintf("%c", 9); cr = sprintf("%c", 13) }
    {
      gsub(/\\/, "\\\\")
      gsub(/"/, "\\\"")
      gsub(cr, "")
      gsub(tab, "\\t")
      if (NR > 1) printf "\\n"
      printf "%s", $0
    }' | tr -d '[[:cntrl:]]'
  else
    # Degraded path (no awk, no busybox): keep the JSON valid by flattening
    # line breaks instead of escaping them.
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' | tr '\n\r\t' '   ' | tr -d '[[:cntrl:]]'
  fi
}

jstr() {
  case "$1" in
    "") printf '""' ;;
    *) printf '"%s"' "$(printf '%s' "$1" | _ui_esc)" ;;
  esac
}

jbool() { if [ "$1" = "1" ]; then printf 'true'; else printf 'false'; fi; }

jnum() {
  case "$1" in
    ''|*[!0-9-]*) printf '0' ;;
    *) printf '%s' "$1" ;;
  esac
}

_ui_ok() { printf '{"ok":true}\n'; }
_ui_err() { printf '{"ok":false,"error":%s}\n' "$(jstr "$1")"; }

_ui_size() {
  if [ -f "$1" ]; then
    wc -c < "$1" 2>/dev/null | tr -d ' '
  else
    printf '0'
  fi
}

_ui_kv() {
  [ -f "$1" ] || return 0
  sed -n "s/^$2=//p" "$1" 2>/dev/null | head -n 1
}

_ui_join_objs() {
  _s=""
  printf '['
  while IFS= read -r _l; do
    [ -n "$_l" ] || continue
    printf '%s%s' "$_s" "$_l"
    _s=","
  done
  printf ']'
}

_ui_join_strs() {
  _s=""
  printf '['
  while IFS= read -r _l; do
    [ -n "$_l" ] || continue
    printf '%s%s' "$_s" "$(jstr "$_l")"
    _s=","
  done
  printf ']'
}

# ----------------------------------------------------------- path helpers
_ui_self() {
  if [ -n "$ISODRIVE_SELF" ] && [ -x "$ISODRIVE_SELF" ]; then
    printf '%s\n' "$ISODRIVE_SELF"
    return 0
  fi
  _s="$(command -v isodrive 2>/dev/null)"
  [ -n "$_s" ] || _s="$0"
  case "$_s" in
    /*) ;;
    *) _s="$(cd "$(dirname "$_s")" 2>/dev/null && pwd)/$(basename "$_s")" ;;
  esac
  printf '%s\n' "$_s"
}

_ui_module_dir() {
  _s="$(_ui_self)"
  for _d in "$(dirname "$_s")/../.." /data/adb/modules/isodriveplus /data/adb/modules_update/isodriveplus; do
    [ -f "$_d/module.prop" ] && { ( cd "$_d" 2>/dev/null && pwd ); return 0; }
  done
  printf '%s\n' "/data/adb/modules/isodriveplus"
}

_ui_webroot() {
  _s="$(_ui_self)"
  for _d in "$(dirname "$_s")/../../webroot" /data/adb/modules/isodriveplus/webroot /data/adb/modules_update/isodriveplus/webroot; do
    [ -f "$_d/index.html" ] && { ( cd "$_d" 2>/dev/null && pwd ); return 0; }
  done
  return 1
}

_ui_token() {
  _t=""
  if [ -r /dev/urandom ]; then
    _t="$(head -c 16 /dev/urandom 2>/dev/null | od -An -tx1 2>/dev/null | tr -d ' \n')"
  fi
  [ -n "$_t" ] || _t="$(date +%s)$$"
  printf '%s\n' "$_t"
}

_ui_ext() {
  basename "$1" 2>/dev/null | sed 's/.*\.//' | tr 'A-Z' 'a-z'
}

# ------------------------------------------------------------ runtime state
_ui_gadget_info() {
  _ui_gname=""
  _ui_gudc=""
  _ui_gms="no"
  _cf="$(configfs_root)"
  [ -n "$_cf" ] || return 0
  for _g in "$_cf"/usb_gadget/*; do
    [ -d "$_g" ] || continue
    if ls "$_g/functions" 2>/dev/null | grep -q '^mass_storage'; then
      _ui_gname="$(basename "$_g")"
      _ui_gms="yes"
      _ui_gudc="$(tr -d ' \n' < "$_g/UDC" 2>/dev/null)"
      return 0
    fi
  done
  for _g in "$_cf"/usb_gadget/*; do
    [ -d "$_g" ] || continue
    _u="$(tr -d ' \n' < "$_g/UDC" 2>/dev/null)"
    if [ -n "$_u" ]; then
      _ui_gname="$(basename "$_g")"
      _ui_gudc="$_u"
      return 0
    fi
  done
  _ui_gname="$(ls "$_cf/usb_gadget" 2>/dev/null | head -n 1)"
}

_ui_lun_info() {
  _ui_lun_file=""
  _ui_lun_cdrom="0"
  _ui_lun_ro="1"
  _cf="$(configfs_root)"
  if [ -n "$_cf" ]; then
    for _f in "$_cf"/usb_gadget/*/functions/mass_storage*/lun.0/file; do
      [ -f "$_f" ] || continue
      _v="$(tr -d '\n' < "$_f" 2>/dev/null)"
      [ -n "$_v" ] || continue
      _dn="$(dirname "$_f")"
      _ui_lun_file="$_v"
      _ui_lun_cdrom="$(tr -d ' \n' < "$_dn/cdrom" 2>/dev/null)"
      _ui_lun_ro="$(tr -d ' \n' < "$_dn/ro" 2>/dev/null)"
      [ -n "$_ui_lun_cdrom" ] || _ui_lun_cdrom="0"
      [ -n "$_ui_lun_ro" ] || _ui_lun_ro="1"
      return 0
    done
  fi
  for _f in /sys/devices/virtual/android_usb/android0/f_mass_storage/lun/file /sys/class/android_usb/android0/f_mass_storage/lun/file; do
    [ -f "$_f" ] || continue
    _v="$(tr -d '\n' < "$_f" 2>/dev/null)"
    [ -n "$_v" ] && { _ui_lun_file="$_v"; return 0; }
  done
  return 1
}

_ui_server_status() {
  _ui_srv_pid=""
  _ui_srv_port=""
  _ui_srv_token=""
  _ui_srv_url=""
  [ -f "$UI_STATE" ] || return 1
  _ui_srv_pid="$(_ui_kv "$UI_STATE" pid)"
  _ui_srv_port="$(_ui_kv "$UI_STATE" port)"
  _ui_srv_token="$(_ui_kv "$UI_STATE" token)"
  if [ -z "$_ui_srv_pid" ] || [ -z "$_ui_srv_port" ]; then
    rm -f "$UI_STATE"
    return 1
  fi
  if ! kill -0 "$_ui_srv_pid" 2>/dev/null; then
    rm -f "$UI_STATE"
    return 1
  fi
  _ui_srv_url="http://127.0.0.1:$_ui_srv_port/#$_ui_srv_token"
  return 0
}

_ui_open_url() {
  _ui_opened=0
  if command -v am >/dev/null 2>&1 && am start -a android.intent.action.VIEW -d "$1" >/dev/null 2>&1; then
    _ui_opened=1
    return 0
  fi
  if command -v cmd >/dev/null 2>&1 && cmd activity start-activity -a android.intent.action.VIEW -d "$1" >/dev/null 2>&1; then
    _ui_opened=1
    return 0
  fi
  return 1
}

_ui_print_server() {
  echo "ISODrive+ $UI_VERSION - WebUI 已启动 / UI is running"
  echo ""
  echo "  $1"
  echo ""
  if [ "$_ui_opened" = "1" ]; then
    echo "已在浏览器中打开。如果没有看到页面，请手动复制上面的地址。"
  else
    echo "未能自动打开浏览器，请手动在浏览器里打开上面的地址。"
  fi
  echo ""
  echo "停止服务: su -c isodrive ui stop"
}

# ------------------------------------------------------------- status / env
cmd_ui_status() {
  _load_cfg
  _ui_gadget_info
  _ui_lun_info
  _mounted=0
  [ -n "$_ui_lun_file" ] && _mounted=1
  _src=""
  _args=""
  _backend=""
  if [ -f "$STATE_FILE" ]; then
    _src="$(_ui_kv "$STATE_FILE" src)"
    _args="$(_ui_kv "$STATE_FILE" args)"
    _backend="$(_ui_kv "$STATE_FILE" backend)"
    [ -n "$_src" ] && _mounted=1
  fi
  _disp="$_ui_lun_file"
  [ -n "$_disp" ] || _disp="$_src"
  _rw=0
  [ "$_ui_lun_ro" = "0" ] && _rw=1
  _bound=0
  [ -n "$_ui_gudc" ] && _bound=1
  _srv_run=0
  _srv_url2=""
  _srv_port2="0"
  if _ui_server_status; then
    _srv_run=1
    _srv_url2="$_ui_srv_url"
    _srv_port2="$_ui_srv_port"
  fi
  printf '{"ok":true,"version":%s,"mounted":%s,"path":%s,"name":%s,"src":%s,"args":%s,"backend":%s,"cdrom":%s,"rw":%s,"selinux":%s,"gadget":%s,"udc":%s,"bound":%s,"persist":%s,"autorestore":%s,"engine":%s,"server":{"running":%s,"port":%s,"url":%s}}\n' \
    "$(jstr "$UI_VERSION")" \
    "$(jbool "$_mounted")" \
    "$(jstr "$_disp")" \
    "$(jstr "$(basename "$_disp" 2>/dev/null)")" \
    "$(jstr "$_src")" \
    "$(jstr "$_args")" \
    "$(jstr "$_backend")" \
    "$(jbool "$_ui_lun_cdrom")" \
    "$(jbool "$_rw")" \
    "$(jstr "$(getenforce 2>/dev/null)")" \
    "$(jstr "$_ui_gname")" \
    "$(jstr "$_ui_gudc")" \
    "$(jbool "$_bound")" \
    "$(jstr "$(cfg_get PERSIST_MOUNT 0)")" \
    "$(jstr "$(cfg_get AUTO_RESTORE 1)")" \
    "$(jstr "$(find_native 2>/dev/null)")" \
    "$(jbool "$_srv_run")" \
    "$(jnum "$_srv_port2")" \
    "$(jstr "$_srv_url2")"
}

cmd_ui_version() {
  _load_cfg
  printf '{"ok":true,"version":%s,"engine":%s,"module":%s,"selinux":%s}\n' \
    "$(jstr "$UI_VERSION")" \
    "$(jstr "$(find_native 2>/dev/null)")" \
    "$(jstr "$(_ui_module_dir)")" \
    "$(jstr "$(getenforce 2>/dev/null)")"
}

cmd_ui_env() {
  _load_cfg
  _ui_gadget_info
  _cf="$(configfs_root)"
  _au=0
  android_usb_ok && _au=1
  _cfok=0
  [ -n "$_cf" ] && _cfok=1
  _bin="$(find_native 2>/dev/null)"
  _root="$(_ui_webroot)"
  _gadgets=""
  [ "$_cfok" = "1" ] && _gadgets="$(ls "$_cf/usb_gadget" 2>/dev/null)"
  _udcs="$(list_udc 2>/dev/null)"
  mkdir -p "$UI_DIR"
  _sp="$UI_DIR/scanpaths.$$.tmp"
  _dl="$UI_DIR/dirs.$$.tmp"
  scan_paths_list > "$_sp" 2>/dev/null
  : > "$_dl"
  while IFS= read -r _d; do
    [ -n "$_d" ] || continue
    _e=0
    [ -d "$_d" ] && _e=1
    printf '{"path":%s,"exists":%s}\n' "$(jstr "$_d")" "$(jbool "$_e")" >> "$_dl"
  done < "$_sp"
  _srv_run=0
  _srv_url2=""
  _srv_port2="0"
  if _ui_server_status; then
    _srv_run=1
    _srv_url2="$_ui_srv_url"
    _srv_port2="$_ui_srv_port"
  fi
  printf '{"ok":true,"version":%s,"selinux":%s,"configfs":%s,"configfs_root":%s,"usb_gadget":%s,"android_usb":%s,"mass_storage":%s,"gadget":%s,"udc":%s,"bound":%s,"gadgets":%s,"udcs":%s,"engine":%s,"module_dir":%s,"webroot":%s,"paths":%s,"dirs":%s,"persist":%s,"autorestore":%s,"server":{"running":%s,"port":%s,"url":%s}}\n' \
    "$(jstr "$UI_VERSION")" \
    "$(jstr "$(getenforce 2>/dev/null)")" \
    "$(jbool "$_cfok")" \
    "$(jstr "$_cf")" \
    "$(jbool "$_cfok")" \
    "$(jbool "$_au")" \
    "$(jbool "$_ui_gms")" \
    "$(jstr "$_ui_gname")" \
    "$(jstr "$_ui_gudc")" \
    "$(if [ -n "$_ui_gudc" ]; then printf 'true'; else printf 'false'; fi)" \
    "$(printf '%s\n' "$_gadgets" | _ui_join_strs)" \
    "$(printf '%s\n' "$_udcs" | _ui_join_strs)" \
    "$(jstr "$_bin")" \
    "$(jstr "$(_ui_module_dir)")" \
    "$(jstr "$_root")" \
    "$(scan_paths_list | _ui_join_strs)" \
    "$(_ui_join_objs < "$_dl")" \
    "$(jstr "$(cfg_get PERSIST_MOUNT 0)")" \
    "$(jstr "$(cfg_get AUTO_RESTORE 1)")" \
    "$(jbool "$_srv_run")" \
    "$(jnum "$_srv_port2")" \
    "$(jstr "$_srv_url2")"
  rm -f "$_sp" "$_dl"
}

# ---------------------------------------------------------------- image scan
_ui_scan_files() {
  mkdir -p "$UI_DIR"
  _sp="$UI_DIR/scanpaths.$$.tmp"
  _raw="$UI_DIR/scanraw.$$.tmp"
  _canon="$UI_DIR/scancanon.$$.tmp"
  scan_paths_list > "$_sp" 2>/dev/null
  : > "$_raw"
  while IFS= read -r _d; do
    [ -n "$_d" ] || continue
    [ -d "$_d" ] || continue
    find "$_d" -maxdepth 3 -type f \( -iname '*.iso' -o -iname '*.img' -o -iname '*.bin' \) 2>/dev/null >> "$_raw"
  done < "$_sp"
  : > "$_canon"
  while IFS= read -r _f; do
    [ -n "$_f" ] || continue
    _r="$(readlink -f "$_f" 2>/dev/null)"
    [ -n "$_r" ] || _r="$_f"
    printf '%s\n' "$_r" >> "$_canon"
  done < "$_raw"
  sort -u "$_canon"
  rm -f "$_sp" "$_raw" "$_canon"
}

cmd_ui_images() {
  _load_cfg
  _ui_lun_info
  _limit="$1"
  case "$_limit" in ''|*[!0-9]*) _limit="400" ;; esac
  mkdir -p "$UI_DIR"
  _sp="$UI_DIR/scanpaths.$$.tmp"
  _dl="$UI_DIR/dirs.$$.tmp"
  scan_paths_list > "$_sp" 2>/dev/null
  : > "$_dl"
  while IFS= read -r _d; do
    [ -n "$_d" ] || continue
    _e=0
    [ -d "$_d" ] && _e=1
    printf '{"path":%s,"exists":%s}\n' "$(jstr "$_d")" "$(jbool "$_e")" >> "$_dl"
  done < "$_sp"
  _all="$(_ui_scan_files | head -n "$_limit")"
  _objs="$UI_DIR/objs.$$.tmp"
  : > "$_objs"
  _n=0
  while IFS= read -r _f; do
    [ -n "$_f" ] || continue
    _sz="$(stat -c %s "$_f" 2>/dev/null)"
    [ -n "$_sz" ] || _sz="$(wc -c < "$_f" 2>/dev/null | tr -d ' ')"
    [ -n "$_sz" ] || _sz=0
    _mt="$(stat -c %Y "$_f" 2>/dev/null)"
    [ -n "$_mt" ] || _mt=0
    _mtched=0
    [ "$_f" = "$_ui_lun_file" ] && _mtched=1
    printf '{"path":%s,"name":%s,"dir":%s,"size":%s,"mtime":%s,"ext":%s,"mounted":%s}\n' \
      "$(jstr "$_f")" "$(jstr "$(basename "$_f")")" "$(jstr "$(dirname "$_f")")" \
      "$(jnum "$_sz")" "$(jnum "$_mt")" "$(jstr "$(_ui_ext "$_f")")" "$(jbool "$_mtched")" >> "$_objs"
    _n=$((_n + 1))
  done <<EOF
$_all
EOF
  printf '{"ok":true,"count":%s,"files":%s,"dirs":%s,"scan_file":%s,"mounted_path":%s}\n' \
    "$(jnum "$_n")" "$(_ui_join_objs < "$_objs")" "$(_ui_join_objs < "$_dl")" \
    "$(jstr "$SCAN_FILE")" "$(jstr "$_ui_lun_file")"
  rm -f "$_sp" "$_dl" "$_objs"
}

# --------------------------------------------------------------- scan paths
cmd_ui_paths() {
  _load_cfg
  _exists=0
  [ -f "$SCAN_FILE" ] && _exists=1
  _custom="[]"
  if [ "$_exists" = "1" ]; then
    _custom="$(grep -v '^#' "$SCAN_FILE" 2>/dev/null | grep -v '^$' | _ui_join_strs)"
  fi
  printf '{"ok":true,"file":%s,"exists":%s,"custom":%s,"defaults":%s,"active":%s}\n' \
    "$(jstr "$SCAN_FILE")" "$(jbool "$_exists")" \
    "$_custom" "$(scan_paths_default | _ui_join_strs)" "$(scan_paths_list | _ui_join_strs)"
}

cmd_ui_path_add() {
  _load_cfg
  _p="$1"
  [ -n "$_p" ] || { _ui_err "需要目录路径 / missing path"; return 1; }
  scan_paths_add "$_p"
  printf '{"ok":true,"path":%s,"active":%s}\n' "$(jstr "$_p")" "$(scan_paths_list | _ui_join_strs)"
}

cmd_ui_path_del() {
  _load_cfg
  _p="$1"
  [ -n "$_p" ] || { _ui_err "需要目录路径 / missing path"; return 1; }
  scan_paths_del "$_p"
  printf '{"ok":true,"path":%s,"active":%s}\n' "$(jstr "$_p")" "$(scan_paths_list | _ui_join_strs)"
}

# -------------------------------------------------------------------- config
cmd_ui_cfg() {
  _load_cfg
  printf '{"ok":true,"persist":%s,"autorestore":%s,"file":"/data/adb/isodriveplus/config"}\n' \
    "$(jstr "$(cfg_get PERSIST_MOUNT 0)")" "$(jstr "$(cfg_get AUTO_RESTORE 1)")"
}

cmd_ui_cfg_set() {
  _load_cfg
  _k="$1"
  _v="$2"
  case "$_k" in
    persist) cfg_set PERSIST_MOUNT "$_v" ;;
    autorestore) cfg_set AUTO_RESTORE "$_v" ;;
    *) _ui_err "未知设置 / unknown key: $_k"; return 1 ;;
  esac
  cmd_ui_cfg
}

# ------------------------------------------------------------------ diagnose
cmd_ui_diag() {
  _t="$( ( cmd_diag ) 2>&1 )"
  printf '{"ok":true,"text":%s}\n' "$(jstr "$_t")"
}

cmd_ui_log() {
  _n="$1"
  case "$_n" in ''|*[!0-9]*) _n="200" ;; esac
  _t=""
  [ -f "$LOG_FILE" ] && _t="$(tail -n "$_n" "$LOG_FILE" 2>/dev/null)"
  printf '{"ok":true,"lines":%s,"text":%s}\n' "$(jnum "$_n")" "$(jstr "$_t")"
}

# --------------------------------------------------------------------- jobs
ui_job_spawn() {
  _kind="$1"
  shift
  _self="$(_ui_self)"
  mkdir -p "$UI_JOBS"
  _i=0
  for _old in $(ls -t "$UI_JOBS" 2>/dev/null); do
    _i=$((_i + 1))
    [ "$_i" -gt 10 ] && rm -rf "$UI_JOBS/$_old"
  done
  _id="job-$(date +%s)-$$"
  _jd="$UI_JOBS/$_id"
  mkdir -p "$_jd"
  printf '%s\n' "$_kind" > "$_jd/kind"
  printf '%s\n' "$_self" > "$_jd/self"
  if [ $# -gt 0 ]; then printf '%s\n' "$@" > "$_jd/args"; else : > "$_jd/args"; fi
  date +%s > "$_jd/started"
  : > "$_jd/log"
  if command -v setsid >/dev/null 2>&1; then
    ISODRIVE_SELF="$_self" setsid "$_self" ui job-run "$_jd" >>"$_jd/log" 2>&1 </dev/null &
  else
    ISODRIVE_SELF="$_self" nohup "$_self" ui job-run "$_jd" >>"$_jd/log" 2>&1 </dev/null &
  fi
  _pid=$!
  printf '%s\n' "$_pid" > "$_jd/pid"
  printf '%s\n' "$_id"
}

cmd_ui_job_run() {
  _jd="$1"
  [ -d "$_jd" ] || { echo "job dir missing: $_jd"; return 1; }
  # Inside a job our stdout is the job log, so keep wrapper progress lines
  # visible there instead of writing them to the global log only.
  UI_JSON=0
  _kind="$(cat "$_jd/kind" 2>/dev/null)"
  set --
  while IFS= read -r _line; do
    set -- "$@" "$_line"
  done < "$_jd/args"
  (
    case "$_kind" in
      mount) cmd_mount "$@" ;;
      convert) ui_run_convert "$@" ;;
      ventoy-init) _load_cfg; ventoy_init "$@" ;;
      ventoy-add) _load_cfg; ventoy_add "$@" ;;
      blank) cmd_blank "$@" ;;
      restore) restore_usb ;;
      *) echo "unknown job kind: $_kind"; exit 2 ;;
    esac
  )
  _rc=$?
  printf '%s\n' "$_rc" > "$_jd/rc"
  echo "[isodrive-ui] job finished rc=$_rc"
  return 0
}

cmd_ui_job_poll() {
  _id="$1"
  _jd="$UI_JOBS/$_id"
  [ -d "$_jd" ] || { _ui_err "任务不存在 / no such job: $_id"; return 1; }
  _kind="$(cat "$_jd/kind" 2>/dev/null)"
  _running=1
  _rc=""
  if [ -f "$_jd/rc" ]; then
    _running=0
    _rc="$(cat "$_jd/rc" 2>/dev/null)"
  fi
  _size="$(_ui_size "$_jd/log")"
  _tail="$(tail -c 12000 "$_jd/log" 2>/dev/null)"
  _started="$(cat "$_jd/started" 2>/dev/null)"
  if [ -n "$_rc" ]; then _rcjson="$(jnum "$_rc")"; else _rcjson="null"; fi
  printf '{"ok":true,"id":%s,"kind":%s,"running":%s,"rc":%s,"size":%s,"started":%s,"tail":%s}\n' \
    "$(jstr "$_id")" "$(jstr "$_kind")" "$(jbool "$_running")" "$_rcjson" \
    "$(jnum "$_size")" "$(jnum "$_started")" "$(jstr "$_tail")"
}

cmd_ui_job_cancel() {
  _id="$1"
  _jd="$UI_JOBS/$_id"
  [ -d "$_jd" ] || { _ui_err "任务不存在 / no such job: $_id"; return 1; }
  if [ -f "$_jd/rc" ]; then
    printf '{"ok":true,"cancelled":false,"running":false}\n'
    return 0
  fi
  printf '1\n' > "$_jd/killing"
  _pid="$(cat "$_jd/pid" 2>/dev/null)"
  if [ -n "$_pid" ]; then
    kill -TERM "-$_pid" 2>/dev/null || kill -TERM "$_pid" 2>/dev/null
    sleep 1
    kill -KILL "-$_pid" 2>/dev/null || kill -KILL "$_pid" 2>/dev/null
  fi
  printf '{"ok":true,"cancelled":true,"running":false}\n'
}

cmd_ui_jobs() {
  mkdir -p "$UI_DIR"
  _objs="$UI_DIR/jobs.$$.tmp"
  : > "$_objs"
  _i=0
  for _id in $(ls -t "$UI_JOBS" 2>/dev/null); do
    _i=$((_i + 1))
    [ "$_i" -gt 8 ] && break
    _jd="$UI_JOBS/$_id"
    [ -d "$_jd" ] || continue
    _kind="$(cat "$_jd/kind" 2>/dev/null)"
    _running=1
    _rc="null"
    if [ -f "$_jd/rc" ]; then
      _running=0
      _rc="$(jnum "$(cat "$_jd/rc" 2>/dev/null)")"
    fi
    printf '{"id":%s,"kind":%s,"running":%s,"rc":%s,"size":%s,"started":%s}\n' \
      "$(jstr "$_id")" "$(jstr "$_kind")" "$(jbool "$_running")" "$_rc" \
      "$(jnum "$(_ui_size "$_jd/log")")" "$(jnum "$(cat "$_jd/started" 2>/dev/null)")" >> "$_objs"
  done
  printf '{"ok":true,"jobs":%s}\n' "$(_ui_join_objs < "$_objs")"
  rm -f "$_objs"
}

ui_run_convert() {
  _u=""
  for _p in /data/adb/modules/isodriveplus/system/bin/udf2disk /data/adb/modules_update/isodriveplus/system/bin/udf2disk "$(dirname "$(_ui_self)")/udf2disk"; do
    [ -x "$_p" ] && { _u="$_p"; break; }
  done
  [ -n "$_u" ] || { echo "udf2disk binary missing"; return 1; }
  mkdir -p "$IMG_DIR"
  "$_u" "$@"
  _rc=$?
  echo "[isodrive-ui] convert exit=$_rc"
  return $_rc
}

cmd_ui_mount() {
  _file=""
  _flags=""
  for _a in "$@"; do
    case "$_a" in
      -*) _flags="$_flags $_a" ;;
      *) [ -z "$_file" ] && _file="$_a" ;;
    esac
  done
  [ -n "$_file" ] || { _ui_err "缺少镜像路径 / missing image path"; return 1; }
  [ -f "$_file" ] || { _ui_err "文件不存在 / file not found: $_file"; return 1; }
  _id="$(ui_job_spawn mount "$_file" $_flags)"
  printf '{"ok":true,"job":%s,"kind":"mount","path":%s}\n' "$(jstr "$_id")" "$(jstr "$_file")"
}

cmd_ui_unmount() {
  _t="$( ( restore_usb ) 2>&1 )"
  _rc=$?
  if [ "$_rc" = "0" ]; then _ok=1; else _ok=0; fi
  printf '{"ok":%s,"code":%s,"text":%s}\n' "$(jbool "$_ok")" "$(jnum "$_rc")" "$(jstr "$_t")"
}

cmd_ui_job_start_cmd() {
  _kind="$1"
  shift
  case "$_kind" in
    mount|convert|ventoy-init|ventoy-add|blank|restore) ;;
    *) _ui_err "非法任务类型 / bad kind: $_kind"; return 1 ;;
  esac
  _id="$(ui_job_spawn "$_kind" "$@")"
  printf '{"ok":true,"job":%s,"kind":%s}\n' "$(jstr "$_id")" "$(jstr "$_kind")"
}

# --------------------------------------------------------------- http server
cmd_ui_start() {
  _no_open=0
  _human=0
  _port=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --no-open) _no_open=1 ;;
      --human) _human=1 ;;
      --port) shift; _port="$1" ;;
    esac
    shift
  done
  if _ui_server_status; then
    _url="$_ui_srv_url"
    _ui_opened=0
    [ "$_no_open" = "1" ] || _ui_open_url "$_url"
    if [ "$_human" = "1" ]; then
      _ui_print_server "$_url"
    else
      printf '{"ok":true,"running":true,"reused":true,"url":%s,"port":%s}\n' "$(jstr "$_url")" "$(jnum "$_ui_srv_port")"
    fi
    return 0
  fi
  _bin="$(find_native 2>/dev/null)"
  [ -n "$_bin" ] || { _ui_err "引擎缺失 (isodrive.bin) / engine missing"; return 1; }
  _root="$(_ui_webroot)"
  [ -n "$_root" ] || { _ui_err "WebUI 文件缺失 (webroot) / webroot missing"; return 1; }
  _self="$(_ui_self)"
  _token="$(_ui_token)"
  mkdir -p "$UI_DIR"
  rm -f "$UI_STATE"
  if command -v setsid >/dev/null 2>&1; then
    _runner="setsid"
  else
    _runner="nohup"
  fi
  if [ -n "$_port" ]; then
    $_runner "$_bin" serve --root "$_root" --cli "$_self" --state "$UI_STATE" --token "$_token" --idle "$UI_IDLE" --port "$_port" >>"$UI_SERVER_LOG" 2>&1 </dev/null &
  else
    $_runner "$_bin" serve --root "$_root" --cli "$_self" --state "$UI_STATE" --token "$_token" --idle "$UI_IDLE" >>"$UI_SERVER_LOG" 2>&1 </dev/null &
  fi
  _i=0
  while [ "$_i" -lt 8 ]; do
    [ -s "$UI_STATE" ] && break
    sleep 1
    _i=$((_i + 1))
  done
  if ! _ui_server_status; then
    _e="$(tail -n 6 "$UI_SERVER_LOG" 2>/dev/null)"
    _ui_err "UI 服务启动失败 / server failed: $_e"
    return 1
  fi
  _url="$_ui_srv_url"
  _ui_opened=0
  [ "$_no_open" = "1" ] || _ui_open_url "$_url"
  if [ "$_human" = "1" ]; then
    _ui_print_server "$_url"
  else
    printf '{"ok":true,"running":true,"reused":false,"url":%s,"port":%s,"opened":%s}\n' \
      "$(jstr "$_url")" "$(jnum "$_ui_srv_port")" "$(jbool "$_ui_opened")"
  fi
}

cmd_ui_stop() {
  if ! _ui_server_status; then
    rm -f "$UI_STATE"
    printf '{"ok":true,"stopped":false,"running":false}\n'
    return 0
  fi
  _pid="$_ui_srv_pid"
  kill -TERM "$_pid" 2>/dev/null
  _i=0
  while [ "$_i" -lt 4 ]; do
    if ! kill -0 "$_pid" 2>/dev/null; then break; fi
    sleep 1
    _i=$((_i + 1))
  done
  if kill -0 "$_pid" 2>/dev/null; then kill -KILL "$_pid" 2>/dev/null; fi
  rm -f "$UI_STATE"
  printf '{"ok":true,"stopped":true,"running":false}\n'
}

# ---------------------------------------------------------------- interactive
ui_help() {
  cat <<EOF
ISODrive+ $UI_VERSION - WebUI / UI 服务

  isodrive ui                 交互菜单（终端）
  isodrive ui env             能力/配置 JSON（WebUI 用）
  isodrive ui status          挂载状态 JSON
  isodrive ui images [N]      扫描镜像 JSON
  isodrive ui mount <file> [-cdrom|-hdd|-rw|-windows|-usb3]
  isodrive ui unmount         卸载并恢复 MTP/ADB
  isodrive ui paths / path-add <dir> / path-del <dir>
  isodrive ui cfg / cfg-set persist|autorestore 0|1
  isodrive ui convert <iso> [out.img]
  isodrive ui ventoy-init [img] [GB] / ventoy-add <img> <iso>
  isodrive ui jobs / job-poll <id> / job-cancel <id>
  isodrive ui log [N]         日志
  isodrive ui diag            诊断（LUN / AVC）
  isodrive ui start [--human] [--no-open] [--port N]
  isodrive ui stop            停止内置 WebUI 服务

纯 Magisk：点模块的“操作/Action”按钮即会打开图形界面（内置本地服务 + 浏览器）。
KernelSU：模块页 -> 打开 WebUI；也可和纯 Magisk 一样用 ui start。
EOF
}

ui_menu_mount() {
  _load_cfg
  echo "扫描镜像…"
  _files="$(_ui_scan_files)"
  if [ -z "$_files" ]; then
    echo "未找到 ISO/IMG。先用 isodrive paths-add /your/dir 添加目录。"
    return 0
  fi
  _i=0
  while IFS= read -r _f; do
    _i=$((_i + 1))
    printf '%3d) %s\n' "$_i" "$_f"
  done <<EOF
$_files
EOF
  printf '选择编号（回车取消）: '
  read -r _n || return 0
  case "$_n" in ''|*[!0-9]*) return 0 ;; esac
  _f="$(printf '%s\n' "$_files" | sed -n "$_n""p")"
  [ -n "$_f" ] || return 0
  echo "模式: 1) 自动  2) CD-ROM  3) 硬盘  4) Windows ISO"
  printf '选择模式（默认 1）: '
  read -r _m || return 0
  _flags=""
  case "$_m" in
    2) _flags="-cdrom" ;;
    3) _flags="-hdd" ;;
    4) _flags="-windows" ;;
  esac
  printf '只读 / 可写 (r/w，默认 r): '
  read -r _rw || return 0
  [ "$_rw" = "w" ] && _flags="$_flags -rw"
  printf 'USB 3.0 描述符? (y/N): '
  read -r _u3 || return 0
  [ "$_u3" = "y" ] && _flags="$_flags -usb3"
  echo "挂载 $_f $_flags …"
  cmd_mount "$_f" $_flags
}

cmd_ui_menu() {
  if [ ! -t 0 ] || [ ! -t 1 ]; then
    ui_help
    return 0
  fi
  while true; do
    echo ""
    echo "============================================"
    echo " ISODrive+ $UI_VERSION  交互菜单"
    _ui_lun_info
    if [ -n "$_ui_lun_file" ]; then
      echo " 状态: 已挂载 -> $_ui_lun_file"
    else
      echo " 状态: 未挂载"
    fi
    echo "============================================"
    echo "  1) 能力探测 / 详细状态"
    echo "  2) 浏览镜像并挂载"
    echo "  3) 卸载并恢复 MTP/ADB"
    echo "  4) 打开图形界面（浏览器）"
    echo "  5) 诊断（LUN / AVC）"
    echo "  6) 开机挂载 / 拔线恢复设置"
    echo "  7) 帮助"
    echo "  0) 退出"
    printf '请选择: '
    read -r _c || return 0
    case "$_c" in
      1) cmd_probe ;;
      2) ui_menu_mount ;;
      3) restore_usb ;;
      4) cmd_ui_start --human ;;
      5) cmd_diag ;;
      6) _load_cfg
         echo "PERSIST_MOUNT=$(cfg_get PERSIST_MOUNT 0)  AUTO_RESTORE=$(cfg_get AUTO_RESTORE 1)"
         echo "输入 {开机挂载 拔线恢复}，如 '1 1'；回车跳过:"
         read -r _p _a || true
         [ -n "$_p" ] && cfg_set PERSIST_MOUNT "$_p"
         [ -n "$_a" ] && cfg_set AUTO_RESTORE "$_a"
         echo "已保存" ;;
      7) ui_help ;;
      0|q|Q) return 0 ;;
      *) echo "无效选项" ;;
    esac
  done
}

# ------------------------------------------------------------------- dispatch
ui_dispatch() {
  _sub="$1"
  [ $# -gt 0 ] && shift
  case "$_sub" in
    ''|menu|tty) cmd_ui_menu ;;
    version) cmd_ui_version ;;
    env) cmd_ui_env ;;
    status) cmd_ui_status ;;
    images|list) cmd_ui_images "$1" ;;
    paths) cmd_ui_paths ;;
    path-add|paths-add) cmd_ui_path_add "$1" ;;
    path-del|paths-del) cmd_ui_path_del "$1" ;;
    cfg|config) cmd_ui_cfg ;;
    cfg-set) cmd_ui_cfg_set "$1" "$2" ;;
    mount) cmd_ui_mount "$@" ;;
    unmount|restore|umount) cmd_ui_unmount ;;
    convert|udf2disk) cmd_ui_job_start_cmd convert "$@" ;;
    ventoy-init) cmd_ui_job_start_cmd ventoy-init "$@" ;;
    ventoy-add) cmd_ui_job_start_cmd ventoy-add "$@" ;;
    blank) cmd_ui_job_start_cmd blank "$@" ;;
    job-run) cmd_ui_job_run "$1" ;;
    job-poll) cmd_ui_job_poll "$1" ;;
    job-cancel) cmd_ui_job_cancel "$1" ;;
    jobs) cmd_ui_jobs ;;
    log|logs) cmd_ui_log "$1" ;;
    diag) cmd_ui_diag ;;
    start|on|open) cmd_ui_start "$@" ;;
    stop|off) cmd_ui_stop ;;
    help|-h|--help) ui_help ;;
    *) _ui_err "未知 ui 命令 / unknown ui command: $_sub" ;;
  esac
}
