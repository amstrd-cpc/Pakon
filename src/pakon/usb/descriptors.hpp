#pragma once

// Read-only USB descriptor / topology discovery for the present booted
// Pakon scanner (0F05:F135 runtime).
//
// Answers "what does this device look like on the wire": the raw device
// and configuration descriptors, every interface / alternate setting /
// endpoint (direction, transfer type, max packet size, interval,
// class/subclass/protocol), the referenced USB string descriptors, the
// link speed, and the WinUSB interface path used — plus an independent
// WinUSB-side topology walk
// (QueryInterfaceSettings/QueryPipe) to cross-check the raw parse.
//
// How the data is obtained — standard, read-only traffic ONLY:
//   * GET_DESCRIPTOR (device / configuration / string), issued through
//     WinUsb_GetDescriptor — standard USB requests (bmRequestType 0x80
//     and 0x82), no vendor request;
//   * WinUSB local queries that talk to the driver, not the wire:
//     WinUsb_QueryDeviceInformation(DEVICE_SPEED),
//     WinUsb_QueryInterfaceSettings, WinUsb_QueryPipe,
//     WinUsb_GetAssociatedInterface. (The WinUSB API exposes no
//     bConfigurationValue read; an active configuration is implied by
//     WinUsb_Initialize succeeding.)
//
// Explicitly NOT done (the safety envelope for this diagnostic):
//   * no vendor-specific control transfers — nothing of the
//     0xA0/0xA3/0xA4/0xA9 family (or any bRequest >= 0x40);
//   * no firmware upload;
//   * no USB / port reset;
//   * no device reconfiguration — no set-configuration operation,
//     pipe-policy changes and alt-setting switches are never issued; the
//     device is left exactly as found;
//   * no bulk traffic of any kind.
//
// Evidence framing matches docs/BOOT_CHAIN.md: observed bytes are
// [PROVEN] from the device; role/meaning readings are [INFERRED];
// unknowns stay [UNKNOWN] (see docs/F135_TOPOLOGY.md).
//
// Windows-only; the non-Windows stub reports supported() == false.

#include <cstdint>
#include <string>
#include <vector>

namespace pakon::usb {

struct DescriptorEndpoint {
    std::uint8_t address{};      // bEndpointAddress (0x80 bit = IN)
    std::uint8_t attributes{};   // bmAttributes (bits 1..0 = transfer type)
    std::uint16_t w_max_packet{}; // wMaxPacketSize, raw 16-bit value
    std::uint8_t interval{};     // bInterval, raw (encoding varies per type)
};

struct DescriptorAltSetting {
    std::uint8_t interface_number{};
    std::uint8_t alternate_setting{};
    std::uint8_t class_code{};
    std::uint8_t subclass_code{};
    std::uint8_t protocol_code{};
    std::uint8_t string_index{}; // iInterface
    std::vector<DescriptorEndpoint> endpoints;
};

struct DescriptorInterface {
    std::uint8_t interface_number{};
    std::vector<DescriptorAltSetting> alt_settings;
};

struct DescriptorString {
    std::uint8_t index{};
    std::uint16_t language_id{}; // 0x0409 normally; 0 = language list read
    bool read = false;
    std::string text;  // UTF-8 payload ("" when not read as a string desc)
    std::string error; // "" when read succeeded
};

struct DescriptorScan {
    // False in non-Windows builds (nothing to scan with).
    bool supported = false;
    // Top-level failure ("" = the scan ran). Only set for hard stops:
    // no F135 present, no openable interface, CreateFile or
    // WinUsb_Initialize failure. Per-stage failures land in the fields
    // below so partial evidence is still reported.
    std::string error;

    // Device located via SetupAPI identity rules (no I/O for this step).
    std::string instance_id;
    std::string hardware_id;
    std::string device_path; // WinUSB device interface path used to open
    std::string open_mode;   // access mask that succeeded at CreateFile

    // WinUSB context (driver queries, no wire traffic). bConfigurationValue
    // is not exposed by the WinUSB API (no WinUsb_GetConfiguration
    // exists); an active configuration is implied by WinUsb_Initialize.
    bool link_speed_known = false;
    std::string link_speed; // low / full / high

    // GET_DESCRIPTOR(DEVICE) — 18 raw bytes + parsed standard fields.
    bool device_descriptor_read = false;
    std::string device_descriptor_error;
    std::vector<std::uint8_t> device_descriptor;
    std::uint16_t bcd_usb = 0;
    std::uint8_t device_class = 0;
    std::uint8_t device_sub_class = 0;
    std::uint8_t device_protocol = 0;
    std::uint8_t b_max_packet_size0 = 0;
    std::uint16_t id_vendor = 0;
    std::uint16_t id_product = 0;
    std::uint16_t bcd_device = 0;
    std::uint8_t i_manufacturer = 0;
    std::uint8_t i_product = 0;
    std::uint8_t i_serial = 0;
    std::uint8_t b_num_configurations = 0;

    // GET_DESCRIPTOR(CONFIGURATION) — 9-byte peek for wTotalLength, then
    // the full descriptor tree as returned by the device.
    bool config_descriptor_read = false;
    std::string config_descriptor_error;
    std::vector<std::uint8_t> config_descriptor;
    std::string config_parse_error; // first break in the TLV chain ("" = ok)
    std::uint8_t b_num_interfaces = 0;
    std::uint8_t b_configuration_value = 0;
    std::uint8_t i_configuration = 0;
    std::uint8_t bm_attributes = 0;
    std::uint8_t max_power = 0; // raw; 2 mA units at USB <= 2.0, 8 mA at 3.0

    // Topology parsed from the raw configuration descriptor bytes
    // (preferred: exactly what the device sent).
    std::vector<DescriptorInterface> interfaces;

    // Independent topology walk through WinUSB (QueryInterfaceSettings /
    // QueryPipe over the primary and every associated interface) — a
    // cross-check against the raw parse, not a copy of it.
    std::vector<DescriptorInterface> winusb_interfaces;
    std::string winusb_walk_error; // "" = walk completed

    // String descriptors referenced by the above (device iManufacturer /
    // iProduct / iSerial, iConfiguration, iInterface), deduplicated.
    std::vector<DescriptorString> strings;
};

// True when this build can perform descriptor discovery (Windows).
bool descriptor_scan_supported();

// Locate the present booted F135 (0F05:F135) and scan it read-only.
// Cold F235 units are never touched by this diagnostic. Never throws
// and never aborts: hard failures are returned in `error`, per-stage
// failures in their dedicated fields.
DescriptorScan scan_pakon_descriptors();

} // namespace pakon::usb
