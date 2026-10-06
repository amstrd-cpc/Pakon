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
//               module-info READ reg 0x07 (12 bytes) — command-reference.md
//                 § "Initialisation" ("module-info read from each
//                 controller"); register number from the capture corpus
//               bridge-info READ HOST reg 0x03 (2 bytes) — capture corpus
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
// documented in pakon-reference; observed payloads are preserved raw:
//   PICL+  0f 0a 05 00 00 '12345' 00 00
//   PICM+  10 06 05 00 00 '12345' 00 00
struct ModuleInfo {
    std::array<std::uint8_t, 12> raw{};

    // Any run of printable ASCII in the payload (for display only).
    std::string printable() const;
};

struct Identity {
    Model model{Model::unknown};
    protocol::ControllerAddresses addresses{};
    bool light_present{false};
    bool motor_present{false};
    std::optional<ModuleInfo> light_module;
    std::optional<ModuleInfo> motor_module;
    // HOST reg 0x03 read, observed 0f 03 in captures; semantics unknown.
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
