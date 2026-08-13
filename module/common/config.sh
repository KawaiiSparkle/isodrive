# ISODrive+ user config helpers
CFG_DIR="/data/adb/isodriveplus"
CFG_FILE="$CFG_DIR/config"
SCAN_FILE="$CFG_DIR/scan.paths"
LAST_FILE="$CFG_DIR/last.mount"

cfg_get() {
  # cfg_get KEY default
  _k="$1"; _d="$2"
  [ -f "$CFG_FILE" ] || { echo "$_d"; return 0; }
  _v="$(grep "^$_k=" "$CFG_FILE" 2>/dev/null | tail -1 | cut -d= -f2-)"
  [ -n "$_v" ] && echo "$_v" || echo "$_d"
}

cfg_set() {
  mkdir -p "$CFG_DIR"
  touch "$CFG_FILE"
  _k="$1"; _v="$2"
  if grep -q "^$_k=" "$CFG_FILE" 2>/dev/null; then
    sed -i "s|^$_k=.*|$_k=$_v|" "$CFG_FILE"
  else
    echo "$_k=$_v" >> "$CFG_FILE"
  fi
}

scan_paths_default() {
  cat <<'E'
/data/adb/isodriveplus/images
/data/media/0/Download
/data/media/0/ISO
/data/media/0/Download/ISO
/sdcard/Download
/sdcard/ISO
E
}

scan_paths_list() {
  if [ -f "$SCAN_FILE" ]; then
    grep -v '^#' "$SCAN_FILE" | grep -v '^$'
  else
    scan_paths_default
  fi
}

scan_paths_add() {
  mkdir -p "$CFG_DIR"
  touch "$SCAN_FILE"
  [ -s "$SCAN_FILE" ] || scan_paths_default > "$SCAN_FILE"
  grep -qxF "$1" "$SCAN_FILE" 2>/dev/null || echo "$1" >> "$SCAN_FILE"
}

scan_paths_del() {
  [ -f "$SCAN_FILE" ] || return 0
  grep -vxF "$1" "$SCAN_FILE" > "$SCAN_FILE.tmp" && mv "$SCAN_FILE.tmp" "$SCAN_FILE"
}
