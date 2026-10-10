#pragma once

// Minimal baseline TIFF writer for previews: uncompressed, little-endian,
// 16-bit RGB (or 16-bit grey), one strip. Enough for any viewer; no
// dependency. Layout: header, image data, then the IFD.

#include <cstdint>
#include <span>
#include <string>

#include "pakon/errors/error.hpp"

namespace pakon::image {

// `pixels`: width × height × channels samples, row-major, interleaved.
// channels must be 1 (grey) or 3 (RGB).
VoidResult write_tiff16(const std::string& path, std::uint32_t width, std::uint32_t height,
                        std::uint32_t channels, std::span<const std::uint16_t> pixels);

} // namespace pakon::image
