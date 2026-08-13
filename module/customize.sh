#!/system/bin/sh
SKIPUNZIP=0

# KSU fallback may not define these
[ -z "$MODPATH" ] && MODPATH="${0%/*}"
if ! command -v abort >/dev/null 2>&1; then
  abort() {
    ui_print " "
    ui_print "$*"
    ui_print " "
    exit 1
  }
fi

ui_print "******************************"
ui_print " ISODrive+ 1.2.0"
ui_print " USB gadget ISO/IMG (A6+)"
ui_print "******************************"
ui_print "- Probing kernel USB gadget mass storage..."
ui_print "- 正在探测内核是否编译了 USB 大容量存储 Gadget..."

if [ -f "$MODPATH/common/probe_gadget.sh" ]; then
  # shellcheck disable=SC1091
  . "$MODPATH/common/probe_gadget.sh"
else
  abort "! probe_gadget.sh missing / 探测脚本缺失"
fi

probe_gadget

ui_print "  kconfig=$GADGET_KCONFIG  runtime=$GADGET_RUNTIME"
[ -n "$GADGET_DETAIL" ] && ui_print "  $GADGET_DETAIL"

if [ "$GADGET_OK" != "y" ]; then
  ui_print " "
  ui_print "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
  ui_print "! DEVICE NOT SUPPORTED"
  ui_print "! 设备不支持"
  ui_print "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
  ui_print " "
  ui_print "EN: This kernel was NOT built with USB gadget"
  ui_print "    mass storage (CONFIG_USB_CONFIGFS_MASS_STORAGE"
  ui_print "    / CONFIG_USB_F_MASS_STORAGE). ISODrive+ cannot"
  ui_print "    emulate a USB disk or CD-ROM in userspace."
  ui_print "    Flash a kernel/ROM with that option, or use a"
  ui_print "    real USB stick (e.g. EtchDroid)."
  ui_print " "
  ui_print "中文: 当前内核未编译 USB Gadget 大容量存储"
  ui_print "    （CONFIG_USB_CONFIGFS_MASS_STORAGE 或"
  ui_print "    CONFIG_USB_F_MASS_STORAGE）。模块无法在用户态"
  ui_print "    虚拟 U 盘/光驱。请更换编入该选项的内核/ROM，"
  ui_print "    或用 EtchDroid 等工具外接真实 U 盘写入。"
  ui_print " "
  ui_print "  kconfig=$GADGET_KCONFIG  runtime=$GADGET_RUNTIME"
  [ -n "$GADGET_DETAIL" ] && ui_print "  $GADGET_DETAIL"
  abort "! Installation aborted / 已中止安装"
fi

if [ "$GADGET_KCONFIG" = "unknown" ] && [ "$GADGET_RUNTIME" = "y" ]; then
  ui_print "- /proc/config.gz unavailable; runtime gadget OK"
  ui_print "- 读不到内核配置，但运行时可创建 mass_storage，继续安装"
fi

# Install kelexine native binary for this ABI
ABI="$(getprop ro.product.cpu.abi 2>/dev/null)"
[ -z "$ABI" ] && ABI="$ARCH"
[ -z "$ABI" ] && ABI="arm64-v8a"
case "$ABI" in
  arm64-v8a|arm64) ABI=arm64-v8a ;;
  armeabi-v7a|armeabi|arm) ABI=armeabi-v7a ;;
  x86_64) ABI=x86_64 ;;
  x86) ABI=x86 ;;
esac
if [ ! -f "$MODPATH/libs/$ABI/isodrive" ]; then
  abort "! No kelexine binary for ABI=$ABI / 没有该架构的 isodrive 二进制"
fi
mkdir -p "$MODPATH/system/bin"
cp -f "$MODPATH/libs/$ABI/isodrive" "$MODPATH/system/bin/isodrive.bin"
chmod 755 "$MODPATH/system/bin/isodrive.bin"
chmod 755 "$MODPATH/system/bin/isodrive"
rm -rf "$MODPATH/libs"
ui_print "- kelexine isodrive.bin [$ABI] installed"
ui_print "- 已安装 kelexine 原生二进制 [$ABI]"

chmod 755 "$MODPATH/common/sepolicy_live.sh" 2>/dev/null
chmod 755 "$MODPATH/common/probe_gadget.sh" 2>/dev/null

mkdir -p /data/adb/isodriveplus/images /data/adb/isodriveplus/stage
chmod 755 /data/adb/isodriveplus /data/adb/isodriveplus/images /data/adb/isodriveplus/stage

if [ -f "$MODPATH/common/sepolicy_live.sh" ]; then
  . "$MODPATH/common/sepolicy_live.sh"
  apply_live
  ui_print "- Live sepolicy injected (enforcing-safe)"
fi

ui_print "- Supported / 设备支持 mass_storage gadget"
ui_print "- CLI: su -c isodrive"
ui_print "- WebUI: Magisk/KSU/MMRL → ISODrive+"
ui_print "- Reboot once for sepolicy.rule / 请重启一次以加载策略"
