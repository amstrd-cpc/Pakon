#pragma once

// Pure, platform-independent USB identity helpers: hardware-ID
// recognition, PnP instance parsing, and the device interface GUID shared
// with the WinUSB INF package.
//
// Everything here is header-only string logic — no Windows headers, no
// I/O — so the recognition rules are unit-tested on any host
// (tests/usb/identity_test.cpp) while the Windows enumeration backend
// (enumerate_win.cpp) runs the exact same code.
//
// Hardware-ID forms observed/documented (case-insensitive):
//   USB\VID_0F05&PID_F235             cold bootstrap (all models)
//   USB\VID_0F05&PID_F235&REV_::07    cold as our unit enumerates in
//                                     Windows — non-BCD bcdDevice nibbles
//                                     render as ':' (observed live)
//   USB\VID_0F05&PID_F135             warm F-135/F-135+
//   USB\VID_0F05&PID_F135&REV_0002    warm identity after firmware load
//                                     (pakon-reference identity table)
//   USB\VID_0F05&PID_F135&MI_00       composite *function* child devnode —
//                                     never a device; excluded from
//                                     discovery (a scanner must be listed
//                                     once, at its device-level devnode)
//
// Device instance IDs additionally carry the Windows location id after
// the backslash ("USB\VID_0F05&PID_F235\6&1D7D6E45&0&4"). That segment is
// NOT a USB descriptor serial number.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "pakon/usb/transport.hpp"

namespace pakon::usb {

// Which supported Pakon identity a hardware ID denotes.
enum class PakonHardwareId {
    none,
    cold_f235, // 0f05:f235 — bootstrap, firmware not loaded
    warm_f135, // 0f05:f135 — operational F-135/F-135+
};

// Hardware-ID prefixes Windows matches our INF package against
// (driver/PakonWinUSB.inf). Canonical casing as printed by Device
// Manager; matching is case-insensitive because device *paths* are not.
inline constexpr std::string_view kColdHardwareIdPrefix = "USB\\VID_0F05&PID_F235";
inline constexpr std::string_view kWarmHardwareIdPrefix = "USB\\VID_0F05&PID_F135";

// Device interface GUID registered by driver/PakonWinUSB.inf
// ([Dev_AddReg] → HKR,,DeviceInterfaceGUIDs) when WinUSB binds. This
// constant and the INF MUST stay byte-identical; tests/usb/identity_test.cpp
// reads both files and fails if they drift apart.
inline constexpr std::string_view kDeviceInterfaceGuid =
    "{0e9e6f29-e70a-4582-8d02-bde3ad701252}";

namespace detail {

inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = ascii_lower(c);
    return out;
}

// A prefix match only counts at a token boundary: "PID_F135" must not
// match "PID_F1354". Valid boundaries: end of string, '&' (…&REV_xxx,
// …&MI_00), or '\' (hardware-ID segment of a device instance).
inline bool boundary_after(std::string_view text, std::size_t pos) {
    return pos == text.size() || text[pos] == '&' || text[pos] == '\\';
}

// The instance-specific tail of a device instance ID — everything after
// the second backslash ("" when the instance carries only the hardware
// ID). The hardware ID itself contains exactly one backslash
// ("USB\VID_…"); the PnP-generated tail never contains one.
inline std::string_view instance_tail(std::string_view instance) {
    const auto first = instance.find('\\');
    if (first == std::string_view::npos) {
        return {};
    }
    const auto second = instance.find('\\', first + 1);
    return second == std::string_view::npos
               ? std::string_view{}
               : instance.substr(second + 1);
}

} // namespace detail

// Parse "VID_XXXX&PID_XXXX" (case-insensitive) out of a device path,
// hardware ID or device instance ID. Returns nullopt when either field is
// absent or not exactly four hex digits.
inline std::optional<std::pair<std::uint16_t, std::uint16_t>>
parse_vid_pid(std::string_view text) {
    auto find_hex_field = [&](std::string_view key) -> std::optional<std::uint16_t> {
        const std::string lower = detail::ascii_lower(text);
        const auto pos = lower.find(key);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        const auto start = pos + key.size();
        std::uint16_t value = 0;
        int digits = 0;
        for (std::size_t i = start; i < lower.size() && digits < 4; ++i, ++digits) {
            const char c = lower[i];
            value = static_cast<std::uint16_t>(value << 4);
            if (c >= '0' && c <= '9') value |= static_cast<std::uint16_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint16_t>(c - 'a' + 10);
            else return std::nullopt;
        }
        if (digits != 4) return std::nullopt;
        return value;
    };

    const auto vid = find_hex_field("vid_");
    const auto pid = find_hex_field("pid_");
    if (!vid || !pid) {
        return std::nullopt;
    }
    return std::pair<std::uint16_t, std::uint16_t>{*vid, *pid};
}

