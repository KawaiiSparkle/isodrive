#include "util.h"
#include "logger.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mntent.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ISO 9660 constants
constexpr int ISO_SECTOR_SIZE = 2048;
constexpr int ISO_PVD_SECTOR = 16;  // Primary Volume Descriptor is at sector 16
constexpr int ISO_PVD_OFFSET = ISO_PVD_SECTOR * ISO_SECTOR_SIZE;  // 32768

std::string fs_mount_point(const std::string& filesystem_type) {
  struct mntent *ent;
  FILE *mounts;
  std::string mount_point;

  mounts = setmntent("/proc/mounts", "r");
  if (!mounts) {
    log_debug("Failed to open /proc/mounts");
    return "";
  }

  while (nullptr != (ent = getmntent(mounts))) {
    if (filesystem_type == ent->mnt_fsname) {
      mount_point = ent->mnt_dir;
      break;
    }
  }
  endmntent(mounts);

  // Alternate search location on Android
  if (mount_point.empty() && filesystem_type == "configfs") {
    if (fs::exists("/config/usb_gadget")) {
      mount_point = "/config";
      log_debug("Found configfs at /config (Android fallback)");
    }
  }

  if (!mount_point.empty()) {
    log_debug("Found " + filesystem_type + " at " + mount_point);
  }

  return mount_point;
}

bool isdir(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_directory(path, ec);
}

bool isfile(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

bool is_hybrid_iso(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    log_debug("Cannot open file for hybrid ISO check: " + path);
    return false;
  }

  file.seekg(510);
  if (file.fail()) {
    log_debug("Failed to seek to offset 510 in: " + path);
    return false;
  }

  unsigned char buffer[2];
  file.read(reinterpret_cast<char*>(buffer), 2);

  if (file.gcount() != 2) {
    log_debug("Failed to read 2 bytes at offset 510 from: " + path);
    return false;
  }

  bool is_hybrid = (buffer[0] == 0x55 && buffer[1] == 0xAA);
  log_debug("ISO " + path + " hybrid check: " + (is_hybrid ? "true" : "false"));
  return is_hybrid;
}

// Helper: Read ISO 9660 Primary Volume Descriptor and extract volume label
static std::string read_iso_volume_label(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return "";
  }

  // Seek to Primary Volume Descriptor (sector 16)
  file.seekg(ISO_PVD_OFFSET);
  if (file.fail()) {
    return "";
  }

  // Read the sector
  char buffer[ISO_SECTOR_SIZE];
  file.read(buffer, ISO_SECTOR_SIZE);
  if (file.gcount() != ISO_SECTOR_SIZE) {
    return "";
  }

  // Verify this is a Primary Volume Descriptor
  // Byte 0: Type (1 = PVD)
  // Bytes 1-5: "CD001"
  if (buffer[0] != 1 || std::strncmp(buffer + 1, "CD001", 5) != 0) {
    log_debug("Not a valid ISO 9660 Primary Volume Descriptor");
    return "";
  }

  // Volume Identifier is at offset 40, 32 bytes, space-padded
  std::string volume_id(buffer + 40, 32);
  
  // Trim trailing spaces
  size_t end = volume_id.find_last_not_of(' ');
  if (end != std::string::npos) {
    volume_id = volume_id.substr(0, end + 1);
  } else {
    volume_id.clear();
  }

  log_debug("ISO volume label: " + volume_id);
  return volume_id;
}

// Helper: Check if a specific path exists within the ISO (basic check via volume label patterns)
static bool iso_contains_windows_markers(const std::string& volume_label) {
  std::string upper_label = volume_label;
  std::transform(upper_label.begin(), upper_label.end(), upper_label.begin(), ::toupper);

  // Common Windows ISO volume labels
  if (upper_label.find("WIN") != std::string::npos) return true;
  if (upper_label.find("WINDOWS") != std::string::npos) return true;
  if (upper_label.find("CCCOMA") != std::string::npos) return true;  // Windows Media Creation Tool
  if (upper_label.find("ESD-ISO") != std::string::npos) return true;  // Windows ESD
  if (upper_label.find("J_CCSA") != std::string::npos) return true;   // Some Windows ISOs
  if (upper_label.find("CPBA") != std::string::npos) return true;     // Some Windows ISOs
  
  return false;
}

