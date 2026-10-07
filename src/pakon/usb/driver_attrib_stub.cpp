// Non-Windows stub: PnP property queries are a Windows facility, so the
// attribution diagnostic reports unsupported instead of fabricating a
// result. Keeps pakon_core self-contained for the platform-independent
// test suites (same pattern as usb/transport_stub.cpp).

#ifndef _WIN32

#include "pakon/usb/driver_attrib.hpp"

namespace pakon::usb {

bool driver_attribution_supported() { return false; }

std::vector<DriverAttribution> collect_driver_attributions() { return {}; }

} // namespace pakon::usb

#endif // !_WIN32
