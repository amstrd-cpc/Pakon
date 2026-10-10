#pragma once

// Film start / end detection for the film window.
//
// The OEM decides film start and end inside TLB's processing thread from
// the scanned lines (panel LEDs 0x0215 at the leading edge, 0x02D4 at the
// end; the acquire bit is cleared ~20 ms later; no film → window closed
// after ~45 s) — the algorithm itself is not recovered (docs/OEM_RE.md
// §10, [UNKNOWN]). This detector is this stack's own, [INFERRED] design:
// the open-gate level is measured on the leader (the gate is empty when
// the drive starts, as the OEM requires), film is "present" when the
// green level drops below 85 % of it for 16 consecutive lines, and the
// film has passed when the level is back above 95 % for 64 consecutive
// lines. The caller's row budget is always a hard cap on top.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pakon::scan {

class FilmDetector {
public:
    enum class State { measuring_open_gate, waiting_for_film, in_film, ended };

    static constexpr std::size_t kReferenceLines = 32;
    static constexpr std::size_t kEnterLines = 16;
    static constexpr std::size_t kExitLines = 64;
    static constexpr double kEnterRatio = 0.85;
    static constexpr double kExitRatio = 0.95;

    // `pixels`: pixels per line; the level is the mean green sample over
    // the central 60 % of the line.
    explicit FilmDetector(std::size_t pixels) : pixels_(pixels) {}

    // Feed one line (interleaved RGB[, IR block]); returns the new state.
    State feed(std::span<const std::uint16_t> line) {
        const double level = green_level(line);
        ++lines_;
        switch (state_) {
        case State::measuring_open_gate:
            open_sum_ += level;
            if (lines_ >= kReferenceLines) {
                open_ = open_sum_ / static_cast<double>(kReferenceLines);
                state_ = State::waiting_for_film;
            }
            break;
        case State::waiting_for_film:
            run_ = level < kEnterRatio * open_ ? run_ + 1 : 0;
            if (run_ >= kEnterLines) {
                state_ = State::in_film;
                film_start_ = lines_ - kEnterLines;
                run_ = 0;
            }
            break;
        case State::in_film:
            run_ = level > kExitRatio * open_ ? run_ + 1 : 0;
            if (run_ >= kExitLines) {
                state_ = State::ended;
                film_end_ = lines_ - kExitLines;
            }
            break;
        case State::ended:
            break;
        }
        return state_;
    }

    State state() const { return state_; }
    double open_level() const { return open_; }
    std::optional<std::size_t> film_start() const { return film_start_; }
    std::optional<std::size_t> film_end() const { return film_end_; }
    std::size_t lines() const { return lines_; }

private:
    double green_level(std::span<const std::uint16_t> line) const {
        const std::size_t from = pixels_ / 5;
        const std::size_t to = pixels_ - pixels_ / 5;
        double s = 0;
        std::size_t n = 0;
        for (std::size_t p = from; p < to && 3 * p + 1 < line.size(); ++p) {
            s += line[3 * p + 1];
            ++n;
        }
        return n == 0 ? 0.0 : s / static_cast<double>(n);
    }

    std::size_t pixels_;
    State state_{State::measuring_open_gate};
    std::size_t lines_{0};
    std::size_t run_{0};
    double open_sum_{0};
    double open_{0};
    std::optional<std::size_t> film_start_;
    std::optional<std::size_t> film_end_;
};

} // namespace pakon::scan
