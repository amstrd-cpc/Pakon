// USB enumeration on Windows via SetupAPI.
//
// Two discovery passes are merged by device instance ID:
//
//   1. PnP device tree (DIGCF_PRESENT | DIGCF_ALLCLASSES): every present
//      devnode, driver or no driver. This is what makes a Code 28 cold
//      unit discoverable: without a function driver Windows registers no
//      GUID_DEVINTERFACE_USB_DEVICE interface instance, so interface-only
//      enumeration never sees the device — but its devnode exists as soon
//      as the device is on the bus. Hardware-ID matching lives in
//      usb/identity.hpp (unit-tested on any host).
//   2. Device interfaces (GUID_DEVINTERFACE_USB_DEVICE): supplies the
//      device_path used to open a device, keeps non-Pakon devices
//      visible, and merges into pass 1's entry when both see the unit.
//
// Neither pass opens the device for I/O and neither depends on WinUSB
// being bound: VID/PID/serial/instance are parsed from the device
// identity, so a unit owned by the legacy Pakon driver is still listed.
// Interface/endpoint detail is added opportunistically by opening the
// device with WinUSB when that driver binding allows it.
//
// Listing devices and reading descriptors is read-only and safe
// (per-unit-data-and-safety.md: "Reading, polling and PPB commands to the
// known controller addresses have no recorded incident").

#ifdef _WIN32

#include "pakon/usb/identity.hpp"
#include "pakon/usb/transport.hpp"
#include "pakon/usb/win_usb_open.hpp"

#include <windows.h>
#include <setupapi.h>
#include <usbiodef.h>
#include <winusb.h>
#include <objbase.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pakon::usb {
namespace {

// Discovery bookkeeping shared between the two passes.
struct Discovery {
    std::vector<DeviceInfo> devices;

    // Lowercased instance ID → index into `devices` (pass 1 only).
    std::map<std::string, std::size_t> by_instance;

    // vid:pid identities pass 1 listed at device level — used to drop
    // duplicate &MI_ function entries for scanners already counted once.
    std::set<std::pair<std::uint16_t, std::uint16_t>> pnp_identities;

    // Lowercased instance ID → driver service name ("" = none installed).
    std::map<std::string, std::string> service_by_instance;
};

std::string instance_key(std::string_view instance) {
    return detail::ascii_lower(instance);
}

// First hardware ID of the devnode (SPDRP_HARDWAREID, REG_MULTI_SZ), e.g.
// "USB\VID_0F05&PID_F235&REV_::07". Falls back to the instance's
// hardware-ID segment (hardware_id_of) when the property is unavailable.
std::string devnode_hardware_id(HDEVINFO set, SP_DEVINFO_DATA& devinfo,
                                std::string_view instance) {
    DWORD required = 0;
    SetupDiGetDeviceRegistryPropertyA(set, &devinfo, SPDRP_HARDWAREID, nullptr, nullptr, 0,
                                      &required);
    if (required > 0) {
        std::vector<std::uint8_t> buffer(required);
        if (SetupDiGetDeviceRegistryPropertyA(set, &devinfo, SPDRP_HARDWAREID, nullptr,
                                              buffer.data(), required, nullptr)) {
            const auto* first = reinterpret_cast<const char*>(buffer.data());
            if (first[0] != '\0') {
                return first;
            }
        }
    }
    return std::string(hardware_id_of(instance));
}

// Function-driver service bound to the devnode (SPDRP_SERVICE). Empty
// when nothing is installed (Code 28).
std::string devnode_service(HDEVINFO set, SP_DEVINFO_DATA& devinfo) {
    char service[256] = {};
    if (!SetupDiGetDeviceRegistryPropertyA(set, &devinfo, SPDRP_SERVICE, nullptr,
                                           reinterpret_cast<BYTE*>(service),
                                           sizeof(service), nullptr)) {
        return {};
    }
    return service;
}

// Pass 1 — PnP device tree: works without any driver bound.
void enumerate_pnp(Discovery& discovery) {
    HDEVINFO set = SetupDiGetClassDevsA(nullptr, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) {
        return;
    }

    SP_DEVINFO_DATA devinfo{};
    devinfo.cbSize = sizeof(devinfo);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &devinfo); ++index) {
        char instance[512] = {};
        if (!SetupDiGetDeviceInstanceIdA(set, &devinfo, instance, sizeof(instance),
                                         nullptr)) {
            continue;
        }

        const std::string hardware = devnode_hardware_id(set, devinfo, instance);
        auto info = device_info_from_pnp(hardware, instance); // rejects non-Pakon + &MI_
        if (!info) {
            continue;
        }

        const std::string key = instance_key(instance);
        discovery.service_by_instance.emplace(key, devnode_service(set, devinfo));
        discovery.pnp_identities.emplace(info->vendor_id, info->product_id);
        discovery.by_instance.emplace(key, discovery.devices.size());
        discovery.devices.push_back(std::move(*info));
    }

    SetupDiDestroyDeviceInfoList(set);
}

