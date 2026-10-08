#include "pakon/ppb/packet.hpp"

#include <format>

namespace pakon::ppb {

bool is_known_frame_type(std::uint8_t v) {
    switch (static_cast<FrameType>(v)) {
    case FrameType::read:
    case FrameType::write:
    case FrameType::read_status:
    case FrameType::cmd:
    case FrameType::ack:
        return true;
    }
    return false;
}

const char* to_string(Status status) {
    switch (status) {
    case Status::ok: return "ok";
    case Status::not_acknowledged: return "not_acknowledged";
    case Status::invalid_packet: return "invalid_packet";
    case Status::bad_checksum: return "bad_checksum";
    case Status::usb_error_1: return "usb_error";
    case Status::usb_error_2: return "usb_error";
    case Status::usb_error_3: return "usb_error";
    case Status::host_algorithm_error: return "host_algorithm_error";
    case Status::success_alt: return "success_alt";
    case Status::bus_error: return "bus_error";
    }
    return "unknown";
}

bool is_success(Status status) {
    return status == Status::ok || status == Status::success_alt;
}

bool is_read_success(const Reply& reply) {
    if (reply.type != FrameType::read) {
        return false;
    }
    // Valid READ flags: 0x08 ordinary, 0x88 = 0x08 | 0x80 event pending
    // (docs/PPB.md; observed 2657:1 in the capture corpus). The 0x80
    // bit is ignored for success and reported separately by
    // Reply::event_pending(); everything else — including 0x00 — is not
    // a documented READ flags byte and stays rejected.
    const auto flags = static_cast<std::uint8_t>(reply.status);
    return (flags & 0x7F) == 0x08;
}

Result<std::vector<std::uint8_t>> Frame::serialize() const {
    if (data.empty()) {
        return failure<std::vector<std::uint8_t>>(
            ErrorKind::ppb_invalid_frame,
            "frame data must contain at least the bus address byte");
    }
    if (data.size() > 255) {
        return failure<std::vector<std::uint8_t>>(
            ErrorKind::ppb_invalid_frame,
            "frame data exceeds 255 bytes (count field overflow)");
    }
    const auto type_byte = static_cast<std::uint8_t>(type);
    // SAFETY: type byte 0 wedges the FX2 bridge until power cycle
    // [CONFIRMED] (per-unit-data-and-safety.md). The enum cannot hold 0,
    // but guard anyway — this is the single most dangerous wire byte.
    if (type_byte == 0) {
        return failure<std::vector<std::uint8_t>>(
            ErrorKind::ppb_invalid_frame,
            "refusing to serialize frame with type byte 0 (wedges FX2 bridge)");
    }
    std::vector<std::uint8_t> out;
    out.reserve(2 + data.size());
    out.push_back(type_byte);
    out.push_back(static_cast<std::uint8_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

Result<Frame> Frame::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 3) {
        // 2-byte header + at least the address byte.
        return failure<Frame>(ErrorKind::ppb_invalid_frame,
                              "frame shorter than 3 bytes");
    }
    const auto type_byte = bytes[0];
    if (type_byte == 0) {
        return failure<Frame>(
            ErrorKind::ppb_invalid_frame,
            "frame with type byte 0 (would wedge the FX2 bridge)");
    }
    if (!is_known_frame_type(type_byte)) {
        return failure<Frame>(ErrorKind::ppb_invalid_frame,
                              "unknown frame type byte");
    }
    const auto count = bytes[1];
    if (count == 0) {
        return failure<Frame>(ErrorKind::ppb_invalid_frame,
                              "frame with empty data block (no bus address)");
    }
    if (bytes.size() != static_cast<std::size_t>(2) + count) {
        return failure<Frame>(
            ErrorKind::ppb_invalid_frame,
            std::format("frame length {} does not match count {} (expected {})",
                        bytes.size(), count, 2 + count));
    }
    Frame frame;
    frame.type = static_cast<FrameType>(type_byte);
    frame.data.assign(bytes.begin() + 2, bytes.end());
    return frame;
}

Result<Reply> parse_reply(std::span<const std::uint8_t> bytes,
                          FrameType request_type) {
    // "A reply too short to contain a status byte carries no status; it
    //  must not be read as success." [DOCUMENTED]
    if (bytes.size() < 4) {
        return failure<Reply>(
            ErrorKind::ppb_reply_too_short,
            std::format("reply of {} bytes carries no status byte", bytes.size()));
    }
    const auto type_byte = bytes[0];
    if (type_byte == 0) {
        return failure<Reply>(ErrorKind::ppb_invalid_frame,
                              "reply with type byte 0");
    }
    if (!is_known_frame_type(type_byte)) {
        return failure<Reply>(ErrorKind::ppb_invalid_frame,
                              "reply with unknown type byte");
    }

    // "Reply types mirror the request" [CONFIRMED on hardware, August 2026]:
    // READ replies come back 0x01, status polls 0x03, CMD/WRITE acks 0x07.
    const auto reply_type = static_cast<FrameType>(type_byte);
    const bool expected =
        (request_type == FrameType::read && reply_type == FrameType::read) ||
        (request_type == FrameType::read_status &&
         reply_type == FrameType::read_status) ||
        ((request_type == FrameType::cmd || request_type == FrameType::write) &&
         reply_type == FrameType::ack);
    if (!expected) {
        return failure<Reply>(
            ErrorKind::ppb_unexpected_reply,
            std::format("reply type 0x{:02x} does not mirror request type 0x{:02x}",
                        type_byte, static_cast<std::uint8_t>(request_type)));
    }

    const auto count = bytes[1];
    if (bytes.size() != static_cast<std::size_t>(2) + count) {
        return failure<Reply>(
            ErrorKind::ppb_invalid_frame,
            std::format("reply length {} does not match count {} (expected {})",
                        bytes.size(), count, 2 + count));
    }

    Reply reply;
    reply.type = reply_type;
    reply.count = count;
    reply.address = bytes[2];
    reply.status = static_cast<Status>(bytes[3]);

    if (reply_type == FrameType::read) {
        // READ reply data = [addr][status/flags][n payload bytes] →
        // count = 2 + n. Observed: count=2 (1-byte payload), count=32
        // (30-byte 0x90 sensor payload).
        if (count < 2) {
            return failure<Reply>(
                ErrorKind::ppb_reply_too_short,
                std::format("READ reply count {} too small for status byte", count));
        }
        reply.payload.assign(bytes.begin() + 4, bytes.end());
    }
    // ACK / READ_STATUS replies carry no payload beyond the status.
    return reply;
}

Frame make_cmd(std::uint8_t address, std::uint8_t reg) {
    // Verbatim examples:
    //   04 03 10 00 85   HostReset         (ppb-protocol.md)
    //   04 03 <picm> 00 00 motor probe    (ppb-protocol.md)
    //   04 03 44 00 00   ResetMotor probe (capture corpus)
    Frame f;
    f.type = FrameType::cmd;
    f.data = {address, 0x00, reg};
    return f;
}

Frame make_write(std::uint8_t address, std::uint8_t reg,
                 std::span<const std::uint8_t> payload) {
    // Verbatim example: 02 04 10 01 8f 00 (HostSetMode)
    //   data = [addr 10][count 01][reg 8f][payload 00]
    // "type is 4 when count == 0, else 2" [captures + pakon-tlx-macos].
    Frame f;
    if (payload.empty()) {
        f.type = FrameType::cmd;
        f.data = {address, 0x00, reg};
        return f;
    }
    f.type = FrameType::write;
    f.data.reserve(3 + payload.size());
    f.data.push_back(address);
    f.data.push_back(static_cast<std::uint8_t>(payload.size()));
    f.data.push_back(reg);
    f.data.insert(f.data.end(), payload.begin(), payload.end());
    return f;
}

Frame make_read(std::uint8_t address, std::uint8_t byte_count,
                std::uint8_t reg) {
    // Verbatim from the capture corpus:
    //   01 03 40 1e 90  READ PICL+ reg 0x90, 30 bytes
    //     → 01 20 40 08 <30 bytes>   (count = 2 + 30 = 0x20)
    //   01 03 40 01 83  READ PICL+ reg 0x83, 1 byte
    //     → 01 03 40 08 00
    Frame f;
    f.type = FrameType::read;
    f.data = {address, byte_count, reg};
    return f;
}

Frame make_read_status(std::uint8_t address) {
    // Verbatim, dx-barcode.md and the capture corpus:
    //   03 01 44 → 03 02 44 00   (controller poll, 4-byte reply)
    //   03 01 10 → 03 03 10 00 aa (HOST poll, 5-byte reply)
    Frame f;
    f.type = FrameType::read_status;
    f.data = {address};
    return f;
}

} // namespace pakon::ppb
