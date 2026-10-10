#pragma once

// Command transport for the scan layer.
//
// The wire has two independent channels and the code keeps them
// separate: PPB command frames ride bulk OUT 0x01 / bulk IN 0x81
// (ICommandChannel), pixel data rides bulk IN 0x86 (stream/
// image_stream.hpp). Neither interface can do the other's job.

#include <cstdint>
#include <utility>

#include "pakon/errors/error.hpp"
#include "pakon/ppb/client.hpp"
#include "pakon/ppb/packet.hpp"

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

} // namespace pakon::scan
