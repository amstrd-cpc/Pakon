#pragma once

// Structured error type for the pakon core library.
//
// pakon-reference has no std::expected equivalent to translate; this is
// plain infrastructure. We use std::expected when available (C++23, or
// MSVC/GCC/Clang with a sufficiently new library in C++20 mode) and fall
// back to a minimal otherwise-identical implementation so call sites never
// change.

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
#include <expected>
#define PAKON_HAS_STD_EXPECTED 1
#else
#include <optional>
#define PAKON_HAS_STD_EXPECTED 0
#endif

#include <string>
#include <string_view>
#include <utility>

namespace pakon {

// Error codes for the stack. Strongly typed so callers can branch without
// string comparison. Grouped by layer.
enum class ErrorKind {
    // Generic
    unknown,
    io_failure,          // host-side file I/O (raw image output)

    // USB transport layer
    usb_device_not_found,
    usb_open_failed,
    usb_io_failed,
    usb_timeout,
    usb_access_denied,
    usb_not_supported,
    usb_short_transfer,       // control transfer returned fewer bytes than requested

    // PPB protocol layer
    ppb_invalid_frame,      // type byte 0 or truncated frame (see safety rules)
    ppb_bad_status,         // device reported a non-success status
    ppb_unexpected_reply,   // reply type/form does not match the request
    ppb_reply_too_short,    // reply too short to carry a status byte (must not be read as success)

    // Scanner layer
    scanner_not_connected,
    scanner_unexpected_state,
    scanner_timeout,
    scanner_absent_controller, // presence probe reported controller absent

    // Calibration layer
    calibration_crc_mismatch,
    calibration_bad_header,

    // Image layer
    image_truncated,
    image_bad_marker,
    image_no_completion_policy, // scan window configured without a completion policy
    image_bad_geometry,         // row window/geometry cannot hold the requested layout

    // Process/operator layer
    cancelled,                  // interrupt (Ctrl+C) observed at a bounded point of a run
};

// A single error: what went wrong and optionally why (device message,
// status byte, etc.). Copyable and cheap.
struct Error {
    ErrorKind kind{ErrorKind::unknown};
    std::string message;

    static Error make(ErrorKind k, std::string msg) {
        return Error{k, std::move(msg)};
    }
};

// Human-readable name for logs.
std::string_view to_string(ErrorKind kind);

#if PAKON_HAS_STD_EXPECTED

template <typename T>
using Result = std::expected<T, Error>;

using VoidResult = std::expected<void, Error>;

#else

// Minimal std::expected stand-in for toolchains without <expected>.
// Only what this project uses: value/error construction, has_value,
// operator*, value(), error(), operator->.
template <typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)

    // Implicit error construction — mirrors std::expected's error
    // converting constructor, so call sites can `return result.error();`
    // to re-propagate an error in both configurations.
    Result(Error error) : has_(false), error_(std::move(error)) {} // NOLINT(google-explicit-constructor)

    static Result failure(Error error) {
        Result r;
        r.has_ = false;
        r.error_ = std::move(error);
        return r;
    }

    bool has_value() const noexcept { return has_; }
    explicit operator bool() const noexcept { return has_; }

    T& value() & { return *value_; }
    const T& value() const& { return *value_; }
    T&& value() && { return std::move(*value_); }

    T& operator*() & { return *value_; }
    const T& operator*() const& { return *value_; }
    T* operator->() { return &*value_; }
    const T* operator->() const { return &*value_; }

    Error& error() & { return error_; }
    const Error& error() const& { return error_; }

private:
    Result() = default;

    bool has_{true};
    std::optional<T> value_;
    Error error_{};
};

template <>
class Result<void> {
public:
    Result() = default;

    // See Result<T>'s error constructor.
    Result(Error error) : has_(false), error_(std::move(error)) {} // NOLINT(google-explicit-constructor)

    static Result failure(Error error) {
        Result r;
        r.has_ = false;
        r.error_ = std::move(error);
        return r;
    }

    bool has_value() const noexcept { return has_; }
    explicit operator bool() const noexcept { return has_; }

    Error& error() & { return error_; }
    const Error& error() const& { return error_; }

private:
    bool has_{true};
    Error error_{};
};

using VoidResult = Result<void>;

#endif

// Helpers that work in both configurations.
template <typename T>
inline Result<T> failure(ErrorKind kind, std::string msg) {
#if PAKON_HAS_STD_EXPECTED
    return std::unexpected(Error{kind, std::move(msg)});
#else
    return Result<T>::failure(Error{kind, std::move(msg)});
#endif
}

inline VoidResult void_failure(ErrorKind kind, std::string msg) {
#if PAKON_HAS_STD_EXPECTED
    return std::unexpected(Error{kind, std::move(msg)});
#else
    return VoidResult::failure(Error{kind, std::move(msg)});
#endif
}

} // namespace pakon