// Helper: Detect Windows version from volume label
static WindowsVersion detect_version_from_label(const std::string& volume_label) {
  std::string upper_label = volume_label;
  std::transform(upper_label.begin(), upper_label.end(), upper_label.begin(), ::toupper);

  // Windows 11 patterns
  if (upper_label.find("WIN11") != std::string::npos) return WindowsVersion::WIN11;
  if (upper_label.find("WINDOWS 11") != std::string::npos) return WindowsVersion::WIN11;
  if (upper_label.find("W11") != std::string::npos) return WindowsVersion::WIN11;
  
  // Windows 10 patterns
  if (upper_label.find("WIN10") != std::string::npos) return WindowsVersion::WIN10;
  if (upper_label.find("WINDOWS 10") != std::string::npos) return WindowsVersion::WIN10;
  if (upper_label.find("W10") != std::string::npos) return WindowsVersion::WIN10;

  // Recent Windows ISO naming conventions
  // CCCOMA_X64FRE - typically Windows 10/11
  // Check for presence of newer patterns
  if (upper_label.find("CCCOMA") != std::string::npos) {
    // This is a Media Creation Tool ISO, but we can't determine version without more info
    return WindowsVersion::WIN_UNKNOWN;
  }

  return WindowsVersion::WIN_UNKNOWN;
}

// Helper: Search ISO for specific file signatures
static bool search_iso_for_bootloader(std::ifstream& file, bool& has_uefi, bool& has_legacy) {
  has_uefi = false;
  has_legacy = false;

  // Read El Torito Boot Record at sector 17
  file.seekg(17 * ISO_SECTOR_SIZE);
  if (file.fail()) {
    return false;
  }

  char buffer[ISO_SECTOR_SIZE];
  file.read(buffer, ISO_SECTOR_SIZE);
  if (file.gcount() != ISO_SECTOR_SIZE) {
    return false;
  }

  // Check for El Torito signature
  // Byte 0: Type (0 = Boot Record)
  // Bytes 1-5: "CD001"
  // Bytes 7-38: "EL TORITO SPECIFICATION"
  if (buffer[0] == 0 && std::strncmp(buffer + 1, "CD001", 5) == 0) {
    if (std::strncmp(buffer + 7, "EL TORITO SPECIFICATION", 23) == 0) {
      log_debug("Found El Torito boot record");
      has_legacy = true;
    }
  }

  // For UEFI detection, we look for the EFI system partition marker
  // by scanning the volume descriptors for EFI boot catalog entries
  // In practice, we check for common patterns in the first few sectors

  // Simple heuristic: If it's a Windows ISO and has El Torito, 
  // modern Windows ISOs almost always have UEFI support
  // A more thorough check would require parsing the directory structure

  // Check sectors for EFI signatures
  for (int sector = 16; sector < 20; sector++) {
    file.seekg(sector * ISO_SECTOR_SIZE);
    if (file.fail()) break;
    
    file.read(buffer, ISO_SECTOR_SIZE);
    if (file.gcount() != ISO_SECTOR_SIZE) break;

    // Look for "EFI" string in the sector (boot catalog reference)
    std::string sector_str(buffer, ISO_SECTOR_SIZE);
    if (sector_str.find("EFI BOOT") != std::string::npos ||
        sector_str.find("efi") != std::string::npos ||
        sector_str.find("BOOTX64") != std::string::npos) {
      has_uefi = true;
      log_debug("Found UEFI boot markers in ISO");
      break;
    }
  }

  // If we found El Torito and it's a recent ISO, assume UEFI is supported
  // (This is a reasonable heuristic for modern Windows ISOs)
  if (has_legacy && !has_uefi) {
    // Most modern Windows ISOs are dual-boot (UEFI + Legacy)
    // Mark UEFI as likely available
    log_debug("Assuming UEFI support for modern Windows ISO");
    has_uefi = true;
  }

  return true;
}

static bool scan_iso_for_windows_files(const std::string& path, bool& saw_wim, bool& saw_bootmgr, bool& saw_efi) {
  saw_wim = saw_bootmgr = saw_efi = false;
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  // Walk the first ~8 MiB of ISO9660 directory records as raw haystack.
  const size_t CHUNK = 65536;
  const size_t LIMIT = 8 * 1024 * 1024;
  std::string buf(CHUNK, '\0');
  size_t off = 0;
  while (off < LIMIT && file) {
    file.read(&buf[0], CHUNK);
    std::streamsize n = file.gcount();
    if (n <= 0) break;
    std::string_view sv(buf.data(), static_cast<size_t>(n));
    auto has = [&](const char* s) {
      return sv.find(s) != std::string_view::npos;
    };
    if (has("INSTALL.WIM") || has("install.wim") || has("INSTALL.ESD") || has("install.esd"))
      saw_wim = true;
    if (has("BOOTMGR") || has("bootmgr"))
      saw_bootmgr = true;
    if (has("BOOTX64.EFI") || has("bootx64.efi") || has("EFI\\BOOT") || has("efi/boot"))
      saw_efi = true;
    if (saw_wim && saw_bootmgr) return true;
    off += static_cast<size_t>(n);
  }
  return saw_wim || saw_bootmgr;
}

