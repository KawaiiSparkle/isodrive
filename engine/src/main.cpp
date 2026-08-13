#include "androidusbisomanager.h"
#include "configfsisomanager.h"
#include "logger.h"
#include "util.h"
#include <iostream>
#include <string>
#include <unistd.h>

void print_help() {
  std::cout <<
      "ISODrive+ engine (kelexine fork)\n"
      "Usage:\n"
      "  isodrive <FILE> [options]   Mount FILE as USB disk/CD-ROM\n"
      "  isodrive                    Unmount + restore gadget\n"
      "  isodrive restore            Unmount + restore gadget\n"
      "  isodrive probe              Print gadget/UDC capability\n\n"
      "Options:\n"
      "  -rw            Read-write disk\n"
      "  -cdrom         CD-ROM LUN (official Windows ISOs)\n"
      "  -hdd           Force disk (disable auto-detect)\n"
      "  -windows       Windows ISO mode (CD-ROM+RO; no VID rewrite)\n"
      "  -win10/-win11  Force Windows version\n"
      "  -usb3          USB 3.0 descriptors (only with -rewrite-ids)\n"
      "  -rewrite-ids   Rewrite gadget VID/PID (breaks MTP until restore)\n"
      "  -no-stage      Do not remap /sdcard → /data/media or stage dir\n"
      "  -configfs      Force configfs\n"
      "  -usbgadget     Force legacy android_usb sysfs\n"
      "  -v / -q        Verbose / quiet\n";
}

bool configs(const std::string& iso_target, bool cdrom, bool ro,
             const WindowsMountOptions& win_opts) {
  log_info("Using configfs");
  if (!supported()) {
    log_error("usb_gadget is not supported");
    return false;
  }
  return mount_iso(iso_target, cdrom, ro, win_opts);
}

bool usb(const std::string& iso_target, bool cdrom, bool ro) {
  log_info("Using android_usb sysfs");
  if (!usb_supported()) {
    log_error("android_usb sysfs is not supported");
    return false;
  }
  if (cdrom || !ro) {
    log_warn("cdrom/ro ignored on sysfs backend");
  }
  if (iso_target.empty()) return usb_reset_iso();
  return usb_mount_iso(iso_target);
}

int main(int argc, char *argv[]) {
  if (getuid() != 0) {
    std::cerr << "Permission denied\n";
    return 1;
  }

  std::string iso_target;
  bool cdrom = false;
  bool ro = true;
  bool force_configfs = false;
  bool force_usbgadget = false;
  bool force_hdd = false;
  bool windows_mode = false;
  bool force_win10 = false;
  bool force_win11 = false;
  bool use_usb3 = false;
  bool rewrite_ids = false;
  bool do_stage = true;
  bool cmd_restore = false;
  bool cmd_probe = false;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-rw") ro = false;
    else if (arg == "-cdrom") cdrom = true;
    else if (arg == "-windows") windows_mode = true;
    else if (arg == "-win10") { windows_mode = true; force_win10 = true; }
    else if (arg == "-win11") { windows_mode = true; force_win11 = true; }
    else if (arg == "-usb3") use_usb3 = true;
    else if (arg == "-hdd") force_hdd = true;
    else if (arg == "-rewrite-ids") rewrite_ids = true;
    else if (arg == "-no-stage") do_stage = false;
    else if (arg == "-configfs") force_configfs = true;
    else if (arg == "-usbgadget") force_usbgadget = true;
    else if (arg == "-v" || arg == "-verbose") log_set_level(LogLevel::DEBUG);
    else if (arg == "-q" || arg == "-quiet") log_set_level(LogLevel::ERROR);
    else if (arg == "restore" || arg == "umount" || arg == "unmount") cmd_restore = true;
    else if (arg == "probe") cmd_probe = true;
    else if (arg == "-h" || arg == "--help" || arg == "help") {
      print_help();
      return 0;
    } else if (iso_target.empty() && arg[0] != '-') {
      iso_target = arg;
    }
  }

  if (cmd_probe) {
    probe_configfs();
    std::cout << "android_usb=" << (usb_supported() ? "yes" : "no") << "\n";
    return 0;
  }

  if (argc == 1 || cmd_restore || iso_target.empty()) {
    if (argc == 1) print_help();
    bool ok = true;
    if (supported()) ok = restore_gadget() && ok;
    if (usb_supported()) ok = usb_reset_iso() && ok;
    return ok ? 0 : 1;
  }

  if (cdrom && !ro && !windows_mode) {
    log_error("Incompatible arguments -cdrom and -rw");
    return 1;
  }
  if (cdrom && force_hdd) {
    log_error("Incompatible arguments -cdrom and -hdd");
    return 1;
  }
  if (force_win10 && force_win11) {
    log_error("Incompatible arguments -win10 and -win11");
    return 1;
  }
  if (!isfile(iso_target)) {
    log_error("File not found: " + iso_target);
    return 1;
  }
  if (looks_like_block_device(iso_target)) {
    log_error("Refusing block device (would expose internal storage)");
    return 1;
  }

  if (do_stage) {
    std::string staged = resolve_backing_file(iso_target);
    if (staged.empty()) return 1;
    if (staged != iso_target) log_info("Using backing file " + staged);
    iso_target = staged;
  }

  WindowsMountOptions win_opts = {};
  win_opts.enabled = false;
  win_opts.version = WindowsVersion::NONE;
  win_opts.use_usb3 = use_usb3;
  win_opts.rewrite_ids = rewrite_ids;

  if (!force_hdd) {
    WindowsIsoInfo iso_info = get_windows_iso_info(iso_target);
    if (iso_info.is_windows || windows_mode) {
      win_opts.enabled = true;
      if (force_win11) win_opts.version = WindowsVersion::WIN11;
      else if (force_win10) win_opts.version = WindowsVersion::WIN10;
      else if (iso_info.is_windows) win_opts.version = iso_info.version;
      else win_opts.version = WindowsVersion::WIN_UNKNOWN;
      win_opts.has_uefi = iso_info.has_uefi;
      win_opts.has_legacy = iso_info.has_legacy;
      if (iso_info.is_windows && !windows_mode) {
        log_info("Windows ISO detected: " + iso_info.volume_label);
      }
    } else if (!is_hybrid_iso(iso_target) && !cdrom) {
      log_info("Non-hybrid ISO — mounting as CD-ROM");
      cdrom = true;
    }
  }

  bool success = false;
  if (force_configfs) {
    success = configs(iso_target, cdrom, ro, win_opts);
  } else if (force_usbgadget) {
    success = usb(iso_target, cdrom, ro);
  } else if (supported()) {
    success = configs(iso_target, cdrom, ro, win_opts);
  } else if (usb_supported()) {
    success = usb(iso_target, cdrom, ro);
  } else {
    log_error("Device does not support isodrive");
    return 1;
  }
  return success ? 0 : 1;
}
