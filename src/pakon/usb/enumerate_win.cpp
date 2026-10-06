// USB enumeration on Windows via SetupAPI.
//
// Deliberately does NOT open the device for I/O and does NOT depend on
// WinUSB being bound: VID/PID/serial are parsed from the device instance
// identity, so a unit owned by the legacy Pakon driver is still listed.
// Interface/endpoint detail is added opportunistically by opening the
// device with WinUSB when that driver binding allows it.
//
// Listing devices and reading descriptors is read-only and safe
// (per-unit-data-and-safety.md: "Reading, polling and PPB commands to the
// known controller addresses have no recorded incident").

#ifdef _WIN32

#include "pakon/usb/transport.hpp"

#include <windows.h>
#include <setupapi.h>
#include <usbiodef.h>
#include <winusb.h>

#include <optional>

namespace pakon::usb {
namespace {

// Parse "VID_XXXX&PID_XXXX" (case-insensitive) out of a device path or
// hardware id.
std::optional<std::pair<std::uint16_t, std::uint16_t>>
parse_vid_pid(const std::string& text) {
    auto find_hex_field = [&](const char* key) -> std::optional<std::uint16_t> {
        const std::string lower = [&] {
            std::string s = text;
            for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            return s;
        }();
        const auto pos = lower.find(key);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        const auto start = pos + std::char_traits<char>::length(key);
        std::uint16_t value = 0;
        int digits = 0;
        for (std::size_t i = start; i < lower.size() && digits < 4; ++i, ++digits) {
            const char c = lower[i];
            value = static_cast<std::uint16_t>(value << 4);
            if (c >= '0' && c <= '9') value |= static_cast<std::uint16_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint16_t>(c - 'a' + 10);
            else return std::nullopt;
        }
        if (digits != 4) return std::nullopt;
        return value;
    };

    const auto vid = find_hex_field("vid_");
    const auto pid = find_hex_field("pid_");
    if (!vid || !pid) {
        return std::nullopt;
    }
    return std::pair<std::uint16_t, std::uint16_t>{*vid, *pid};
}

// For a USB device the device-instance specific part is the serial number
// when the descriptor carries one:  "USB\VID_0F05&PID_F135\<serial>".
std::optional<std::string> serial_from_instance(const std::string& instance) {
    const auto backslash = instance.rfind('\\');
    if (backslash == std::string::npos || backslash + 1 >= instance.size()) {
        return std::nullopt;
    }
    auto serial = instance.substr(backslash + 1);
    // Instance paths embed parent relationships with "&" — a real serial
    // from the descriptor has no '&'. Keep only plausible values.
    if (serial.find('&') != std::string::npos) {
        return std::nullopt;
    }
    return serial;
}

// Try to add interface/endpoint detail via WinUSB. Fails harmlessly when
// the device is not WinUSB-bound (e.g. legacy Pakon driver owns it).
void enrich_interfaces(DeviceInfo& info) {
    HANDLE device = CreateFileA(info.device_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        // Retry read-only: some bindings allow query but not write access.
        device = CreateFileA(info.device_path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (device == INVALID_HANDLE_VALUE) {
        info.interface_note =
            "interface detail unavailable (device not openable; likely owned by "
            "the installed Pakon driver or another process)";
        return;
    }

    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(device, &usb)) {
        info.interface_note =
            "interface detail unavailable (not WinUSB-bound; run pakon-cli list "
            "on a system where the device is exposed via WinUSB to see endpoints)";
        CloseHandle(device);
        return;
    }

    const auto read_interface = [&](WINUSB_INTERFACE_HANDLE handle) {
        USB_INTERFACE_DESCRIPTOR descriptor{};
        if (!WinUsb_QueryInterfaceSettings(handle, 0, &descriptor)) {
            return;
        }

        InterfaceInfo iface;
        iface.number = descriptor.bInterfaceNumber;
        iface.alternate_setting = descriptor.bAlternateSetting;
        iface.class_code = descriptor.bInterfaceClass;
        iface.subclass_code = descriptor.bInterfaceSubClass;
        iface.protocol_code = descriptor.bInterfaceProtocol;

        for (UCHAR pipe = 0; pipe < descriptor.bNumEndpoints; ++pipe) {
            WINUSB_PIPE_INFORMATION pipe_info{};
            if (!WinUsb_QueryPipe(handle, descriptor.bAlternateSetting, pipe,
                                  &pipe_info)) {
                break;
            }
            EndpointInfo endpoint;
            endpoint.address = pipe_info.PipeId;
            endpoint.attributes = static_cast<std::uint8_t>(
                pipe_info.PipeType == UsbdPipeTypeBulk ? 2
                : pipe_info.PipeType == UsbdPipeTypeInterrupt ? 3
                : pipe_info.PipeType == UsbdPipeTypeIsochronous ? 1
                : 0);
            endpoint.max_packet_size = pipe_info.MaximumPacketSize;
            iface.endpoints.push_back(endpoint);
        }
        info.interfaces.push_back(std::move(iface));
    };

    // Interface 0 (the default interface WinUsb_Initialize bound) …
    read_interface(usb);

    // … and the associated interfaces, each with its own handle that must
    // be released with WinUsb_Free.
    for (UCHAR index = 0;; ++index) {
        WINUSB_INTERFACE_HANDLE associated = nullptr;
        if (!WinUsb_GetAssociatedInterface(usb, index, &associated)) {
            break; // ERROR_NO_MORE_ITEMS when there are no more
        }
        read_interface(associated);
        WinUsb_Free(associated);
    }

    WinUsb_Free(usb);
    CloseHandle(device);
}

} // namespace

std::vector<DeviceInfo> enumerate() {
    std::vector<DeviceInfo> devices;

    HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        return devices;
    }

    SP_DEVICE_INTERFACE_DATA interface_data{};
    interface_data.cbSize = sizeof(interface_data);

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_USB_DEVICE, index,
                                     &interface_data);
         ++index) {

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailA(set, &interface_data, nullptr, 0, &required, nullptr);
        if (required == 0) {
            continue;
        }
        std::vector<std::uint8_t> buffer(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        SP_DEVINFO_DATA devinfo{};
        devinfo.cbSize = sizeof(devinfo);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &interface_data, detail, required,
                                              nullptr, &devinfo)) {
            continue;
        }

        DeviceInfo info;
        info.device_path = detail->DevicePath;

        const auto vid_pid = parse_vid_pid(info.device_path);
        if (!vid_pid) {
            continue; // not a USB device path with a parseable identity
        }
        info.vendor_id = vid_pid->first;
        info.product_id = vid_pid->second;

        char instance[512] = {};
        if (SetupDiGetDeviceInstanceIdA(set, &devinfo, instance, sizeof(instance) - 1,
                                        nullptr)) {
            info.serial_number = serial_from_instance(instance);
        }

        devices.push_back(std::move(info));
    }

    SetupDiDestroyDeviceInfoList(set);

    // Endpoint detail for Pakon devices only — do not poke unrelated hardware.
    for (auto& device : devices) {
        if (device.is_pakon()) {
            enrich_interfaces(device);
        }
    }
    return devices;
}

} // namespace pakon::usb

#endif // _WIN32