bool is_windows_iso(const std::string& path) {
  return get_windows_iso_info(path).is_windows;
}

WindowsIsoInfo get_windows_iso_info(const std::string& path) {
  WindowsIsoInfo info = {};
  info.is_windows = false;
  info.version = WindowsVersion::NONE;
  info.has_uefi = false;
  info.has_legacy = false;

  info.volume_label = read_iso_volume_label(path);

  bool saw_wim = false, saw_bootmgr = false, saw_efi = false;
  scan_iso_for_windows_files(path, saw_wim, saw_bootmgr, saw_efi);

  info.is_windows = iso_contains_windows_markers(info.volume_label) || saw_wim ||
                    (saw_bootmgr && saw_efi);
  if (!info.is_windows) {
    return info;
  }
  if (saw_efi) info.has_uefi = true;

  // Detect version
  info.version = detect_version_from_label(info.volume_label);

  // Check for boot support
  std::ifstream file(path, std::ios::binary);
  if (file) {
    bool uefi = false, legacy = false;
    search_iso_for_bootloader(file, uefi, legacy);
    info.has_uefi = info.has_uefi || uefi || saw_efi;
    info.has_legacy = info.has_legacy || legacy;
  }

  log_debug("Windows ISO detected: " + info.volume_label + 
            ", version: " + windows_version_to_string(info.version) +
            ", UEFI: " + (info.has_uefi ? "yes" : "no") +
            ", Legacy: " + (info.has_legacy ? "yes" : "no"));

  return info;
}

WindowsVersion get_windows_version(const std::string& path) {
  WindowsIsoInfo info = get_windows_iso_info(path);
  return info.version;
}

std::string windows_version_to_string(WindowsVersion version) {
  switch (version) {
    case WindowsVersion::WIN10:
      return "Windows 10";
    case WindowsVersion::WIN11:
      return "Windows 11";
    case WindowsVersion::WIN_UNKNOWN:
      return "Windows (unknown version)";
    case WindowsVersion::NONE:
    default:
      return "Not Windows";
  }
}

bool sysfs_write_raw(const std::string& path, const std::string& content) {
  log_debug("Write: [" + content + "] -> " + path);
  FILE* f = fopen(path.c_str(), "w");
  if (!f) {
    log_error("Failed to open " + path + " for writing.");
    return false;
  }
  // One write, no extra std::endl. Kernel accepts with or without \n;
  // we send the value plus a single newline (configfs convention).
  std::string payload = content;
  if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
  size_t n = fwrite(payload.data(), 1, payload.size(), f);
  int err = ferror(f);
  fclose(f);
  if (n != payload.size() || err) {
    log_error("Short/failed write to " + path);
    return false;
  }
  return true;
}

bool sysfs_write(const std::string& path, const std::string& content) {
  return sysfs_write_raw(path, content);
}

std::string sysfs_read(const std::string& path) {
  std::ifstream sysfsFile(path);
  if (!sysfsFile.is_open()) {
    log_debug("Cannot open for reading: " + path);
    return "";
  }
  std::string value;
  std::getline(sysfsFile, value);
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' '))
    value.pop_back();
  log_debug("Read: " + value + " <- " + path);
  return value;
}

std::string first_udc_name() {
  std::error_code ec;
  fs::path udc_dir("/sys/class/udc");
  if (!fs::exists(udc_dir, ec)) return "";
  for (const auto& e : fs::directory_iterator(udc_dir, ec)) {
    std::string n = e.path().filename().string();
    if (!n.empty() && n[0] != '.') return n;
  }
  return "";
}

bool ensure_configfs_mounted() {
  if (fs::exists("/config/usb_gadget") || fs::exists("/sys/kernel/config/usb_gadget"))
    return true;
  fs::create_directories("/sys/kernel/config");
  if (run_cmd("mount -t configfs configfs /sys/kernel/config") == 0 &&
      fs::exists("/sys/kernel/config/usb_gadget"))
    return true;
  fs::create_directories("/config");
  if (run_cmd("mount -t configfs configfs /config") == 0 &&
      fs::exists("/config/usb_gadget"))
    return true;
  return fs::exists("/config/usb_gadget") || fs::exists("/sys/kernel/config/usb_gadget");
}

