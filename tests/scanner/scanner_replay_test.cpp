// Scanner connect/identify/status tests against recorded exchanges.
//
// The replayed pairs are from alibosworth/pakon-captures,
// captures/f135plus-serial16402-4expneg-20260820/base4.jsonl (the OEM
// stack driving a real F-135+, serial 16402), plus the documented
// F-135-address probe described in pakon-reference/docs/ppb-protocol.md
// § "Presence probes are model detection" [CONFIRMED on hardware,
// August 2026: an F-135+ answers 0x44 present and 0x24 absent].
//
// If the implementation sends anything other than the recorded request
// bytes, ReplayUsbTransport fails the exchange and the test fails.

#include "pakon/scanner/scanner.hpp"
#include "support/replay_transport.hpp"
#include "support/test_harness.hpp"

#include <array>

using pakon::test::ReplayUsbTransport;

namespace {

// Script the exchanges of a Plus-unit session, in order.
void script_plus_unit_session(ReplayUsbTransport& t) {
    // Open handshake (capture lines 1-4):
    t.expect("0403100085", "07021000");     // HostReset
    t.expect("020410018f00", "07021000");   // HostSetMode
    // Presence probes (capture lines 5-6 show the 0x44 probe; the 0x24
    // probe answering 0x01 on a Plus is the documented inverted result):
    t.expect("0403440000", "07024400");
    t.expect("0403240000", "07022401");
    // Module-info reads (capture lines 19-26):
    t.expect("0103400c07", "010e40080f0a05000031323334350000");
    t.expect("0103440c07", "010e4408100605000031323334350000");
    // Bridge info (capture lines 7-8):
    t.expect("0103100203", "010410080f03");
    // Status polls (corpus: 030110 -> 03031000aa, 8037x; 030140 ->
    // 03024000; 030144 -> 03024400):
    t.expect("030110", "03031000aa");
    t.expect("030140", "03024000");
    t.expect("030144", "03024400");
    // Status registers (corpus):
    t.expect("0103400183", "0103400800");   // CCD status
    t.expect("0103400284", "010440088002"); // light status
    t.expect("0103400488", "0106400882023801"); // temperature
}

std::unique_ptr<ReplayUsbTransport> make_transport() {
    auto t = std::make_unique<ReplayUsbTransport>();
    script_plus_unit_session(*t);
    return t;
}

} // namespace

PAKON_TEST(connect_runs_documented_open_handshake) {
    auto transport = make_transport();
    auto result = pakon::scanner::Scanner::connect(std::move(transport));
    EXPECT(result.has_value());
    if (result) {
        EXPECT((*result)->state() == pakon::scanner::State::ready);
        (*result)->disconnect();
        EXPECT((*result)->state() == pakon::scanner::State::disconnected);
    }
}

PAKON_TEST(identify_detects_f135_plus) {
    auto transport = make_transport();
    auto scanner = pakon::scanner::Scanner::connect(std::move(transport));
    EXPECT(scanner.has_value());
    if (!scanner) {
        return;
    }

    auto identity = (*scanner)->identify();
    EXPECT(identity.has_value());
    if (!identity) {
        return;
    }
    EXPECT(identity->model == pakon::scanner::Model::f135_plus);
    EXPECT(identity->motor_present);
    EXPECT(identity->light_present);
    EXPECT_EQ(identity->addresses.light, pakon::protocol::kAddrPiclPlus);
    EXPECT_EQ(identity->addresses.motor, pakon::protocol::kAddrPicmPlus);
    EXPECT(identity->light_module.has_value());
    EXPECT(identity->motor_module.has_value());
    if (identity->light_module) {
        // Captured payload contains the ASCII run "12345".
        EXPECT_EQ(identity->light_module->printable(), std::string("12345"));
    }
    EXPECT(identity->bridge_info.has_value());
    if (identity->bridge_info) {
        EXPECT_EQ((*identity->bridge_info)[0], std::uint8_t{0x0f});
        EXPECT_EQ((*identity->bridge_info)[1], std::uint8_t{0x03});
    }
}

PAKON_TEST(status_reads_documented_registers) {
    auto transport = make_transport();
    auto scanner = pakon::scanner::Scanner::connect(std::move(transport));
    EXPECT(scanner.has_value());
    if (!scanner) {
        return;
    }
    EXPECT((*scanner)->identify().has_value());

    auto status = (*scanner)->status();
    EXPECT(status.has_value());
    if (!status) {
        return;
    }
    EXPECT_EQ(status->host_poll, std::uint8_t{0x00});
    EXPECT_EQ(status->light_poll, std::uint8_t{0x00});
    EXPECT_EQ(status->motor_poll, std::uint8_t{0x00});
    EXPECT(status->ccd_status.has_value());
    EXPECT(status->light_status.has_value());
    EXPECT(status->temperature.has_value());
    if (status->light_status) {
        EXPECT_EQ((*status->light_status)[0], std::uint8_t{0x80});
        EXPECT_EQ((*status->light_status)[1], std::uint8_t{0x02});
    }
}

PAKON_TEST(identify_refuses_unknown_model_when_both_probes_present) {
    auto t = std::make_unique<ReplayUsbTransport>();
    t->expect("0403100085", "07021000");
    t->expect("020410018f00", "07021000");
    // Pathological: both motor addresses answer present → do not guess.
    t->expect("0403440000", "07024400");
    t->expect("0403240000", "07022400");

    auto scanner = pakon::scanner::Scanner::connect(std::move(t));
    EXPECT(scanner.has_value());
    if (!scanner) {
        return;
    }
    auto identity = (*scanner)->identify();
    EXPECT(identity.has_value());
    if (identity) {
        EXPECT(identity->model == pakon::scanner::Model::unknown);
    }
}

PAKON_TEST(status_before_identify_fails_cleanly) {
    auto transport = make_transport();
    auto scanner = pakon::scanner::Scanner::connect(std::move(transport));
    EXPECT(scanner.has_value());
    if (!scanner) {
        return;
    }
    auto status = (*scanner)->status();
    EXPECT(!status.has_value());
    if (!status) {
        EXPECT(status.error().kind == pakon::ErrorKind::scanner_unexpected_state);
    }
}

PAKON_TEST(client_refuses_bootloader_destination) {
    // Safety: bootloader addresses are never addressed by this stack
    // (type-4 frames with command bytes 0x0C–0x0F erase flash rows —
    // a real unit lost motor firmware this way).
    auto transport = make_transport();
    pakon::ppb::Client client(std::move(transport));
    auto reply = client.exchange(pakon::ppb::make_cmd(pakon::protocol::kAddrPicmPlusBoot,
                                                     0x00));
    EXPECT(!reply.has_value());
    if (!reply) {
        EXPECT(reply.error().kind == pakon::ErrorKind::ppb_invalid_frame);
    }
}

PAKON_TEST(client_refuses_eeprom_bus_destination) {
    auto transport = make_transport();
    pakon::ppb::Client client(std::move(transport));
    auto reply = client.exchange(pakon::ppb::make_write(0xA2, 0x00, std::array<std::uint8_t, 1>{0x00}));
    EXPECT(!reply.has_value());
    if (!reply) {
        EXPECT(reply.error().kind == pakon::ErrorKind::ppb_invalid_frame);
    }
}

int main() {
    return pakon::test::run_all();
}
