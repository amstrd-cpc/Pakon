// Bootstrap probe tests — no hardware, no Windows APIs.
//
// Two jobs:
//   1. Pin the stage-1 personality read to the documented request
//      (0xA9 / wValue 0 / wIndex 0 / 8 bytes) so it cannot drift
//      silently, and pin the safety rule that no forbidden vendor
//      request (0xA2 EEPROM write, 0xA4 EEPROM select) is ever used.
//   2. Prove with a recording fake transport that probe() performs
//      EXACTLY one control read and no bulk traffic and no control
//      writes — the structural guarantee that the probe is read-only
//      and exposes no PPB type-byte-0 risk.
//
// The scripted reply bytes are arbitrary test data: the C0 record's byte
// layout is not documented in-repo, so the tests never decode it.

#include "pakon/bootstrap/probe.hpp"
#include "pakon/usb/transport.hpp"
#include "support/test_harness.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using pakon::ErrorKind;
using pakon::Result;
using pakon::bootstrap::hex_string;
using pakon::bootstrap::kPersonalityIndex;
using pakon::bootstrap::kPersonalityLength;
using pakon::bootstrap::kPersonalityRequest;
using pakon::bootstrap::kPersonalityValue;
using pakon::bootstrap::parse_personality;
using pakon::bootstrap::probe;
using pakon::usb::DeviceInfo;
using pakon::usb::IUsbTransport;
using pakon::usb::kColdPidF235;
using pakon::usb::kVendorId;

