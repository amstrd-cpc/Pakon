// Offline tests for raw image output: the PAKRAW01 stream-raw header
// layout (byte offsets pinned) and the OEM-planar geometry, both read
// back from disk.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pakon/image/raw_writer.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace pakon;
using namespace pakon::image;

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file),
                                     std::istreambuf_iterator<char>());
}

std::uint32_t u32_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint16_t u16_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::string temp_path(const char* name) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.string();
}

// 3 rows x 9 samples; samples are r0 g0 b0 r1 g1 b1 r2 g2 b2 per row.
RawImage three_row_image() {
    RawImage image;
    image.geometry.samples_per_row = 9;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t i = 0; i < 9; ++i) {
            image.samples.push_back(static_cast<std::uint16_t>(100 * row + i * 2 + 1));
        }
    }
    return image;
}

} // namespace

PAKON_TEST(stream_raw_header_and_payload_are_pinned) {
    const auto path = temp_path("pakon_stream_raw_test.pakraw");
    auto image = three_row_image();
    RawStreamMeta meta;
    meta.has_ir_lane = true;
    meta.ir_region = std::pair<std::uint32_t, std::uint32_t>{6, 3};

    auto written = write_stream_raw(path, image, meta);
    EXPECT(written.has_value());
    if (!written) {
        return;
    }

    const auto bytes = read_file(path);
    // magic "PAKRAW01"
    EXPECT(bytes.size() == kStreamRawHeaderBytes + image.samples.size() * 2);
    EXPECT(bytes[0] == 'P' && bytes[1] == 'A' && bytes[2] == 'K' && bytes[3] == 'R');
    EXPECT(bytes[4] == 'A' && bytes[5] == 'W' && bytes[6] == '0' && bytes[7] == '1');
    EXPECT_EQ(u32_at(bytes, 0x08), 1u);            // version
    EXPECT_EQ(u32_at(bytes, 0x0C), 9u);            // samples per row
    EXPECT_EQ(u32_at(bytes, 0x10), 3u);            // rows
    EXPECT_EQ(u32_at(bytes, 0x14), 16u);           // bits per sample
    EXPECT_EQ(u32_at(bytes, 0x18), 1u);            // flags: IR lane present
    EXPECT_EQ(u32_at(bytes, 0x1C), 6u);            // IR region offset
    EXPECT_EQ(u32_at(bytes, 0x20), 3u);            // IR region length
    EXPECT_EQ(u32_at(bytes, 0x24), 0u);            // reserved
    EXPECT_EQ(u32_at(bytes, 0x28), 0u);            // reserved

    // Payload: little-endian samples verbatim, in row-major order.
    for (std::size_t i = 0; i < image.samples.size(); ++i) {
        EXPECT_EQ(u16_at(bytes, kStreamRawHeaderBytes + i * 2), image.samples[i]);
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

PAKON_TEST(stream_raw_records_an_unknown_ir_region) {
    const auto path = temp_path("pakon_stream_raw_noregion.pakraw");
    auto image = three_row_image();
    RawStreamMeta meta;
    meta.has_ir_lane = false;

    EXPECT(write_stream_raw(path, image, meta).has_value());
    const auto bytes = read_file(path);
    EXPECT_EQ(u32_at(bytes, 0x18), 0u);
    EXPECT_EQ(u32_at(bytes, 0x1C), 0xFFFFFFFFu);
    EXPECT_EQ(u32_at(bytes, 0x20), 0u);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

PAKON_TEST(oem_planar_layout_is_whole_image_planes) {
    const auto path = temp_path("pakon_planar_test.planar");
    auto image = three_row_image();

    auto written = write_oem_planar(path, image, PlanarWindow{0, 9});
    EXPECT(written.has_value());
    if (!written) {
        return;
    }

    const auto bytes = read_file(path);
    EXPECT_EQ(bytes.size(), 16u + 3 * 9 * 2u);
    EXPECT_EQ(u32_at(bytes, 0x00), 16u);  // SiPlanarFileHeader size
    EXPECT_EQ(u32_at(bytes, 0x04), 3u);   // width  = pixels per row
    EXPECT_EQ(u32_at(bytes, 0x08), 3u);   // height = rows as streamed
    EXPECT_EQ(u32_at(bytes, 0x0C), 48u);  // bits per pixel = 3 x 16

    // Planes: all R (every 3rd sample from every row), then all G, then
    // all B — the layout calibration.md confirms from the OEM exports.
    const std::size_t plane = 3 * 3;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t p = 0; p < 3; ++p) {
            const auto r = image.samples[row * 9 + p * 3];
            const auto g = image.samples[row * 9 + p * 3 + 1];
            const auto b = image.samples[row * 9 + p * 3 + 2];
            EXPECT_EQ(u16_at(bytes, 16 + (0 * plane + row * 3 + p) * 2), r);
            EXPECT_EQ(u16_at(bytes, 16 + (1 * plane + row * 3 + p) * 2), g);
            EXPECT_EQ(u16_at(bytes, 16 + (2 * plane + row * 3 + p) * 2), b);
        }
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

PAKON_TEST(planar_window_must_fit_and_align) {
    const auto path = temp_path("pakon_planar_bad.planar");
    auto image = three_row_image();

    // Window runs past the row end.
    auto overflow = write_oem_planar(path, image, PlanarWindow{6, 9});
    EXPECT(!overflow.has_value());
    if (!overflow) {
        EXPECT_EQ(overflow.error().kind, ErrorKind::image_bad_geometry);
    }
    // Not a whole number of triplets.
    auto ragged = write_oem_planar(path, image, PlanarWindow{0, 8});
    EXPECT(!ragged.has_value());
    if (!ragged) {
        EXPECT_EQ(ragged.error().kind, ErrorKind::image_bad_geometry);
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

PAKON_TEST(stream_raw_rejects_inconsistent_geometry) {
    const auto path = temp_path("pakon_stream_bad.pakraw");
    RawImage image;
    image.geometry.samples_per_row = 4;
    image.samples = {1, 2, 3}; // not a whole number of 4-sample rows
    auto bad = write_stream_raw(path, image, RawStreamMeta{});
    EXPECT(!bad.has_value());
    if (!bad) {
        EXPECT_EQ(bad.error().kind, ErrorKind::image_bad_geometry);
    }

    RawImage empty;
    empty.geometry.samples_per_row = 0;
    auto zeros = write_stream_raw(path, empty, RawStreamMeta{});
    EXPECT(zeros.has_value()); // zero rows is well-formed

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

int main() { return pakon::test::run_all(); }
