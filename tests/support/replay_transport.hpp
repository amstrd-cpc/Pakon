#pragma once

// ReplayUsbTransport: a mock IUsbTransport driven by recorded
// request → reply pairs.
//
// Every pair comes from the pakon-captures corpus (alibosworth/
// pakon-captures, F-135+ serial 16402, OEM-driven sessions) or from
// frames quoted verbatim in pakon-reference. Nothing is fabricated: an
// unexpected request is a test failure, which is exactly what we want
// when the implementation drifts from the documented protocol.

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/usb/transport.hpp"

namespace pakon::test {

class ReplayUsbTransport final : public usb::IUsbTransport {
public:
    using Handler = std::function<Result<std::vector<std::uint8_t>>(
        std::span<const std::uint8_t>)>;

    // Wire a literal request hex (as in the captures) to a literal reply.
    void expect(std::string_view request_hex, std::string_view reply_hex) {
        const auto bytes = from_hex(request_hex);
        const std::string request(bytes.begin(), bytes.end());
        auto reply = from_hex(reply_hex);
        unseen_.insert(request);
        handlers_[request] =
            [reply](std::span<const std::uint8_t>) -> Result<std::vector<std::uint8_t>> {
            return reply;
        };
    }

    // Handler for requests matching a prefix (e.g. all reads of one reg).
    void expect_prefix(std::string_view request_prefix_hex, Handler handler) {
        prefix_handlers_.emplace_back(from_hex(request_prefix_hex), std::move(handler));
    }

    // True when every literal expectation was consumed at least once.
    bool all_expectations_met() const { return !handlers_.empty() && unseen_.empty(); }

    // Every frame the implementation sent, in order (for diagnostics).
    const std::vector<std::vector<std::uint8_t>>& sent() const { return sent_; }

    Result<std::vector<std::uint8_t>>
    command_exchange(std::span<const std::uint8_t> frame) override {
        sent_.emplace_back(frame.begin(), frame.end());

        if (auto it = handlers_.find(std::string(frame.begin(), frame.end()));
            it != handlers_.end()) {
            unseen_.erase(it->first);
            return it->second(frame);
        }
        for (auto& [prefix, handler] : prefix_handlers_) {
            if (frame.size() >= prefix.size() &&
                std::equal(prefix.begin(), prefix.end(), frame.begin())) {
                return handler(frame);
            }
        }
        return failure<std::vector<std::uint8_t>>(
            ErrorKind::usb_io_failed,
            "ReplayUsbTransport: unexpected request (no recorded pair)");
    }

    Result<std::vector<std::uint8_t>> bulk_read(std::uint8_t, std::size_t) override {
        return failure<std::vector<std::uint8_t>>(ErrorKind::usb_not_supported,
                                                  "no image stream in replay");
    }

    Result<std::vector<std::uint8_t>>
    control_read(std::uint8_t, std::uint16_t, std::uint16_t,
                 std::uint16_t) override {
        return failure<std::vector<std::uint8_t>>(ErrorKind::usb_not_supported,
                                                  "no control read scripted");
    }

    Result<void> control_write(std::uint8_t, std::uint16_t, std::uint16_t) override {
        return void_failure(ErrorKind::usb_not_supported,
                            "no control write scripted");
    }

    const usb::DeviceInfo& device_info() const override { return info_; }

    static std::vector<std::uint8_t> from_hex(std::string_view hex) {
        std::vector<std::uint8_t> out;
        auto nibble = [](char c) -> std::uint8_t {
            if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
            return 0;
        };
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
            out.push_back(static_cast<std::uint8_t>((nibble(hex[i]) << 4) |
                                                    nibble(hex[i + 1])));
        }
        return out;
    }

private:
    usb::DeviceInfo info_{}; // tests do not inspect descriptors
    // Keyed by the frame bytes as a string: std::vector<uint8_t>'s <=>
    // trips a GCC 16 -Wstringop-overread false positive at -O2.
    std::map<std::string, Handler> handlers_;
    std::vector<std::pair<std::vector<std::uint8_t>, Handler>> prefix_handlers_;
    std::set<std::string> unseen_;
    std::vector<std::vector<std::uint8_t>> sent_;
};

} // namespace pakon::test
