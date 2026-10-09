#pragma once

// Command transport for the scan layer.
//
// The wire has two independent channels and the code keeps them
// separate: PPB command frames ride bulk OUT 0x01 / bulk IN 0x81
// (ICommandChannel), pixel data rides bulk IN 0x86 (UsbImageSource /
// image/source.hpp). Neither interface can do the other's job.

#include <cstdint>
#include <utility>

#include "pakon/errors/error.hpp"
#include "pakon/image/source.hpp"
#include "pakon/ppb/client.hpp"
#include "pakon/ppb/packet.hpp"
#include "pakon/scan/cancel.hpp"
#include "pakon/usb/transport.hpp"

namespace pakon::scan {

class ICommandChannel {
public:
    virtual ~ICommandChannel() = default;
    // One write-then-read exchange. The reply's form is validated
    // against the request type inside the client; failures are
    // transport errors (a bad status arrives as a parsed Reply and is
    // the caller's policy decision).
    virtual Result<ppb::Reply> exchange(const ppb::Frame& frame) = 0;
};

// Live adapter over the PPB session client. exchange() runs the frame
// through the client's address safety allow-list before anything
// reaches the wire (ppb/client.hpp).
class PpbCommandChannel final : public ICommandChannel {
public:
    explicit PpbCommandChannel(ppb::Client& client) : client_(client) {}
    Result<ppb::Reply> exchange(const ppb::Frame& frame) override {
        return client_.exchange(frame);
    }

private:
    ppb::Client& client_;
};

// Live adapter over the bulk image endpoint for the image layer.
// Read deadlines (usb_timeout) and zero-length reads are idle ticks —
// never errors and never end-of-stream: on live hardware a device
// that has stopped feeding simply stops answering, and window
// completion for that case is the quiescence policy's job
// (image/completion.hpp). The live backend's read deadline is
// usb::kPipeTimeoutMs, applied as WinUSB pipe policy for endpoint
// 0x86 at open (usb/win_usb_transport.cpp) — WinUSB's default is no
// timeout at all. Non-timeout transport failures propagate.
//
// Nothing outside tests constructs this class: window-level tests use
// image/source.hpp's scripted sources, and usb_image_source_test pins
// this adapter against a fake transport. Endpoint 0x86 is pinned by
// image/source.hpp (capture + topology evidence).
class UsbImageSource final : public image::IImageSource {
public:
    // `cancel` (optional): checked BEFORE each bulk read, so Ctrl+C is
    // observed within at most one in-flight transfer (pipe deadline
    // usb::kPipeTimeoutMs) even mid-window — and never turns into an
    // idle tick: an interrupt is an error (ErrorKind::cancelled), not
    // part of the completion policy's quiet-counting.
    explicit UsbImageSource(usb::IUsbTransport& transport,
                            const CancelToken* cancel = nullptr)
        : transport_(transport), cancel_(cancel) {}

    Result<image::ImageChunk> read(std::size_t max_bytes) override {
        if (cancel_ && cancel_->requested()) {
            return failure<image::ImageChunk>(
                ErrorKind::cancelled, "interrupt observed before an image read");
        }
        auto bytes = transport_.bulk_read(image::kImageEndpoint, max_bytes);
        if (!bytes) {
            if (bytes.error().kind == ErrorKind::usb_timeout) {
                return image::ImageChunk{{}, true, false};
            }
            return bytes.error();
        }
        if (bytes->empty()) {
            return image::ImageChunk{{}, true, false};
        }
        return image::ImageChunk{std::move(*bytes), false, false};
    }

private:
    usb::IUsbTransport& transport_;
    const CancelToken* cancel_;
};

} // namespace pakon::scan
