// Read-only descriptor discovery — standard GET_DESCRIPTOR reads plus
// WinUSB local descriptor queries.
//
// Discovery reuses the shared two-pass enumeration (usb/enumerate_win.cpp:
// PnP devnode + registered interface, identity rules from
// usb/identity.hpp) to locate the booted F135; nothing is opened for
// that step. The scan then opens the device with the shared,
// hardware-proven WinUSB open parameters (usb/win_usb_open.hpp — same
// overlapped CreateFile as the transport and `list`) and reads:
//
//   1. GET_DESCRIPTOR(DEVICE)         — 18 raw bytes (WinUsb_GetDescriptor);
//   2. GET_DESCRIPTOR(CONFIGURATION)  — 9-byte peek for wTotalLength, then
//                                       the full tree;
//   3. the string descriptors those reference (GET_DESCRIPTOR(STRING));
//   4. WinUsb_QueryDeviceInformation(DEVICE_SPEED) — driver-side state,
//      no wire traffic;
//   5. QueryInterfaceSettings / QueryPipe over the primary and every
//      associated interface — an independent topology cross-check.
//
// Note: the WinUSB API exposes no bConfigurationValue read (there is no
// WinUsb_GetConfiguration); the report records that a configuration is
// active implicitly — WinUsb_Initialize only succeeds on one.
//
// Safety (see also the header): WinUsb_GetDescriptor issues only
// standard GET_DESCRIPTOR requests (bmRequestType 0x80/0x82). No vendor
// control transfer (0xA0/0xA3/0xA4/0xA9 family), no WinUsb_Set* — no
// configuration change, no alt-setting switch, no pipe-policy change —
// no reset, no bulk traffic, no firmware upload. The handle is closed
// immediately afterwards; the device is left exactly as found.

#ifdef _WIN32

#include "pakon/usb/descriptors.hpp"
#include "pakon/usb/identity.hpp"
#include "pakon/usb/transport.hpp"
#include "pakon/usb/win_usb_open.hpp"

#include <windows.h>
#include <winusb.h>

#include <cstddef>
#include <string>
#include <vector>

namespace pakon::usb {
namespace {

// USB descriptor type values (usb100.h names vary across SDKs; the
// numbers are fixed by the USB specification).
constexpr UCHAR kDescriptorDevice = 0x01;
constexpr UCHAR kDescriptorConfiguration = 0x02;
constexpr UCHAR kDescriptorString = 0x03;
constexpr UCHAR kDescriptorInterface = 0x04;
constexpr UCHAR kDescriptorEndpoint = 0x05;

// Plausibility cap for wTotalLength — a real F135 configuration is tens
// of bytes; anything absurd indicates a malformed read, not a descriptor.
constexpr std::size_t kMaxConfigBytes = 8192;

std::uint8_t byte_at(const std::vector<std::uint8_t>& data, std::size_t pos) {
    return pos < data.size() ? data[pos] : std::uint8_t{0};
}

std::uint16_t word_at(const std::vector<std::uint8_t>& data, std::size_t pos) {
    return static_cast<std::uint16_t>(byte_at(data, pos)) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(byte_at(data, pos + 1)) << 8);
}

std::string last_error_text() {
    return "GetLastError=" + std::to_string(GetLastError());
}

// Two uppercase hex digits (for honest "0x1F"-style messages).
std::string hex_byte(std::uint8_t value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.push_back(digits[(value >> 4) & 0x0F]);
    out.push_back(digits[value & 0x0F]);
    return out;
}

// USB string descriptor payload (UTF-16LE, starting after the 2-byte
// header) → UTF-8. BMP characters and surrogate pairs are handled; an
// unpaired surrogate decodes as U+FFFD (never a crash, never a lie).
std::string utf16le_to_utf8(const std::uint8_t* data, std::size_t bytes) {
    auto emit = [](std::string& out, std::uint32_t codepoint) {
        if (codepoint < 0x80) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(
                static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(
                static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(
                static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    };

    std::string out;
    for (std::size_t i = 0; i + 1 < bytes; i += 2) {
        std::uint32_t codepoint =
            static_cast<std::uint32_t>(data[i]) |
            (static_cast<std::uint32_t>(data[i + 1]) << 8);
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF &&
            i + 3 < bytes) {
            const std::uint32_t low =
                static_cast<std::uint32_t>(data[i + 2]) |
                (static_cast<std::uint32_t>(data[i + 3]) << 8);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) +
                            (low - 0xDC00);
                i += 2;
            } else {
                codepoint = 0xFFFD;
            }
        } else if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            codepoint = 0xFFFD;
        }
        emit(out, codepoint);
    }
    return out;
}

