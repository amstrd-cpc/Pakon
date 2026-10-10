#pragma once

// USB transport abstraction for the Pakon F-X35 family.
//
// Higher layers (PPB, scanner) depend only on IUsbTransport; concrete
// backends (Windows WinUSB, later libusb) plug in behind it.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/usb/bulk_pipe.hpp"

namespace pakon::usb {

// USB identity from pakon-reference/docs/usb-identity-and-firmware.md,
// "Cold and warm identity":
//
//   Cold (bootstrap, all models):  0f05:f235
//   Warm (operational 135-line):   0f05:f135
//   Warm F-235:                    0f05:35f2
//   Warm F-335:                    0f05:f335
inline constexpr std::uint16_t kVendorId = 0x0f05;
inline constexpr std::uint16_t kColdPidF235 = 0xf235;  // needs firmware load
inline constexpr std::uint16_t kWarmPidF135 = 0xf135;  // operational F-135/F-135+
inline constexpr std::uint16_t kWarmPidF235 = 0x35f2;
inline constexpr std::uint16_t kWarmPidF335 = 0xf335;

// Command channel endpoints, from ppb-protocol.md "Command channel":
//   "endpoint 0x01 OUT carries the host→device frame and 0x81 IN carries
//    the reply, as an atomic write-then-read."
inline constexpr std::uint8_t kCommandOutEndpoint = 0x01;
inline constexpr std::uint8_t kCommandInEndpoint = 0x81;

// Per-transfer deadline for EVERY pipe of an open session
// (WinUsbTransport::apply_timeouts): command OUT 0x01, command IN
// 0x81, image IN 0x86 (image::kImageEndpoint). WinUSB's default
// PIPE_TRANSFER_TIMEOUT is 0 = wait indefinitely: with no explicit
// policy a device that stops feeding 0x86 blocks WinUsb_ReadPipe
// forever, no usb_timeout surfaces, no idle tick is produced, and the
// quiescence policy (image/completion.hpp) can never fire - the scan
// would hang with the lamp on. With the deadline a stopped device
// costs one usb_timeout, which scan/transport.hpp maps to exactly one
// idle tick: a pause merely increments the idle counter (reset by the
// next data), so --idle-reads N (>= 2 enforced) waits
// N * kPipeTimeoutMs of real silence before a window ends - a single
// transient timeout can never end a scan.
inline constexpr unsigned long kPipeTimeoutMs = 2000;

// The image stream rides "a separate bulk IN endpoint" whose number is NOT
// stated in pakon-reference; it must be read from the device's endpoint
// descriptors at enumeration (image-stream.md: endpoint max packet 512,
// high speed). Do not hardcode a guessed number.

// One endpoint as reported by the device descriptor.
struct EndpointInfo {
    std::uint8_t address{};      // includes direction bit (0x80 = IN)
    std::uint8_t attributes{};   // transfer type: 2 = bulk
    std::uint16_t max_packet_size{};

    bool is_bulk() const noexcept { return (attributes & 0x03) == 2; }
    bool is_in() const noexcept { return (address & 0x80) != 0; }
};

struct InterfaceInfo {
    std::uint8_t number{};
    std::uint8_t alternate_setting{};
    std::uint8_t class_code{};
    std::uint8_t subclass_code{};
    std::uint8_t protocol_code{};
    std::vector<EndpointInfo> endpoints;
};

// A detected device, as reported by descriptors + string reads.
struct DeviceInfo {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint16_t bcd_device{};
    std::optional<std::string> serial_number;
    std::optional<std::string> manufacturer;
    std::optional<std::string> product;
    std::vector<InterfaceInfo> interfaces;

    // OS device path (Windows: the SetupAPI interface path), needed to
    // open the device. Empty when no function driver has registered an
    // interface — e.g. a Code 28 cold unit, which PnP discovery still
    // finds (see usb/identity.hpp). Not a USB descriptor field.
    std::string device_path;

    // Windows PnP device instance ID, e.g.
    // "USB\VID_0F05&PID_F235\6&1D7D6E45&0&4". Stable across driver
    // changes; the handle used to install/bind a driver later. Note the
    // segment after the backslash is a PnP location id, not a serial
    // number (usb/identity.hpp::serial_from_instance).
    std::string instance_id;

