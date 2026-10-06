// USB identity recognition tests — no hardware, no Windows APIs.
//
// Vectors are grounded in evidence, not invented:
//   - "USB\VID_0F05&PID_F235&REV_::07" and the device instance
//     "USB\VID_0F05&PID_F235\6&1D7D6E45&0&4" were read from the actual
//     attached unit via Device Manager (Code 28, no service);
//   - warm identity "0f05:f135 & REV_0002" and serial "16402" come from
//     pakon-reference/docs/usb-identity-and-firmware.md and the
//     alibosworth/pakon-captures corpus (F-135+ serial 16402);
//   - the composite "&MI_" form is the standard Windows instance shape
//     for USB function children.
// The suite also pins the WinUSB INF package's DeviceInterfaceGUID to the
// C++ constant so the two can never drift apart silently.

#include "pakon/usb/identity.hpp"
#include "pakon/usb/transport.hpp"
#include "support/test_harness.hpp"

#include <fstream>
#include <sstream>
#include <string>

using pakon::usb::DeviceInfo;
using pakon::usb::PakonHardwareId;
using pakon::usb::classify_pakon_hardware_id;
using pakon::usb::device_info_from_pnp;
using pakon::usb::hardware_id_of;
using pakon::usb::is_usb_interface_id;
using pakon::usb::kColdPidF235;
using pakon::usb::kDeviceInterfaceGuid;
using pakon::usb::kVendorId;
using pakon::usb::kWarmPidF135;
using pakon::usb::parse_vid_pid;
using pakon::usb::serial_from_instance;

constexpr std::string_view kColdInstance = "USB\\VID_0F05&PID_F235\\6&1D7D6E45&0&4";
constexpr std::string_view kWarmInstance = "USB\\VID_0F05&PID_F135\\16402";

namespace {

std::optional<DeviceInfo> pnp_info(std::string_view hardware_id,
                                   std::string_view instance) {
    return device_info_from_pnp(hardware_id, instance);
}

} // namespace

PAKON_TEST(cold_f235_hardware_id_recognition) {
    // Exact hardware ID of the attached unit (Windows renders the non-BCD
    // bcdDevice nibbles of F235 personality AA07 as "::07").
    EXPECT(classify_pakon_hardware_id("USB\\VID_0F05&PID_F235&REV_::07") ==
           PakonHardwareId::cold_f235);
    // Base hardware ID, and the lowercased form seen in device paths.
    EXPECT(classify_pakon_hardware_id("USB\\VID_0F05&PID_F235") ==
           PakonHardwareId::cold_f235);
    EXPECT(classify_pakon_hardware_id("usb\\vid_0f05&pid_f235&rev_::07") ==
           PakonHardwareId::cold_f235);

    const auto vid_pid = parse_vid_pid("USB\\VID_0F05&PID_F235&REV_::07");
    EXPECT(vid_pid.has_value());
    if (vid_pid) {
        EXPECT_EQ(vid_pid->first, kVendorId);
        EXPECT_EQ(vid_pid->second, kColdPidF235);
    }

    const auto info = pnp_info("USB\\VID_0F05&PID_F235&REV_::07", kColdInstance);
    EXPECT(info.has_value());
    if (info) {
        EXPECT_EQ(info->vendor_id, kVendorId);
        EXPECT_EQ(info->product_id, kColdPidF235);
        EXPECT(info->is_pakon());
        EXPECT(info->is_cold());
        EXPECT_EQ(info->hardware_id, std::string("USB\\VID_0F05&PID_F235&REV_::07"));
        EXPECT_EQ(info->instance_id, std::string(kColdInstance));
    }
}

PAKON_TEST(warm_f135_hardware_id_recognition) {
    EXPECT(classify_pakon_hardware_id("USB\\VID_0F05&PID_F135&REV_0002") ==
           PakonHardwareId::warm_f135);
    EXPECT(classify_pakon_hardware_id("USB\\VID_0F05&PID_F135") ==
           PakonHardwareId::warm_f135);
    EXPECT(classify_pakon_hardware_id("usb\\vid_0f05&pid_f135&rev_0002") ==
           PakonHardwareId::warm_f135);

    const auto info = pnp_info("USB\\VID_0F05&PID_F135&REV_0002", kWarmInstance);
    EXPECT(info.has_value());
    if (info) {
        EXPECT_EQ(info->vendor_id, kVendorId);
        EXPECT_EQ(info->product_id, kWarmPidF135);
        EXPECT(info->is_pakon());
        EXPECT(!info->is_cold());
        // Descriptor serial survives the instance parse.
        EXPECT(info->serial_number.has_value());
        if (info->serial_number) {
            EXPECT_EQ(*info->serial_number, std::string("16402"));
        }
    }
}

PAKON_TEST(unrelated_vid_pid_rejected) {
    EXPECT(classify_pakon_hardware_id("USB\\VID_046D&PID_C52B") ==
           PakonHardwareId::none); // unrelated peripheral
    EXPECT(classify_pakon_hardware_id("USB\\VID_1234&PID_5678") ==
           PakonHardwareId::none);
    EXPECT(classify_pakon_hardware_id("PCI\\VEN_8086&DEV_29C4") ==
           PakonHardwareId::none); // not USB at all
    EXPECT(classify_pakon_hardware_id("USB\\VID_0F05&PID_35F2") ==
           PakonHardwareId::none); // Pakon vendor, but out-of-scope identity
    EXPECT(classify_pakon_hardware_id("USB\\VID_0F05&PID_F1354") ==
           PakonHardwareId::none); // prefix boundary: PID_F1354 ≠ PID_F135
    EXPECT(classify_pakon_hardware_id("") == PakonHardwareId::none);
    EXPECT(!device_info_from_pnp("USB\\VID_046D&PID_C52B", "USB\\VID_046D&PID_C52B\\x")
               .has_value());
    EXPECT(!device_info_from_pnp("USB\\VID_0F05&PID_35F2", "USB\\VID_0F05&PID_35F2\\x")
               .has_value());
}