// USBD_PIPE_TYPE → bmAttributes low bits (matches the wire encoding:
// control 0, isochronous 1, bulk 2, interrupt 3). If-chains instead of
// a switch to keep /W4 quiet about enum coverage on both toolchains.
std::uint8_t attributes_from_pipe_type(USBD_PIPE_TYPE pipe_type) {
    if (pipe_type == UsbdPipeTypeIsochronous) return 1;
    if (pipe_type == UsbdPipeTypeBulk) return 2;
    if (pipe_type == UsbdPipeTypeInterrupt) return 3;
    return 0; // UsbdPipeTypeControl
}

DescriptorAltSetting* find_alt_setting(DescriptorInterface& iface,
                                       std::uint8_t alternate_setting) {
    for (auto& alt : iface.alt_settings) {
        if (alt.alternate_setting == alternate_setting) return &alt;
    }
    return nullptr;
}

DescriptorInterface* find_interface(std::vector<DescriptorInterface>& list,
                                    std::uint8_t interface_number) {
    for (auto& iface : list) {
        if (iface.interface_number == interface_number) return &iface;
    }
    return nullptr;
}

// Parse the raw configuration descriptor tree exactly as the device
// sent it: walk the TLV chain, record interface / alternate-setting /
// endpoint descriptors, skip everything else (IAD, class-specific) by
// bLength. The first anomaly is recorded; the walk itself stays safe
// (bLength 0 or out-of-range stops it).
void parse_config_tree(const std::vector<std::uint8_t>& bytes,
                       DescriptorScan& report) {
    if (bytes.size() < 9) {
        report.config_parse_error =
            "configuration descriptor shorter than 9 bytes";
        return;
    }
    report.b_num_interfaces = bytes[4];
    report.b_configuration_value = bytes[5];
    report.i_configuration = bytes[6];
    report.bm_attributes = bytes[7];
    report.max_power = bytes[8];

    DescriptorAltSetting* current_alt = nullptr;
    std::size_t pos = 0;
    while (pos + 2 <= bytes.size()) {
        const std::uint8_t length = bytes[pos];
        const std::uint8_t type = bytes[pos + 1];
        if (length < 2 || pos + length > bytes.size()) {
            if (report.config_parse_error.empty()) {
                report.config_parse_error =
                    "descriptor chain broken at offset " +
                    std::to_string(pos) + " (bLength=" +
                    std::to_string(length) + ")";
            }
            return;
        }

        if (type == kDescriptorInterface) {
            if (length < 9) {
                if (report.config_parse_error.empty()) {
                    report.config_parse_error =
                        "interface descriptor at offset " +
                        std::to_string(pos) + " shorter than 9 bytes";
                }
                return;
            }
            DescriptorAltSetting alt;
            alt.interface_number = bytes[pos + 2];
            alt.alternate_setting = bytes[pos + 3];
            alt.class_code = bytes[pos + 5];
            alt.subclass_code = bytes[pos + 6];
            alt.protocol_code = bytes[pos + 7];
            alt.string_index = bytes[pos + 8];

            DescriptorInterface* iface =
                find_interface(report.interfaces, alt.interface_number);
            if (!iface) {
                report.interfaces.push_back(DescriptorInterface{});
                iface = &report.interfaces.back();
                iface->interface_number = alt.interface_number;
            }
            iface->alt_settings.push_back(alt);
            current_alt = &iface->alt_settings.back();
        } else if (type == kDescriptorEndpoint) {
            if (length < 7) {
                if (report.config_parse_error.empty()) {
                    report.config_parse_error =
                        "endpoint descriptor at offset " +
                        std::to_string(pos) + " shorter than 7 bytes";
                }
                return;
            }
            if (!current_alt) {
                if (report.config_parse_error.empty()) {
                    report.config_parse_error =
                        "endpoint descriptor at offset " +
                        std::to_string(pos) +
                        " precedes any interface descriptor";
                }
                // keep walking — the chain itself is still well-formed
            } else {
                DescriptorEndpoint endpoint;
                endpoint.address = bytes[pos + 2];
                endpoint.attributes = bytes[pos + 3];
                endpoint.w_max_packet = word_at(bytes, pos + 4);
                endpoint.interval = bytes[pos + 6];
                current_alt->endpoints.push_back(endpoint);
            }
        }
        pos += length;
    }
}

