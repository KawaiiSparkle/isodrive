#include "configfsisomanager.h"
#include "logger.h"
#include "util.h"
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static const char kOurGadget[] = "g_isodriveplus";
static const char kOurFunction[] = "mass_storage.isodp";

bool supported() {
    ensure_configfs_mounted();
    return !fs_mount_point("configfs").empty() ||
           fs::exists("/config/usb_gadget") ||
           fs::exists("/sys/kernel/config/usb_gadget");
}

static std::string usb_gadget_parent() {
  ensure_configfs_mounted();
  std::string configFsRoot = fs_mount_point("configfs");
  if (configFsRoot.empty()) {
    if (fs::exists("/config/usb_gadget")) return "/config/usb_gadget";
    if (fs::exists("/sys/kernel/config/usb_gadget")) return "/sys/kernel/config/usb_gadget";
    return "";
  }
  return (fs::path(configFsRoot) / "usb_gadget").string();
}

std::string get_gadget_root() {
  std::string usbGadgetRoot = usb_gadget_parent();
  if (usbGadgetRoot.empty() || !fs::is_directory(usbGadgetRoot)) {
      log_debug("usb_gadget directory not found");
      return "";
  }

  // 1) already-active gadget (vendor g1 etc.) — prefer this so we stay on the composite
  std::string first;
  for (const auto& entry : fs::directory_iterator(usbGadgetRoot)) {
      if (entry.path().filename().string()[0] == '.') continue;
      fs::path gadget = entry.path();
      if (first.empty()) first = gadget.string();
      if (!sysfs_read((gadget / "UDC").string()).empty()) {
          log_debug("Found active gadget: " + gadget.string());
          return gadget.string();
      }
  }

  // 2) reuse our gadget if we created it earlier
  fs::path ours = fs::path(usbGadgetRoot) / kOurGadget;
  if (fs::exists(ours)) return ours.string();

  // 3) any existing gadget (UDC not bound yet — cable unplugged)
  if (!first.empty()) {
    log_info("No UDC bound; using existing gadget " + first);
    return first;
  }

  // 4) create our own
  std::error_code ec;
  fs::create_directories(ours / "configs" / "c.1" / "strings" / "0x409", ec);
  fs::create_directories(ours / "strings" / "0x409", ec);
  fs::create_directories(ours / "functions", ec);
  if (!fs::exists(ours)) {
    log_error("Failed to create gadget " + ours.string());
    return "";
  }
  sysfs_write((ours / "idVendor").string(), "0x18d1");
  sysfs_write((ours / "idProduct").string(), "0x4e26");
  sysfs_write((ours / "bcdUSB").string(), "0x0200");
  sysfs_write((ours / "strings/0x409/manufacturer").string(), "ISODrive+");
  sysfs_write((ours / "strings/0x409/product").string(), "ISODrive+ Mass Storage");
  sysfs_write((ours / "strings/0x409/serialnumber").string(), "ISODRIVEPLUS001");
  sysfs_write((ours / "configs/c.1/MaxPower").string(), "500");
  log_info("Created gadget " + ours.string());
  return ours.string();
}

std::string get_config_root() {
  std::string gadgetRoot = get_gadget_root();
  if (gadgetRoot.empty()) return "";

  fs::path usbConfigRoot = fs::path(gadgetRoot) / "configs";

  if (!fs::exists(usbConfigRoot) || !fs::is_directory(usbConfigRoot)) {
    log_debug("configs directory not found at " + usbConfigRoot.string());
    return "";
  }

  for (const auto& entry : fs::directory_iterator(usbConfigRoot)) {
       if (entry.path().filename().string()[0] != '.') {
           return entry.path().string();
       }
  }
  log_debug("No config found in " + usbConfigRoot.string());
  return "";
}

