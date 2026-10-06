#include "pakon/scanner/scanner.hpp"

#include <format>

#include "pakon/logging/logger.hpp"
#include "pakon/ppb/packet.hpp"
#include "pakon/protocol/commands.hpp"

namespace pakon::scanner {

using ppb::FrameType;
using ppb::Reply;
using ppb::Status;

std::string_view to_string(Model model) {
    switch (model) {
    case Model::unknown: return "unknown";
    case Model::f135: return "F-135";
    case Model::f135_plus: return "F-135+";
    }
    return "unknown";
}

std::string_view to_string(State state) {
    switch (state) {
    case State::disconnected: return "disconnected";
    case State::connecting: return "connecting";
    case State::ready: return "ready";
    case State::error: return "error";
    }
    return "unknown";
}

std::string ModuleInfo::printable() const {
    std::string out;
    for (const auto b : raw) {
        if (b >= 0x20 && b < 0x7F) {
            out.push_back(static_cast<char>(b));
        }
    }
    return out;
}

void Scanner::transition(State next) {
    if (next != state_) {
        log::Logger::instance().log(log::Level::debug, "state {} -> {}",
                                    to_string(state_), to_string(next));
        state_ = next;
    }
}

Result<std::unique_ptr<Scanner>> Scanner::connect(
    std::unique_ptr<usb::IUsbTransport> transport) {
    auto scanner = std::unique_ptr<Scanner>(new Scanner());
    scanner->client_ = std::make_unique<ppb::Client>(std::move(transport));
    scanner->transition(State::connecting);

    // ppb-protocol.md § "The open handshake" (verbatim capture):
    //   host → 04 03 10 00 85     (HostReset)   → 07 02 10 00
    //   host → 02 04 10 01 8f 00  (HostSetMode) → 07 02 10 00
    // Both replies are first-open-only; treat a timeout as best-effort.
    const auto handshake = [&](const ppb::Frame& frame, const char* what) -> VoidResult {
        auto reply = scanner->client_->exchange(frame);
        if (!reply) {
            if (reply.error().kind == ErrorKind::usb_timeout) {
                log::Logger::instance().log(
                    log::Level::debug,
                    "{} reply timed out (expected on opens after the first; "
                    "best-effort)",
                    what);
                return {};
            }
            return reply.error();
        }
        if (!ppb::is_success(reply->status)) {
            return void_failure(
                ErrorKind::ppb_bad_status,
                std::format("{} rejected with status {}", what,
                            ppb::to_string(reply->status)));
        }
        return {};
    };

    if (auto r = handshake(ppb::make_cmd(protocol::kAddrHost,
                                         protocol::to_byte(protocol::HostCommand::host_reset)),
                           "HostReset");
        !r) {
        scanner->transition(State::error);
        return r.error();
    }
    if (auto r = handshake(ppb::make_write(
                               protocol::kAddrHost,
                               protocol::to_byte(protocol::HostCommand::host_set_mode),
                               std::array<std::uint8_t, 1>{0x00}),
                           "HostSetMode");
        !r) {
        scanner->transition(State::error);
        return r.error();
    }

    scanner->transition(State::ready);
    return scanner;
}

Result<Identity> Scanner::identify() {
    if (state_ != State::ready) {
        return failure<Identity>(ErrorKind::scanner_unexpected_state,
                                 "identify() requires a connected scanner");
    }

    // Presence probes — the documented model detection. Each probe is a
    // CMD with reg 0x00 (ResetMotor) to a candidate motor-controller
    // address; status 0x00 = present, 0x01 = absent. An F-135+ answers
    // 0x44 present / 0x24 absent, inverted on an F-135
    // [CONFIRMED on hardware, August 2026].
    const auto probe = [&](std::uint8_t address) -> Result<bool> {
        auto reply = client_->exchange(ppb::make_cmd(address, 0x00),
                                       FrameType::ack);
        if (!reply) {
            return reply.error();
        }
        if (reply->status == Status::ok) {
            return true;
        }
        if (reply->status == Status::not_acknowledged) {
            return false;
        }
        return failure<bool>(
            ErrorKind::ppb_bad_status,
            std::format("presence probe 0x{:02x} answered status {}", address,
                        ppb::to_string(reply->status)));
    };

    const auto plus_motor = probe(protocol::kAddrPicmPlus);
    if (!plus_motor) {
        return plus_motor.error();
    }
    const auto base_motor = probe(protocol::kAddrPicm);
    if (!base_motor) {
        return base_motor.error();
    }

    Identity identity;
    identity.motor_present = *plus_motor || *base_motor;
    if (*plus_motor && !*base_motor) {
        identity.model = Model::f135_plus;
        identity.addresses = protocol::kF135Plus;
    } else if (*base_motor && !*plus_motor) {
        identity.model = Model::f135;
        identity.addresses = protocol::kF135;
    } else {
        // Both or neither answered: do not guess. Report and leave the
        // model unknown rather than assuming an address set.
        identity.model = Model::unknown;
        log::Logger::instance().log(
            log::Level::warn,
            "presence probes inconclusive: 0x44 present={}, 0x24 present={}",
            *plus_motor, *base_motor);
    }

    // PICL = PICM − 4 (paired address constants, ppb-protocol.md; the OEM
    // engine derives it the same way). Only used when the model is known.
    if (identity.model != Model::unknown) {
        identity.light_present = true;

        const auto read_module = [&](std::uint8_t address)
            -> Result<std::optional<ModuleInfo>> {
            auto reply = client_->exchange(
                ppb::make_read(address, 12, 0x07), FrameType::read);
            if (!reply) {
                return reply.error();
            }
            if (reply->status == Status::not_acknowledged) {
                return std::optional<ModuleInfo>{};
            }
            if (!ppb::is_success(reply->status)) {
                return failure<std::optional<ModuleInfo>>(
                    ErrorKind::ppb_bad_status,
                    std::format("module-info read from 0x{:02x}: status {}",
                                address, ppb::to_string(reply->status)));
            }
            if (reply->payload.size() != 12) {
                return failure<std::optional<ModuleInfo>>(
                    ErrorKind::ppb_unexpected_reply,
                    std::format("module-info read from 0x{:02x}: expected 12 "
                                "payload bytes, got {}",
                                address, reply->payload.size()));
            }
            ModuleInfo info;
            std::copy(reply->payload.begin(), reply->payload.end(), info.raw.begin());
            return std::optional<ModuleInfo>{info};
        };

        if (auto light = read_module(identity.addresses.light); !light) {
            return light.error();
        } else {
            identity.light_module = *light;
            identity.light_present = light->has_value();
        }
        if (auto motor = read_module(identity.addresses.motor); !motor) {
            return motor.error();
        } else {
            identity.motor_module = *motor;
        }

        // Bridge info: READ HOST reg 0x03, 2 bytes — observed as
        // 0f 03 in the capture corpus; semantics not documented.
        auto bridge = client_->exchange(ppb::make_read(protocol::kAddrHost, 2, 0x03),
                                        FrameType::read);
        if (bridge && ppb::is_success(bridge->status) &&
            bridge->payload.size() == 2) {
            identity.bridge_info = std::array<std::uint8_t, 2>{
                bridge->payload[0], bridge->payload[1]};
        } else if (bridge) {
            log::Logger::instance().log(
                log::Level::debug, "bridge info read answered status {}",
                ppb::to_string(bridge->status));
        }
    }

    identity_ = identity;
    log::Logger::instance().log(
        log::Level::info,
        "identified {} (light={} motor={} light_present={})",
        to_string(identity.model),
        std::format("0x{:02x}", identity.addresses.light),
        std::format("0x{:02x}", identity.addresses.motor),
        identity.light_present);
    return identity;
}

Result<StatusReport> Scanner::status() {
    if (state_ != State::ready) {
        return failure<StatusReport>(ErrorKind::scanner_unexpected_state,
                                     "status() requires a connected scanner");
    }
    if (identity_.model == Model::unknown) {
        return failure<StatusReport>(ErrorKind::scanner_unexpected_state,
                                     "identify() must succeed before status()");
    }

    const auto poll = [&](std::uint8_t address) -> Result<std::uint8_t> {
        auto reply = client_->exchange(ppb::make_read_status(address),
                                       FrameType::read_status);
        if (!reply) {
            return reply.error();
        }
        return static_cast<std::uint8_t>(reply->status);
    };

    StatusReport report;

    if (auto r = poll(protocol::kAddrHost); !r) {
        return r.error();
    } else {
        report.host_poll = *r;
    }
    if (auto r = poll(identity_.addresses.light); !r) {
        return r.error();
    } else {
        report.light_poll = *r;
    }
    if (auto r = poll(identity_.addresses.motor); !r) {
        return r.error();
    } else {
        report.motor_poll = *r;
    }

    const auto light = identity_.addresses.light;

    // READ 0x83 CCD status (1 byte), 0x84 light status (2 bytes),
    // 0x88 temperature (4 bytes) — command-reference.md tables.
    const auto read_fixed = [&](std::uint8_t reg, std::size_t size,
                                const char* what)
        -> Result<std::optional<std::vector<std::uint8_t>>> {
        auto reply = client_->exchange(ppb::make_read(light, static_cast<std::uint8_t>(size), reg),
                                       FrameType::read);
        if (!reply) {
            return reply.error();
        }
        if (reply->status == Status::not_acknowledged) {
            return std::optional<std::vector<std::uint8_t>>{};
        }
        if (!ppb::is_success(reply->status)) {
            return failure<std::optional<std::vector<std::uint8_t>>>(
                ErrorKind::ppb_bad_status,
                std::format("read 0x{:02x} ({}): status {}", reg, what,
                            ppb::to_string(reply->status)));
        }
        if (reply->payload.size() != size) {
            return failure<std::optional<std::vector<std::uint8_t>>>(
                ErrorKind::ppb_unexpected_reply,
                std::format("read 0x{:02x} ({}): expected {} payload bytes, "
                            "got {}",
                            reg, what, size, reply->payload.size()));
        }
        return std::optional<std::vector<std::uint8_t>>{reply->payload};
    };

    if (auto r = read_fixed(0x83, 1, "CCD status"); !r) {
        return r.error();
    } else if (r->has_value()) {
        report.ccd_status = (*r)->at(0);
    }
    if (auto r = read_fixed(0x84, 2, "light status"); !r) {
        return r.error();
    } else if (r->has_value()) {
        report.light_status = std::array<std::uint8_t, 2>{(*r)->at(0), (*r)->at(1)};
    }
    if (auto r = read_fixed(0x88, 4, "temperature"); !r) {
        return r.error();
    } else if (r->has_value()) {
        report.temperature = std::array<std::uint8_t, 4>{
            (*r)->at(0), (*r)->at(1), (*r)->at(2), (*r)->at(3)};
    }

    log::Logger::instance().log(
        log::Level::debug,
        "status: host=0x{:02x} light=0x{:02x} motor=0x{:02x} ccd={}",
        report.host_poll, report.light_poll, report.motor_poll,
        report.ccd_status ? std::format("0x{:02x}", *report.ccd_status)
                          : std::string("<n/a>"));
    return report;
}

void Scanner::disconnect() {
    if (state_ != State::disconnected) {
        // No documented teardown frame exists for closing the session;
        // the OEM simply closes the handle. Release the transport.
        client_.reset();
        transition(State::disconnected);
    }
}

} // namespace pakon::scanner