    // Windows PnP hardware ID, e.g. "USB\VID_0F05&PID_F235&REV_::07" —
    // the string driver/PakonWinUSB.inf matches against. Empty when the
    // device was discovered through its interface path only.
    std::string hardware_id;

    // Why interface/endpoint detail could not be read (e.g. the device is
    // owned by another driver that is not WinUSB-bound, or no function
    // driver is installed at all). Empty when the detail was read
    // successfully.
    std::string interface_note;

    // True if this is a known Pakon F-X35 identity (cold or warm).
    bool is_pakon() const noexcept { return vendor_id == kVendorId; }

    // True when the OS has a registered device interface for this device
    // (a function driver bound and exposed GUID_DEVINTERFACE_USB_DEVICE).
    // Necessary — but not sufficient — for openability: the binding may
    // still not be WinUSB (interface_note says so when opening fails).
    // False for a Code 28 device: discovered via PnP, nothing to open.
    bool has_device_interface() const noexcept { return !device_path.empty(); }

    // "cold" = bootstrap identity, firmware not yet loaded.
    bool is_cold() const noexcept {
        return vendor_id == kVendorId && product_id == kColdPidF235;
    }

    // Bulk IN endpoints excluding the command reply endpoint — the
    // candidates for the image stream endpoint.
    std::vector<EndpointInfo> bulk_in_endpoints() const {
        std::vector<EndpointInfo> out;
        for (const auto& iface : interfaces) {
            for (const auto& ep : iface.endpoints) {
                if (ep.is_bulk() && ep.is_in() && ep.address != kCommandInEndpoint) {
                    out.push_back(ep);
                }
            }
        }
        return out;
    }
};

// Enumerate all USB devices the backend can see (no device opened). On
// Windows this merges PnP device-tree discovery (finds devices with no
// function driver, e.g. Code 28 cold units) with device-interface
// discovery (supplies device_path where a function driver registered one).
std::vector<DeviceInfo> enumerate();

// Enumerate only Pakon devices (vendor 0f05).
std::vector<DeviceInfo> enumerate_pakon();

// A live connection to one device. RAII: opening claims the interface,
// closing (destructor) releases it.
class IUsbTransport {
public:
    virtual ~IUsbTransport() = default;

    // Bulk OUT then bulk IN on the command endpoint pair. The PPB layer
    // treats this as an atomic write-then-read exchange.
    virtual Result<std::vector<std::uint8_t>>
    command_exchange(std::span<const std::uint8_t> frame) = 0;

    // Bulk read from an arbitrary IN endpoint (image stream).
    virtual Result<std::vector<std::uint8_t>>
    bulk_read(std::uint8_t endpoint, std::size_t max_length) = 0;

    // Asynchronous bulk-IN pipe on `endpoint` with up to `max_slots`
    // reads in flight (usb/bulk_pipe.hpp) — the image stream's transport.
    // Backends without overlapped I/O report usb_not_supported.
    virtual Result<std::unique_ptr<IBulkInPipe>>
    open_bulk_in(std::uint8_t /*endpoint*/, std::size_t /*max_slots*/) {
        return failure<std::unique_ptr<IBulkInPipe>>(
            ErrorKind::usb_not_supported, "no asynchronous bulk-IN pipe on this backend");
    }

    // Vendor control request. Used for EEPROM/personality reads
    // (bmRequestType 0x40 OUT / 0xC0 IN, bRequest 0xA4/0xA9).
    virtual Result<std::vector<std::uint8_t>>
    control_read(std::uint8_t request, std::uint16_t value,
                 std::uint16_t index, std::uint16_t length) = 0;

    virtual Result<void> control_write(std::uint8_t request,
                                       std::uint16_t value,
                                       std::uint16_t index) = 0;

    // Underlying device identity for logging.
    virtual const DeviceInfo& device_info() const = 0;
};

// Open the first detected Pakon device. With `cold_ok == false` only an
// operational (warm) device is accepted — firmware loading is out of scope
// for now, and pointing the driver at a cold device would fail anyway.
Result<std::unique_ptr<IUsbTransport>> open_first(bool cold_ok = false);

} // namespace pakon::usb