int run_cmd(const std::string& cmd) {
  log_debug("exec: " + cmd);
  return system(cmd.c_str());
}

bool looks_like_block_device(const std::string& path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return false;
  return S_ISBLK(st.st_mode);
}

static bool chcon_best_effort(const std::string& path, const char* ctx) {
  return run_cmd(std::string("chcon ") + ctx + " \"" + path + "\"") == 0;
}

std::string resolve_backing_file(const std::string& path) {
  if (path.empty() || !isfile(path)) return "";
  if (looks_like_block_device(path)) {
    log_error("Refusing to use a block device as LUN backing: " + path);
    return "";
  }

  char real[PATH_MAX];
  std::string abs = path;
  if (realpath(path.c_str(), real)) abs = real;

  auto map_sdcard = [](const std::string& p) -> std::string {
    const char* prefs[] = {
      "/sdcard/", "/storage/emulated/0/", "/storage/self/primary/", "/mnt/sdcard/", nullptr
    };
    const char* rest_from[] = {
      "/sdcard/", "/storage/emulated/0/", "/storage/self/primary/", "/mnt/sdcard/", nullptr
    };
    for (int i = 0; prefs[i]; i++) {
      if (p.rfind(prefs[i], 0) == 0) {
        return std::string("/data/media/0/") + p.substr(strlen(rest_from[i]));
      }
    }
    return "";
  };

  if (abs.rfind("/data/adb/", 0) == 0 || abs.rfind("/data/local/", 0) == 0 ||
      abs.rfind("/data/media/", 0) == 0) {
    chmod(abs.c_str(), 0644);
    chcon_best_effort(abs, "u:object_r:system_data_file:s0");
    return abs;
  }

  std::string mapped = map_sdcard(abs);
  if (!mapped.empty() && isfile(mapped)) {
    chmod(mapped.c_str(), 0644);
    chcon_best_effort(mapped, "u:object_r:media_rw_data_file:s0");
    log_info("Mapped FUSE path -> " + mapped);
    return mapped;
  }

  fs::create_directories(kStageDir);
  std::string dst = std::string(kStageDir) + "/" + fs::path(abs).filename().string();
  unlink(dst.c_str());
  if (link(abs.c_str(), dst.c_str()) == 0) {
    chcon_best_effort(dst, "u:object_r:system_data_file:s0");
    log_info("Hardlinked backing file -> " + dst);
    return dst;
  }
  // bind-mount
  FILE* t = fopen(dst.c_str(), "a");
  if (t) fclose(t);
  if (run_cmd("mount -o bind \"" + abs + "\" \"" + dst + "\"") == 0) {
    chcon_best_effort(dst, "u:object_r:system_data_file:s0");
    log_info("Bind-mounted backing file -> " + dst);
    return dst;
  }
  log_warn("Copying image into stage (slow)...");
  if (run_cmd("cp -f \"" + abs + "\" \"" + dst + "\"") != 0) {
    log_error("Cannot stage file for kernel: " + abs);
    return "";
  }
  chmod(dst.c_str(), 0644);
  chcon_best_effort(dst, "u:object_r:system_data_file:s0");
  return dst;
}

bool try_usb_role_device() {
  // Best-effort: flip dwc3/role switches so the controller is a device (gadget).
  const char* candidates[] = {
    "/sys/class/udc",
    nullptr
  };
  (void)candidates;
  std::error_code ec;
  if (fs::exists("/sys/class/udc", ec)) {
    for (const auto& e : fs::directory_iterator("/sys/class/udc", ec)) {
      fs::path mode = e.path() / "device" / "mode";
      if (fs::exists(mode)) {
        log_info("Setting USB role device: " + mode.string());
        sysfs_write(mode.string(), "device");
        sysfs_write(mode.string(), "peripheral");
      }
    }
  }
  // Common phy/role-switch nodes
  const char* extra[] = {
    "/sys/class/typec/port0/data_role",
    "/sys/devices/platform/soc/a600000.ssusb/mode",
    nullptr
  };
  for (int i = 0; extra[i]; i++) {
    if (fs::exists(extra[i])) {
      sysfs_write(extra[i], "device");
    }
  }
  return true;
}