// One WinUSB open attempt with the shared parameters. Falls back to
// read-only access like enumerate_win.cpp (some bindings allow query
// but not write access) and records which mask succeeded.
HANDLE open_device(const std::string& path, std::string& mode,
                   std::string& error) {
    HANDLE file = CreateFileA(
        path.c_str(),
        static_cast<DWORD>(kWinUsbOpenParams.desired_access),
        static_cast<DWORD>(kWinUsbOpenParams.share_mode), nullptr,
        static_cast<DWORD>(kWinUsbOpenParams.creation_disposition),
        static_cast<DWORD>(kWinUsbOpenParams.flags_and_attributes), nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        mode = "GENERIC_READ|GENERIC_WRITE, FILE_FLAG_OVERLAPPED";
        return file;
    }

    WinUsbOpenParams read_only = kWinUsbOpenParams;
    read_only.desired_access = 0x80000000ul; // GENERIC_READ
    file = CreateFileA(
        path.c_str(), static_cast<DWORD>(read_only.desired_access),
        static_cast<DWORD>(read_only.share_mode), nullptr,
        static_cast<DWORD>(read_only.creation_disposition),
        static_cast<DWORD>(read_only.flags_and_attributes), nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        mode = "GENERIC_READ (read-only fallback), FILE_FLAG_OVERLAPPED";
        return file;
    }

    error = "CreateFile failed, " + last_error_text();
    return INVALID_HANDLE_VALUE;
}

// GET_DESCRIPTOR through WinUSB. Returns the bytes actually transferred
// (0 on failure, with `error` set).
std::vector<std::uint8_t> get_descriptor(WINUSB_INTERFACE_HANDLE usb,
                                         UCHAR type, UCHAR index,
                                         USHORT language,
                                         ULONG requested,
                                         std::string& error) {
    std::vector<std::uint8_t> buffer(requested, 0);
    ULONG transferred = 0;
    if (!WinUsb_GetDescriptor(usb, type, index, language, buffer.data(),
                              requested, &transferred)) {
        error = "WinUsb_GetDescriptor(type=" + std::to_string(type) +
                ", index=" + std::to_string(index) + ") failed, " +
                last_error_text();
        return {};
    }
    buffer.resize(transferred);
    return buffer;
}

// Walk every alternate setting of one interface handle and record its
// endpoints (WinUSB's view of the topology — independent of the raw
// descriptor parse).
void walk_interface(WINUSB_INTERFACE_HANDLE handle,
                    std::vector<DescriptorInterface>& out,
                    std::string& error) {
    for (unsigned alt_index = 0; alt_index < 256; ++alt_index) {
        USB_INTERFACE_DESCRIPTOR desc{};
        if (!WinUsb_QueryInterfaceSettings(handle,
                                           static_cast<UCHAR>(alt_index),
                                           &desc)) {
            if (alt_index == 0 && error.empty()) {
                error = "WinUsb_QueryInterfaceSettings failed, " +
                        last_error_text();
            }
            break; // no further alternate settings
        }

        DescriptorInterface* iface = find_interface(out, desc.bInterfaceNumber);
        if (!iface) {
            out.push_back(DescriptorInterface{});
            iface = &out.back();
            iface->interface_number = desc.bInterfaceNumber;
        }

        DescriptorAltSetting alt;
        alt.interface_number = desc.bInterfaceNumber;
        alt.alternate_setting = desc.bAlternateSetting;
        alt.class_code = desc.bInterfaceClass;
        alt.subclass_code = desc.bInterfaceSubClass;
        alt.protocol_code = desc.bInterfaceProtocol;
        alt.string_index = desc.iInterface;
        if (find_alt_setting(*iface, desc.bAlternateSetting)) {
            continue; // already recorded (defensive; should not happen)
        }

        for (UCHAR pipe_index = 0; pipe_index < desc.bNumEndpoints;
             ++pipe_index) {
            WINUSB_PIPE_INFORMATION pipe{};
            if (!WinUsb_QueryPipe(handle, static_cast<UCHAR>(alt_index),
                                  pipe_index, &pipe)) {
                if (error.empty()) {
                    error = "WinUsb_QueryPipe(interface " +
                            std::to_string(desc.bInterfaceNumber) +
                            " alt " + std::to_string(alt_index) + " pipe " +
                            std::to_string(pipe_index) + ") failed, " +
                            last_error_text();
                }
                break;
            }
            DescriptorEndpoint endpoint;
            endpoint.address = pipe.PipeId;
            endpoint.attributes = attributes_from_pipe_type(pipe.PipeType);
            endpoint.w_max_packet = pipe.MaximumPacketSize;
            endpoint.interval = pipe.Interval;
            alt.endpoints.push_back(endpoint);
        }
        iface->alt_settings.push_back(std::move(alt));
    }
}

} // namespace

