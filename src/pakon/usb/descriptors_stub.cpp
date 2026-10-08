// Non-Windows stub: descriptor discovery needs WinUSB (GetDescriptor +
// interface/pipe queries), so the diagnostic reports unsupported instead
// of fabricating a result. Keeps pakon_core self-contained for the
// platform-independent test suites (same pattern as
// usb/driver_attrib_stub.cpp).

#ifndef _WIN32

#include "pakon/usb/descriptors.hpp"

namespace pakon::usb {

bool descriptor_scan_supported() { return false; }

DescriptorScan scan_pakon_descriptors() {
    DescriptorScan scan;
    scan.error =
        "descriptor discovery requires Windows (WinUSB descriptor "
        "queries); see docs/F135_TOPOLOGY.md";
    return scan;
}

} // namespace pakon::usb

#endif // !_WIN32
