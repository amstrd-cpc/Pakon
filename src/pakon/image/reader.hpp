#pragma once

// Window reader: drains an image source into framed rows until an
// explicit completion policy ends the window.
//
// One ImageReceiver instance == one image window. The capture corpus
// records two windows per scan (a pre-scan calibration pass and the
// film-transport pass) with different durations and no evidence of
// shared geometry, so windows are framed by separate RowFramer
// instances that each measure their own marker period.

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "pakon/image/completion.hpp"
#include "pakon/image/format.hpp"
#include "pakon/image/row_sync.hpp"
#include "pakon/image/source.hpp"

namespace pakon::image {

// One framed window in memory: rows of little-endian 16-bit samples,
// row-major, marker-aligned. `samples` holds exactly
// rows * geometry.samples_per_row samples.
struct RawImage {
    RowGeometry geometry;
    std::vector<std::uint16_t> samples;

    std::size_t rows() const {
        return geometry.samples_per_row == 0
                   ? 0
                   : samples.size() / geometry.samples_per_row;
    }
};

// What happened when a window ended.
struct WindowReport {
    std::size_t rows{0};
    std::size_t bytes_received{0};
    std::size_t truncated_tail_bytes{0}; // partial row dropped at EOS
    const char* completion{""};          // the policy that ended it
};

class ImageReceiver {
public:
    // A completion policy is mandatory — there is no default. Returns
    // a failure when the policy is missing.
    static Result<std::unique_ptr<ImageReceiver>>
    create(std::shared_ptr<const ICompletionPolicy> policy,
           std::size_t read_bytes = kCaptureChunkBytes);

    // Drain `source` until the policy completes. Framed rows are
    // appended to `out` (its geometry is set from the window's
    // measured line period). A device that stops mid-row at
    // end-of-stream drops that partial row and reports its size; any
    // other truncation is an error (image_truncated) — padding it
    // would fabricate pixels.
    Result<WindowReport> run(IImageSource& source, RawImage& out);

    const ICompletionPolicy& policy() const { return *policy_; }

private:
    ImageReceiver(std::shared_ptr<const ICompletionPolicy> policy, std::size_t read_bytes)
        : policy_(std::move(policy)), read_bytes_(read_bytes) {}

    std::shared_ptr<const ICompletionPolicy> policy_;
    std::size_t read_bytes_;
};

} // namespace pakon::image
