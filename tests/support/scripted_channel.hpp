#pragma once

// ScriptedCommandChannel: an ordered (request, reply) script driving
// the scan layer's ICommandChannel in tests.
//
// Unlike the map-based ReplayUsbTransport, this is strictly ordered:
// every exchange must match the next scripted step exactly (hex for
// hex), which is what the capture-backed replay tests need — polls
// repeat with different replies, and an extra or reordered frame from
// the implementation is a test failure, not a silent pass.

#include <format>
#include <string>
#include <vector>

#include "pakon/scan/transport.hpp"
#include "support/replay_transport.hpp"

namespace pakon::test {

class ScriptedCommandChannel final : public scan::ICommandChannel {
public:
    struct Step {
        std::string req; // request frame, hex (verbatim capture or fixture)
        std::string rsp; // reply, hex
    };

    explicit ScriptedCommandChannel(std::vector<Step> script)
        : script_(std::move(script)) {}

    pakon::Result<pakon::ppb::Reply> exchange(const pakon::ppb::Frame& frame) override {
        auto bytes = frame.serialize();
        if (!bytes) {
            return pakon::failure<pakon::ppb::Reply>(bytes.error().kind,
                                                     bytes.error().message);
        }
        sent_.push_back(to_hex(*bytes));
        const auto& sent = sent_.back();
        if (pos_ >= script_.size()) {
            return pakon::failure<pakon::ppb::Reply>(
                ErrorKind::usb_io_failed,
                std::format("script exhausted after {} frames; got unexpected {}",
                            pos_, sent));
        }
        const auto& step = script_[pos_++];
        if (step.req != sent) {
            return pakon::failure<pakon::ppb::Reply>(
                ErrorKind::ppb_unexpected_reply,
                std::format("frame {}: expected {}, got {}", pos_ - 1, step.req, sent));
        }
        return pakon::ppb::parse_reply(ReplayUsbTransport::from_hex(step.rsp), frame.type);
    }

    const std::vector<std::string>& sent() const { return sent_; }
    bool exhausted() const { return pos_ == script_.size(); }
    std::size_t position() const { return pos_; }

    static std::string to_hex(std::span<const std::uint8_t> bytes) {
        std::string out;
        for (const auto b : bytes) {
            out += std::format("{:02x}", b);
        }
        return out;
    }

private:
    std::vector<Step> script_;
    std::size_t pos_{0};
    std::vector<std::string> sent_;
};

} // namespace pakon::test
