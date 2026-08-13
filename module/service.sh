#!/system/bin/sh
# late_start: sepolicy, optional remount, unplug watcher
MODDIR="${0%/*}"
export PATH="$MODDIR/system/bin:/system/bin:$PATH"

sleep 15
[ -f "$MODDIR/common/sepolicy_live.sh" ] && . "$MODDIR/common/sepolicy_live.sh"
apply_live >/dev/null 2>&1
[ -f "$MODDIR/common/config.sh" ] && . "$MODDIR/common/config.sh"

mkdir -p /data/adb/isodriveplus

# Persist last mount (Ventoy img or ISO)
if [ "$(cfg_get PERSIST_MOUNT 0)" = "1" ] && [ -f /data/adb/isodriveplus/last.mount ]; then
  _line="$(head -1 /data/adb/isodriveplus/last.mount)"
  if [ -n "$_line" ]; then
    isodrive $_line >/data/adb/isodriveplus/persist.log 2>&1
  fi
fi

# Watch: if we had a LUN and UDC dropped (host unplug), restore MTP
(
  LAST_HAD=0
  while true; do
    sleep 8
    [ "$(cfg_get AUTO_RESTORE 1)" = "1" ] || continue
    HAD=0
    [ -f /data/adb/isodriveplus/engine.state ] && HAD=1
    [ -f /data/adb/isodriveplus/state ] && HAD=1
    UDC=""
    [ -d /sys/class/udc ] && UDC="$(ls /sys/class/udc 2>/dev/null | head -1)"
    BOUND=""
    for g in /config/usb_gadget/*/UDC /sys/kernel/config/usb_gadget/*/UDC; do
      [ -f "$g" ] || continue
      _v="$(tr -d ' \n' < "$g")"
      [ -n "$_v" ] && BOUND=1
    done
    if [ "$HAD" = "1" ] && [ "$LAST_HAD" = "1" ] && [ -z "$BOUND" ]; then
      isodrive restore >/data/adb/isodriveplus/autorestore.log 2>&1
      HAD=0
    fi
    LAST_HAD=$HAD
  done
) &

true