static bool configure_windows_descriptors(const std::string& gadgetRoot, const WindowsMountOptions& win_opts) {
  log_info("");
  log_info("=== Configuring Windows-compatible USB descriptors ===");
  
  fs::path root = gadgetRoot;
  bool success = true;

  // Set vendor/product IDs that Windows recognizes
  // Using IDs commonly associated with CD-ROM/mass storage devices
  success &= sysfs_write((root / "idVendor").string(), "0x058f");  // Alcor Micro Corp
  success &= sysfs_write((root / "idProduct").string(), "0x6387"); // Mass Storage
  
  // Set USB version based on options
  if (win_opts.use_usb3) {
    log_info("Using USB 3.0 descriptors");
    success &= sysfs_write((root / "bcdUSB").string(), "0x0300");
  } else {
    success &= sysfs_write((root / "bcdUSB").string(), "0x0200");
  }
  
  // Set device version
  success &= sysfs_write((root / "bcdDevice").string(), "0x0100");
  
  // Set device class to 0x00 (defined at interface level)
  success &= sysfs_write((root / "bDeviceClass").string(), "0x00");
  success &= sysfs_write((root / "bDeviceSubClass").string(), "0x00");
  success &= sysfs_write((root / "bDeviceProtocol").string(), "0x00");
  
  // Set max power (important for USB 3.0)
  fs::path maxPowerFile = root / "configs/c.1/MaxPower";
  if (fs::exists(maxPowerFile.parent_path())) {
    if (win_opts.use_usb3) {
      success &= sysfs_write(maxPowerFile.string(), "896");  // 896mA for USB 3.0
    } else {
      success &= sysfs_write(maxPowerFile.string(), "500");  // 500mA for USB 2.0
    }
  }
  
  // Configure device strings (important for Windows driver binding)
  fs::path stringsPath = root / "strings/0x409";
  
  // Create strings directory if it doesn't exist
  if (!fs::exists(stringsPath)) {
    std::error_code ec;
    fs::create_directories(stringsPath, ec);
    if (ec) {
      log_error("Failed to create strings directory: " + ec.message());
      return false;
    }
  }
  
  // Set product string based on Windows version
  std::string product_string = "USB Mass Storage";
  if (win_opts.version == WindowsVersion::WIN11) {
    product_string = "USB CD-ROM Drive";
  } else if (win_opts.version == WindowsVersion::WIN10) {
    product_string = "USB CD-ROM Drive";
  }
  
  success &= sysfs_write((stringsPath / "manufacturer").string(), "Generic");
  success &= sysfs_write((stringsPath / "product").string(), product_string);
  success &= sysfs_write((stringsPath / "serialnumber").string(), "000000000001");
  
  if (success) {
    log_info("Windows USB descriptors configured");
  } else {
    log_warn("Some Windows USB descriptor writes failed");
  }
  
  return success;
}

static bool configure_windows_mass_storage(const std::string& lunRoot, const WindowsMountOptions& win_opts) {
  log_info("Configuring Windows mass storage settings...");
  
  fs::path root = lunRoot;
  bool success = true;

  // Set removable flag (critical for Windows CD-ROM recognition)
  success &= sysfs_write((root / "removable").string(), "1");
  
  // Disable forced unit access for better stability
  fs::path nofuaFile = root / "nofua";
  if (fs::exists(nofuaFile)) {
    success &= sysfs_write(nofuaFile.string(), "1");
  }
  
  // Set inquiry string based on Windows version
  fs::path inquiryFile = root / "inquiry_string";
  if (fs::exists(inquiryFile)) {
    std::string inquiry;
    if (win_opts.version == WindowsVersion::WIN11) {
      inquiry = "Generic  USB CD-ROM       1.00";
    } else if (win_opts.version == WindowsVersion::WIN10) {
      inquiry = "Generic  USB CD-ROM       1.00";
    } else {
      inquiry = "Generic  USB CD-ROM       1.00";
    }
    success &= sysfs_write(inquiryFile.string(), inquiry);
  }
  
  if (success) {
    log_info("Windows mass storage settings configured");
  } else {
    log_warn("Some Windows mass storage settings failed");
  }
  
  return success;
}

