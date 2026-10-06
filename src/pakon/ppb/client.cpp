#include "pakon/ppb/client.hpp"

#include <format>

#include "pakon/logging/logger.hpp"
#include "pakon/protocol/addresses.hpp"

namespace pakon::ppb {

bool is_allowed_destination(std::uint8_t address) {
    for (const auto allowed : protocol::kKnownControllerAddresses) {
        if (address == allowed) {
            return true;
        }
    }
    return false;
}

Client::Client(std::unique_ptr<usb::IUsbTransport> transport)
    : transport_(std::move(transport)) {}

Result<Reply> Client::exchange(const Frame& request, FrameType expected_reply) {
    auto result = exchange(request);
    if (!result) {
        return result;
    }
    if (result->type != expected_reply) {
        // exchange() already validated the mirror mapping; this catches
        // callers passing a wrong expectation.
        return failure<Reply>(
            ErrorKind::ppb_unexpected_reply,
            std::format("expected reply type 0x{:02x}, got 0x{:02x}",
                        static_cast<unsigned>(expected_reply),
                        static_cast<unsigned>(result->type)));
    }
    return result;
}

Result<Reply> Client::exchange(const Frame& request) {
    auto wire = request.serialize();
    if (!wire) {
        return wire.error();
    }

    // serialize() guarantees data is non-empty, so front() is the address.
    if (!is_allowed_destination(request.data.front())) {
        return failure<Reply>(
            ErrorKind::ppb_invalid_frame,
            std::format("destination 0x{:02x} not in the controller allow-list "
                        "(safety rule: bootloaders and EEPROM bus addresses are "
                        "never written)",
                        request.data.front()));
    }

    auto reply_bytes = transport_->command_exchange(*wire);
    if (!reply_bytes) {
        return reply_bytes.error();
    }

    auto reply = parse_reply(*reply_bytes, request.type);
    if (!reply) {
        log::Logger::instance().hex(log::Level::error, "BAD-RX", 0x81,
                                    std::span<const std::uint8_t>(*reply_bytes));
        return reply;
    }

    // "In each, data[0] echoes the address" [DOCUMENTED]. Also guards
    // against a stale reply from an earlier exchange being consumed here.
    if (reply->address != request.data.front()) {
        log::Logger::instance().hex(log::Level::error, "BAD-RX", 0x81,
                                    std::span<const std::uint8_t>(*reply_bytes));
        return failure<Reply>(
            ErrorKind::ppb_unexpected_reply,
            std::format("reply address 0x{:02x} does not echo request "
                        "destination 0x{:02x}",
                        reply->address, request.data.front()));
    }

    if (!is_success(reply->status) && reply->status != Status::not_acknowledged) {
        // not_acknowledged is a meaningful, expected answer to presence
        // probes (absent controller); everything else non-success is
        // surfaced with context.
        log::Logger::instance().log(
            log::Level::warn,
            "reply status {} (0x{:02x}) from device 0x{:02x}",
            to_string(reply->status), static_cast<unsigned>(reply->status),
            reply->address);
    }
    return reply;
}

} // namespace pakon::ppb
