// PPB packet serialization/parsing tests.
//
// Vectors are verbatim from two sources:
//  1. frames quoted in pakon-reference/docs/ppb-protocol.md and
//     docs/dx-barcode.md;
//  2. request/reply pairs from alibosworth/pakon-captures,
//     captures/f135plus-serial16402-4expneg-20260820 (OEM software
//     driving a real F-135+).
// Nothing here is fabricated.

#include "pakon/ppb/packet.hpp"
#include "pakon/protocol/addresses.hpp"
#include "pakon/protocol/commands.hpp"
#include "support/test_harness.hpp"

#include <array>

using pakon::ErrorKind;
using pakon::ppb::Frame;
using pakon::ppb::FrameType;
using pakon::ppb::Status;

namespace {

std::vector<std::uint8_t> hex(std::string_view s) {
    std::vector<std::uint8_t> out;
    auto n = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
        return 0;
    };
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>((n(s[i]) << 4) | n(s[i + 1])));
    }
    return out;
}

std::string hex_of(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    for (auto b : bytes) {
        out += std::format("{:02x}", b);
    }
    return out;
}

} // namespace

// --- Builders reproduce documented/captured frames exactly -----------------

PAKON_TEST(build_host_reset_matches_reference) {
    // pakon-reference/docs/ppb-protocol.md: "host → 04 03 10 00 85"
    const auto frame = pakon::ppb::make_cmd(
        pakon::protocol::kAddrHost,
        pakon::protocol::to_byte(pakon::protocol::HostCommand::host_reset));
    const auto wire = frame.serialize();
    EXPECT(wire.has_value());
    EXPECT_EQ(hex_of(*wire), std::string("0403100085"));
}

PAKON_TEST(build_host_set_mode_matches_reference) {
    // pakon-reference/docs/ppb-protocol.md: "host → 02 04 10 01 8f 00"
    const std::array<std::uint8_t, 1> payload{0x00};
    const auto frame = pakon::ppb::make_write(
        pakon::protocol::kAddrHost,
        pakon::protocol::to_byte(pakon::protocol::HostCommand::host_set_mode),
        payload);
    const auto wire = frame.serialize();
    EXPECT(wire.has_value());
    EXPECT_EQ(hex_of(*wire), std::string("020410018f00"));
}

PAKON_TEST(build_motor_probe_matches_reference) {
    // pakon-reference/docs/ppb-protocol.md: "host → 04 03 <picm> 00 00"
    // capture corpus: "0403440000" (Plus motor probe)
    const auto frame = pakon::ppb::make_cmd(pakon::protocol::kAddrPicmPlus, 0x00);
    const auto wire = frame.serialize();
    EXPECT(wire.has_value());
    EXPECT_EQ(hex_of(*wire), std::string("0403440000"));
}

PAKON_TEST(build_sensor_read_matches_capture) {
    // capture corpus: "0103401e90" READ PICL+ reg 0x90, 30 bytes
    const auto frame = pakon::ppb::make_read(pakon::protocol::kAddrPiclPlus, 30, 0x90);
    EXPECT_EQ(hex_of(frame.serialize().value()), std::string("0103401e90"));
}

PAKON_TEST(build_ccd_status_read_matches_capture) {
    // capture corpus: "0103400183" READ PICL+ reg 0x83, 1 byte
    const auto frame = pakon::ppb::make_read(pakon::protocol::kAddrPiclPlus, 1, 0x83);
    EXPECT_EQ(hex_of(frame.serialize().value()), std::string("0103400183"));
}

PAKON_TEST(build_status_poll_matches_reference) {
    // pakon-reference/docs/dx-barcode.md: "A `03 01 <addr>` status poll"
    const auto frame = pakon::ppb::make_read_status(pakon::protocol::kAddrPicmPlus);
    EXPECT_EQ(hex_of(frame.serialize().value()), std::string("030144"));
}

PAKON_TEST(build_write_with_payload_layout) {
    // capture corpus: "020740048fe8ff1800" — SetLightConfig, count=4.
    // data = [addr 40][count 04][reg 8f][payload e8 ff 18 00], len = 7.
    const std::array<std::uint8_t, 4> payload{0xe8, 0xff, 0x18, 0x00};
    const auto frame = pakon::ppb::make_write(pakon::protocol::kAddrPiclPlus, 0x8F,
                                              payload);
    EXPECT_EQ(hex_of(frame.serialize().value()), std::string("020740048fe8ff1800"));
}

PAKON_TEST(build_write_with_empty_payload_degrades_to_cmd) {
    // "type is 4 when count == 0, else 2" [captures + pakon-tlx-macos]
    const auto frame = pakon::ppb::make_write(pakon::protocol::kAddrHost, 0x85, {});
    EXPECT(frame.type == FrameType::cmd);
    EXPECT_EQ(hex_of(frame.serialize().value()), std::string("0403100085"));
}

// --- Reply parsing against captured replies --------------------------------

PAKON_TEST(parse_ack_reply) {
    // capture: "07021000" (HostReset ack, status ok)
    const auto reply = pakon::ppb::parse_reply(hex("07021000"), FrameType::cmd);
    EXPECT(reply.has_value());
    EXPECT(reply->address == 0x10);
    EXPECT(reply->status == Status::ok);
    EXPECT(!reply->event_pending());
}

