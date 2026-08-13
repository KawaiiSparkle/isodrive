#!/system/bin/sh
# Shared kernel / gadget probe. Sets:
#   GADGET_KCONFIG=y|n|unknown
#   GADGET_RUNTIME=y|n
#   GADGET_OK=y|n
#   GADGET_DETAIL=...

_read_kconfig() {
  if [ -f /proc/config.gz ]; then
    if command -v zcat >/dev/null 2>&1; then
      zcat /proc/config.gz 2>/dev/null
      return
    fi
    if command -v gzip >/dev/null 2>&1; then
      gzip -dc /proc/config.gz 2>/dev/null
      return
    fi
    toybox zcat /proc/config.gz 2>/dev/null
    return
  fi
  if [ -f /proc/config ]; then
    cat /proc/config
    return
  fi
  # some devices drop a copy here
  if [ -f /boot/config ]; then
    cat /boot/config
    return
  fi
  return 1
}

# Returns 0 if a gadget mass-storage *option* is compiled y/m
_kconfig_has_gadget_ms() {
  echo "$1" | grep -E '^CONFIG_USB_CONFIGFS_MASS_STORAGE=[ym]' >/dev/null 2>&1 && return 0
  echo "$1" | grep -E '^CONFIG_USB_F_MASS_STORAGE=[ym]' >/dev/null 2>&1 && return 0
  echo "$1" | grep -E '^CONFIG_USB_FILE_STORAGE=[ym]' >/dev/null 2>&1 && return 0
  echo "$1" | grep -E '^CONFIG_USB_MASS_STORAGE_GADGET=[ym]' >/dev/null 2>&1 && return 0
  return 1
}

_kconfig_explicitly_disabled() {
  echo "$1" | grep -E '^# CONFIG_USB_CONFIGFS_MASS_STORAGE is not set' >/dev/null 2>&1 && return 0
  echo "$1" | grep -E '^CONFIG_USB_CONFIGFS_MASS_STORAGE=n' >/dev/null 2>&1 && return 0
  echo "$1" | grep -E '^# CONFIG_USB_F_MASS_STORAGE is not set' >/dev/null 2>&1 && return 0
  return 1
}

_runtime_ms() {
  # legacy Android UMS
  for p in \
    /sys/class/android_usb/android0/f_mass_storage \
    /sys/devices/virtual/android_usb/android0/f_mass_storage
  do
    [ -d "$p" ] || [ -e "$p/lun/file" ] || [ -e "$p/lun0/file" ] && return 0
  done

  _cf=""
  if [ -d /config/usb_gadget ]; then
    _cf=/config
  elif [ -d /sys/kernel/config/usb_gadget ]; then
    _cf=/sys/kernel/config
  else
    mkdir -p /sys/kernel/config 2>/dev/null
    mount -t configfs configfs /sys/kernel/config 2>/dev/null
    [ -d /sys/kernel/config/usb_gadget ] && _cf=/sys/kernel/config
  fi
  [ -n "$_cf" ] || return 1

  _gdir="$_cf/usb_gadget"
  _g=""
  for g in "$_gdir"/*; do
    [ -d "$g" ] || continue
    case "$(basename "$g")" in .*) continue ;; esac
    _g="$g"
    # prefer one with UDC bound
    if [ -f "$g/UDC" ] && [ -n "$(tr -d ' \n' < "$g/UDC" 2>/dev/null)" ]; then
      _g="$g"
      break
    fi
  done
  if [ -z "$_g" ]; then
    mkdir -p "$_gdir/g_isodrive_probe/functions" 2>/dev/null
    _g="$_gdir/g_isodrive_probe"
  fi
  [ -d "$_g/functions" ] || return 1

  # Creating this directory is how configfs instantiates the function.
  # If the kernel was built without MASS_STORAGE, mkdir fails.
  mkdir "$_g/functions/mass_storage.isodrive_probe" 2>/dev/null
  if [ -d "$_g/functions/mass_storage.isodrive_probe" ]; then
    rmdir "$_g/functions/mass_storage.isodrive_probe" 2>/dev/null
    # also remove probe gadget if we created it empty
    rmdir "$_g/functions" "$_g" 2>/dev/null
    return 0
  fi
  # already present from OEM
  [ -d "$_g/functions/mass_storage.0" ] && return 0
  return 1
}

probe_gadget() {
  GADGET_KCONFIG=unknown
  GADGET_RUNTIME=n
  GADGET_OK=n
  GADGET_DETAIL=""

  _cfg="$(_read_kconfig)"
  if [ -n "$_cfg" ]; then
    if _kconfig_has_gadget_ms "$_cfg"; then
      GADGET_KCONFIG=y
      _hit="$(echo "$_cfg" | grep -E 'CONFIG_USB_(CONFIGFS_MASS_STORAGE|F_MASS_STORAGE|FILE_STORAGE|MASS_STORAGE_GADGET)=' | head -5 | tr '\n' ' ')"
      GADGET_DETAIL="kconfig: $_hit"
    elif _kconfig_explicitly_disabled "$_cfg"; then
      GADGET_KCONFIG=n
      GADGET_DETAIL="kconfig: CONFIG_USB_CONFIGFS_MASS_STORAGE is not set"
    else
      # config readable but gadget MS symbols absent → treat as not compiled
      GADGET_KCONFIG=n
      GADGET_DETAIL="kconfig: no USB gadget mass-storage option found"
    fi
  else
    GADGET_KCONFIG=unknown
    GADGET_DETAIL="kconfig: /proc/config.gz not available"
  fi

  if _runtime_ms; then
    GADGET_RUNTIME=y
  else
    GADGET_RUNTIME=n
  fi

  # Compiled-in (kconfig=y) or runtime can instantiate the function.
  # Explicit kconfig=n always fails even if something looks similar.
  if [ "$GADGET_KCONFIG" = "n" ]; then
    GADGET_OK=n
  elif [ "$GADGET_KCONFIG" = "y" ] || [ "$GADGET_RUNTIME" = "y" ]; then
    GADGET_OK=y
  else
    GADGET_OK=n
  fi
}
