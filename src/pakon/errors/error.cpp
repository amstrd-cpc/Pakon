#include "pakon/errors/error.hpp"

namespace pakon {

std::string_view to_string(ErrorKind kind) {
    switch (kind) {
    case ErrorKind::unknown: return "unknown";
    case ErrorKind::usb_device_not_found: return "usb_device_not_found";
    case ErrorKind::usb_open_failed: return "usb_open_failed";
    case ErrorKind::usb_io_failed: return "usb_io_failed";
    case ErrorKind::usb_timeout: return "usb_timeout";
    case ErrorKind::usb_access_denied: return "usb_access_denied";
    case ErrorKind::usb_not_supported: return "usb_not_supported";
    case ErrorKind::ppb_invalid_frame: return "ppb_invalid_frame";
    case ErrorKind::ppb_bad_status: return "ppb_bad_status";
    case ErrorKind::ppb_unexpected_reply: return "ppb_unexpected_reply";
    case ErrorKind::ppb_reply_too_short: return "ppb_reply_too_short";
    case ErrorKind::scanner_not_connected: return "scanner_not_connected";
    case ErrorKind::scanner_unexpected_state: return "scanner_unexpected_state";
    case ErrorKind::scanner_timeout: return "scanner_timeout";
    case ErrorKind::scanner_absent_controller: return "scanner_absent_controller";
    case ErrorKind::calibration_crc_mismatch: return "calibration_crc_mismatch";
    case ErrorKind::calibration_bad_header: return "calibration_bad_header";
    case ErrorKind::image_truncated: return "image_truncated";
    case ErrorKind::image_bad_marker: return "image_bad_marker";
    }
    return "unknown";
}

} // namespace pakon
