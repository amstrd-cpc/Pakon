#include "pakon/image/raw_writer.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace pakon::image {

namespace {

void put_u32_le(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void put_u16_le(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

Result<void> write_all(const std::string& path, std::span<const std::uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return void_failure(ErrorKind::io_failure, std::format("cannot open {}", path));
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        return void_failure(ErrorKind::io_failure, std::format("short write to {}", path));
    }
    return {};
}

std::uint32_t row_stride(const RawImage& image) { return image.geometry.samples_per_row; }

} // namespace

Result<void> write_stream_raw(const std::string& path, const RawImage& image,
                              const RawStreamMeta& meta) {
    if (image.geometry.samples_per_row == 0 && !image.samples.empty()) {
        return void_failure(ErrorKind::image_bad_geometry,
                            "non-empty image with zero samples per row");
    }
    if (image.samples.size() !=
        static_cast<std::size_t>(row_stride(image)) * image.rows()) {
        return void_failure(ErrorKind::image_bad_geometry,
                            std::format("{} samples do not form {} rows of {}",
                                        image.samples.size(), image.rows(),
                                        row_stride(image)));
    }

    std::vector<std::uint8_t> out;
    out.reserve(kStreamRawHeaderBytes + image.samples.size() * 2);
    static constexpr std::array<char, 8> kMagic{'P', 'A', 'K', 'R', 'A', 'W', '0', '1'};
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    put_u32_le(out, 1); // version
    put_u32_le(out, row_stride(image));
    put_u32_le(out, static_cast<std::uint32_t>(image.rows()));
    put_u32_le(out, 16); // bits per sample
    put_u32_le(out, meta.has_ir_lane ? 1u : 0u);
    if (meta.ir_region) {
        put_u32_le(out, meta.ir_region->first);
        put_u32_le(out, meta.ir_region->second);
    } else {
        put_u32_le(out, 0xFFFFFFFFu);
        put_u32_le(out, 0);
    }
    put_u32_le(out, 0); // reserved
    put_u32_le(out, 0); // reserved
    for (const auto sample : image.samples) {
        put_u16_le(out, sample);
    }
    return write_all(path, out);
}

Result<void> write_oem_planar(const std::string& path, const RawImage& image,
                              PlanarWindow window) {
    const auto stride = row_stride(image);
    if (window.rgb_samples == 0 || window.rgb_samples % 3 != 0) {
        return void_failure(ErrorKind::image_bad_geometry,
                            std::format("RGB window of {} samples is not a whole "
                                        "number of R,G,B triplets",
                                        window.rgb_samples));
    }
    if (static_cast<std::size_t>(window.offset_samples) + window.rgb_samples > stride) {
        return void_failure(
            ErrorKind::image_bad_geometry,
            std::format("RGB window [{}, {}) does not fit a {}-sample row",
                        window.offset_samples,
                        static_cast<std::size_t>(window.offset_samples) +
                            window.rgb_samples,
                        stride));
    }

    const std::uint32_t pixels = window.rgb_samples / 3;

    std::vector<std::uint8_t> out;
    out.reserve(16 + static_cast<std::size_t>(window.rgb_samples) * image.rows() * 2);
    // SiPlanarFileHeader: header size (16), width, height, bits per
    // pixel (48 = 3 x 16-bit). Height = rows as streamed (not the
    // OEM's rotated orientation — see raw_writer.hpp).
    put_u32_le(out, 16);
    put_u32_le(out, pixels);
    put_u32_le(out, static_cast<std::uint32_t>(image.rows()));
    put_u32_le(out, 48);

    for (std::uint32_t channel = 0; channel < 3; ++channel) {
        for (std::size_t row = 0; row < image.rows(); ++row) {
            const auto* base =
                image.samples.data() + row * stride + window.offset_samples;
            for (std::uint32_t p = 0; p < pixels; ++p) {
                put_u16_le(out, base[p * 3 + channel]);
            }
        }
    }
    return write_all(path, out);
}

} // namespace pakon::image
