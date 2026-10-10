#pragma once

// SimDevice: a stateful fake F-135+ behind the IUsbTransport seam.
//
// It is the test backbone for the scan path and the backend of the Wine
// shim server (tools/sim_server): everything the C++ stack and the OEM
// stack can send reaches the same model. Behaviour follows
// docs/OEM_RE.md; where the model has to choose (optics, timing) the
// choice is stated next to the parameter.
//
// What it models:
//  - PPB replies in the captured forms (07 02 a st; 03 02 a st; the HOST
//    poll with the 0x80 event bit; READ flags 0x08 / 0x88);
//  - controller state: lamp mask, LED currents (values above the board's
//    ceiling are VIOLATIONS and are clamped), LED on-times, CCD FPGA
//    control/geometry/integration, A/D gains + sign-magnitude offsets,
//    motor rate/go/stop, DX start/stop, the dev-info page select;
//  - the service protocol: the lamp becomes stable lamp_ready_delay after
//    the lamp-temperature init and raises PICL status 0x02 until acked;
//    film edges raise 0x20 and the DX record flags;
//  - the per-unit EEPROM over 0xA4/0xA9 (wIndex 0x1234); 0xA2 and
//    write-selects are VIOLATIONS;
//  - EP6 on its own clock: while the FPGA acquire bit is set, lines in
//    the recovered format (marker = LSB of the first sample, R,G,B
//    interleaved, IR block) are produced at the real line rate
//    (integration × 0.48 µs, OEM_RE.md §9) into whatever reads are queued;
//    with no read queued they go into a bounded device FIFO and, when it
//    is full, are LOST (counted — this is how a host that falls behind
//    loses sync on the real FX2);
//  - fault injection at any command frame and in the stream.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pakon/usb/transport.hpp"

namespace pakon::sim {

enum class FaultKind {
    transport_error, // the exchange fails (usb_io_failed)
    bad_status,      // the device answers status 0x02 (invalid packet)
    timeout,         // the exchange times out (usb_timeout)
};

struct FilmModel {
    std::size_t lead_lines{300};  // open-gate lines of transport before the leading edge
    std::size_t film_lines{1500}; // lines of film in the gate
};

struct SimConfig {
    // >1 runs the stream clock and the service delays faster than real
    // time; 1.0 is the real F-135+ line rate.
    double speed{1.0};
    std::chrono::milliseconds lamp_ready_delay{6500}; // CAP base4.jsonl: 6.5 s
    std::chrono::microseconds command_latency{2500};  // CAP: ~2.5 ms per exchange
    std::size_t device_fifo_bytes{4096};              // FX2 quad-buffered 512 B + slack
    std::optional<FilmModel> film;                    // nullopt = no film fed
    std::optional<std::pair<std::size_t, FaultKind>> fault_at_frame;
    bool fault_sticky{false}; // after the fault every exchange fails (unplug)
    std::optional<std::uint64_t> stream_fault_after_bytes;
    std::vector<std::uint8_t> eeprom;  // empty = make_eeprom()
    bool host_poll_six_bytes{true};    // lab unit form 03 04 10 st aa aa
};

struct EepromSpec {
    std::uint32_t serial{17373};
    // Base 4/8/16: Offset, MotorSpeed, MotorSpeed IR — deliberately NOT
    // unit 16402's values, so tests prove per-unit derivation.
    std::array<std::array<std::uint16_t, 3>, 3> base{{{28, 25100, 18800},
                                                     {56, 11200, 7400},
                                                     {59, 5800, 4700}}};
    std::uint16_t adjust{1000};
    bool corrupt_primary_a{false}; // flip one payload byte (like unit 16402)
    bool corrupt_all{false};       // both copies of both sections bad
    bool blank{false};             // all 0xFF
};

std::vector<std::uint8_t> make_eeprom(const EepromSpec& spec = {});

class SimDevice final : public usb::IUsbTransport {
public:
    explicit SimDevice(SimConfig config = {});
    ~SimDevice() override;
    SimDevice(const SimDevice&) = delete;
    SimDevice& operator=(const SimDevice&) = delete;

    // --- IUsbTransport ---------------------------------------------------
    Result<std::vector<std::uint8_t>>
    command_exchange(std::span<const std::uint8_t> frame) override;
    Result<std::vector<std::uint8_t>> bulk_read(std::uint8_t endpoint,
                                                std::size_t max_length) override;
    Result<std::unique_ptr<usb::IBulkInPipe>> open_bulk_in(std::uint8_t endpoint,
                                                           std::size_t max_slots) override;
    Result<std::vector<std::uint8_t>> control_read(std::uint8_t request, std::uint16_t value,
                                                   std::uint16_t index,
                                                   std::uint16_t length) override;
    VoidResult control_write(std::uint8_t request, std::uint16_t value,
                             std::uint16_t index) override;
    const usb::DeviceInfo& device_info() const override { return info_; }

