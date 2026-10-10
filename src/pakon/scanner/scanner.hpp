#pragma once

// Scanner session: documented connect handshake, model identification,
// and status queries.
//
// Everything sent from this layer is a frame documented by
// pakon-reference and/or observed verbatim in the capture corpus:
//
//   connect:    HostReset (04 03 10 00 85), HostSetMode (02 04 10 01 8f 00)
//               — ppb-protocol.md § "The open handshake"
//   identify:   presence probes 04 03 <motor> 00 00 at 0x44 and 0x24
//               — ppb-protocol.md § "Presence probes are model detection",
//                 command-reference.md (CMD 0x00 = ResetMotor)
//               then the OEM's own order (docs/OEM_RE.md §4.1): bridge
//               version READ HOST 0x03 (2 B), PICM 0x97 = 01, and per
//               controller the dev-info page select 0x03 = 01 followed by
//               READ 0x07 (12 B) — TLB@0x1001c3e0 / 0x1000a370, every
//               capture (base4.jsonl 5.795-5.814)
//   status:     polls 03 01 <addr>; READ 0x83 (CCD status), 0x84 (light
//               status), 0x88 (temperature) — command-reference.md table
//
// No motor motion, no light/CCD configuration, no TEC writes: those are
// Phase 5+ operations.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "pakon/errors/error.hpp"
#include "pakon/ppb/client.hpp"
#include "pakon/protocol/addresses.hpp"

namespace pakon::scanner {

// Model as determined by presence probes (USB identity cannot distinguish
// F-135 from F-135+ — pakon-reference/docs/ppb-protocol.md).
enum class Model {
    unknown,
    f135,       // controllers at 0x20/0x24
    f135_plus,  // controllers at 0x40/0x44
};

std::string_view to_string(Model model);

// Explicit session states. (The fuller operational states — Loading,
// Scanning, Transferring — arrive with the film-transport/image phases;
// they are not part of Phase 4.)
enum class State {
    disconnected,
    connecting,
    ready,
    error,
};

std::string_view to_string(State state);

// 12-byte module-info read (READ reg 0x07). Semantic decoding is not
// documented in pakon-reference. Capture evidence (alibosworth/
// pakon-captures, base4.jsonl events 151/159, F-135+ serial 16402):
//   PICL+  0f 0a 05 00 00 '12345' 00 00
//   PICM+  10 06 05 00 00 '12345' 00 00
// bytes [5..9] hold the same 5-byte ASCII id in both captured payloads
// and every other byte is non-textual in the captures. The lab unit
// (010-203-04, 2026-10-08) returns an entirely different, non-printable
// layout from the same request:
//   PICL+  04 20 40 12 04 c0 21 02 00 00 92 00
//   PICM+  02 20 00 a0 00 8c 08 00 00 20 00 00
// and the light bytes [0..3] are not stable across days (2026-10-09:
// 82 02 d4 01 — matching the same-session temperature read's 82 02
// shape) while [4..11] and the motor payload stay byte-identical.
// Resolved (OEM_RE.md §4.2): the lab unit was read WITHOUT the OEM's
// 0x03 = 01 page select, so it answered a different page. On the
// selected page b[2], b[1] are the firmware version the OEM logs as
// "Lamp 0x05,0x0A Motor 0x05,0x06" (this unit's log reports the same).
struct ModuleInfo {
    std::array<std::uint8_t, 12> raw{};

    // The capture-evidenced 5-byte ASCII id at offset 5, returned only
    // when all five bytes are printable; empty otherwise. Scanning the
    // whole payload for printable bytes used to fabricate strings such
    // as "@!" out of the lab unit's version/address bytes.
    std::string printable() const;

    // Full payload as space-separated hex, e.g. "04 20 40 12 ... 92 00".
    std::string hex() const;

    // Firmware version as the OEM prints it ("Lamp 0x05,0x0A"): (b[2],
    // b[1]) of the dev-info page (TLB@0x1001c3e0, OEM_RE.md §4.1).
    // Meaningful for a reply read after the 0x03 = 01 page select, which
    // identify() now always sends.
    std::pair<std::uint8_t, std::uint8_t> firmware() const { return {raw[2], raw[1]}; }
};

struct Identity {
    Model model{Model::unknown};
    protocol::ControllerAddresses addresses{};
    bool light_present{false};
    bool motor_present{false};
    std::optional<ModuleInfo> light_module;
    std::optional<ModuleInfo> motor_module;
    // HOST reg 0x03 read: bridge firmware version, printed by the OEM as
    // "USB 0x03,0x0F" (payload 0f 03, little-endian; OEM_RE.md §3).
    std::optional<std::array<std::uint8_t, 2>> bridge_info;
};

struct StatusReport {
    std::uint8_t host_poll{0};
    std::uint8_t light_poll{0};
    std::uint8_t motor_poll{0};
    std::optional<std::uint8_t> ccd_status;
    std::optional<std::array<std::uint8_t, 2>> light_status;
    std::optional<std::array<std::uint8_t, 4>> temperature;
};

class Scanner {
public:
    // Open the PPB session: bridge open handshake, then state=ready.
    //
    // The HostReset/HostSetMode replies arrive only on the first open
    // after power-on or firmware load; on later opens they time out while
    // the bridge keeps working normally. They are therefore treated as
    // best-effort, not as failures
    // [CONFIRMED on hardware, August 2026] (ppb-protocol.md).
    static Result<std::unique_ptr<Scanner>>
    connect(std::unique_ptr<usb::IUsbTransport> transport);

    // Presence probes + module-info reads; determines the model.
    Result<Identity> identify();

    // Status polls and read-only status registers. Requires identify() to
    // have determined the controller addresses.
    Result<StatusReport> status();

    void disconnect();

    State state() const noexcept { return state_; }
    const Identity& identity() const noexcept { return identity_; }
    ppb::Client& client() { return *client_; }

private:
    Scanner() = default;

    void transition(State next);

    std::unique_ptr<ppb::Client> client_;
    State state_{State::disconnected};
    Identity identity_{};
};

} // namespace pakon::scanner
