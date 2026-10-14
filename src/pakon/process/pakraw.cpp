#include "pakon/process/pakraw.hpp"

#include <array>
#include <filesystem>
#include <format>

#include "pakon/image/raw_writer.hpp"

namespace pakon::process {

namespace {

std::uint32_t u32(const std::array<std::uint8_t, image::kStreamRawHeaderBytes>& h, std::size_t o) {
    return static_cast<std::uint32_t>(h[o]) | (static_cast<std::uint32_t>(h[o + 1]) << 8) |
           (static_cast<std::uint32_t>(h[o + 2]) << 16) | (static_cast<std::uint32_t>(h[o + 3]) << 24);
}

} // namespace

Result<std::unique_ptr<PakrawReader>> PakrawReader::open(const std::string& path) {
    auto r = std::make_unique<PakrawReader>();
    r->path_ = path;
    r->file_.open(path, std::ios::binary);
    std::array<std::uint8_t, image::kStreamRawHeaderBytes> h{};
    if (!r->file_ ||
        !r->file_.read(reinterpret_cast<char*>(h.data()), static_cast<std::streamsize>(h.size()))) {
        return failure<std::unique_ptr<PakrawReader>>(ErrorKind::io_failure,
                                                      std::format("cannot read {}", path));
    }
    if (std::string_view(reinterpret_cast<const char*>(h.data()), 8) != "PAKRAW01" ||
        u32(h, 0x08) != 1 || u32(h, 0x14) != 16 || u32(h, 0x0C) == 0) {
        return failure<std::unique_ptr<PakrawReader>>(
            ErrorKind::image_bad_input, std::format("{} is not a PAKRAW01 v1 16-bit file", path));
    }
    r->samples_per_row_ = u32(h, 0x0C);
    r->ir_ = (u32(h, 0x18) & 1u) != 0;
    if (u32(h, 0x1C) != 0xFFFFFFFFu && u32(h, 0x20) != 0) {
        r->ir_region_ = std::pair<std::uint32_t, std::uint32_t>{u32(h, 0x1C), u32(h, 0x20)};
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    const std::uint64_t row_bytes = 2ull * r->samples_per_row_;
    const std::uint64_t in_file = ec ? 0 : (size - image::kStreamRawHeaderBytes) / row_bytes;
    const std::uint64_t header_rows = u32(h, 0x10);
    // Unfinished file (rows = 0): trust the size. Otherwise never claim
    // more rows than the file holds.
    r->rows_ = header_rows == 0 ? in_file : std::min(header_rows, in_file);
    r->bytes_.resize(row_bytes);
    return r;
}

VoidResult PakrawReader::read_row(std::uint64_t index, std::span<std::uint16_t> row) {
    if (index >= rows_ || row.size() != samples_per_row_) {
        return void_failure(ErrorKind::image_bad_geometry,
                            std::format("row {} of {} ({} samples requested)", index, rows_,
                                        row.size()));
    }
    if (index != next_) {
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(image::kStreamRawHeaderBytes +
                                                index * 2ull * samples_per_row_));
    }
    if (!file_.read(bytes_.data(), static_cast<std::streamsize>(bytes_.size()))) {
        return void_failure(ErrorKind::image_truncated,
                            std::format("{} ends inside row {}", path_, index));
    }
    for (std::size_t i = 0; i < row.size(); ++i) {
        row[i] = static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes_[2 * i]) |
                                            (static_cast<std::uint8_t>(bytes_[2 * i + 1]) << 8));
    }
    next_ = index + 1;
    return {};
}

} // namespace pakon::process