PAKON_TEST(composite_interface_ids_rejected) {
    EXPECT(is_usb_interface_id("USB\\VID_0F05&PID_F135&MI_00"));
    EXPECT(is_usb_interface_id("usb\\vid_0f05&pid_f135&mi_01")); // case-insensitive
    EXPECT(!is_usb_interface_id("USB\\VID_0F05&PID_F135"));
    EXPECT(!is_usb_interface_id(kColdInstance));

    // A composite function child is not a device: discovery must not list
    // one scanner once per interface.
    EXPECT(classify_pakon_hardware_id("USB\\VID_0F05&PID_F135&MI_00") ==
           PakonHardwareId::none);
    EXPECT(classify_pakon_hardware_id("USB\\VID_0F05&PID_F235&MI_01") ==
           PakonHardwareId::none);
    EXPECT(!device_info_from_pnp("USB\\VID_0F05&PID_F135&MI_00",
                                 "USB\\VID_0F05&PID_F135&MI_00\\6&1D7D6E45&0&0002")
               .has_value());
}

PAKON_TEST(pnp_instance_location_is_not_serial) {
    // Location-assigned device: the instance segment after the backslash
    // is a PnP relationship path, never a descriptor serial.
    EXPECT(!serial_from_instance(kColdInstance).has_value());
    EXPECT(!serial_from_instance("USB\\VID_0F05&PID_F135\\6&1D7D6E45&0&0002")
               .has_value());
    EXPECT(!serial_from_instance("USB\\VID_0F05&PID_F135").has_value());

    // Descriptor serials (no '&') are kept as-is.
    const auto serial = serial_from_instance(kWarmInstance);
    EXPECT(serial.has_value());
    if (serial) {
        EXPECT_EQ(*serial, std::string("16402"));
    }

    // The hardware-ID segment is separate from the location segment.
    EXPECT_EQ(std::string(hardware_id_of(kColdInstance)),
              std::string("USB\\VID_0F05&PID_F235"));

    const auto info = pnp_info(std::string(hardware_id_of(kColdInstance)), kColdInstance);
    EXPECT(info.has_value());
    if (info) {
        EXPECT(!info->serial_number.has_value()); // no fake serial invented
        EXPECT_EQ(info->instance_id, std::string(kColdInstance));
    }
}

PAKON_TEST(cold_device_discovered_but_not_openable) {
    // PnP-only discovery representation of the attached Code 28 unit:
    // discovered — full identity, instance, hardware ID …
    const auto info = pnp_info("USB\\VID_0F05&PID_F235&REV_::07", kColdInstance);
    EXPECT(info.has_value());
    if (!info) {
        return;
    }
    EXPECT(info->is_pakon());
    EXPECT(info->is_cold());
    EXPECT_EQ(info->instance_id, std::string(kColdInstance));
    EXPECT(!info->hardware_id.empty());

    // … but explicitly NOT openable: no function driver registered an
    // interface, so there is no device path to CreateFile.
    EXPECT(info->device_path.empty());
    EXPECT(!info->has_device_interface());
    EXPECT(!info->serial_number.has_value());
    EXPECT(info->interfaces.empty());

    // Contrast: a warm device whose WinUSB interface exists is openable.
    DeviceInfo warm;
    warm.vendor_id = kVendorId;
    warm.product_id = kWarmPidF135;
    warm.instance_id = std::string(kWarmInstance);
    warm.device_path = "\\\\?\\usb#vid_0f05&pid_f135&rev_0002#16402#{guid}";
    EXPECT(warm.is_pakon());
    EXPECT(!warm.is_cold());
    EXPECT(warm.has_device_interface());
}

PAKON_TEST(device_interface_guid_matches_inf_package) {
    // The C++ constant is the documented shared name for the GUID the
    // WinUSB INF registers; both sides must agree, always.
    EXPECT_EQ(std::string(kDeviceInterfaceGuid),
              std::string("{0e9e6f29-e70a-4582-8d02-bde3ad701252}"));

    std::ifstream inf(std::string(PAKON_SOURCE_DIR) + "/driver/PakonWinUSB.inf");
    EXPECT(inf.good());
    if (!inf) {
        return;
    }
    std::stringstream contents;
    contents << inf.rdbuf();
    const std::string text = contents.str();

    EXPECT(text.find(std::string(kDeviceInterfaceGuid)) != std::string::npos);
    // INF hardware IDs: cold (all revisions, device level) and warm
    // (post-load identity).
    EXPECT(text.find("USB\\VID_0F05&PID_F235") != std::string::npos);
    EXPECT(text.find("USB\\VID_0F05&PID_F135&REV_0002") != std::string::npos);
    // In-box WinUSB: the INF must install Microsoft's driver, not ours.
    EXPECT(text.find("winusb.inf") != std::string::npos);
    EXPECT(text.find("WINUSB.NT") != std::string::npos);
}

int main() {
    return pakon::test::run_all();
}
