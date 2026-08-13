#ifndef LOGGER_H
#define LOGGER_H

#include <string>

/**
 * @file logger.h
 * @brief Logging system with verbosity levels for isodrive.
 * 
 * Provides a simple logging interface with multiple verbosity levels,
 * allowing users to control output via command-line flags.
 */

/**
 * @enum LogLevel
 * @brief Verbosity levels for the logging system.
 * 
 * Levels are ordered from least verbose (SILENT) to most verbose (DEBUG).
 * Setting a log level enables all messages at that level and above.
 */
enum class LogLevel {
    SILENT = 0,  ///< No output at all
    ERROR = 1,   ///< Only error messages (failures that prevent operation)
    WARN = 2,    ///< Warnings and errors (non-fatal issues)
    INFO = 3,    ///< Informational messages, warnings, and errors (default)
    DEBUG = 4    ///< All messages including debug details
};

/**
 * @brief Set the global logging verbosity level.
 * @param level The desired log level.
 */
void log_set_level(LogLevel level);

/**
 * @brief Get the current global logging verbosity level.
 * @return The current log level.
 */
LogLevel log_get_level();

/**
 * @brief Log an error message.
 * 
 * Errors indicate failures that prevent the requested operation.
 * Output goes to stderr. Shown at LogLevel::ERROR and above.
 * 
 * @param message The error message to log.
 */
void log_error(const std::string& message);

/**
 * @brief Log a warning message.
 * 
 * Warnings indicate potential issues that don't prevent operation.
 * Output goes to stderr. Shown at LogLevel::WARN and above.
 * 
 * @param message The warning message to log.
 */
void log_warn(const std::string& message);

/**
 * @brief Log an informational message.
 * 
 * Info messages provide progress and status updates.
 * Output goes to stdout. Shown at LogLevel::INFO and above.
 * 
 * @param message The informational message to log.
 */
void log_info(const std::string& message);

/**
 * @brief Log a debug message.
 * 
 * Debug messages provide detailed internal state information.
 * Output goes to stdout. Shown only at LogLevel::DEBUG.
 * 
 * @param message The debug message to log.
 */
void log_debug(const std::string& message);

#endif // ifndef LOGGER_H
