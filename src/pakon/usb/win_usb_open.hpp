#pragma once

// Shared CreateFile parameters for opening the WinUSB device interface.
//
// Why this exists: the project originally had TWO WinUSB open sites and
// they drifted apart — the exact bug class this header prevents:
//
//   * enumerate_win.cpp / enrich_interfaces (what `list` reads interface
//     detail with) was fixed on hardware to open FILE_FLAG_OVERLAPPED;
//   * win_usb_transport.cpp / WinUsbTransport::open (what `probe`,
//     `identify` and `status` open with) kept the original non-overlapped
//     open from the first commit and was never updated.
//
// Hardware observation (cold unit, 2026-10-07, Service=WINUSB): both
// sites receive the SAME device path and differed ONLY in
// dwFlagsAndAttributes. The overlapped open succeeded — CreateFile,
// WinUsb_Initialize and WinUsb_QueryInterfaceSettings all worked (that
// is how `list` prints interface detail) — while the non-overlapped open
// got ERROR_INVALID_HANDLE (6) from WinUsb_Initialize. winusb.sys
// requires an overlapped file object; Microsoft's WinUSB samples always
// specify FILE_FLAG_OVERLAPPED for this reason.
//
// All open sites must build their CreateFile from kWinUsbOpenParams —
// currently: win_usb_transport.cpp, enumerate_win.cpp, and
// descriptors_win.cpp (the read-only descriptor scan):
//   - static_asserts in win_usb_transport.cpp pin the values to the real
//     Windows macros (compile-time, Windows builds);
//   - tests/usb/identity_test.cpp pins the values on any host AND reads
//     the first two sites' source files to fail if either stops using
//     this header or reintroduces its own (non-overlapped) attributes.

namespace pakon::usb {

// Windows API values as platform-independent constants (the file must
// stay free of <windows.h> so any host can test it). Values are the
// documented Win32 macros:
//   GENERIC_READ 0x80000000 | GENERIC_WRITE 0x40000000
//   FILE_SHARE_READ 0x1     | FILE_SHARE_WRITE 0x2
//   OPEN_EXISTING 3
//   FILE_FLAG_OVERLAPPED 0x40000000   (FILE_ATTRIBUTE_NORMAL is 0x80 —
//                                      never use it for WinUSB opens)
struct WinUsbOpenParams {
    unsigned long desired_access;
    unsigned long share_mode;
    unsigned long creation_disposition;
    unsigned long flags_and_attributes;
};

inline constexpr WinUsbOpenParams kWinUsbOpenParams{
    0x80000000ul | 0x40000000ul, // GENERIC_READ | GENERIC_WRITE
    0x00000001ul | 0x00000002ul, // FILE_SHARE_READ | FILE_SHARE_WRITE
    3ul,                         // OPEN_EXISTING
    0x40000000ul,                // FILE_FLAG_OVERLAPPED — mandatory
};

} // namespace pakon::usb
