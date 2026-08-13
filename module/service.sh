#!/system/bin/sh
# late_start: re-apply live policy after zygote (some OEM reload sepolicy)
MODDIR="${0%/*}"
sleep 15
[ -f "$MODDIR/common/sepolicy_live.sh" ] && . "$MODDIR/common/sepolicy_live.sh"
apply_live >/dev/null 2>&1
true