    // --- observation (thread-safe) ----------------------------------------
    std::vector<std::vector<std::uint8_t>> frames() const; // every command frame, in order
    std::vector<std::string> violations() const;           // safety-rule breaches
    std::vector<std::string> unknown_writes() const;       // registers outside the model
    std::uint64_t lost_bytes() const;     // device-FIFO overflow (host fell behind)
    std::uint64_t streamed_bytes() const; // bytes the device produced
    std::uint64_t lines_produced() const;
    bool acquiring() const;
    bool lamp_lit() const;
    bool motor_running() const;
    std::array<std::uint8_t, 5> led_currents() const; // [B, IR, R, 0, G]
    std::array<int, 3> ad_offsets() const;
    std::array<std::uint16_t, 3> ad_gains() const;
    std::uint16_t motor_rate() const;
    std::size_t control_requests() const;

    // Implementation detail shared with the pipe.
    struct Pending {
        std::size_t slot;
        std::span<std::uint8_t> buffer;
        std::size_t filled{0};
    };

private:
    friend class SimPipe;

    Result<std::vector<std::uint8_t>> handle_locked(std::span<const std::uint8_t> frame);
    std::vector<std::uint8_t> ack(std::uint8_t address, std::uint8_t status) const;
    std::vector<std::uint8_t> read_reply(std::uint8_t address, std::uint8_t flags,
                                         std::span<const std::uint8_t> payload) const;
    void write_register(std::uint8_t address, std::uint8_t reg,
                        std::span<const std::uint8_t> payload);
    void command(std::uint8_t address, std::uint8_t reg);
    std::vector<std::uint8_t> read_register(std::uint8_t address, std::uint8_t reg,
                                            std::uint8_t count, bool& ok);
    void set_control(std::uint16_t value);
    void service_tick_locked();
    bool event_pending_locked() const;

    void producer_loop();
    void produce_line_locked();
    void deliver_locked(std::span<const std::uint8_t> bytes);
    void complete_head_locked(bool short_ok);
    std::size_t line_samples_locked() const;
    double line_time_us_locked() const;

    SimConfig config_;
    usb::DeviceInfo info_{};
    std::vector<std::uint8_t> eeprom_;

    mutable std::mutex mutex_;
    std::condition_variable stream_cv_;
    std::condition_variable completion_cv_;
    std::thread producer_;
    bool quit_{false};

    // Recorded traffic.
    std::vector<std::vector<std::uint8_t>> frames_;
    std::vector<std::string> violations_;
    std::vector<std::string> unknown_writes_;
    std::size_t exchange_count_{0};
    bool dead_{false};
    std::size_t control_count_{0};
    bool eeprom_selected_{false};

    // Light controller (PICL).
    std::uint8_t lamp_mask_{0};
    std::array<std::uint8_t, 5> currents_{}; // [B, IR, R, 0, G]
    std::array<std::uint16_t, 6> ontime_{};  // [B, IR, R, 0, G, base]
    bool resample_{false};
    bool dx_running_{false};
    std::uint8_t picl_status_{0};
    std::uint8_t picm_status_{0};
    bool lamp_stable_{false};
    bool temp_init_{false};
    std::chrono::steady_clock::time_point temp_init_at_{};
    std::uint8_t dx_flags_{0};
    std::uint16_t dx_counter_{0x3d86};
    std::array<bool, 2> info_page_{};

    // CCD FPGA (PICM 0x82/0x84).
    std::uint16_t control_{0};
    std::uint16_t pixel_start_{0x3E};
    std::uint16_t pixel_end_{0x80E};
    std::uint16_t integration_{0xFFD};
    std::uint16_t leds_{0};
    std::array<std::uint16_t, 3> gains_{};
    std::array<int, 3> offsets_{};

    // Motor.
    std::uint16_t rate_{0};
    bool going_{false};
    std::uint64_t travel_lines_{0}; // lines acquired while the film moves

    // Stream.
    std::vector<std::uint8_t> line_;  // current line bytes
    std::size_t line_pos_{0};
    std::deque<std::uint8_t> fifo_;   // device-side buffer
    std::deque<Pending> pending_;     // reads queued by the host
    std::deque<usb::PipeCompletion> completions_;
    bool pipe_open_{false};
    std::uint64_t lost_{0};
    std::uint64_t streamed_{0};
    std::uint64_t lines_{0};
    double budget_{0};
    std::chrono::steady_clock::time_point last_tick_{};
};

} // namespace pakon::sim
