#pragma once

// The image layer's input interface: a stream of bulk-IN chunks from
// the image endpoint.
//
// Command transport (PPB frames on endpoints 0x01/0x81) and image
// reception (bulk IN 0x86) are separate channels on the wire and
// separate interfaces here: nothing in this header can send a command,
// and nothing in ppb/ can read image data. The live adapter
// (usb::IUsbTransport::bulk_read on 0x86) lives in scan/usb_image_
// source.hpp; tests provide their own sources.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "pakon/errors/error.hpp"

namespace pakon::image {

// Image endpoint (bulk IN). Capture-evidenced: the OEM-driven sessions
// in pakon-captures show the host reading this endpoint for pixel
// data (EP_IMG_IN in bridge/pakonusb.py), and the F-135+ descriptors
// show bulk IN 512 on it (docs/F135_TOPOLOGY.md § 0x86).
inline constexpr std::uint8_t kImageEndpoint = 0x86;

// Every recorded image transfer in the capture corpus is exactly this
// size (20480 = 0x5000). image-stream.md: that is the OEM host's
// ring-packet choice, not an endpoint property — other hosts read
// other sizes without ill effect — so it is only a default here.
inline constexpr std::size_t kCaptureChunkBytes = 20480;

// One read attempt's outcome.
struct ImageChunk {
    std::vector<std::uint8_t> bytes;  // empty when nothing was delivered
    bool timed_out{false};            // read deadline expired with no data
    bool end_of_stream{false};        // device stopped feeding the endpoint
};

class IImageSource {
public:
    virtual ~IImageSource() = default;
    // Deliver up to max_bytes. Errors are transport failures (not
    // completion signals); an idle device is reported as an empty
    // chunk with timed_out (and/or end_of_stream) set, never as an
    // error, so completion policy stays the only place ending a read.
    virtual Result<ImageChunk> read(std::size_t max_bytes) = 0;
};

} // namespace pakon::image
