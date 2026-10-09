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
// (image/completion.hpp). Non-timeout transport failures propagate.
//
// Nothing in the offline test suite constructs this class; tests use
// image/source.hpp's scripted sources instead. Endpoint 0x86 is
// pinned by image/source.hpp (capture + topology evidence).
class UsbImageSource final : public image::IImageSource {
public:
    explicit UsbImageSource(usb::IUsbTransport& transport) : transport_(transport) {}

    Result<image::ImageChunk> read(std::size_t max_bytes) override {
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
};

} // namespace pakon::scan
