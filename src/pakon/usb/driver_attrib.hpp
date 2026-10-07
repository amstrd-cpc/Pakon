#pragma once

// Read-only PnP/driver attribution for attached Pakon devices.
//
// Answers "what does Windows currently think this device is": VID/PID,
// hardware and compatible IDs, bound service, device class + class GUID,
// DeviceDesc / FriendlyName / BusReportedDeviceDesc, and the driver INF
// + provider that bound. Every field is a PnP property query over the
// present device list — no device is opened, no USB traffic is sent,
// no driver state is modified. This is the attribution half of
// docs/BOOT_CHAIN.md § 6 (which INF bound the booted F135?).
//
// Windows-only; the non-Windows stub reports supported() == false.

#include <cstdint>
#include <string>
#include <vector>

#include "pakon/usb/identity.hpp"

namespace pakon::usb {

struct DriverAttribution {
    std::string instance_id; // device instance ID as PnP reports it
    PakonHardwareId kind = PakonHardwareId::none; // cold_f235 / warm_f135
    std::uint16_t vendor_id = 0; // parsed from the first hardware ID
    std::uint16_t product_id = 0;

    std::vector<std::string> hardware_ids;   // DEVPKEY_Device_HardwareIds
    std::vector<std::string> compatible_ids; // DEVPKEY_Device_CompatibleIds
    std::string service;       // DEVPKEY_Device_Service ("" = none / Code 28)
    std::string class_name;    // DEVPKEY_Device_Class
    std::string class_guid;    // DEVPKEY_Device_ClassGuid
    std::string device_desc;   // DEVPKEY_Device_DeviceDesc
    std::string friendly_name; // DEVPKEY_Device_FriendlyName ("" = not set)
    std::string bus_reported_desc; // DEVPKEY_Device_BusReportedDeviceDesc
    std::string driver_inf;    // DEVPKEY_Device_DriverInfPath ("" = not set)
    std::string driver_provider; // DEVPKEY_Device_DriverProvider
};

// True when this build can query PnP properties (Windows).
bool driver_attribution_supported();

// Present Pakon devnodes at device level (composite &MI_ function
// children are excluded by the shared identity rules, so one scanner is
// reported once). Never fails: a field whose property is unavailable is
// left empty (the CLI prints "(not set)").
std::vector<DriverAttribution> collect_driver_attributions();

} // namespace pakon::usb
