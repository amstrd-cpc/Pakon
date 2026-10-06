#pragma once

// PPB frame serialization/parsing.
//
// Primary spec: pakon-reference/docs/ppb-protocol.md § "Frame format",
// § "Type byte values", § "Reply frames and the status byte", § "Reply
// structure, recovered by driving the scanner".
//
// Field layout below (data[1] = payload count, data[2] = register) was
// cross-checked against the OEM capture corpus
// (github.com/alibosworth/pakon-captures, F-135+ serial 16402): all
// 198,425 frames in the six session files satisfy wire_length == 2 +
// count, and every request form matches. pakon-reference states the
// coarser rule ("data[0] is the destination bus address, and for
// command/write frames data[2] is the command byte") and leaves data[1]
// unnamed; the captures (and pakon-tlx-macos/docs/PROTOCOL.md, which
// pakon-reference cites) show data[1] is the payload/byte count and that
// type is 0x04 exactly when that count is 0, else 0x02.
//
// Frame on the wire:
//
//     [type:1][count:1][data:count]        wire length = 2 + count
//
//   WRITE / CMD:  data = [addr][payload_count][reg][payload…]
//   READ:         data = [addr][byte_count][reg]      (count field = 3)
//   READ_STATUS:  data = [addr]                       (count field = 1)
//
// There is NO checksum and NO padding; the protocol relies on USB's own
// integrity. [CONFIRMED] "the frame length always equals 2 + count".
//
// SAFETY: a frame whose type byte is 0 is not accepted by the FX2 bridge
// and leaves it unable to drain its command endpoint until a power cycle
// (per-unit-data-and-safety.md [CONFIRMED]). serialize() refuses to
// produce such frames; parse() rejects them from the wire.

#include <cstdint>
#include <span>
#include <vector>

#include "pakon/errors/error.hpp"

namespace pakon::ppb {

// Frame type values. [DOCUMENTED] 0x04 on host commands and 0x07 on their
// acknowledgements are observed directly in the open-handshake captures;
// WRITE/READ/READ_STATUS values are read from the OEM driver's IOCTL
// encoding.
enum class FrameType : std::uint8_t {
    read = 0x01,        // host reads N bytes back from a device
    write = 0x02,       // host writes N bytes to a device register/buffer
    read_status = 0x03, // one-byte status poll (no command byte)
    cmd = 0x04,         // a command with no data payload (write with count == 0)
    ack = 0x07,         // device→host acknowledgement reply (to CMD and WRITE)
};

// True if v is one of the five known frame types.
bool is_known_frame_type(std::uint8_t v);

// Reply status byte. [DOCUMENTED] from the OEM driver's status enumeration.
enum class Status : std::uint8_t {
    ok = 0x00,                 // success / acknowledged
    not_acknowledged = 0x01,   // device absent or command rejected
    invalid_packet = 0x02,     // invalid packet (harmless; payload rejected)
    bad_checksum = 0x03,       // bad checksum
    usb_error_1 = 0x04,        // USB-layer error
    usb_error_2 = 0x05,        // USB-layer error
    usb_error_3 = 0x06,        // USB-layer error
    host_algorithm_error = 0x07,
    success_alt = 0x08,        // "also reported as success in some sequences";
                               // also the ordinary READ-reply flags byte
    bus_error = 0x09,
};

// Human-readable status name for logs.
const char* to_string(Status status);

// Statuses that may be treated as success. Note 0x08: for ACK/POLL replies
// the reference says it is "also reported as success in some sequences";
// for READ replies it is the ordinary value of the status/flags byte (see
// Reply below).
bool is_success(Status status);

// A host→device frame. Owns its bytes; serialization is explicit.
struct Frame {
    FrameType type{};
    std::vector<std::uint8_t> data; // data[0] = bus address

    std::size_t wire_length() const noexcept { return 2 + data.size(); }

    // Serialize to wire bytes. Fails on: count > 255, empty data (no
    // address byte), or a type byte that would produce 0.
    Result<std::vector<std::uint8_t>> serialize() const;

