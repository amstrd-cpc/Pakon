#pragma once

// Asynchronous bulk-IN pipe: the transport primitive under the image
// stream (stream/image_stream.hpp).
//
// Why asynchronous: the scanner streams endpoint 0x86 on its own clock
// (~6.4 MB/s at Base 4) for as long as the CCD FPGA acquire bit is set,
// and the FX2's FIFO is only a few packets deep. The OEM kernel driver
// therefore keeps reads queued straight into its ring
// (docs/OEM_RE.md §6, F135usb2.sys@0x11634); one synchronous read at a
// time loses data between reads. A pipe here keeps `slots` reads in
// flight; the stream thread resubmits each slot as soon as it completes.
//
// Contract:
//  - submit(slot, buffer) queues one read into `buffer` (which must stay
//    valid until that slot completes); at most one read per slot.
//  - wait(timeout) returns the next completion, or nullopt on timeout.
//  - abort() cancels every queued read; each one still completes (with
//    `aborted` set) and must be collected by wait() before the buffers
//    are released. abort() is callable from another thread.
//  - Implementations never log the image bytes.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "pakon/errors/error.hpp"

namespace pakon::usb {

struct PipeCompletion {
    std::size_t slot{0};
    std::size_t bytes{0};    // bytes transferred (0 for a ZLP)
    bool aborted{false};     // cancelled by abort()
    bool failed{false};      // transfer error (message in `error`)
    std::string error;
};

class IBulkInPipe {
public:
    virtual ~IBulkInPipe() = default;
    virtual VoidResult submit(std::size_t slot, std::span<std::uint8_t> buffer) = 0;
    virtual Result<std::optional<PipeCompletion>> wait(std::chrono::milliseconds timeout) = 0;
    virtual void abort() = 0;
};

} // namespace pakon::usb