PAKON_TEST(parse_controller_poll_reply) {
    // capture: "03024400"
    const auto reply = pakon::ppb::parse_reply(hex("03024400"), FrameType::read_status);
    EXPECT(reply.has_value());
    EXPECT(reply->address == 0x44);
    EXPECT(reply->status == Status::ok);
    EXPECT(reply->payload.empty());
}

PAKON_TEST(parse_host_poll_reply_with_trailing_byte) {
    // capture (8037x): "03031000aa" — HOST poll replies carry count=3
    // with a constant trailing 0xaa; pakon-reference documents only the
    // 4-byte controller form, the 5-byte HOST form is capture evidence.
    const auto reply = pakon::ppb::parse_reply(hex("03031000aa"),
                                               FrameType::read_status);
    EXPECT(reply.has_value());
    EXPECT(reply->address == 0x10);
    EXPECT(reply->status == Status::ok);
}

PAKON_TEST(parse_host_poll_event_reply) {
    // capture (1x): "03031080aa" — status 0x80 = host event pending
    const auto reply = pakon::ppb::parse_reply(hex("03031080aa"),
                                               FrameType::read_status);
    EXPECT(reply.has_value());
    EXPECT(reply->status == static_cast<Status>(0x80));
}

PAKON_TEST(parse_read_reply_1_byte) {
    // capture: "0103400800" — READ reg 0x83, flags 0x08, payload 00
    const auto reply = pakon::ppb::parse_reply(hex("0103400800"), FrameType::read);
    EXPECT(reply.has_value());
    EXPECT(reply->address == 0x40);
    EXPECT(reply->status == Status::success_alt); // 0x08 ordinary flags byte
    EXPECT_EQ(reply->payload.size(), std::size_t{1});
    EXPECT(!reply->event_pending());
}

PAKON_TEST(parse_read_reply_30_byte_sensor) {
    // capture: "01204008863d01000000..." — reg 0x90, count = 0x20 = 2 + 30
    const auto reply = pakon::ppb::parse_reply(
        hex("01204008863d01000000000000000000000000000000000000000000000000000000"),
        FrameType::read);
    EXPECT(reply.has_value());
    EXPECT_EQ(reply->count, std::uint8_t{0x20});
    EXPECT_EQ(reply->payload.size(), std::size_t{30});
    // payload[0..1] = 0x863d big-endian position counter (dx-barcode.md)
    EXPECT_EQ(reply->payload[0], std::uint8_t{0x86});
    EXPECT_EQ(reply->payload[1], std::uint8_t{0x3d});
    EXPECT_EQ(reply->payload[2], std::uint8_t{0x01}); // entry count
}

PAKON_TEST(parse_read_reply_event_flag) {
    // flags 0x88 = 0x08 | 0x80 event pending
    // [CONFIRMED on hardware, August 2026]; observed once in captures
    const auto reply = pakon::ppb::parse_reply(hex("0103408800"), FrameType::read);
    EXPECT(reply.has_value());
    EXPECT(reply->event_pending());
}

// --- READ flags success predicate (is_read_success) -------------------------
//
// A READ reply's status byte is a flags byte: 0x08 ordinary, 0x88
// (= 0x08 | 0x80) successful READ with an event pending (docs/PPB.md;
// 2657:1 in the capture corpus). Ordinary is_success() keeps its exact
// pre-existing meaning for ACK/poll replies.

PAKON_TEST(read_flags_0x08_accepted) {
    // Ordinary READ flags byte 0x08 — the documented success value.
    const auto reply = pakon::ppb::parse_reply(hex("0103400800"), FrameType::read);
    EXPECT(reply.has_value());
    EXPECT(pakon::ppb::is_read_success(*reply));
    EXPECT(!reply->event_pending());
}

PAKON_TEST(read_flags_0x88_accepted) {
    // 0x88 = 0x08 | 0x80: successful READ + event pending. Vector is
    // the module-info reply the warm lab unit sent on 2026-10-08
    // (identify run that previously failed ppb_bad_status):
    //   01 0e 40 88 04 20 40 12 04 c0 21 02 00 00 92 00
    const auto reply = pakon::ppb::parse_reply(
        hex("010e40880420401204c0210200009200"), FrameType::read);
    EXPECT(reply.has_value());
    EXPECT(pakon::ppb::is_read_success(*reply));
    EXPECT_EQ(reply->payload.size(), std::size_t{12});
    // The ordinary predicate still refuses 0x88 — nothing global changed.
    EXPECT(!pakon::ppb::is_success(reply->status));
}

PAKON_TEST(read_flags_0x88_reports_event_pending) {
    // Acceptance must not swallow the event bit.
    const auto reply = pakon::ppb::parse_reply(hex("0103408800"), FrameType::read);
    EXPECT(reply.has_value());
    EXPECT(reply->event_pending());
    EXPECT(pakon::ppb::is_read_success(*reply));
}

