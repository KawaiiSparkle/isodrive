#ifndef CONFIGFSISOMANAGER_H
#define CONFIGFSISOMANAGER_H

#include <string>
#include "util.h"

/**
 * @file configfsisomanager.h
 * @brief ConfigFS-based USB gadget mass storage manager.
 * 
 * Provides functions to mount ISO files as USB mass storage devices
 * using the Linux ConfigFS USB gadget interface. This is the modern,
 * preferred backend for USB gadget control.
 * 
 * ConfigFS is typically mounted at /sys/kernel/config on Linux
 * or /config on Android devices.
 */

/**
 * @struct WindowsMountOptions
 * @brief Options for Windows ISO mounting.
 */
struct WindowsMountOptions {
    bool enabled;               ///< Windows mode enabled
    WindowsVersion version;     ///< Detected/forced Windows version
    bool use_usb3;              ///< Use USB 3.0 descriptors
    bool has_uefi;              ///< ISO has UEFI boot support
    bool has_legacy;            ///< ISO has legacy BIOS boot
    bool rewrite_ids;           ///< Rewrite gadget VID/PID (breaks MTP; off by default)
};

/**
 * @brief Check if ConfigFS USB gadget is supported on this system.
 * 
 * Verifies that configfs is mounted and usb_gadget is available.
 * 
 * @return true if ConfigFS USB gadget is available, false otherwise.
 */
bool supported();

/**
 * @brief Find the root path of the active USB gadget.
 * 
 * Searches the usb_gadget directory for a gadget with an active UDC.
 * 
 * @return Path to the gadget root (e.g., "/sys/kernel/config/usb_gadget/g1"),
 *         or empty string if no active gadget is found.
 */
std::string get_gadget_root();

/**
 * @brief Find the root path of the gadget's configuration.
 * 
 * Locates the configs directory within the active gadget.
 * 
 * @return Path to the config root (e.g., ".../configs/c.1"),
 *         or empty string if not found.
 */
std::string get_config_root();

/**
 * @brief Mount an ISO file as a USB mass storage device.
 * 
 * Configures the USB gadget mass storage function to expose the
 * specified ISO file. Supports CD-ROM mode and Windows ISO compatibility.
 * 
 * If iso_path is empty, unmounts any currently mounted ISO.
 * 
 * @param iso_path Path to the ISO file to mount, or empty to unmount.
 * @param cdrom If true, mount as CD-ROM device.
 * @param ro If true, mount as read-only.
 * @param win_opts Windows-specific mount options.
 * @return true if the operation succeeded, false on error.
 */
bool mount_iso(const std::string& iso_path, bool cdrom, bool ro, const WindowsMountOptions& win_opts);

/**
 * @brief Set the USB Device Controller for a gadget.
 * 
 * Binds or unbinds the gadget to/from the UDC. Pass empty string
 * to unbind (disable) the gadget.
 * 
 * @param udc The UDC name (e.g., "musb-hdrc.0"), or empty to unbind.
 * @param gadget Path to the gadget root.
 * @return true if the UDC was set successfully, false on error.
 */
bool set_udc(const std::string& udc, const std::string& gadget);
std::string get_udc();

/** Unmount our LUN and restore the previous gadget/UDC/USB props. */
bool restore_gadget();

/** Print gadget / UDC / mass_storage capability (key=value). */
void probe_configfs();

#endif // ifndef CONFIGFSISOMANAGER_H
