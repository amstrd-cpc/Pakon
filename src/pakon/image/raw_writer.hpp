#pragma once

// Raw image output.
//
// Two layouts, both writing little-endian 16-bit samples as the
// scanner sent them (the stream is pre-calibration data; the OEM's own
// exported files are post-calibration 14-bit — see
// pakon-reference/docs/calibration.md):
//
//  1. stream-raw ("PAKRAW01"): the framed rows exactly as
//     marker-aligned out of the stream, row-major, storage order. This
//     is the faithful acquisition output: no rotation, no
//     de-interleave, marker bits intact. (pakon-reference notes the
//     scanner stores rows rotated 90° to the film axis and the OEM
//     export undoes that; we do not — a raw acquisition file should
//     not silently reorder what the device produced.)
//
//  2. OEM-planar: the SiPlanarFileHeader geometry [CONFIRMED from the
//     OEM's exported raw files, calibration.md § "The OEM raw export"]:
//     16-byte little-endian header {header size = 16, width, height,
//     bits per pixel = 48 (3 x 16-bit)} followed by planar RGB (all R,
//     then all G, then all B). The RGB window inside the row (offset,
//     sample count) is REQUIRED from the caller: the visible/IR split
//     inside a row is not derivable from the offline evidence, so
//     nothing here guesses it. The OEM export also rotates the axes
//     (width = travel direction); that rotation is NOT applied — this
//     file's height axis is the row axis as streamed, and the comment
//     on write_oem_planar says so at the call site.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "pakon/errors/error.hpp"
#include "pakon/image/reader.hpp"

namespace pakon::image {

// PAKRAW01 stream-raw header, 44 bytes:
//   0x00  char[8] magic "PAKRAW01"
//   0x08  u32     version (1)
//   0x0C  u32     samples per row (measured marker period)
//   0x10  u32     rows
//   0x14  u32     bits per sample (16)
//   0x18  u32     flags: bit0 = IR lane present in the row
//   0x1C  u32     IR region offset in samples (0xFFFFFFFF when unknown)
//   0x20  u32     IR region length in samples (0 when absent)
//   0x24  u32     reserved (0)
//   0x28  u32     reserved (0)
inline constexpr std::size_t kStreamRawHeaderBytes = 44;

struct RawStreamMeta {
    // Whether the window was captured with the IR lane on. When on but
    // the region is unknown (the offline default — the reference does
    // not place the lane inside the row), the lane stays inside the
    // opaque row data and flag bit0 records its presence.
    bool has_ir_lane{false};
    std::optional<std::pair<std::uint32_t, std::uint32_t>> ir_region; // offset, length
};

// Write the framed rows verbatim with the PAKRAW01 header.
Result<void> write_stream_raw(const std::string& path, const RawImage& image,
                              const RawStreamMeta& meta);

// RGB window inside one row: sample offset, then sample count (must be
// a multiple of 3).
struct PlanarWindow {
    std::uint32_t offset_samples{0};
    std::uint32_t rgb_samples{0};
};

// Write the OEM-planar layout (16-byte SiPlanarFileHeader + planar
// RGB). NOT rotated: height = rows as streamed (see file header note).
Result<void> write_oem_planar(const std::string& path, const RawImage& image,
                              PlanarWindow window);

} // namespace pakon::image
