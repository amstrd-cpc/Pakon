// WinUSB backend for IUsbTransport (Windows).
//
// Endpoint facts implemented here are from pakon-reference:
//   - command channel: bulk OUT 0x01 -> bulk IN 0x81, "an atomic
//     write-then-read" (docs/ppb-protocol.md sec. Command channel)
//   - image stream: a separate bulk IN endpoint, max packet 512, host
//     reads it in transfers up to 0x5000 (20480) bytes (docs/image-stream.md)
//   - vendor control requests 0xA4/0xA9 for EEPROM reads
//     (docs/calibration.md sec. The read)

#ifdef _WIN32

#include "pakon/usb/transport.hpp"
#include "pakon/usb/win_usb_open.hpp"
#include "pakon/logging/logger.hpp"

#include <windows.h>
#include <winusb.h>

#include <array>
#include <format>

namespace pakon::usb {

// The shared open parameters are used verbatim by both Windows open
// sites (this file and enumerate_win.cpp); pin them to the real Win32
// macros so a value typo cannot compile (any drift between the sites is
// additionally pinned by tests/usb/identity_test.cpp on any host).
static_assert(kWinUsbOpenParams.desired_access ==
              static_cast<unsigned long>(GENERIC_READ | GENERIC_WRITE));
static_assert(kWinUsbOpenParams.share_mode ==
              static_cast<unsigned long>(FILE_SHARE_READ | FILE_SHARE_WRITE));
static_assert(kWinUsbOpenParams.creation_disposition ==
              static_cast<unsigned long>(OPEN_EXISTING));
static_assert(kWinUsbOpenParams.flags_and_attributes ==
              static_cast<unsigned long>(FILE_FLAG_OVERLAPPED));

namespace {

// Human-readable text for a Win32 error code (FormatMessage), with the
// trailing newline/period FormatMessage appends trimmed off. "unknown"
// when the system has no text for the code.
std::string win32_error_text(DWORD error) {
    char buffer[512] = {};
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        error, 0, buffer, static_cast<DWORD>(sizeof(buffer)), nullptr);
    if (length == 0) {
        return "unknown";
    }
    std::string text(buffer, length);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' ||
                             text.back() == ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text;
}

class WinUsbTransport final : public IUsbTransport {
public:
    static Result<std::unique_ptr<WinUsbTransport>> open(const DeviceInfo& info) {
        // Shared, unit-tested parameters (usb/win_usb_open.hpp). The
        // handle MUST be opened overlapped: a non-overlapped open of the
        // same path made WinUsb_Initialize fail with ERROR_INVALID_HANDLE
        // on the real cold unit (2026-10-07) while the overlapped
        // enumeration open succeeded - see the header for the evidence.
        // Stage 1: CreateFile. Its failure is reported on its own, with
        // the Windows error code, and never confused with stage 2.
        HANDLE device = CreateFileA(
            info.device_path.c_str(),
            static_cast<DWORD>(kWinUsbOpenParams.desired_access),
            static_cast<DWORD>(kWinUsbOpenParams.share_mode), nullptr,
            static_cast<DWORD>(kWinUsbOpenParams.creation_disposition),
            static_cast<DWORD>(kWinUsbOpenParams.flags_and_attributes), nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            const auto kind = (error == ERROR_ACCESS_DENIED)
                                  ? ErrorKind::usb_access_denied
                                  : ErrorKind::usb_open_failed;
            return failure<std::unique_ptr<WinUsbTransport>>(
                kind, std::format("CreateFile failed for {}: Windows error {} ({})",
                                  info.device_path, error,
                                  win32_error_text(error)));
        }

        // Stage 2: WinUsb_Initialize on the raw CreateFile handle (the
        // handle is neither closed nor replaced between the stages).
        WINUSB_INTERFACE_HANDLE usb = nullptr;
        if (!WinUsb_Initialize(device, &usb)) {
            const DWORD error = GetLastError();
            CloseHandle(device);
            return failure<std::unique_ptr<WinUsbTransport>>(
                ErrorKind::usb_open_failed,
                std::format("WinUsb_Initialize failed for {}: Windows error {} ({}) "
                            "- CreateFile succeeded, so the handle itself is valid; "
                            "the bound function driver may not be WinUSB "
                            "(see docs/USB.md)",
                            info.device_path, error, win32_error_text(error)));
        }

        auto transport = std::unique_ptr<WinUsbTransport>(
            new WinUsbTransport(info, device, usb));
        transport->apply_timeouts();
        return transport;
    }

