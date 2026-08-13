#include "androidusbisomanager.h"
#include "logger.h"
#include "util.h"
#include <string>

bool usb_supported() { 
  return isfile(ANDROID0_SYSFS_ENABLE); 
}

bool usb_mount_iso(const std::string& iso_path) {
  log_debug("Mounting ISO via Android sysfs: " + iso_path);

  if (usb_enabled()) {
    if (!usb_set_enabled(false)) {
      log_error("Failed to disable USB before mounting");
      return false;
    }
  }

  if (!sysfs_write(ANDROID0_SYSFS_IMG_FILE, iso_path)) {
    log_error("Failed to set ISO file path");
    usb_set_enabled(true);
    return false;
  }

  if (!sysfs_write(ANDROID0_SYSFS_FEATURES, "mass_storage")) {
    log_error("Failed to set USB function to mass_storage");
    usb_set_enabled(true);
    return false;
  }

  if (!usb_set_enabled(true)) {
    log_error("Failed to re-enable USB after mounting");
    return false;
  }

  log_info("ISO mounted via Android sysfs");
  return true;
}

bool usb_reset_iso() {
  log_debug("Resetting Android USB to default state");

  // Clear the image file first
  if (!sysfs_write(ANDROID0_SYSFS_IMG_FILE, "")) {
    log_warn("Failed to clear ISO file path");
  }

  if (usb_enabled()) {
    if (!usb_set_enabled(false)) {
      log_error("Failed to disable USB during reset");
      return false;
    }
  }

  if (!sysfs_write(ANDROID0_SYSFS_FEATURES, "mtp")) {
    log_error("Failed to restore USB function to mtp");
    usb_set_enabled(true);
    return false;
  }

  if (!usb_set_enabled(true)) {
    log_error("Failed to re-enable USB after reset");
    return false;
  }

  log_info("USB reset to MTP mode");
  return true;
}

bool usb_enabled() {
  return sysfs_read(ANDROID0_SYSFS_ENABLE) == "1";
}

bool usb_set_enabled(bool enabled) {
  return sysfs_write(ANDROID0_SYSFS_ENABLE, enabled ? "1" : "0");
}
