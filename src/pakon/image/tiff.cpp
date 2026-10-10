#include "pakon/image/tiff.hpp"

#include <cstdio>
#include <format>
#include <vector>

namespace pakon::image {

namespace {

void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

} // namespace

VoidResult write_tiff16(const std::string& path, std::uint32_t width, std::uint32_t height,
                        std::uint32_t channels, std::span<const std::uint16_t> pixels) {
    if ((channels != 1 && channels != 3) || width == 0 || height == 0 ||
        pixels.size() != static_cast<std::size_t>(width) * height * channels) {
        return void_failure(ErrorKind::image_bad_geometry,
                            std::format("TIFF {}x{}x{} does not match {} samples", width, height,
                                        channels, pixels.size()));
    }
    const std::uint64_t data_bytes = static_cast<std::uint64_t>(pixels.size()) * 2;
    if (data_bytes > 0xF0000000ull) {
        return void_failure(ErrorKind::image_bad_geometry, "preview too large for one TIFF strip");
    }
    std::vector<std::uint8_t> head;
    head.insert(head.end(), {'I', 'I', 42, 0});
    const std::uint32_t data_off = 8;
    const std::uint32_t bps_off = data_off + static_cast<std::uint32_t>(data_bytes);
    const std::uint32_t ifd_off = bps_off + 6 + (bps_off + 6) % 2;
    put32(head, ifd_off);

    std::vector<std::uint8_t> tail;
    // BitsPerSample array for RGB (3 × 16), then padding to even.
    put16(tail, 16);
    put16(tail, 16);
    put16(tail, 16);
    while ((bps_off + tail.size()) < ifd_off) {
        tail.push_back(0);
    }
    struct Entry {
        std::uint16_t tag, type;
        std::uint32_t count, value;
    };
    const Entry entries[] = {
        {256, 4, 1, width},                                   // ImageWidth
        {257, 4, 1, height},                                  // ImageLength
        {258, 3, channels, channels == 3 ? bps_off : 16u},    // BitsPerSample
        {259, 3, 1, 1},                                       // Compression: none
        {262, 3, 1, channels == 3 ? 2u : 1u},                 // RGB / BlackIsZero
        {273, 4, 1, data_off},                                // StripOffsets
        {277, 3, 1, channels},                                // SamplesPerPixel
        {278, 4, 1, height},                                  // RowsPerStrip
        {279, 4, 1, static_cast<std::uint32_t>(data_bytes)},  // StripByteCounts
        {284, 3, 1, 1},                                       // PlanarConfig: chunky
    };
    put16(tail, static_cast<std::uint16_t>(std::size(entries)));
    for (const auto& e : entries) {
        put16(tail, e.tag);
        put16(tail, e.type);
        put32(tail, e.count);
        if (e.type == 3 && e.count == 1) {
            put16(tail, static_cast<std::uint16_t>(e.value));
            put16(tail, 0);
        } else {
            put32(tail, e.value);
        }
    }
    put32(tail, 0); // no next IFD

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return void_failure(ErrorKind::io_failure, std::format("cannot create {}", path));
    }
    std::vector<std::uint8_t> data;
    data.reserve(static_cast<std::size_t>(data_bytes));
    for (const auto v : pixels) {
        put16(data, v);
    }
    const bool ok = std::fwrite(head.data(), 1, head.size(), f) == head.size() &&
                    std::fwrite(data.data(), 1, data.size(), f) == data.size() &&
                    std::fwrite(tail.data(), 1, tail.size(), f) == tail.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        return void_failure(ErrorKind::io_failure, std::format("write failed: {}", path));
    }
    return {};
}

} // namespace pakon::image