namespace {

constexpr std::array<std::uint8_t, 8> kScriptedPersonality{
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

// Recording transport: scripts the control-read answer, counts every
// call, and flags any I/O a read-only probe must never perform.
class FakeTransport final : public IUsbTransport {
public:
    // Scripted behaviour.
    std::vector<std::uint8_t> reply{kScriptedPersonality.begin(),
                                    kScriptedPersonality.end()};
    bool fail_control_read = false;
    ErrorKind fail_kind = ErrorKind::usb_timeout;

    // Call accounting.
    int control_reads = 0;
    int forbidden_calls = 0;  // bulk or control-write attempts
    std::uint8_t last_request{};
    std::uint16_t last_value{};
    std::uint16_t last_index{};
    std::uint16_t last_length{};
    DeviceInfo info{};

    Result<std::vector<std::uint8_t>>
    command_exchange(std::span<const std::uint8_t>) override {
        ++forbidden_calls;
        return failure<std::vector<std::uint8_t>>(
            ErrorKind::usb_not_supported, "bulk exchange must not be probed");
    }

    Result<std::vector<std::uint8_t>>
    bulk_read(std::uint8_t, std::size_t) override {
        ++forbidden_calls;
        return failure<std::vector<std::uint8_t>>(
            ErrorKind::usb_not_supported, "bulk read must not be probed");
    }

    Result<std::vector<std::uint8_t>>
    control_read(std::uint8_t request, std::uint16_t value,
                 std::uint16_t index, std::uint16_t length) override {
        ++control_reads;
        last_request = request;
        last_value = value;
        last_index = index;
        last_length = length;
        if (fail_control_read) {
            return failure<std::vector<std::uint8_t>>(
                fail_kind, "scripted control-read failure");
        }
        return reply;
    }

    Result<void> control_write(std::uint8_t, std::uint16_t,
                               std::uint16_t) override {
        ++forbidden_calls;
        return void_failure(ErrorKind::usb_not_supported,
                            "control writes must not be probed");
    }

    const DeviceInfo& device_info() const override { return info; }
};

DeviceInfo cold_device() {
    DeviceInfo info;
    info.vendor_id = kVendorId;
    info.product_id = kColdPidF235;
    info.hardware_id = "USB\\VID_0F05&PID_F235&REV_::07";
    info.instance_id = "USB\\VID_0F05&PID_F235\\6&1D7D6E45&0&4";
    return info;
}

} // namespace

PAKON_TEST(personality_read_matches_documented_stage1_request) {
    // usb-identity-and-firmware.md via PAKON_REFERENCE.md § 2.1:
    // "read via vendor request 0xA9 with wIndex 0 from the stage-1
    //  loader [CONFIRMED on hardware, August 2026]" — 8-byte C0 record.
    // wValue 0 = offset 0 applies 0xA9's documented wValue semantics
    // (calibration.md § The read); see docs/BOOTSTRAP.md.
    EXPECT_EQ(static_cast<unsigned>(kPersonalityRequest), 0xA9u);
    EXPECT_EQ(static_cast<int>(kPersonalityValue), 0);
    EXPECT_EQ(static_cast<int>(kPersonalityIndex), 0);
    EXPECT_EQ(static_cast<int>(kPersonalityLength), 8);
}

PAKON_TEST(personality_read_never_targets_forbidden_requests) {
    // Safety pin (per-unit-data-and-safety.md): vendor 0xA2 is the EEPROM
    // write and 0xA4 the EEPROM select — neither may ever appear in the
    // probe path. The probe's only request is the 0xA9 read.
    EXPECT(kPersonalityRequest != 0xA2);
    EXPECT(kPersonalityRequest != 0xA4);
    EXPECT(kPersonalityRequest == 0xA9);
}

PAKON_TEST(probe_issues_exactly_one_control_read_and_nothing_else) {
    FakeTransport transport;
    transport.info = cold_device();

    const auto report = probe(transport);
    EXPECT(report.has_value());
    EXPECT_EQ(transport.control_reads, 1);
    EXPECT_EQ(transport.forbidden_calls, 0);
    EXPECT_EQ(static_cast<unsigned>(transport.last_request), 0xA9u);
    EXPECT_EQ(static_cast<int>(transport.last_value), 0);
    EXPECT_EQ(static_cast<int>(transport.last_index), 0);
    EXPECT_EQ(static_cast<int>(transport.last_length), 8);
    if (report) {
        EXPECT(report->device.is_cold());
        EXPECT(report->personality.bytes == kScriptedPersonality);
    }
}

PAKON_TEST(probe_rejects_wrong_length_reads) {
    // A short (or impossible oversized) transfer must never be accepted
    // as a personality — ErrorKind::usb_short_transfer, not success.
    const std::size_t bad_lengths[] = {0, 7, 9};
    for (const std::size_t length : bad_lengths) {
        FakeTransport transport;
        transport.reply.assign(length, 0x5a);
        const auto report = probe(transport);
        EXPECT(!report.has_value());
        if (!report) {
            EXPECT(report.error().kind == ErrorKind::usb_short_transfer);
            EXPECT(!report.error().message.empty());
        }
        EXPECT_EQ(transport.forbidden_calls, 0);
    }
}

PAKON_TEST(probe_propagates_control_read_failure) {
    // A stalled/failed read surfaces its original error unchanged; the
    // probe never retries with a different (unproven) request.
    FakeTransport transport;
    transport.fail_control_read = true;
    transport.fail_kind = ErrorKind::usb_timeout;

    const auto report = probe(transport);
    EXPECT(!report.has_value());
    if (!report) {
        EXPECT(report.error().kind == ErrorKind::usb_timeout);
    }
    EXPECT_EQ(transport.control_reads, 1);
    EXPECT_EQ(transport.forbidden_calls, 0);
}

PAKON_TEST(parse_personality_accepts_exactly_eight_bytes) {
    const auto ok = parse_personality(kScriptedPersonality);
    EXPECT(ok.has_value());
    if (ok) {
        EXPECT(ok->bytes == kScriptedPersonality);
    }

    const std::array<std::uint8_t, 7> short_read{};
    const auto short_result = parse_personality(short_read);
    EXPECT(!short_result.has_value());
    if (!short_result) {
        EXPECT(short_result.error().kind == ErrorKind::usb_short_transfer);
    }
}

PAKON_TEST(hex_string_formats_bytes) {
    const std::vector<std::uint8_t> bytes{0x00, 0x0f, 0xa5, 0xff};
    EXPECT(hex_string(bytes) == std::string("00 0f a5 ff"));
    const std::vector<std::uint8_t> empty;
    EXPECT(hex_string(empty) == std::string(""));
}

int main() { return pakon::test::run_all(); }
