#pragma once

// Diagnostic logging for the pakon stack.
//
// Two jobs:
//  1. leveled messages (error..trace) so normal operation stays quiet;
//  2. hex dumps of raw packet traffic, e.g.
//
//       TX endpoint=0x01 length=5
//       04 03 10 00 85
//
// Packet logging is opt-in via LogLevel::trace / set_level(), so ordinary
// runs do not produce enormous logs.

#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pakon::log {

enum class Level : std::uint8_t {
    off = 0,
    error,
    warn,
    info,
    debug,
    trace,
};

std::string_view to_string(Level level);

// Parse a level name ("off", "error", ..., "trace"). Returns nullopt on
// an unknown name.
std::optional<Level> parse_level(std::string_view name);

class Logger {
public:
    static Logger& instance();

    void set_level(Level level) noexcept { level_ = level; }
    Level level() const noexcept { return level_; }

    // Optional prefix for all messages, e.g. a session tag.
    void set_prefix(std::string prefix);

    template <typename... Args>
    void log(Level lvl, std::format_string<Args...> fmt, Args&&... args) {
        if (lvl > level_) {
            return;
        }
        write(lvl, std::format(fmt, std::forward<Args>(args)...));
    }

    // Hex dump of raw bytes, prefixed "TX"/"RX"/custom, with endpoint and
    // length, per the project's packet-logging format.
    void hex(Level lvl, std::string_view direction, std::uint8_t endpoint,
             std::span<const std::uint8_t> bytes);

    // Hex dump without direction/endpoint (e.g. firmware or EEPROM bytes).
    void hex(Level lvl, std::string_view label, std::span<const std::uint8_t> bytes);

private:
    Logger() = default;

    void write(Level lvl, std::string_view text);
    void write_unlocked(Level lvl, std::string_view text);

    Level level_{Level::info};
    std::string prefix_;
    std::mutex mutex_;
};

// Convenience helpers targeting the singleton.
template <typename... Args>
inline void error(std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log(Level::error, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void warn(std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log(Level::warn, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void info(std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log(Level::info, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void debug(std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log(Level::debug, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void trace(std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log(Level::trace, fmt, std::forward<Args>(args)...);
}

} // namespace pakon::log
