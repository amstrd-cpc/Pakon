#pragma once

// ScanDevice: the OEM driver-level operations of TLB.dll (bDrvLampOn,
// bDrvPutCcdFpgaSettings, bDrvResetFifos, bDrvGetPpbInterruptStatus,
// bDriveMotorStop, ...) over the PPB command channel, with the state the
// teardown needs (docs/OEM_RE.md §3, §7, §8).
//
// Hard safety rules enforced here:
//  - LED currents are clamped to the firmware ceilings for the probed
//    board and the IR state being lit (strictest row when the board is
//    unknown) before any 0x81 write — Corrections output included;
//  - teardown() runs at most once, attempts every step exactly once even
//    when steps fail, and is never interrupted: acquire off, lamp off,
//    FIFO reset, DX stop, motor stop (the OEM's bAfterScan), then the
//    bridge's rate=0 → go → idle stop as a second, independent stop.

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/protocol/scan_commands.hpp"
#include "pakon/scan/modes.hpp"
#include "pakon/scan/transport.hpp"

namespace pakon::scan {

struct Duties {
    double r{1.0};
    double g{1.0};
    double b{1.0};
    double ir{0.0};
};

struct LampState {
    bool visible{false};
    bool ir{false};
    protocol::scan::LedCurrents currents{};
    protocol::scan::LedOnTimes on_times{};
};

// What one service poll found (OEM_RE.md §8).
struct ServiceEvent {
    bool pending{false};             // HOST poll had the 0x80 event bit
    std::uint8_t light_status{0};    // PICL reg 0x02 value (acked)
    std::uint8_t motor_status{0};    // PICM reg 0x02 value (acked)
    std::optional<std::uint8_t> lamp_flags;       // PICL 0x83
    std::optional<double> lamp_setpoint_c;        // PICL 0x84, 1/16 °C
    std::optional<std::pair<double, double>> lamp_temps_c; // PICL 0x88
    std::optional<std::uint8_t> dx_flags;         // 0x90 record header byte
    bool lamp_stable() const { return lamp_flags && (*lamp_flags & 0x02) != 0; }
};

struct TeardownStep {
    std::string what;
    bool ok{false};
    std::string error;
};

struct TeardownReport {
    bool ran{false};
    std::vector<TeardownStep> steps;
    bool all_ok() const {
        for (const auto& s : steps) {
            if (!s.ok) {
                return false;
            }
        }
        return ran;
    }
};

// The teardown frames, in order (OEM bAfterScan, then the bridge stop);
// `idle_control` = the FPGA control word with the acquire bit clear.
struct TeardownFrame {
    const char* what;
    ppb::Frame frame;
};
std::vector<TeardownFrame> teardown_frames(const protocol::ControllerAddresses& a,
                                           std::uint16_t idle_control);

class ScanDevice {
public:
    // `settle`: called with the LED settle time after a current increase
    // (the OEM's WaitForLamp, 5 s × Δcurrent / ceiling); the runner drains
    // the image stream while it waits.
    using SettleFn = std::function<void(std::chrono::milliseconds)>;

    ScanDevice(ICommandChannel& channel, protocol::ControllerAddresses addresses);

    void set_settle(SettleFn settle) { settle_ = std::move(settle); }

    // One frame; any non-success status is an error (never retried).
    VoidResult send(const ppb::Frame& frame);
    Result<ppb::Reply> read(const ppb::Frame& frame);

    VoidResult reset_fifos();
    VoidResult lamp_on(bool visible, bool ir, protocol::scan::LedCurrents currents,
                       std::uint16_t integration, const Duties& duties);
    VoidResult lamp_off();
    // bDrvPutCcdFpgaSettings: integration, control word (no acquire),
    // pixel window, resample.
    VoidResult fpga_settings(const Geometry& g, std::uint16_t integration);
    VoidResult acquire(bool on); // FPGA control bit0 — the stream gate
    VoidResult dx_start(std::uint16_t word);
    VoidResult dx_stop();
    VoidResult ad_gains(const std::array<std::uint16_t, 3>& codes);
    VoidResult ad_offsets(const std::array<int, 3>& offsets);
    VoidResult panel_leds(std::uint16_t state);
    VoidResult motor_rate(std::uint16_t rate);
    VoidResult motor_go();
    VoidResult motor_stop();

    // HOST poll; on an event read + ack PICL/PICM reg 0x02 and follow the
    // OEM's status bits (0xA4 → DX records, 0x5B → lamp status).
    Result<ServiceEvent> poll_service();

    // Read the lamp status registers without an event (warm-up check).
    Result<ServiceEvent> read_lamp_status();

    // The guaranteed stop. Idempotent: the second call returns the first
    // report without sending anything.
    const TeardownReport& teardown();
    const TeardownReport& teardown_report() const { return teardown_; }

    const LampState& lamp() const { return lamp_; }
    const protocol::ControllerAddresses& addresses() const { return addresses_; }
    std::uint16_t control() const { return control_; }
    std::size_t frames_sent() const { return frames_sent_; }

private:
    ICommandChannel& channel_;
    protocol::ControllerAddresses addresses_;
    SettleFn settle_;
    LampState lamp_{};
    std::uint16_t control_{protocol::scan::kControlInit};
    std::size_t frames_sent_{0};
    TeardownReport teardown_{};
};

} // namespace pakon::scan
