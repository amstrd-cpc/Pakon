#pragma once

// ScanRunner: the OEM scan state machine (docs/OEM_RE.md §4, §7-§10) on
// the concurrent image stream.
//
//   init      the OEM init block, byte-identical to the capture
//             (base4.jsonl 6.601-6.696)
//   warm-up   service polls every 200 ms until the lamp reports stable
//             (bLampTemperatureStable: ≤ 300 s)
//   corrections  stream started BEFORE the acquire bit; the recovered
//             closed loop (scan/corrections.hpp) inside one window
//   film      geometry at this unit's EEPROM Offset, Corrections
//             offsets/gains, lamp on, motor rate from the EEPROM, go,
//             acquire + DX start; lines consumed while service polls run;
//             the window ends on film end, no-film timeout or the row
//             budget (hard cap) — always by the HOST clearing the
//             acquire bit, never by waiting for the device to go quiet
//   teardown  ScanDevice::teardown(), exactly once, on every path
//             (success, error, Ctrl+C)
//
// first-light runs init, warm-up and Corrections with the lamp on and the
// motor never engaged, keeps dark + white lines, then tears down.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pakon/eeprom/eeprom.hpp"
#include "pakon/image/format.hpp"
#include "pakon/scan/cancel.hpp"
#include "pakon/scan/corrections.hpp"
#include "pakon/scan/device.hpp"
#include "pakon/scan/modes.hpp"
#include "pakon/scan/transport.hpp"
#include "pakon/stream/image_stream.hpp"

namespace pakon::scan {

enum class ScanKind { first_light, film };

struct RunnerConfig {
    ScanMode mode{};
    ScanKind kind{ScanKind::first_light};
    protocol::ControllerAddresses addresses{protocol::kF135Plus};
    eeprom::UnitCalibration unit{};          // from eeprom::read_sections + parse
    std::size_t first_light_lines{256};      // white lines kept by first-light
    std::size_t film_row_budget{0};          // film: hard cap, required (> 0)
    std::chrono::milliseconds poll_interval{200};       // OEM idle poll
    std::chrono::milliseconds warmup_timeout{300000};   // bLampTemperatureStable
    std::chrono::milliseconds no_film_timeout{45000};   // capture: ~45 s
    std::chrono::milliseconds line_timeout{std::chrono::seconds(5)};
    double settle_scale{1.0};                // LED settle waits (tests: 0)
    const CancelToken* cancel{nullptr};
};

struct PhaseTiming {
    std::string name;
    double seconds{0};
};

enum class FilmEnd { none, film_passed, no_film_timeout, row_budget };
std::string_view to_string(FilmEnd end);

struct ScanResult {
    ScanMode mode{};
    ScanKind kind{};
    eeprom::UnitCalibration unit{};
    std::uint16_t motor_rate{0};
    Geometry film_geometry{};
    CorrectionsResult corrections{};
    image::RawImage white;  // first-light: white lines at the final settings
    image::RawImage film;   // film window lines, as streamed
    FilmEnd film_end{FilmEnd::none};
    std::optional<std::size_t> film_start_line;
    std::optional<std::size_t> film_end_line;
    std::vector<PhaseTiming> phases;
    stream::StreamStats stream{};
    std::size_t resyncs{0};
    double line_period_ms{0};
    std::vector<std::string> events; // service events seen
    TeardownReport teardown;
};

class ScanRunner {
public:
    static Result<std::unique_ptr<ScanRunner>> create(RunnerConfig config);

    // Run the whole session. On failure the teardown has already run;
    // teardown_report() and partial() describe what happened.
    Result<ScanResult> run(ICommandChannel& commands, stream::IImageStream& stream);

    const ScanResult& partial() const { return result_; }
    const TeardownReport& teardown_report() const { return result_.teardown; }

    // The exact init block (OEM_RE.md §4.1 steps 6-9), for the dry run
    // and the capture-fidelity test.
    static std::vector<ppb::Frame> init_frames(const protocol::ControllerAddresses& a);

private:
    explicit ScanRunner(RunnerConfig config) : config_(std::move(config)) {}
    VoidResult run_phases(ScanDevice& device, stream::IImageStream& stream);

    RunnerConfig config_;
    ScanResult result_;
};

} // namespace pakon::scan