static void print_windows_info(const WindowsMountOptions& win_opts) {
  log_info("");
  log_info("****************************************");
  log_info("***    WINDOWS ISO MODE ENABLED     ***");
  log_info("****************************************");
  log_info("");
  
  // Show detected version
  log_info("Detected: " + windows_version_to_string(win_opts.version));
  
  // Show boot mode info
  if (win_opts.has_uefi && win_opts.has_legacy) {
    log_info("Boot Mode: UEFI + Legacy BIOS (dual boot)");
  } else if (win_opts.has_uefi) {
    log_info("Boot Mode: UEFI only");
  } else if (win_opts.has_legacy) {
    log_info("Boot Mode: Legacy BIOS only");
  } else {
    log_info("Boot Mode: Unknown");
  }
  
  // Show USB mode
  if (win_opts.use_usb3) {
    log_info("USB Mode: USB 3.0 (SuperSpeed)");
  } else {
    log_info("USB Mode: USB 2.0 (High Speed)");
  }
  
  log_info("");
}

static void save_engine_state(const std::string& gadget, const std::string& udc,
                              const std::string& function, const std::string& file) {
  fs::create_directories(kStateDir);
  FILE* f = fopen(kStateFile, "w");
  if (!f) return;
  fprintf(f, "gadget=%s\n", gadget.c_str());
  fprintf(f, "udc=%s\n", udc.c_str());
  fprintf(f, "function=%s\n", function.c_str());
  fprintf(f, "file=%s\n", file.c_str());
  fclose(f);
}

bool mount_iso(const std::string& iso_path, bool cdrom, bool ro, const WindowsMountOptions& win_opts) {
  try_usb_role_device();

  std::string gadgetRoot = get_gadget_root();
  if (gadgetRoot.empty()) {
    log_error("No usb_gadget available (kernel missing configfs gadget?)");
    return false;
  }
  std::string configRoot = get_config_root();
  if (configRoot.empty()) {
    log_error("No gadget config (configs/c.1) found");
    return false;
  }

  std::string udc = sysfs_read((fs::path(gadgetRoot) / "UDC").string());
  if (udc.empty()) udc = first_udc_name();
  if (udc.empty()) {
    log_error("No UDC in /sys/class/udc — controller not in device role?");
    return false;
  }

  // Unique function name so we do not steal vendor mass_storage.0
  fs::path massStorageRoot = fs::path(gadgetRoot) / "functions" / kOurFunction;
  fs::path lunRoot = massStorageRoot / "lun.0";
  fs::path lunFile = lunRoot / "file";

  bool success = true;

  // configfs requires UDC unbound to add functions
  if (!set_udc("", gadgetRoot)) {
    log_warn("Failed to unbind UDC before configuration");
  }

  if (win_opts.enabled) {
    print_windows_info(win_opts);
    cdrom = true;
    ro = true;
    log_info("Forced CD-ROM + read-only for Windows ISO");
    if (win_opts.rewrite_ids) {
      log_warn("Rewriting gadget VID/PID (MTP/ADB will break until restore)");
      configure_windows_descriptors(gadgetRoot, win_opts);
    }
  }

  if (!fs::exists(massStorageRoot)) {
    std::error_code ec;
    fs::create_directories(massStorageRoot, ec);
    if (ec || !fs::exists(massStorageRoot)) {
      log_error("Cannot create " + massStorageRoot.string() +
                " — CONFIG_USB_CONFIGFS_MASS_STORAGE is likely =n");
      set_udc(udc, gadgetRoot);
      return false;
    }
  }

  success &= sysfs_write((massStorageRoot / "stall").string(), "0");
  success &= sysfs_write(lunFile.string(), "");

  if (!iso_path.empty()) {
    fs::path linkPath = fs::path(configRoot) / kOurFunction;
    if (!fs::exists(linkPath)) {
      std::error_code ec;
      fs::create_directory_symlink(massStorageRoot, linkPath, ec);
      if (ec) {
        log_error("Failed to link function into config: " + ec.message());
        set_udc(udc, gadgetRoot);
        return false;
      }
    }

    // flags BEFORE file
    success &= sysfs_write((lunRoot / "cdrom").string(), cdrom ? "1" : "0");
    success &= sysfs_write((lunRoot / "ro").string(), ro ? "1" : "0");
    if (fs::exists(lunRoot / "removable"))
      sysfs_write((lunRoot / "removable").string(), "1");
    if (fs::exists(lunRoot / "nofua"))
      sysfs_write((lunRoot / "nofua").string(), "1");

    if (win_opts.enabled) {
      configure_windows_mass_storage(lunRoot.string(), win_opts);
    }

    if (!sysfs_write(lunFile.string(), iso_path)) {
      log_error("Kernel rejected LUN file (SELinux or unreadable): " + iso_path);
      set_udc(udc, gadgetRoot);
      return false;
    }
    std::string got = sysfs_read(lunFile.string());
    if (got.empty()) {
      log_error("LUN file write ignored (enforcing AVC?). dmesg | grep avc");
      set_udc(udc, gadgetRoot);
      return false;
    }
    log_info("LUN backing: " + got);
    save_engine_state(gadgetRoot, udc, kOurFunction, iso_path);
  } else {
    fs::path linkPath = fs::path(configRoot) / kOurFunction;
    if (fs::exists(linkPath)) {
      std::error_code ec;
      fs::remove(linkPath, ec);
    }
  }

  // brief settle for dwc3
  usleep(80000);
  if (!set_udc(udc, gadgetRoot)) {
    log_error("Failed to bind UDC " + udc);
    return false;
  }

  return success;
}