bool descriptor_scan_supported() { return true; }

DescriptorScan scan_pakon_descriptors() {
    DescriptorScan report;
    report.supported = true;

    // 1 — locate the booted F135 (identity rules over the present list;
    // this step performs no I/O). Cold F235 units are intentionally out
    // of scope for this diagnostic.
    const std::vector<DeviceInfo> devices = enumerate_pakon();
    const DeviceInfo* target = nullptr;
    bool cold_present = false;
    bool other_present = false;
    for (const auto& device : devices) {
        if (device.is_cold()) {
            cold_present = true;
        } else if (device.product_id == kWarmPidF135) {
            if (!target) target = &device;
        } else {
            other_present = true;
        }
    }
    if (!target) {
        report.error =
            cold_present
                ? "no booted F135 present — the attached Pakon unit is "
                  "the cold 0F05:F235 bootstrap identity (this "
                  "diagnostic only scans the booted 0F05:F135 runtime)"
                : other_present
                      ? "a Pakon device is present but not the booted "
                        "0F05:F135 identity (out of scope for this "
                        "diagnostic)"
                      : "no Pakon device present";
        return report;
    }
    report.instance_id = target->instance_id;
    report.hardware_id = target->hardware_id;
    if (!target->has_device_interface()) {
        report.error =
            "F135 present but not openable: " +
            (target->interface_note.empty()
                 ? std::string("no device interface registered")
                 : target->interface_note);
        return report;
    }
    report.device_path = target->device_path;

    // 2 — open with the shared, hardware-proven WinUSB parameters.
    HANDLE file = open_device(report.device_path, report.open_mode,
                              report.error);
    if (file == INVALID_HANDLE_VALUE) {
        return report;
    }
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(file, &usb)) {
        report.error = "WinUsb_Initialize failed, " + last_error_text();
        CloseHandle(file);
        return report;
    }

    // 3 — GET_DESCRIPTOR(DEVICE): raw bytes + standard fields.
    {
        std::string stage_error;
        auto bytes = get_descriptor(usb, kDescriptorDevice, 0, 0, 18,
                                    stage_error);
        if (bytes.size() == 18) {
            report.device_descriptor = bytes;
            report.device_descriptor_read = true;
            report.bcd_usb = word_at(bytes, 2);
            report.device_class = byte_at(bytes, 4);
            report.device_sub_class = byte_at(bytes, 5);
            report.device_protocol = byte_at(bytes, 6);
            report.b_max_packet_size0 = byte_at(bytes, 7);
            report.id_vendor = word_at(bytes, 8);
            report.id_product = word_at(bytes, 10);
            report.bcd_device = word_at(bytes, 12);
            report.i_manufacturer = byte_at(bytes, 14);
            report.i_product = byte_at(bytes, 15);
            report.i_serial = byte_at(bytes, 16);
            report.b_num_configurations = byte_at(bytes, 17);
        } else {
            report.device_descriptor_error =
                stage_error.empty()
                    ? "short device descriptor (" +
                          std::to_string(bytes.size()) + " of 18 bytes)"
                    : stage_error;
        }
    }

    // 4 — GET_DESCRIPTOR(CONFIGURATION): 9-byte peek for wTotalLength,
    // then the full tree; parse it into the interface topology.
    {
        std::string stage_error;
        auto head = get_descriptor(usb, kDescriptorConfiguration, 0, 0, 9,
                                   stage_error);
        if (head.size() < 9) {
            report.config_descriptor_error =
                stage_error.empty()
                    ? "short configuration peek (" +
                          std::to_string(head.size()) + " of 9 bytes)"
                    : stage_error;
        } else {
            const std::uint16_t total = word_at(head, 2);
            if (total < 9 ||
                static_cast<std::size_t>(total) > kMaxConfigBytes) {
                report.config_descriptor_error =
                    "implausible wTotalLength " + std::to_string(total);
            } else {
                std::string full_error;
                auto full =
                    get_descriptor(usb, kDescriptorConfiguration, 0, 0,
                                   static_cast<ULONG>(total), full_error);
                if (full.size() < 9) {
                    report.config_descriptor_error =
                        full_error.empty()
                            ? "short configuration descriptor (" +
                                  std::to_string(full.size()) + " bytes)"
                            : full_error;
                } else {
                    report.config_descriptor = full;
                    report.config_descriptor_read = true;
                    parse_config_tree(full, report);
                }
            }
        }
    }

    // 4b — driver-side context (no wire traffic): link speed. (The WinUSB
    // API has no bConfigurationValue read — an active configuration is
    // implied by WinUsb_Initialize having succeeded above.)
    {
        ULONG length = static_cast<ULONG>(sizeof(UCHAR));
        UCHAR speed = 0;
        if (WinUsb_QueryDeviceInformation(usb, DEVICE_SPEED, &length,
                                          &speed)) {
            // WinUSB's DEVICE_SPEED encoding is NOT the USB_DEVICE_SPEED
            // enum: per Microsoft's WinUsb_QueryDeviceInformation docs,
            // 0x01 = low/full speed, 0x03 = high speed or above (the
            // lab unit returned 0x03 — matches its 512-byte bulk
            // endpoints and bcdUSB 0200).
            report.link_speed_known = true;
            report.link_speed = speed == 1 ? "low or full (code 0x01)"
                               : speed == 3 ? "high or above (code 0x03)"
                                            : "code 0x" + hex_byte(speed);
        }
    }

    // 6 — string descriptors referenced by what we read (deduplicated,
    // index 0 = language list never requested as a payload). Standard
    // GET_DESCRIPTOR(STRING) only; a failed index is reported, not
    // retried with other languages beyond one documented fallback.
    {
        std::vector<std::uint8_t> wanted;
        auto add = [&](std::uint8_t index) {
            if (index == 0) return;
            for (std::uint8_t seen : wanted) {
                if (seen == index) return;
            }
            wanted.push_back(index);
        };
        add(report.i_manufacturer);
        add(report.i_product);
        add(report.i_serial);
        add(report.i_configuration);
        for (const auto& iface : report.interfaces) {
            for (const auto& alt : iface.alt_settings) {
                add(alt.string_index);
            }
        }

        for (std::uint8_t index : wanted) {
            DescriptorString entry;
            entry.index = index;
            std::string stage_error;
            std::uint16_t language_used = 0x0409;
            auto bytes = get_descriptor(usb, kDescriptorString, index,
                                        language_used, 256, stage_error);
            if (bytes.size() < 2) {
                // one documented fallback: retry without a language ID
                stage_error.clear();
                language_used = 0;
                bytes = get_descriptor(usb, kDescriptorString, index,
                                       language_used, 256, stage_error);
            }
            if (bytes.size() >= 2 && bytes[1] == kDescriptorString) {
                entry.read = true;
                entry.language_id = language_used;
                entry.text =
                    utf16le_to_utf8(bytes.data() + 2, bytes.size() - 2);
            } else if (bytes.size() >= 2) {
                entry.error = "unexpected descriptor type 0x" +
                              hex_byte(bytes[1]) + " for string index " +
                              std::to_string(index);
            } else {
                entry.error = stage_error.empty()
                                  ? "no data for string index " +
                                        std::to_string(index)
                                  : stage_error;
            }
            report.strings.push_back(std::move(entry));
        }
    }

    // 7 — independent topology walk through WinUSB: primary interface
    // plus every associated interface, all alternate settings, all pipes.
    walk_interface(usb, report.winusb_interfaces, report.winusb_walk_error);
    for (UCHAR index = 0;; ++index) {
        WINUSB_INTERFACE_HANDLE associated = nullptr;
        if (!WinUsb_GetAssociatedInterface(usb, index, &associated)) {
            break; // ERROR_NO_MORE_ITEMS when there are no more
        }
        walk_interface(associated, report.winusb_interfaces,
                       report.winusb_walk_error);
        WinUsb_Free(associated);
    }

    WinUsb_Free(usb);
    CloseHandle(file);
    return report;
}

} // namespace pakon::usb

#endif // _WIN32