    ~WinUsbTransport() override {
        if (usb_ != nullptr) {
            WinUsb_Free(usb_);
        }
        if (device_ != INVALID_HANDLE_VALUE) {
            CloseHandle(device_);
        }
    }

    WinUsbTransport(const WinUsbTransport&) = delete;
    WinUsbTransport& operator=(const WinUsbTransport&) = delete;

    Result<std::vector<std::uint8_t>>
    command_exchange(std::span<const std::uint8_t> frame) override {
        if (frame.empty()) {
            return failure<std::vector<std::uint8_t>>(ErrorKind::ppb_invalid_frame,
                                                      "empty command frame");
        }
        log::Logger::instance().hex(log::Level::trace, "TX", kCommandOutEndpoint, frame);

        ULONG written = 0;
        if (!WinUsb_WritePipe(usb_, kCommandOutEndpoint,
                               const_cast<PUCHAR>(frame.data()),
                               static_cast<ULONG>(frame.size()), &written, nullptr)) {
            const auto error = GetLastError();
            return failure<std::vector<std::uint8_t>>(
                error == ERROR_SEM_TIMEOUT ? ErrorKind::usb_timeout : ErrorKind::usb_io_failed,
                std::format("command WritePipe(0x{:02x}) failed: {}",
                            kCommandOutEndpoint, error));
        }

        // Reply arrives as one bulk IN transfer; read until the frame is
        // complete (frame length = 2 + count, validated by the PPB parser).
        std::vector<std::uint8_t> reply;
        reply.reserve(64);
        std::array<std::uint8_t, 1024> chunk{};
        for (;;) {
            ULONG read = 0;
            if (!WinUsb_ReadPipe(usb_, kCommandInEndpoint, chunk.data(),
                                 static_cast<ULONG>(chunk.size()), &read, nullptr)) {
                const auto error = GetLastError();
                return failure<std::vector<std::uint8_t>>(
                    error == ERROR_SEM_TIMEOUT ? ErrorKind::usb_timeout : ErrorKind::usb_io_failed,
                    std::format("command ReadPipe(0x{:02x}) failed after {} reply bytes: {}",
                                kCommandInEndpoint, reply.size(), error));
            }
            if (read == 0) {
                break; // short packet / ZLP terminates the transfer
            }
            reply.insert(reply.end(), chunk.begin(), chunk.begin() + read);
            if (reply.size() >= 2) {
                const std::size_t expected = 2 + reply[1];
                if (reply.size() >= expected) {
                    break;
                }
            }
        }

        log::Logger::instance().hex(log::Level::trace, "RX", kCommandInEndpoint, reply);
        return reply;
    }

    Result<std::vector<std::uint8_t>>
    bulk_read(std::uint8_t endpoint, std::size_t max_length) override {
        std::vector<std::uint8_t> buffer(max_length);
        ULONG read = 0;
        if (!WinUsb_ReadPipe(usb_, endpoint, buffer.data(),
                             static_cast<ULONG>(max_length), &read, nullptr)) {
            const auto error = GetLastError();
            return failure<std::vector<std::uint8_t>>(
                error == ERROR_SEM_TIMEOUT ? ErrorKind::usb_timeout : ErrorKind::usb_io_failed,
                std::format("ReadPipe(0x{:02x}) failed: {}", endpoint, error));
        }
        buffer.resize(read);
        log::Logger::instance().hex(log::Level::trace, "RX", endpoint,
                                    std::span<const std::uint8_t>(buffer));
        return buffer;
    }

    Result<std::vector<std::uint8_t>>
    control_read(std::uint8_t request, std::uint16_t value, std::uint16_t index,
                 std::uint16_t length) override {
        WINUSB_SETUP_PACKET setup{};
        setup.RequestType = 0xC0; // vendor IN, device, standard
        setup.Request = request;
        setup.Value = value;
        setup.Index = index;
        setup.Length = length;

        std::vector<std::uint8_t> buffer(length);
        ULONG transferred = 0;
        if (!WinUsb_ControlTransfer(usb_, setup, buffer.data(), length, &transferred, nullptr)) {
            const auto error = GetLastError();
            return failure<std::vector<std::uint8_t>>(
                error == ERROR_SEM_TIMEOUT ? ErrorKind::usb_timeout : ErrorKind::usb_io_failed,
                std::format("control read 0x{:02x}/0x{:04x}/0x{:04x} failed: {}",
                            request, value, index, error));
        }
        buffer.resize(transferred);
        log::Logger::instance().hex(
            log::Level::trace,
            std::format("CTL-IN req=0x{:02x} value=0x{:04x} index=0x{:04x}", request, value, index),
            std::span<const std::uint8_t>(buffer));
        return buffer;
    }

