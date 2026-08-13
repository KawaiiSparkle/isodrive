#!/system/bin/sh
# Apply the same rules live (Android 6 / old Magisk / KSU / APatch).
# magiskpolicy --live ignores unknown types; that's OK.

apply_live() {
  _POL=""
  if command -v magiskpolicy >/dev/null 2>&1; then
    _POL="magiskpolicy"
  elif command -v magisk >/dev/null 2>&1; then
    _POL="magiskpolicy"
  elif command -v ksud >/dev/null 2>&1; then
    _POL="ksud sepolicy patch"
  elif command -v supolicy >/dev/null 2>&1; then
    _POL="supolicy"
  else
    return 1
  fi

  live() {
    if [ "$_POL" = "magiskpolicy" ]; then
      magiskpolicy --live "$1" 2>/dev/null
    elif [ "$_POL" = "supolicy" ]; then
      supolicy --live "$1" 2>/dev/null
    else
      # ksud: best-effort
      ksud sepolicy patch --live "$1" 2>/dev/null
    fi
  }

  # Keep in sync with sepolicy.rule (subset of the most critical)
  for src in magisk su toolbox shell; do
    live "allow $src configfs dir { add_name create getattr ioctl lock open read remove_name rmdir search setattr write }"
    live "allow $src configfs file { create getattr ioctl lock open read setattr unlink write }"
    live "allow $src configfs lnk_file { create getattr ioctl read setattr unlink write }"
    live "allow $src sysfs file { getattr open read write }"
    live "allow $src unlabeled dir { add_name create getattr open read remove_name search write }"
    live "allow $src unlabeled file { create getattr open read write unlink }"
  done

  for t in fuse sdcardfs fuseblk media_rw_data_file system_data_file unlabeled app_data_file sdcard_posix tmpfs; do
    live "allow kernel $t file { getattr open read write ioctl lock }"
    live "allow kernel $t dir { search getattr }"
  done
  live "allow kernel block_device blk_file { getattr open read write ioctl }"
  live "allow kernel loop_device blk_file { getattr open read write ioctl }"
  return 0
}
