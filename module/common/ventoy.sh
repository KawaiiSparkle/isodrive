#!/system/bin/sh
# Build a Ventoy disk image and mount it as a USB HDD (multi-ISO).
# Official installer when bash+loop work; otherwise ventoy.disk.img + FAT32 data.
# Not UDF — Windows/BIOS see a real Ventoy-style disk, not a burnt CD.

VENTOY_CACHE="/data/adb/isodriveplus/ventoy-cache"
VENTOY_IMG_DEFAULT="/data/adb/isodriveplus/images/ventoy.img"

ventoy_log() { echo "[ventoy] $*"; }

ventoy_fetch() {
  mkdir -p "$VENTOY_CACHE"
  if [ -f "$VENTOY_CACHE/ventoy.disk.img.xz" ] || [ -f "$VENTOY_CACHE/ventoy.disk.img" ]; then
    ventoy_log "Using cached ventoy.disk.img"
    return 0
  fi
  ventoy_log "Downloading latest Ventoy linux package (GitHub)..."
  _api="https://api.github.com/repos/ventoy/Ventoy/releases/latest"
  _url=""
  if command -v curl >/dev/null 2>&1; then
    _url="$(curl -fsSL "$_api" | grep browser_download_url | grep 'linux.tar.gz' | head -1 | cut -d'"' -f4)"
  fi
  [ -n "$_url" ] || {
    ventoy_log "Cannot resolve Ventoy release URL. Put ventoy.disk.img.xz in $VENTOY_CACHE"
    return 1
  }
  curl -fL --retry 3 -o "$VENTOY_CACHE/ventoy.tgz" "$_url" || return 1
  tar -tzf "$VENTOY_CACHE/ventoy.tgz" >/dev/null 2>&1 || return 1
  _img="$(tar -tzf "$VENTOY_CACHE/ventoy.tgz" | grep 'ventoy.disk.img.xz' | head -1)"
  [ -n "$_img" ] || return 1
  tar -xzf "$VENTOY_CACHE/ventoy.tgz" -C "$VENTOY_CACHE" "$_img"
  find "$VENTOY_CACHE" -name 'ventoy.disk.img.xz' | head -1 | while read -r p; do
    cp -f "$p" "$VENTOY_CACHE/ventoy.disk.img.xz"
  done
  [ -f "$VENTOY_CACHE/ventoy.disk.img.xz" ]
}

ventoy_disk_img() {
  if [ -f "$VENTOY_CACHE/ventoy.disk.img" ]; then
    echo "$VENTOY_CACHE/ventoy.disk.img"
    return 0
  fi
  if [ -f "$VENTOY_CACHE/ventoy.disk.img.xz" ]; then
    if command -v xz >/dev/null 2>&1; then
      xz -dc "$VENTOY_CACHE/ventoy.disk.img.xz" > "$VENTOY_CACHE/ventoy.disk.img" || return 1
    elif command -v busybox >/dev/null 2>&1; then
      busybox xz -dc "$VENTOY_CACHE/ventoy.disk.img.xz" > "$VENTOY_CACHE/ventoy.disk.img" || return 1
    else
      ventoy_log "Need xz to decompress ventoy.disk.img.xz"
      return 1
    fi
    echo "$VENTOY_CACHE/ventoy.disk.img"
    return 0
  fi
  return 1
}

