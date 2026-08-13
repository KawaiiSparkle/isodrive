#include "logger.h"
#include <iostream>

namespace {
    // Global log level, default to INFO
    LogLevel g_log_level = LogLevel::INFO;
}

void log_set_level(LogLevel level) {
    g_log_level = level;
}

LogLevel log_get_level() {
    return g_log_level;
}

void log_error(const std::string& message) {
    if (g_log_level >= LogLevel::ERROR) {
        std::cerr << "[ERROR] " << message << std::endl;
    }
}

void log_warn(const std::string& message) {
    if (g_log_level >= LogLevel::WARN) {
        std::cerr << "[WARN] " << message << std::endl;
    }
}

void log_info(const std::string& message) {
    if (g_log_level >= LogLevel::INFO) {
        std::cout << message << std::endl;
    }
}

void log_debug(const std::string& message) {
    if (g_log_level >= LogLevel::DEBUG) {
        std::cout << "[DEBUG] " << message << std::endl;
    }
}