// Pass 2 — registered device interfaces: device_path for devices a
// function driver exposes, plus the historical enumeration of all other
// USB devices. Pakon devices already listed by pass 1 are merged, not
// duplicated.
void enumerate_interfaces(Discovery& discovery) {
    const std::vector<GUID> interface_guids = {
        GUID_DEVINTERFACE_USB_DEVICE,
    };

    std::vector<GUID> guids = interface_guids;

    {
        const std::string guid_string(kDeviceInterfaceGuid);
        const std::wstring wide_guid(guid_string.begin(), guid_string.end());
        GUID pakon_guid{};
        if (CLSIDFromString(wide_guid.c_str(), &pakon_guid) == S_OK) {
            guids.push_back(pakon_guid);
        }
    }

    for (const GUID& interface_guid : guids) {
        HDEVINFO set = SetupDiGetClassDevsA(
            &interface_guid, nullptr, nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) continue;

        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);

        for (DWORD index = 0;
             SetupDiEnumDeviceInterfaces(
                 set, nullptr, &interface_guid, index, &interface_data);
             ++index) {

            DWORD required = 0;
            SetupDiGetDeviceInterfaceDetailA(
                set, &interface_data, nullptr, 0, &required, nullptr);
            if (required == 0) continue;

            std::vector<std::uint8_t> buffer(required);
            auto* detail =
                reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

            SP_DEVINFO_DATA devinfo{};
            devinfo.cbSize = sizeof(devinfo);
            if (!SetupDiGetDeviceInterfaceDetailA(
                    set, &interface_data, detail, required, nullptr, &devinfo)) {
                continue;
            }

            DeviceInfo info;
            info.device_path = detail->DevicePath;

            const auto vid_pid = parse_vid_pid(info.device_path);
            if (!vid_pid) continue;
            info.vendor_id = vid_pid->first;
            info.product_id = vid_pid->second;

            char instance[512] = {};
            if (SetupDiGetDeviceInstanceIdA(
                    set, &devinfo, instance, sizeof(instance), nullptr)) {
                info.instance_id = instance;
                info.serial_number = serial_from_instance(info.instance_id);
            }

            if (const auto it =
                    discovery.by_instance.find(instance_key(info.instance_id));
                it != discovery.by_instance.end()) {
                discovery.devices[it->second].device_path = info.device_path;
                discovery.devices[it->second].interface_note.clear();
                continue;
            }

            if (is_usb_interface_id(info.instance_id) &&
                discovery.pnp_identities.contains({info.vendor_id, info.product_id})) {
                continue;
            }

            discovery.devices.push_back(std::move(info));
        }

        SetupDiDestroyDeviceInfoList(set);
    }
}// Honest "discovered, but nothing to open" note for a Pakon devnode with
// no registered interface (Code 28, or a driver that exposes none).
std::string pnp_only_note(const Discovery& discovery, const DeviceInfo& device) {
    const auto it = discovery.service_by_instance.find(instance_key(device.instance_id));
    const std::string service = (it == discovery.service_by_instance.end()) ? "" : it->second;
    if (service.empty()) {
        return "discovered via PnP but no function-driver interface is registered "
               "(Code 28: no compatible driver installed) — not openable; install "
               "driver/PakonWinUSB.inf to bind WinUSB (docs/WINUSB_TEST.md)";
    }
    return "discovered via PnP; function driver '" + service +
           "' is bound but registers no GUID_DEVINTERFACE_USB_DEVICE interface "
           "— not openable via WinUSB";
}

// Try to add interface/endpoint detail via WinUSB. Fails harmlessly when
// the device is not WinUSB-bound (e.g. legacy Pakon driver owns it).
void enrich_interfaces(DeviceInfo& info) {
    // Same shared, unit-tested open parameters as the transport
    // (usb/win_usb_open.hpp): both sites must open identically, or they
    // drift — the transport's non-overlapped open is what produced
    // ERROR_INVALID_HANDLE (6) from WinUsb_Initialize on the cold unit
    // (2026-10-07) while this path succeeded.
    HANDLE device = CreateFileA(
        info.device_path.c_str(),
        static_cast<DWORD>(kWinUsbOpenParams.desired_access),
        static_cast<DWORD>(kWinUsbOpenParams.share_mode), nullptr,
        static_cast<DWORD>(kWinUsbOpenParams.creation_disposition),
        static_cast<DWORD>(kWinUsbOpenParams.flags_and_attributes), nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        // Retry read-only: some bindings allow query but not write access.
        WinUsbOpenParams read_only = kWinUsbOpenParams;
        read_only.desired_access = 0x80000000ul; // GENERIC_READ
        device = CreateFileA(
            info.device_path.c_str(),
            static_cast<DWORD>(read_only.desired_access),
            static_cast<DWORD>(read_only.share_mode), nullptr,
            static_cast<DWORD>(read_only.creation_disposition),
            static_cast<DWORD>(read_only.flags_and_attributes), nullptr);
    }
    if (device == INVALID_HANDLE_VALUE) {
        info.interface_note =
            "interface detail unavailable (device not openable; likely owned by "
            "the installed Pakon driver or another process)";
        return;
    }

    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(device, &usb)) {
        const DWORD error = GetLastError();
        info.interface_note =
            "WinUsb_Initialize failed, GetLastError=" + std::to_string(error);
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
    Discovery discovery;
    enumerate_pnp(discovery);
    enumerate_interfaces(discovery);

    // Endpoint detail for Pakon devices only — do not poke unrelated
    // hardware. Devnodes without an interface are reported as discovered
    // but not openable instead of being opened (there is no path to open).
    for (auto& device : discovery.devices) {
        if (!device.is_pakon()) {
            continue;
        }
        if (!device.has_device_interface()) {
            device.interface_note = pnp_only_note(discovery, device);
            continue;
        }
        enrich_interfaces(device);
    }
    return std::move(discovery.devices);
}

} // namespace pakon::usb

#endif // _WIN32