# ventoy_init <out.img> <size_gb>
ventoy_init() {
  _out="${1:-$VENTOY_IMG_DEFAULT}"
  _gb="${2:-16}"
  mkdir -p "$(dirname "$_out")" "$VENTOY_CACHE"
  ventoy_fetch || return 1
  _vdi="$(ventoy_disk_img)" || return 1
  _mb=$((_gb * 1024))
  ventoy_log "Creating ${_gb}G sparse $_out"
  dd if=/dev/zero of="$_out" bs=1M count=0 seek="$_mb" 2>/dev/null || \
    dd if=/dev/zero of="$_out" bs=1048576 count=0 seek="$_mb" || return 1

  # Prefer official Ventoy2Disk on a loop device
  _loop=""
  _tgz="$VENTOY_CACHE/ventoy.tgz"
  if [ -f "$_tgz" ] && command -v losetup >/dev/null 2>&1; then
    _loop="$(losetup -f 2>/dev/null)"
    if [ -n "$_loop" ] && losetup "$_loop" "$_out" 2>/dev/null; then
      _dir="$(mktemp -d 2>/dev/null || echo /data/adb/isodriveplus/tmp-ventoy)"
      mkdir -p "$_dir"
      tar -xzf "$_tgz" -C "$_dir"
      _sh="$(find "$_dir" -name Ventoy2Disk.sh | head -1)"
      if [ -n "$_sh" ] && command -v bash >/dev/null 2>&1; then
        ventoy_log "Running official Ventoy2Disk.sh -I $_loop"
        ( cd "$(dirname "$_sh")" && printf 'y\ny\n' | bash ./Ventoy2Disk.sh -I "$_loop" )
        _rc=$?
        losetup -d "$_loop" 2>/dev/null
        rm -rf "$_dir"
        if [ "$_rc" = "0" ]; then
          chcon u:object_r:system_data_file:s0 "$_out" 2>/dev/null
          ventoy_log "Ventoy installed: $_out"
          echo "$_out"
          return 0
        fi
        ventoy_log "Ventoy2Disk failed (rc=$_rc), fallback layout"
      else
        losetup -d "$_loop" 2>/dev/null
        rm -rf "$_dir"
      fi
    fi
  fi

  # Fallback: prefix official ventoy.disk.img, rest is one FAT32 data partition.
  # BIOS still boots Ventoy from the prefix; ISOs go on the FAT32 part (4GiB/file limit).
  ventoy_log "Fallback: dd ventoy.disk.img + FAT32 data (not UDF)"
  dd if="$_vdi" of="$_out" conv=notrunc 2>/dev/null || return 1
  _vsz="$(stat -c%s "$_vdi" 2>/dev/null || wc -c < "$_vdi")"
  _start=$(( (_vsz + 1048575) / 1048576 + 1 ))
  if command -v sfdisk >/dev/null 2>&1; then
    printf ',,c\n' | sfdisk -q --no-reread -N 3 "$_out" 2>/dev/null || true
  fi
  if command -v losetup >/dev/null 2>&1; then
    _loop="$(losetup -f)"
    losetup -P "$_loop" "$_out" 2>/dev/null || losetup "$_loop" "$_out"
    _part=""
    for p in "${_loop}p3" "${_loop}p2" "${_loop}p1" "${_loop}"; do
      [ -b "$p" ] && _part="$p"
    done
    if [ -n "$_part" ]; then
      if command -v mkfs.fat >/dev/null 2>&1; then
        mkfs.fat -F 32 -n VENTOY "$_part" 2>/dev/null || true
      elif command -v mkfs.vfat >/dev/null 2>&1; then
        mkfs.vfat -F 32 -n VENTOY "$_part" 2>/dev/null || true
      fi
    fi
    losetup -d "$_loop" 2>/dev/null
  fi
  chcon u:object_r:system_data_file:s0 "$_out" 2>/dev/null
  ventoy_log "Ready: $_out  (copy ISOs onto the data volume after mounting as -hdd -rw once, or use ventoy-add)"
  echo "$_out"
}

# Copy an ISO onto the Ventoy data partition
ventoy_add() {
  _img="${1:-$VENTOY_IMG_DEFAULT}"
  _iso="$2"
  [ -f "$_img" ] && [ -f "$_iso" ] || { ventoy_log "usage: ventoy-add <ventoy.img> <file.iso>"; return 1; }
  _loop="$(losetup -f)" || return 1
  losetup -P "$_loop" "$_img" || losetup "$_loop" "$_img" || return 1
  _mnt="/data/adb/isodriveplus/mnt-ventoy"
  mkdir -p "$_mnt"
  _ok=0
  for p in "${_loop}p1" "${_loop}p2" "${_loop}p3"; do
    [ -b "$p" ] || continue
    if mount -t vfat -o rw "$p" "$_mnt" 2>/dev/null || mount -t exfat -o rw "$p" "$_mnt" 2>/dev/null || mount -o rw "$p" "$_mnt" 2>/dev/null; then
      cp -f "$_iso" "$_mnt/" && _ok=1
      sync
      umount "$_mnt"
      [ "$_ok" = "1" ] && break
    fi
  done
  losetup -d "$_loop" 2>/dev/null
  [ "$_ok" = "1" ] && ventoy_log "Copied $(basename "$_iso") into Ventoy image" && return 0
  ventoy_log "Could not mount a data partition in $_img"
  return 1
}
