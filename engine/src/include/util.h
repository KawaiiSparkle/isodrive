#ifndef UTIL_H
#define UTIL_H

#include <string>

/**
 * @file util.h
 * @brief Utility functions for filesystem operations and sysfs/configfs I/O.
 * 
 * Provides helper functions for interacting with the Linux filesystem,
 * including mount point discovery, path type checks, and kernel sysfs operations.
 */

/**
 * @enum WindowsVersion
 * @brief Detected Windows version from an ISO file.
 */
enum class WindowsVersion {
    NONE = 0,       ///< Not a Windows ISO
    WIN_UNKNOWN,    ///< Windows ISO but version unknown
    WIN10,          ///< Windows 10
    WIN11           ///< Windows 11
};

/**
 * @struct WindowsIsoInfo
 * @brief Information about a detected Windows ISO.
 */
struct WindowsIsoInfo {
    bool is_windows;            ///< True if this is a Windows ISO
    WindowsVersion version;     ///< Detected Windows version
    bool has_uefi;              ///< True if ISO has UEFI boot files
    bool has_legacy;            ///< True if ISO has legacy BIOS boot support
    std::string volume_label;   ///< Volume label from ISO
};

/**
 * @brief Find the mount point of a given filesystem type.
 * 
 * Searches /proc/mounts for the specified filesystem type and returns
 * its mount point. On Android, also checks /config for configfs if
 * the standard search fails.
 * 
 * @param filesystem_type The filesystem type to search for (e.g., "configfs").
 * @return The mount point path, or empty string if not found.
 * 
 * @example
 * std::string cfg_root = fs_mount_point("configfs");
 * // Returns "/sys/kernel/config" on most Linux systems
 */
std::string fs_mount_point(const std::string& filesystem_type);

/**
 * @brief Check if a path is a directory.
 * 
 * @param path The path to check.
 * @return true if path exists and is a directory, false otherwise.
 */
bool isdir(const std::string& path);

/**
 * @brief Check if a path is a regular file.
 * 
 * @param path The path to check.
 * @return true if path exists and is a regular file, false otherwise.
 */
bool isfile(const std::string& path);

/**
 * @brief Detect if an ISO file is a hybrid (bootable) ISO.
 * 
 * Checks for the MBR boot signature (0x55AA) at offset 510-511.
 * Hybrid ISOs can be booted from USB as a hard drive.
 * Non-hybrid ISOs (like Windows installers) should be mounted as CD-ROM.
 * 
 * @param path Path to the ISO file.
 * @return true if the ISO has a valid MBR boot signature (hybrid),
 *         false if not present or file cannot be read.
 */
bool is_hybrid_iso(const std::string& path);

/**
 * @brief Detect if an ISO file is a Windows installation media.
 * 
 * Checks for Windows-specific markers in the ISO structure:
 * - Volume label containing "WINDOWS" or "WIN"
 * - Presence of /sources/install.wim or /sources/install.esd
 * - Windows boot files structure
 * 
 * @param path Path to the ISO file.
 * @return true if this appears to be a Windows ISO, false otherwise.
 */
bool is_windows_iso(const std::string& path);

/**
 * @brief Get detailed information about a Windows ISO.
 * 
 * Analyzes the ISO to determine Windows version, UEFI support,
 * and other characteristics useful for USB mounting.
 * 
 * @param path Path to the ISO file.
 * @return WindowsIsoInfo struct with detection results.
 */
WindowsIsoInfo get_windows_iso_info(const std::string& path);

/**
 * @brief Get the Windows version from an ISO file.
 * 
 * @param path Path to the ISO file.
 * @return WindowsVersion enum value.
 */
WindowsVersion get_windows_version(const std::string& path);

/**
 * @brief Convert WindowsVersion enum to human-readable string.
 * 
 * @param version The WindowsVersion value.
 * @return String representation (e.g., "Windows 11").
 */
std::string windows_version_to_string(WindowsVersion version);

/**
 * @brief Read a value from a sysfs/configfs file.
 * 
 * Reads a single whitespace-delimited token from the specified file.
 * Commonly used for reading kernel interface values like UDC names.
 * 
 * @param path Absolute path to the sysfs/configfs file.
 * @return The first token read, or empty string if file cannot be opened.
 */
std::string sysfs_read(const std::string& path);

/**
 * @brief Write a value to a sysfs/configfs file.
 * 
 * Writes the specified content to a kernel interface file.
 * Used for configuring USB gadget parameters.
 * 
 * @param path Absolute path to the sysfs/configfs file.
 * @param content The value to write.
 * @return true if the write succeeded, false if the file could not be opened.
 */
bool sysfs_write(const std::string& path, const std::string& content);

/** Write without appending extra characters; then read back lun/file style nodes. */
bool sysfs_write_raw(const std::string& path, const std::string& content);

std::string first_udc_name();
bool ensure_configfs_mounted();
std::string resolve_backing_file(const std::string& path);
bool looks_like_block_device(const std::string& path);
bool try_usb_role_device();
int run_cmd(const std::string& cmd);

static const char kStateDir[] = "/data/adb/isodriveplus";
static const char kStateFile[] = "/data/adb/isodriveplus/engine.state";
static const char kStageDir[] = "/data/adb/isodriveplus/stage";

#endif // ifndef UTIL_H