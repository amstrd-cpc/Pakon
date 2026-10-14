#pragma once

// Row-by-row reader for PAKRAW01 files (image/raw_writer.hpp): constant
// memory however long the window is. A file whose writer never finished
// has rows = 0 in its header; the row count is then taken from its size.

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "pakon/errors/error.hpp"

namespace pakon::process {

class PakrawReader {
public:
    static Result<std::unique_ptr<PakrawReader>> open(const std::string& path);

    std::uint32_t samples_per_row() const { return samples_per_row_; }
    std::uint64_t rows() const { return rows_; }
    bool has_ir_lane() const { return ir_; }
    // IR lane inside the row (sample offset, length), when recorded.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> ir_region() const { return ir_region_; }

    // Read row `index` into `row` (samples_per_row samples). Sequential
    // reads are the fast path; any index works.
    VoidResult read_row(std::uint64_t index, std::span<std::uint16_t> row);

private:
    std::ifstream file_;
    std::string path_;
    std::uint32_t samples_per_row_{0};
    std::uint64_t rows_{0};
    bool ir_{false};
    std::optional<std::pair<std::uint32_t, std::uint32_t>> ir_region_;
    std::uint64_t next_{0};
    std::string bytes_;
};

} // namespace pakon::process
