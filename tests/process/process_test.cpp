// Offline tests for pakon_process: the sidecar JSON reader, the
// row-by-row .pakraw reader, and frame separation on synthetic profiles.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "pakon/image/raw_writer.hpp"
#include "pakon/process/frames.hpp"
#include "pakon/process/json.hpp"
#include "pakon/process/pakraw.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace pakon;
using namespace pakon::process;

std::string temp_path(const char* name) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.string();
}

// Pitch seed exactly `pitch` lines: line period chosen to match.
ScanInfo info_for_pitch(double pitch, std::size_t start, std::optional<std::size_t> end) {
    ScanInfo i;
    i.mode = "base4";
    i.film_start = start;
    i.film_end = end;
    i.motor_rate = 25676;
    i.line_period_ms = kFramePitchSecondsAtRate25676 / pitch * 1000.0;
    i.pixels = 1000;
    return i;
}

// A strip: leader, then frames of `pitch` lines with `gap` lines of bare
// base between them; `hidden` gap indices look like picture (no gap).
std::vector<LineStats> strip(std::size_t lead, std::size_t frames, std::size_t pitch,
                             std::size_t gap, std::vector<std::size_t> hidden = {}) {
    std::vector<LineStats> p(lead, LineStats{1.0f, 0.98f, 1.02f}); // open gate
    for (std::size_t f = 0; f < frames; ++f) {
        const bool show = std::find(hidden.begin(), hidden.end(), f) == hidden.end();
        for (std::size_t k = 0; k < gap; ++k) {
            p.push_back(show ? LineStats{0.15f, 0.145f, 0.156f} : LineStats{0.09f, 0.02f, 0.14f});
        }
        for (std::size_t k = gap; k < pitch; ++k) {
            p.push_back(LineStats{0.08f, 0.02f, 0.14f});
        }
    }
    for (std::size_t k = 0; k < gap; ++k) {
        p.push_back(LineStats{0.15f, 0.145f, 0.156f});
    }
    return p;
}

} // namespace

PAKON_TEST(json_reads_the_sidecar_shapes) {
    auto j = parse_json(R"({"schema":"pakon-scan/1","n":-1.5e2,"ok":true,"none":null,
                            "a":[[1,2],[3]],"o":{"s":"x\"y"}})");
    EXPECT(j.has_value());
    if (!j) {
        return;
    }
    EXPECT((*j)["schema"].string() == std::optional<std::string>("pakon-scan/1"));
    EXPECT((*j)["n"].number() == std::optional<double>(-150.0));
    EXPECT((*j)["ok"].boolean() == std::optional<bool>(true));
    EXPECT((*j)["none"].is_null());
    EXPECT((*j)["a"][0][1].number() == std::optional<double>(2.0));
    EXPECT((*j)["o"]["s"].string() == std::optional<std::string>("x\"y"));
    EXPECT((*j)["missing"]["deeper"].is_null());
    EXPECT(!parse_json("{\"a\":1,}").has_value());
    EXPECT(!parse_json("[1,2").has_value());
}

PAKON_TEST(pakraw_reader_reads_finished_and_unfinished_files) {
    const auto path = temp_path("pakon_process_reader.pakraw");
    {
        auto w = image::StreamRawWriter::open(path, 6, {});
        EXPECT(w.has_value());
        if (!w) {
            return;
        }
        for (std::uint16_t r = 0; r < 5; ++r) {
            const std::vector<std::uint16_t> row{r, 1, 2, 3, 4, static_cast<std::uint16_t>(65000 + r)};
            EXPECT((*w)->append(row).has_value());
        }
        EXPECT((*w)->finish().has_value());
    }
    auto r = PakrawReader::open(path);
    EXPECT(r.has_value());
    if (!r) {
        return;
    }
    EXPECT_EQ((*r)->rows(), 5u);
    std::vector<std::uint16_t> row(6);
    EXPECT((*r)->read_row(3, row).has_value());
    EXPECT_EQ(row[0], 3u);
    EXPECT_EQ(row[5], 65003u);
    EXPECT((*r)->read_row(1, row).has_value()); // backwards seek
    EXPECT_EQ(row[0], 1u);
    EXPECT(!(*r)->read_row(5, row).has_value());

    // An interrupted scan leaves rows = 0 in the header: the size counts.
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(0x10);
        const char zero[4] = {0, 0, 0, 0};
        f.write(zero, 4);
    }
    auto u = PakrawReader::open(path);
    EXPECT(u.has_value());
    if (u) {
        EXPECT_EQ((*u)->rows(), 5u);
    }
}

PAKON_TEST(frames_are_found_between_the_base_gaps) {
    const auto p = strip(300, 6, 1600, 80);
    const auto split = find_frames(p, info_for_pitch(1630, 300, std::nullopt));
    EXPECT_EQ(split.frames.size(), 6u);
    EXPECT(std::abs(split.pitch - 1600.0) < 2.0);
    EXPECT(std::abs(split.base - 0.15) < 0.01);
    for (std::size_t f = 0; f < split.frames.size(); ++f) {
        // picture lines [lead + f*pitch + gap, lead + (f+1)*pitch)
        EXPECT_EQ(split.frames[f].first, 300 + f * 1600 + 80);
        EXPECT_EQ(split.frames[f].last, 300 + (f + 1) * 1600);
        EXPECT(split.frames[f].leading_gap_seen && split.frames[f].trailing_gap_seen);
    }
}

PAKON_TEST(a_frame_without_a_visible_gap_still_lands_on_the_pitch) {
    // Gap before frame 4 looks like picture (a dark scene edge to edge).
    const auto p = strip(300, 6, 1600, 80, {3});
    const auto split = find_frames(p, info_for_pitch(1600, 300, std::nullopt));
    EXPECT_EQ(split.frames.size(), 6u);
    if (split.frames.size() == 6) {
        EXPECT(!split.frames[2].trailing_gap_seen);
        EXPECT(!split.frames[3].leading_gap_seen);
        // The predicted edge is the grid point: within a few lines of the
        // true gap centre (300 + 3*1600 + 40).
        const double edge = static_cast<double>(split.frames[3].first);
        EXPECT(std::abs(edge - (300 + 3 * 1600 + 40)) < 10.0);
    }
}

PAKON_TEST(frames_stop_at_the_film_end_and_skip_short_pieces) {
    const auto p = strip(300, 6, 1600, 80);
    // Film ends half way into frame 6: that piece is too short to keep.
    const auto split = find_frames(p, info_for_pitch(1600, 300, 300 + 5 * 1600 + 800));
    EXPECT_EQ(split.frames.size(), 5u);
}

int main() { return pakon::test::run_all(); }