    Result<void> control_write(std::uint8_t request, std::uint16_t value,
                               std::uint16_t index) override {
        WINUSB_SETUP_PACKET setup{};
        setup.RequestType = 0x40; // vendor OUT, no data stage
        setup.Request = request;
        setup.Value = value;
        setup.Index = index;
        setup.Length = 0;

        ULONG transferred = 0;
        if (!WinUsb_ControlTransfer(usb_, setup, nullptr, 0, &transferred, nullptr)) {
            const auto error = GetLastError();
            return void_failure(ErrorKind::usb_io_failed,
                                std::format("control write 0x{:02x}/0x{:04x}/0x{:04x} failed: {}",
                                            request, value, index, error));
        }
        log::Logger::instance().log(
            log::Level::trace, "CTL-OUT req=0x{:02x} value=0x{:04x} index=0x{:04x}",
            request, value, index);
        return {};
    }

    const DeviceInfo& device_info() const override { return info_; }

private:
    WinUsbTransport(DeviceInfo info, HANDLE device, WINUSB_INTERFACE_HANDLE usb)
        : info_(std::move(info)), device_(device), usb_(usb) {}

    void apply_timeouts() {
        // Generous per-transfer timeouts; callers may tighten later.
        ULONG timeout_ms = 2000;
        WinUsb_SetPipePolicy(usb_, kCommandOutEndpoint, PIPE_TRANSFER_TIMEOUT,
                             sizeof(timeout_ms), &timeout_ms);
        WinUsb_SetPipePolicy(usb_, kCommandInEndpoint, PIPE_TRANSFER_TIMEOUT,
                             sizeof(timeout_ms), &timeout_ms);
    }

    DeviceInfo info_;
    HANDLE device_{INVALID_HANDLE_VALUE};
    WINUSB_INTERFACE_HANDLE usb_{nullptr};
};

} // namespace

Result<std::unique_ptr<IUsbTransport>> open_first(bool cold_ok) {
    auto devices = enumerate_pakon();

    // Prefer an operational (warm) device with a registered interface -
    // only an interface yields a device_path CreateFile can open. Firmware
    // loading for a cold device is out of scope (docs/usb-identity-and-firmware.md
    // sequence, firmware bytes not shipped by this project), so cold
    // devices are accepted only when the caller explicitly passes
    // cold_ok; a device without an interface is never openable.
    const DeviceInfo* chosen = nullptr;
    const DeviceInfo* cold_fallback = nullptr;
    for (const auto& device : devices) {
        if (!device.has_device_interface()) {
            continue;
        }
        if (!device.is_cold()) {
            chosen = &device;
            break;
        }
        if (cold_ok && cold_fallback == nullptr) {
            cold_fallback = &device;
        }
    }
    if (chosen == nullptr) {
        chosen = cold_fallback;
    }

    if (chosen == nullptr) {
        if (devices.empty()) {
            return failure<std::unique_ptr<IUsbTransport>>(ErrorKind::usb_device_not_found,
                                                           "no Pakon F-X35 device detected");
        }
        const bool any_interface = [&] {
            for (const auto& device : devices) {
                if (device.has_device_interface()) return true;
            }
            return false;
        }();
        if (!any_interface) {
            // Devices were found (list shows them) but nothing can be
            // opened - say exactly why instead of pretending they're absent.
            return failure<std::unique_ptr<IUsbTransport>>(
                ErrorKind::usb_access_denied,
                "Pakon device discovered but not openable: no function-driver "
                "device interface is registered (Code 28, or a driver that does "
                "not expose WinUSB) - see docs/USB.md sec. Enumeration and "
                "docs/WINUSB_TEST.md to bind WinUSB");
        }
        return failure<std::unique_ptr<IUsbTransport>>(
            ErrorKind::usb_device_not_found,
            "Pakon device in cold/bootstrap state (0f05:f235): firmware is not "
            "loaded; firmware loading is not implemented");
    }

    log::Logger::instance().log(log::Level::info,
                                "opening Pakon device vid=0x{:04x} pid=0x{:04x} serial={}",
                                chosen->vendor_id, chosen->product_id,
                                chosen->serial_number.value_or("<none>"));

    auto result = WinUsbTransport::open(*chosen);
    if (!result) {
        return result.error();
    }
    std::unique_ptr<IUsbTransport> transport = std::move(result.value());
    return transport;
}

} // namespace pakon::usb

#endif // _WIN32