    // Parse a wire frame. Rejects truncated frames, count mismatch with
    // the actual length, empty data, and type byte 0.
    static Result<Frame> parse(std::span<const std::uint8_t> bytes);
};

// --- Reply parsing ---------------------------------------------------------
//
// Replies take three forms keyed by the request type [DOCUMENTED]:
//
//   CMD (0x04) or WRITE (0x02)  -> ACK:      07 02 <addr> <status>
//   READ_STATUS (0x03)          ->           03 <count> <addr> <status> …
//   READ (0x01)                 ->           01 <2+n> <addr> <status/flags> <n data bytes>
//
// Controller status polls answer `03 02 <addr> <status>` (4 bytes).
// HOST polls answer `03 03 10 <status> <0xaa>` (5 bytes) — observed 8038
// times in the capture corpus; the trailing 0xaa is constant, and status
// 0x80 means "host event pending". [pakon-reference documents the 4-byte
// form; the 5-byte HOST form is capture evidence.]
//
// READ replies: status/flags byte is 0x08 ordinary, 0x88 when an event is
// pending (0x80 = event flag OR-ed on), observed 2657:1 in the captures.
// [CONFIRMED on hardware, August 2026]
//
// A reply too short to contain a status byte carries no status and MUST
// NOT be read as success.

struct Reply {
    FrameType type{};
    std::uint8_t count{};      // value of the wire count byte
    std::uint8_t address{};    // echoes the request address
    Status status{};           // status, or status/flags for READ replies
    std::vector<std::uint8_t> payload; // READ reply data (empty otherwise)

    // True for READ replies whose flags byte has the 0x80 event bit set
    // (0x88 vs ordinary 0x08).
    bool event_pending() const noexcept {
        return type == FrameType::read &&
               (static_cast<std::uint8_t>(status) & 0x80) != 0;
    }
};

// Parse a reply, validating its form against the request type that
// produced it. Returns:
//   ppb_reply_too_short   — fewer than 4 bytes (no status byte present)
//   ppb_invalid_frame     — count inconsistent with length, type 0, or a
//                           READ reply with count < 2
//   ppb_unexpected_reply  — reply type does not mirror the request type
Result<Reply> parse_reply(std::span<const std::uint8_t> bytes,
                          FrameType request_type);

// --- Frame builders --------------------------------------------------------
//
// Each builder reproduces a request form observed verbatim in the
// capture corpus (alibosworth/pakon-captures) and/or quoted in
// pakon-reference. Bus addresses come from protocol/addresses.hpp.

// CMD (type 0x04): data = [addr][0][reg].
//   HostReset:      04 03 10 00 85   (ppb-protocol.md, verbatim)
//   motor probe:    04 03 <picm> 00 00 (ppb-protocol.md, verbatim)
//   AcquireLine:    04 03 40 00 8a   (capture)
Frame make_cmd(std::uint8_t address, std::uint8_t reg);

// WRITE (type 0x02): data = [addr][len(payload)][reg][payload…].
//   HostSetMode:    02 04 10 01 8f 00 (ppb-protocol.md, verbatim)
// If payload is empty the frame degrades to type 0x04 (CMD), matching
// "type is 4 when count == 0, else 2" [captures + pakon-tlx-macos].
Frame make_write(std::uint8_t address, std::uint8_t reg,
                 std::span<const std::uint8_t> payload);

// READ (type 0x01): data = [addr][byte_count][reg], always 3 data bytes.
//   sensor data:    01 03 40 1e 90   → reply 01 20 40 08 <30 bytes>
//   CCD status:     01 03 40 01 83   → reply 01 03 40 08 <1 byte>
// byte_count is the number of payload bytes the device returns.
Frame make_read(std::uint8_t address, std::uint8_t byte_count,
                std::uint8_t reg);

// READ_STATUS (type 0x03): data = [addr], count field = 1.
//   "A `03 01 <addr>` status poll returns `03 02 <addr> <status>`"
//   (dx-barcode.md, verbatim); HOST polls return 03 03 10 <status> aa.
Frame make_read_status(std::uint8_t address);

} // namespace pakon::ppb
