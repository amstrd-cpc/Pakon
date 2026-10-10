#include "pakon/eeprom/eeprom.hpp"

#include <algorithm>
#include <format>

#include "pakon/logging/logger.hpp"

namespace pakon::eeprom {

namespace {

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

// Validate one copy: header length in (8, max] and CRC over the payload
// (TLB@0x100163c0). Returns the warning bit that fails it, 0 if good.
unsigned check_copy(std::span<const std::uint8_t> image, std::uint16_t base,
                    std::uint16_t max_length) {
    if (image.size() < static_cast<std::size_t>(base) + 8) {
        return kWarnBlank;
    }
    const std::uint32_t length = le32(image, base);
    if (length <= 8 || length > max_length || image.size() < base + length) {
        return kWarnBlank;
    }
    const std::uint32_t stored = le32(image, base + 4);
    const auto payload = image.subspan(base + 8, length - 8);
    return crc32(payload) == stored ? 0u : kWarnChecksumBad;
}

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto b : bytes) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

std::string_view to_string(CopySource source) {
    switch (source) {
    case CopySource::primary: return "primary";
    case CopySource::backup: return "backup";
    case CopySource::fallback: return "FALLBACK (capture unit 16402)";
    }
    return "unknown";
}

std::uint16_t motor_rate(const BaseParams& base, bool ir, bool film_drag) {
    // MotorAdjust word order [normal, normal+drag, IR, IR+drag], each
    // clamped to 900..1100 (TLB@0x10016610); rate clamp for 0x44
    // boards (TLB@0x1000b6d0).
    const std::size_t idx = (ir ? 2u : 0u) + (film_drag ? 1u : 0u);
    const std::uint32_t adjust = std::clamp<std::uint32_t>(base.adjust[idx], 900, 1100);
    const std::uint32_t speed = ir ? base.motor_speed_ir : base.motor_speed;
    const std::uint32_t rate = speed * adjust / 1000;
    return static_cast<std::uint16_t>(std::clamp<std::uint32_t>(rate, 1000, 0x7FFE));
}

UnitCalibration fallback_16402() {
    UnitCalibration cal;
    cal.hardware_version = 400;
    cal.scanner_type = 1351;
    cal.serial = 16402;
    // Offset / MotorSpeed / MotorSpeed-IR: pakon-reference calibration.md
    // reference-unit table, equal to the captured 0xA5 rates and the
    // film-pass sub-4 offsets (base4 0x1E, base8 0x3A, base16 0x3C).
    cal.base[0] = {30, 25726, 19278, {1000, 1000, 1000, 1000}};
    cal.base[1] = {58, 11434, 7557, {1000, 1000, 1000, 1000}};
    cal.base[2] = {60, 5900, 4836, {1000, 1000, 1000, 1000}};
    cal.section_a = CopySource::fallback;
    cal.section_b = CopySource::fallback;
    return cal;
}

UnitCalibration parse(std::span<const std::uint8_t> image) {
    UnitCalibration cal = fallback_16402();
    cal.warnings = 0;

    // Section A.
    {
        unsigned primary = check_copy(image, kSectionA.primary, kSectionA.max_length);
        unsigned backup = primary == 0
                              ? 0u
                              : check_copy(image, kSectionA.backup, kSectionA.max_length);
        std::uint16_t base = 0;
        if (primary == 0) {
            cal.section_a = CopySource::primary;
            base = kSectionA.primary;
        } else if (backup == 0) {
            cal.section_a = CopySource::backup;
            base = kSectionA.backup;
            cal.notes.push_back(std::format(
                "section A primary {} - the backup copy is used (the OEM does the same "
                "silently)",
                primary == kWarnBlank ? "blank/implausible length" : "fails its CRC"));
        } else {
            cal.warnings |= primary | backup;
            cal.notes.push_back(
                "section A: both copies bad - Offset/MotorSpeed are the FALLBACK values "
                "of capture unit 16402, NOT this unit's");
        }
        if (cal.section_a != CopySource::fallback) {
            const auto b = image.subspan(base);
            cal.hardware_version = le32(b, 0x08);
            cal.scanner_type = le32(b, 0x0C);
            cal.serial = le32(b, 0x10);
            for (std::size_t i = 0; i < 3; ++i) {
                const std::size_t at = 0x14 + 6 * i;
                cal.base[i].offset = le16(b, at);
                cal.base[i].motor_speed = le16(b, at + 2);
                cal.base[i].motor_speed_ir = le16(b, at + 4);
            }
        }
    }

    // Section B: MotorAdjust words.
    {
        unsigned primary = check_copy(image, kSectionB.primary, kSectionB.max_length);
        unsigned backup = primary == 0
                              ? 0u
                              : check_copy(image, kSectionB.backup, kSectionB.max_length);
        std::uint16_t base = 0;
        if (primary == 0) {
            cal.section_b = CopySource::primary;
            base = kSectionB.primary;
        } else if (backup == 0) {
            cal.section_b = CopySource::backup;
            base = kSectionB.backup;
            cal.notes.push_back("section B primary bad - the backup copy is used");
        } else {
            cal.warnings |= primary | backup;
            cal.notes.push_back(
                "section B: both copies bad - MotorAdjust words are the FALLBACK 1000 "
                "(no adjustment)");
        }
        if (cal.section_b != CopySource::fallback) {
            const auto b = image.subspan(base);
            for (std::size_t i = 0; i < 3; ++i) {
                for (std::size_t k = 0; k < 4; ++k) {
                    cal.base[i].adjust[k] = le16(b, 0x08 + 8 * i + 2 * k);
                }
            }
        }
    }

    if (cal.section_a != CopySource::fallback && cal.scanner_type != 1351) {
        cal.notes.push_back(std::format(
            "scanner type {} - this stack drives the F-135+ (1351) only", cal.scanner_type));
    }
    return cal;
}

Result<std::vector<std::uint8_t>> read_sections(usb::IUsbTransport& transport) {
    std::vector<std::uint8_t> image(0xC00, 0xFF);
    constexpr auto kSelect = AllowedRequest::select_read();

    // One <= 32-byte chunk: select, then read (TLB@0x100160a0 re-selects
    // before every chunk).
    const auto read_chunk = [&](std::uint16_t offset, std::uint16_t length) -> VoidResult {
        const auto read = AllowedRequest::read_at(offset);
        if (!read || length == 0 || length > kMaxChunk) {
            return void_failure(ErrorKind::calibration_bad_header,
                                std::format("EEPROM read outside the chip: 0x{:04x}+{}",
                                            offset, length));
        }
        const auto& s = kSelect.request();
        if (auto w = transport.control_write(s.request, s.value, s.index); !w) {
            return w;
        }
        const auto& r = read->request();
        auto bytes = transport.control_read(r.request, r.value, r.index, length);
        if (!bytes) {
            return bytes.error();
        }
        if (bytes->size() != length) {
            return void_failure(ErrorKind::usb_short_transfer,
                                std::format("EEPROM read 0x{:04x}: {} of {} bytes", offset,
                                            bytes->size(), length));
        }
        std::copy(bytes->begin(), bytes->end(), image.begin() + offset);
        return {};
    };
    const auto read_range = [&](std::uint16_t offset, std::uint16_t length) -> VoidResult {
        while (length > 0) {
            const std::uint16_t n = std::min<std::uint16_t>(length, kMaxChunk);
            if (auto r = read_chunk(offset, n); !r) {
                return r;
            }
            offset = static_cast<std::uint16_t>(offset + n);
            length = static_cast<std::uint16_t>(length - n);
        }
        return {};
    };

    for (const auto& section : {kSectionA, kSectionB}) {
        for (const std::uint16_t base : {section.primary, section.backup}) {
            // Header, with the OEM's one retry on an implausible length.
            std::uint32_t length = 0;
            for (int attempt = 0; attempt < 2; ++attempt) {
                if (auto r = read_chunk(base, 8); !r) {
                    return r.error();
                }
                length = le32(image, base);
                if (length > 8 && length <= section.max_length) {
                    break;
                }
            }
            if (length > 8 && length <= section.max_length) {
                if (auto r = read_range(static_cast<std::uint16_t>(base + 8),
                                        static_cast<std::uint16_t>(length - 8));
                    !r) {
                    return r.error();
                }
                if (check_copy(image, base, section.max_length) == 0) {
                    break; // first good copy wins; the backup is not read
                }
            }
            log::Logger::instance().log(log::Level::warn,
                                        "EEPROM section {} copy at 0x{:03x} is bad",
                                        section.name, base);
        }
    }
    return image;
}

} // namespace pakon::eeprom