bool restore_gadget() {
  log_info("Restoring gadget / unmounting ISODrive+ LUN");
  std::string gadgetRoot = get_gadget_root();
  if (gadgetRoot.empty()) {
    log_warn("No gadget to restore");
    return true;
  }
  std::string udc = sysfs_read((fs::path(gadgetRoot) / "UDC").string());
  if (udc.empty()) udc = first_udc_name();
  set_udc("", gadgetRoot);

  fs::path ms = fs::path(gadgetRoot) / "functions" / kOurFunction;
  if (fs::exists(ms / "lun.0" / "file"))
    sysfs_write((ms / "lun.0" / "file").string(), "");

  std::string configRoot = get_config_root();
  if (!configRoot.empty()) {
    fs::path link = fs::path(configRoot) / kOurFunction;
    if (fs::exists(link)) {
      std::error_code ec;
      fs::remove(link, ec);
    }
  }

  if (!udc.empty()) {
    usleep(80000);
    set_udc(udc, gadgetRoot);
  }
  unlink(kStateFile);
  return true;
}

void probe_configfs() {
  ensure_configfs_mounted();
  std::string parent = usb_gadget_parent();
  std::cout << "configfs_gadget=" << (parent.empty() ? "no" : parent) << "\n";
  std::cout << "udc=" << first_udc_name() << "\n";
  std::string g = get_gadget_root();
  std::cout << "gadget=" << (g.empty() ? "none" : g) << "\n";
  if (!g.empty()) {
    std::cout << "gadget_udc=" << sysfs_read((fs::path(g) / "UDC").string()) << "\n";
    fs::path probe = fs::path(g) / "functions" / "mass_storage.isodp_probe";
    std::error_code ec;
    fs::create_directories(probe, ec);
    bool ok = fs::exists(probe);
    if (ok) fs::remove(probe, ec);
    std::cout << "mass_storage_create=" << (ok ? "yes" : "no") << "\n";
  }
}

bool set_udc(const std::string& udc, const std::string& gadget) {
  fs::path udcFile = fs::path(gadget) / "UDC";
  return sysfs_write(udcFile.string(), udc);
}

std::string get_udc() {
  std::string gadget_root = get_gadget_root();
  if (gadget_root.empty()) return "";

  fs::path udcFile = fs::path(gadget_root) / "UDC";
  return sysfs_read(udcFile.string());
}