PAKON_TEST(read_invalid_flags_rejected) {
    // Not documented READ flags bytes: 0x09 bus error, 0x89 bus error
    // with event bit, 0x00 (ordinary ACK status — never observed as a
    // READ flags byte). All parse fine; the predicate must refuse them.
    for (const char* wire : {"0103400900", "0103408900", "0103400000"}) {
        const auto reply = pakon::ppb::parse_reply(hex(wire), FrameType::read);
        EXPECT(reply.has_value());
        EXPECT(!pakon::ppb::is_read_success(*reply));
    }
}

PAKON_TEST(ordinary_status_behavior_unchanged) {
    // Ordinary status semantics, byte for byte as before this change.
    EXPECT(pakon::ppb::is_success(Status::ok));                  // 0x00
    EXPECT(pakon::ppb::is_success(Status::success_alt));         // 0x08
    EXPECT(!pakon::ppb::is_success(Status::not_acknowledged));   // 0x01
    EXPECT(!pakon::ppb::is_success(Status::bus_error));          // 0x09
    EXPECT(!pakon::ppb::is_success(static_cast<Status>(0x88)));  // still invalid
    // is_read_success() is READ-specific: it declines ACK and poll
    // replies even when their status is a success.
    const auto ack = pakon::ppb::parse_reply(hex("07021008"), FrameType::cmd);
    EXPECT(ack.has_value());
    EXPECT(pakon::ppb::is_success(ack->status));
    EXPECT(!pakon::ppb::is_read_success(*ack));
    const auto poll = pakon::ppb::parse_reply(hex("03031000aa"),
                                               FrameType::read_status);
    EXPECT(poll.has_value());
    EXPECT(pakon::ppb::is_success(poll->status));
    EXPECT(!pakon::ppb::is_read_success(*poll));
}

// --- Rejections (safety and robustness) ------------------------------------

PAKON_TEST(reject_type_zero_frame) {
    // Type byte 0 wedges the FX2 bridge until power cycle [CONFIRMED].
    const auto frame = Frame::parse(hex("00021000"));
    EXPECT(!frame.has_value());
    EXPECT(frame.error().kind == ErrorKind::ppb_invalid_frame);
}

PAKON_TEST(reject_length_count_mismatch) {
    const auto frame = Frame::parse(hex("0405100085")); // count 5, only 3 data bytes
    EXPECT(!frame.has_value());
    EXPECT(frame.error().kind == ErrorKind::ppb_invalid_frame);
}

PAKON_TEST(reject_short_reply) {
    // "A reply too short to contain a status byte carries no status;
    //  it must not be read as success."
    const auto reply = pakon::ppb::parse_reply(hex("070210"), FrameType::cmd);
    EXPECT(!reply.has_value());
    EXPECT(reply.error().kind == ErrorKind::ppb_reply_too_short);
}

PAKON_TEST(reject_wrong_reply_type) {
    // WRITE/CMD must be answered by 0x07 acks, not by READ replies.
    const auto reply = pakon::ppb::parse_reply(hex("0103400800"), FrameType::write);
    EXPECT(!reply.has_value());
    EXPECT(reply.error().kind == ErrorKind::ppb_unexpected_reply);
}

PAKON_TEST(serialize_refuses_oversize_frame) {
    Frame frame;
    frame.type = FrameType::write;
    frame.data.assign(300, 0x00);
    const auto wire = frame.serialize();
    EXPECT(!wire.has_value());
    EXPECT(wire.error().kind == ErrorKind::ppb_invalid_frame);
}

// --- Field layout cross-checks ---------------------------------------------

PAKON_TEST(read_request_is_always_three_data_bytes) {
    // Every READ in the capture corpus is [01][03][addr][count][reg].
    // Rows are std::array (not std::tuple): passing int literals through
    // tuple's converting constructor triggers MSVC C4244, while aggregate
    // initialization of constants that fit uint8_t does not.
    for (const auto [address, count, reg] : {
             std::array<std::uint8_t, 3>{0x40, 1, 0x02},
             std::array<std::uint8_t, 3>{0x40, 2, 0x84},
             std::array<std::uint8_t, 3>{0x40, 4, 0x88},
             std::array<std::uint8_t, 3>{0x40, 12, 0x07},
             std::array<std::uint8_t, 3>{0x40, 30, 0x90},
             std::array<std::uint8_t, 3>{0x44, 1, 0x02},
             std::array<std::uint8_t, 3>{0x10, 2, 0x03},
         }) {
        const auto frame = pakon::ppb::make_read(address, count, reg);
        EXPECT_EQ(frame.data.size(), std::size_t{3});
        EXPECT_EQ(frame.wire_length(), std::size_t{5});
    }
}

PAKON_TEST(write_length_invariant) {
    // wire_length == 2 + count for builders [CONFIRMED across all
    // 198,425 captured frames].
    const std::array<std::uint8_t, 3> payload{1, 2, 3};
    const auto frame = pakon::ppb::make_write(0x44, 0x82, payload);
    const auto wire = frame.serialize().value();
    EXPECT_EQ(wire.size(), 2u + frame.data.size());
    EXPECT_EQ(wire[1], frame.data.size());
}

int main() {
    return pakon::test::run_all();
}
