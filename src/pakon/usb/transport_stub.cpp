// Non-Windows stub backend.
//
// The first deliverable targets Windows/MSVC. On other platforms the
// project must still build (for unit tests of the protocol layers, which
// are transport-independent), so enumeration reports "no devices" and
// open_first reports that the backend is unavailable. A libusb backend
// can replace this file later without touching higher layers — that is
// the point of IUsbTransport.

#ifndef _WIN32

#include "pakon/usb/transport.hpp"

#include "pakon/logging/logger.hpp"

namespace pakon::usb {

std::vector<DeviceInfo> enumerate() {
    return {};
}

Result<std::unique_ptr<IUsbTransport>> open_first(bool /*cold_ok*/) {
    return failure<std::unique_ptr<IUsbTransport>>(
        ErrorKind::usb_not_supported,
        "no native USB backend on this platform (Windows WinUSB backend "
        "not compiled in)");
}

} // namespace pakon::usb

#endif // !_WIN32
