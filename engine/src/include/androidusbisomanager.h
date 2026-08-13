#ifndef ANDROIDUSBISOMANAGER_H
#define ANDROIDUSBISOMANAGER_H

#include <string>

/**
 * @file androidusbisomanager.h
 * @brief Legacy Android sysfs-based USB mass storage manager.
 * 
 * Provides functions to mount ISO files using the legacy Android
 * USB gadget sysfs interface at /sys/devices/virtual/android_usb/.
 * 
 * This is a fallback for older Android devices that don't support
 * ConfigFS. Note that this backend has limited functionality compared
 * to the ConfigFS backend (no CD-ROM mode, no Windows compatibility).
 */

/**
 * @brief Path to enable/disable the Android USB gadget.
 */
#define ANDROID0_SYSFS_ENABLE "/sys/devices/virtual/android_usb/android0/enable"

/**
 * @brief Path to set the ISO file for mass storage.
 */
#define ANDROID0_SYSFS_IMG_FILE                                                \
  "/sys/devices/virtual/android_usb/android0/f_mass_storage/lun/file"

/**
 * @brief Path to set the USB gadget functions (e.g., "mtp", "mass_storage").
 */
#define ANDROID0_SYSFS_FEATURES                                                \
  "/sys/devices/virtual/android_usb/android0/functions"

/**
 * @brief Check if the legacy Android USB sysfs interface is available.
 * 
 * @return true if /sys/devices/virtual/android_usb/android0/enable exists.
 */
bool usb_supported();

/**
 * @brief Mount an ISO file using the legacy Android USB interface.
 * 
 * Disables USB, sets the image file and functions, then re-enables USB.
 * 
 * @param iso_path Path to the ISO file to mount.
 * @return true if the operation succeeded, false on error.
 */
bool usb_mount_iso(const std::string& iso_path);

/**
 * @brief Unmount any ISO and restore default USB function (MTP).
 * 
 * @return true if the operation succeeded, false on error.
 */
bool usb_reset_iso();

/**
 * @brief Check if the Android USB gadget is currently enabled.
 * 
 * @return true if USB is enabled, false otherwise.
 */
bool usb_enabled();

/**
 * @brief Enable or disable the Android USB gadget.
 * 
 * @param enabled true to enable, false to disable.
 * @return true if the operation succeeded, false on error.
 */
bool usb_set_enabled(bool enabled);

#endif // ifndef ANDROIDUSBISOMANAGER_H