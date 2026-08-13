#!/system/bin/sh
MODDIR="${0%/*}"
[ -f "$MODDIR/common/sepolicy_live.sh" ] && . "$MODDIR/common/sepolicy_live.sh"
apply_live >/dev/null 2>&1

# Ensure configfs is up early
if [ ! -d /config/usb_gadget ] && [ ! -d /sys/kernel/config/usb_gadget ]; then
  mkdir -p /sys/kernel/config 2>/dev/null
  mount -t configfs none /sys/kernel/config 2>/dev/null
fi

mkdir -p /data/adb/isodriveplus/images /data/adb/isodriveplus/stage
# Label work dirs so kernel + magisk can use them
chcon -R u:object_r:system_data_file:s0 /data/adb/isodriveplus 2>/dev/null
true