// True for composite *function* identifiers ("…&MI_00", case-insensitive):
// child devnodes of a composite device, not the physical device itself.
// Discovery filters these so one scanner is never reported per interface.
inline bool is_usb_interface_id(std::string_view id) {
    return detail::ascii_lower(id).find("&mi_") != std::string::npos;
}

// Classify a hardware ID (or the hardware-ID segment of a device
// instance) as one of the supported Pakon identities. Rejects composite
// function IDs and anything outside USB\VID_0F05&PID_F235 / F135 — the
// warm F-235 (35f2) and F-335 identities are out of scope (docs/USB.md).
inline PakonHardwareId classify_pakon_hardware_id(std::string_view hardware_id) {
    if (is_usb_interface_id(hardware_id)) {
        return PakonHardwareId::none;
    }
    const std::string lower = detail::ascii_lower(hardware_id);
    const std::string cold = detail::ascii_lower(kColdHardwareIdPrefix);
    const std::string warm = detail::ascii_lower(kWarmHardwareIdPrefix);
    if (lower.starts_with(cold) && detail::boundary_after(lower, cold.size())) {
        return PakonHardwareId::cold_f235;
    }
    if (lower.starts_with(warm) && detail::boundary_after(lower, warm.size())) {
        return PakonHardwareId::warm_f135;
    }
    return PakonHardwareId::none;
}

// Hardware-ID segment of a device instance:
//   "USB\VID_0F05&PID_F235" ← "USB\VID_0F05&PID_F235\6&1D7D6E45&0&4"
// The split is at the second backslash — the first one belongs to the
// hardware ID itself ("USB\…"), and the instance-specific tail never
// contains one. A bare hardware ID (no tail) is returned unchanged. For
// USB devnodes this doubles as a fallback when SPDRP_HARDWAREID cannot
// be read.
inline std::string_view hardware_id_of(std::string_view instance) {
    const auto first = instance.find('\\');
    if (first == std::string_view::npos) {
        return instance;
    }
    const auto second = instance.find('\\', first + 1);
    return second == std::string_view::npos ? instance : instance.substr(0, second);
}

// USB serial number from a device instance, when the descriptor carries
// one: "USB\VID_0F05&PID_F135\16402" → "16402".
//
// The instance-specific segment of a *location-assigned* device is a
// PnP relationship path such as "6&1D7D6E45&0&4" (parent chain + slot).
// A descriptor serial never contains '&', so those are rejected — a PnP
// location id must never be presented as a serial number.
inline std::optional<std::string> serial_from_instance(std::string_view instance) {
    const auto tail = detail::instance_tail(instance);
    if (tail.empty() || tail.find('&') != std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(tail);
}

// Build a DeviceInfo from PnP data alone — no function driver, no device
// interface, nothing opened. This is how a Code 28 cold unit becomes a
// *discovered* device: present, identified, and explicitly not openable
// (device_path stays empty; has_device_interface() is false).
//
// Returns nullopt when the hardware ID is not a supported Pakon identity
// (including composite &MI_ function IDs).
inline std::optional<DeviceInfo> device_info_from_pnp(std::string_view hardware_id,
                                                      std::string_view instance_id) {
    const auto kind = classify_pakon_hardware_id(hardware_id);
    if (kind == PakonHardwareId::none) {
        return std::nullopt;
    }
    DeviceInfo info;
    info.vendor_id = kVendorId;
    info.product_id = (kind == PakonHardwareId::cold_f235) ? kColdPidF235 : kWarmPidF135;
    info.hardware_id = std::string(hardware_id);
    info.instance_id = std::string(instance_id);
    info.serial_number = serial_from_instance(instance_id);
    return info;
}

} // namespace pakon::usb
