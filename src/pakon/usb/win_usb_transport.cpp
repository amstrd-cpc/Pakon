// WinUSB backend for IUsbTransport (Windows).
//
// Endpoint facts implemented here are from pakon-reference:
//   - command channel: bulk OUT 0x01 → bulk IN 0x81, "an atomic
//     write-then-read" (docs/ppb-protocol.md § Command channel)
//   - image stream: a separate bulk IN endpoint, max packet 512, host
//     reads it in transfers up to 0x5000 (20480) bytes (docs/image-stream.md)
//   - vendor control requests 0xA4/0xA9 for EEPROM reads
//     (docs/calibration.md § The read)

#ifdef _WIN32

#include "pakon/usb/transport.hpp"
#include "pakon/logging/logger.hpp"

#include <windows.h>
#include <winusb.h>

#include <array>
#include <format>

namespace pakon::usb {
namespace {

class WinUsbTransport final : public IUsbTransport {
public:
    static Result<std::unique_ptr<WinUsbTransport>> open(const DeviceInfo& info) {
        HANDLE device = CreateFileA(info.device_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            const auto kind = (error == ERROR_ACCESS_DENIED)
                                  ? ErrorKind::usb_access_denied
                                  : ErrorKind::usb_open_failed;
            return failure<std::unique_ptr<WinUsbTransport>>(
                kind, std::format("CreateFile({}) failed: {}", info.device_path, error));
        }

        WINUSB_INTERFACE_HANDLE usb = nullptr;
        if (!WinUsb_Initialize(device, &usb)) {
            const auto error = GetLastError();
            CloseHandle(device);
            return failure<std::unique_ptr<WinUsbTransport>>(
                ErrorKind::usb_open_failed,
                std::format("WinUsb_Initialize failed: {} (device is probably owned "
                            "by the installed Pakon driver, not WinUSB)",
                            error));
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

    // Prefer an operational (warm) device; firmware loading for a cold
    // device is out of scope (docs/usb-identity-and-firmware.md sequence,
    // firmware bytes not shipped by this project).
    const DeviceInfo* chosen = nullptr;
    for (const auto& device : devices) {
        if (!device.is_cold()) {
            chosen = &device;
            break;
        }
    }
    if (chosen == nullptr && cold_ok && !devices.empty()) {
        chosen = &devices.front();
    }

    if (chosen == nullptr) {
        return failure<std::unique_ptr<IUsbTransport>>(ErrorKind::usb_device_not_found,
                                                       "no Pakon F-X35 device detected");
    }
    if (chosen->is_cold() && !cold_ok) {
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
