// PnP/driver attribution via SetupAPI — property queries only.
//
// Same discovery pass as enumerate_win.cpp (DIGCF_PRESENT |
// DIGCF_ALLCLASSES, device-level devnodes, identity classification from
// usb/identity.hpp), but instead of preparing I/O it collects what
// Windows has bound: service, class + class GUID, descriptions and the
// driver INF/provider. Nothing here opens a device or sends traffic.

#ifdef _WIN32

// initguid.h FIRST: it defines INITGUID and latches guiddef.h/devpropdef.h
// to their defining forms, so the DEVPKEY_* definitions used below are
// emitted in this translation unit (same rationale as the forced
// /FIinitguid.h on usb/enumerate_win.cpp).
#include <initguid.h>

#include <windows.h>
#include <setupapi.h>
#include <devpkey.h>

#include "pakon/usb/driver_attrib.hpp"
#include "pakon/usb/identity.hpp"

#include <string>
#include <vector>

namespace pakon::usb {
namespace {

// DEVPROP_TYPE_STRING payload → UTF-8; "" when absent, empty or wrong type.
std::string wide_to_utf8(const wchar_t* text) {
    if (!text || !text[0]) return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    // size includes the terminator; store the text without it.
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size - 1, nullptr,
                        nullptr);
    return out;
}

std::string prop_string(HDEVINFO set, SP_DEVINFO_DATA& devinfo,
                        const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    DWORD need = 0;
    SetupDiGetDevicePropertyW(set, &devinfo, &key, &type, nullptr, 0, &need, 0);
    if (need == 0 || type != DEVPROP_TYPE_STRING) {
        return {};
    }
    std::vector<BYTE> buffer(need);
    if (!SetupDiGetDevicePropertyW(set, &devinfo, &key, &type, buffer.data(),
                                   need, nullptr, 0)) {
        return {};
    }
    return wide_to_utf8(reinterpret_cast<const wchar_t*>(buffer.data()));
}

// DEVPROP_TYPE_STRING_LIST (double-null-terminated MULTI_SZ) → vector.
std::vector<std::string> prop_string_list(HDEVINFO set, SP_DEVINFO_DATA& devinfo,
                                          const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    DWORD need = 0;
    SetupDiGetDevicePropertyW(set, &devinfo, &key, &type, nullptr, 0, &need, 0);
    if (need == 0 || type != DEVPROP_TYPE_STRING_LIST) {
        return {};
    }
    std::vector<BYTE> buffer(need);
    if (!SetupDiGetDevicePropertyW(set, &devinfo, &key, &type, buffer.data(),
                                   need, nullptr, 0)) {
        return {};
    }
    std::vector<std::string> out;
    const auto* begin = reinterpret_cast<const wchar_t*>(buffer.data());
    const auto* limit =
        reinterpret_cast<const wchar_t*>(buffer.data() + buffer.size());
    for (const wchar_t* cursor = begin; cursor < limit && cursor[0];) {
        out.push_back(wide_to_utf8(cursor));
        cursor += std::char_traits<wchar_t>::length(cursor) + 1;
    }
    return out;
}

} // namespace

bool driver_attribution_supported() { return true; }

std::vector<DriverAttribution> collect_driver_attributions() {
    std::vector<DriverAttribution> out;

    HDEVINFO set = SetupDiGetClassDevsA(nullptr, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) {
        return out;
    }

    SP_DEVINFO_DATA devinfo{};
    devinfo.cbSize = sizeof(devinfo);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &devinfo); ++index) {
        char instance[512] = {};
        if (!SetupDiGetDeviceInstanceIdA(set, &devinfo, instance,
                                         sizeof(instance), nullptr)) {
            continue;
        }

        DriverAttribution attr;
        attr.hardware_ids =
            prop_string_list(set, devinfo, DEVPKEY_Device_HardwareIds);
        const std::string primary = attr.hardware_ids.empty()
                                        ? std::string(hardware_id_of(instance))
                                        : attr.hardware_ids.front();
        attr.kind = classify_pakon_hardware_id(primary);
        if (attr.kind == PakonHardwareId::none) {
            continue; // not a supported Pakon identity (incl. &MI_ children)
        }
        if (const auto vid_pid = parse_vid_pid(primary)) {
            attr.vendor_id = vid_pid->first;
            attr.product_id = vid_pid->second;
        }

        attr.instance_id = instance;
        attr.compatible_ids =
            prop_string_list(set, devinfo, DEVPKEY_Device_CompatibleIds);
        attr.service = prop_string(set, devinfo, DEVPKEY_Device_Service);
        attr.class_name = prop_string(set, devinfo, DEVPKEY_Device_Class);
        attr.class_guid = prop_string(set, devinfo, DEVPKEY_Device_ClassGuid);
        attr.device_desc = prop_string(set, devinfo, DEVPKEY_Device_DeviceDesc);
        attr.friendly_name =
            prop_string(set, devinfo, DEVPKEY_Device_FriendlyName);
        attr.bus_reported_desc =
            prop_string(set, devinfo, DEVPKEY_Device_BusReportedDeviceDesc);
        attr.driver_inf =
            prop_string(set, devinfo, DEVPKEY_Device_DriverInfPath);
        attr.driver_provider =
            prop_string(set, devinfo, DEVPKEY_Device_DriverProvider);
        out.push_back(std::move(attr));
    }

    SetupDiDestroyDeviceInfoList(set);
    return out;
}

} // namespace pakon::usb

#endif // _WIN32
