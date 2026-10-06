#pragma once

// PPB session client: owns a transport, serializes requests, parses
// replies, logs packet traffic, and enforces the address safety
// allow-list.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/ppb/packet.hpp"
#include "pakon/usb/transport.hpp"

namespace pakon::ppb {

// Safety policy for outbound frames, distilled from
// pakon-reference/docs/per-unit-data-and-safety.md:
//
//  - Only the documented controller addresses may be addressed:
//    HOST (0x10), PICL/PICM (0x20/0x24), PICL+/PICM+ (0x40/0x44).
//  - NEVER the bootloader addresses (0x22/0x26/0x42/0x46): type-4 frames
//    with command bytes 0x0C–0x0F there erase flash rows (a real unit
//    lost motor firmware this way).
//  - NEVER the shifted EEPROM bus addresses (0xA2 boot personality,
//    0xA4 per-unit EEPROM): writes there have erased a real unit's
//    personality.
//
// exchange() applies this check to every outbound frame.
bool is_allowed_destination(std::uint8_t address);

class Client {
public:
    explicit Client(std::unique_ptr<usb::IUsbTransport> transport);

    // Send one frame and parse the reply, validating the reply form
    // against the request type. Logs TX/RX at trace level.
    Result<Reply> exchange(const Frame& request);

    // Same, additionally checking that the reply mirrors the expected
    // request type.
    Result<Reply> exchange(const Frame& request, FrameType expected_reply);

    // Access to transport for control transfers (EEPROM reads etc.).
    usb::IUsbTransport& transport() { return *transport_; }

    const usb::DeviceInfo& device_info() const { return transport_->device_info(); }

private:
    std::unique_ptr<usb::IUsbTransport> transport_;
};

} // namespace pakon::ppb
