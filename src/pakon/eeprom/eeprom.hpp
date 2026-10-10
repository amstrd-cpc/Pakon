#pragma once

// Read-only access to the scanner's per-unit EEPROM (I2C chip 0x52).
//
// Wire protocol, from the OEM's own read routine bEEPromRead
// (docs/OEM_RE.md §5, TLB@0x100160a0): for each chunk of <= 32 bytes,
//   vendor OUT 0xA4  wValue 0x00A5  wIndex 0x1234  (no data)   read-select
//   vendor IN  0xA9  wValue offset  wIndex 0x1234  (<= 32 B)    read
// 0x00A5 = ((2 | 0x50) << 1) | 1: chip 2, read direction.
//
// SAFETY — the EEPROM holds irreplaceable factory data. The same OEM
// routine writes with select bit0 clear (0x00A4) followed by 0xA2. This
// module cannot express either: the only requests it can build are the
// two AllowedRequest constants below, both checked by static_assert
// against is_allowed(), and the transport layer refuses 0xA2 and
// write-selects again at runtime (usb/win_usb_transport.cpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/usb/transport.hpp"

namespace pakon::eeprom {

inline constexpr std::uint16_t kIndex = 0x1234;
inline constexpr std::uint8_t kRequestSelect = 0xA4;
inline constexpr std::uint8_t kRequestRead = 0xA9;
inline constexpr std::uint8_t kRequestWrite = 0xA2; // never sent
inline constexpr std::uint16_t kSelectRead = 0x00A5;
inline constexpr std::uint16_t kSize = 0x2000;      // offsets checked by the OEM
inline constexpr std::uint16_t kMaxChunk = 32;

enum class Direction { out, in };

struct ControlRequest {
    Direction direction;
    std::uint8_t request;
    std::uint16_t value;
    std::uint16_t index;
};

// The complete allow-list: read-select (exactly 0x00A5) and reads below
// kSize. Everything else — 0xA2 at any index, a write-select (even
// wValue), any other request on wIndex 0x1234 — is refused.
constexpr bool is_allowed(const ControlRequest& r) {
    if (r.request == kRequestWrite || r.index != kIndex) {
        return false;
    }
    if (r.direction == Direction::out && r.request == kRequestSelect) {
        return r.value == kSelectRead;
    }
    if (r.direction == Direction::in && r.request == kRequestRead) {
        return r.value < kSize;
    }
    return false;
}

static_assert(!is_allowed({Direction::out, kRequestWrite, 0x0000, kIndex}));
static_assert(!is_allowed({Direction::out, kRequestWrite, 0x0000, 0x0000}));
static_assert(!is_allowed({Direction::out, kRequestSelect, 0x00A4, kIndex}),
              "a write-select must be impossible");
static_assert(!is_allowed({Direction::in, kRequestRead, 0x0000, 0x0000}),
              "wIndex 0 is the bootstrap personality read, not this chip");

// The only two request shapes this module can issue.
class AllowedRequest {
public:
    static consteval AllowedRequest select_read() {
        return AllowedRequest({Direction::out, kRequestSelect, kSelectRead, kIndex});
    }
    // Nullopt when the offset is outside the chip.
    static constexpr std::optional<AllowedRequest> read_at(std::uint16_t offset) {
        const ControlRequest r{Direction::in, kRequestRead, offset, kIndex};
        if (!is_allowed(r)) {
            return std::nullopt;
        }
        return AllowedRequest(r);
    }
    constexpr const ControlRequest& request() const { return request_; }

private:
    constexpr explicit AllowedRequest(ControlRequest r) : request_(r) {}
    ControlRequest request_;
};

static_assert(is_allowed(AllowedRequest::select_read().request()));
static_assert(AllowedRequest::read_at(0x0000).has_value());
static_assert(!AllowedRequest::read_at(0x2000).has_value());

// CRC-32 (reflected, polynomial 0xEDB88320, zlib/PKZIP) — the section
// checksum (TLB@0x100163c0).
std::uint32_t crc32(std::span<const std::uint8_t> bytes);

// Section geometry (OEM_RE.md §5).
struct SectionLayout {
    char name;
    std::uint16_t primary;
    std::uint16_t backup;
    std::uint16_t max_length; // header + payload
};
inline constexpr SectionLayout kSectionA{'A', 0x000, 0x400, 398};
inline constexpr SectionLayout kSectionB{'B', 0x800, 0xA00, 36};

enum class CopySource { primary, backup, fallback };
std::string_view to_string(CopySource source);

// Warning bits, the OEM's INITIALIZEW_* values.
inline constexpr unsigned kWarnBlank = 1;
inline constexpr unsigned kWarnChecksumBad = 2;

// Per-base fields. Base index 0/1/2 = Base 4 / 8 / 16.
struct BaseParams {
    std::uint16_t offset{0};                 // FPGA pixel-window start, film pass
    std::uint16_t motor_speed{0};            // no IR
    std::uint16_t motor_speed_ir{0};
    std::array<std::uint16_t, 4> adjust{};   // normal, normal+drag, IR, IR+drag (raw)
};

struct UnitCalibration {
    std::uint32_t hardware_version{0};
    std::uint32_t scanner_type{0};  // 1350 F-135, 1351 F-135+
    std::uint32_t serial{0};
    std::array<BaseParams, 3> base{};
    CopySource section_a{CopySource::fallback};
    CopySource section_b{CopySource::fallback};
    unsigned warnings{0};           // kWarnBlank | kWarnChecksumBad, per OEM
    std::vector<std::string> notes; // what was read, what fell back

    bool uses_fallback() const {
        return section_a == CopySource::fallback || section_b == CopySource::fallback;
    }
};

// The film-pass motor rate: speed × clamp(adjust, 900, 1100) / 1000,
// clamped to the OEM's [1000, 0x7FFE] for 0x44 boards (OEM_RE.md §5, §3).
std::uint16_t motor_rate(const BaseParams& base, bool ir, bool film_drag = false);

// Capture-corpus values of unit 16402 (pakon-captures README +
// pakon-reference calibration.md), used ONLY when this unit's EEPROM
// cannot supply a section — always reported as a fallback.
UnitCalibration fallback_16402();

// Parse a raw dump laid out at the chip's own offsets (at least 0xC00
// bytes; missing ranges count as unreadable). Never fails: a section
// whose two copies are both bad falls back to fallback_16402() with
// the OEM warning bit set and a note.
UnitCalibration parse(std::span<const std::uint8_t> image);

// Read the sections from the device exactly as the OEM does (primary,
// then backup on a bad length or CRC), returning the bytes read placed
// at their chip offsets (unread bytes 0xFF). Only AllowedRequest traffic.
Result<std::vector<std::uint8_t>> read_sections(usb::IUsbTransport& transport);

} // namespace pakon::eeprom
