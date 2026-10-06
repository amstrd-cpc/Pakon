#include "pakon/logging/logger.hpp"

#include <algorithm>
#include <cstdio>

namespace pakon::log {

std::string_view to_string(Level level) {
    switch (level) {
    case Level::off: return "off";
    case Level::error: return "error";
    case Level::warn: return "warn";
    case Level::info: return "info";
    case Level::debug: return "debug";
    case Level::trace: return "trace";
    }
    return "off";
}

std::optional<Level> parse_level(std::string_view name) {
    if (name == "off") return Level::off;
    if (name == "error") return Level::error;
    if (name == "warn") return Level::warn;
    if (name == "info") return Level::info;
    if (name == "debug") return Level::debug;
    if (name == "trace") return Level::trace;
    return std::nullopt;
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_prefix(std::string prefix) {
    std::scoped_lock lock(mutex_);
    prefix_ = std::move(prefix);
}

void Logger::write(Level lvl, std::string_view text) {
    std::scoped_lock lock(mutex_);
    write_unlocked(lvl, text);
}

void Logger::write_unlocked(Level lvl, std::string_view text) {
    if (lvl > level_) {
        return;
    }
    std::fprintf(stderr, "[%s] %.*s\n", to_string(lvl).data(),
                 static_cast<int>(text.size()), text.data());
}

void Logger::hex(Level lvl, std::string_view direction, std::uint8_t endpoint,
                 std::span<const std::uint8_t> bytes) {
    if (lvl > level_) {
        return;
    }
    std::scoped_lock lock(mutex_);
    write_unlocked(lvl, std::format("{} endpoint=0x{:02x} length={}",
                                    direction, endpoint, bytes.size()));
    for (size_t offset = 0; offset < bytes.size(); offset += 16) {
        const auto row = bytes.subspan(offset, std::min<size_t>(16, bytes.size() - offset));
        std::string line;
        for (std::uint8_t b : row) {
            line += std::format("{:02x} ", b);
        }
        write_unlocked(lvl, line);
    }
}

void Logger::hex(Level lvl, std::string_view label,
                 std::span<const std::uint8_t> bytes) {
    if (lvl > level_) {
        return;
    }
    std::scoped_lock lock(mutex_);
    write_unlocked(lvl, std::format("{} length={}", label, bytes.size()));
    for (size_t offset = 0; offset < bytes.size(); offset += 16) {
        const auto row = bytes.subspan(offset, std::min<size_t>(16, bytes.size() - offset));
        std::string line;
        for (std::uint8_t b : row) {
            line += std::format("{:02x} ", b);
        }
        write_unlocked(lvl, line);
    }
}

} // namespace pakon::log
