// The simulator answers in the captured forms and enforces the safety
// rules it is there to check: replies byte-compared with capture
// exchanges, LED ceilings, forbidden addresses, stream gating by the
// FPGA acquire bit, FIFO loss when nobody reads.

#include <chrono>
#include <thread>

#include "pakon/ppb/packet.hpp"
#include "support/replay_transport.hpp"
#include "support/sim_device.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace std::chrono_literals;
using pakon::sim::SimConfig;
using pakon::sim::SimDevice;
using pakon::test::ReplayUsbTransport;

std::vector<std::uint8_t> ex(SimDevice& sim, const char* hex) {
    auto r = sim.command_exchange(ReplayUsbTransport::from_hex(hex));
    EXPECT(r.has_value());
    return r ? *r : std::vector<std::uint8_t>{};
}

SimConfig fast() {
    SimConfig c;
    c.command_latency = 0us;
    return c;
}

} // namespace

PAKON_TEST(replies_match_captured_forms) {
    SimDevice sim(fast());
    EXPECT_EQ(ex(sim, "0403100085"), ReplayUsbTransport::from_hex("07021000"));
    EXPECT_EQ(ex(sim, "0403440000"), ReplayUsbTransport::from_hex("07024400"));
    EXPECT_EQ(ex(sim, "0403240000"), ReplayUsbTransport::from_hex("07022401")); // F-135 absent
    EXPECT_EQ(ex(sim, "0103100203"), ReplayUsbTransport::from_hex("010410080f03"));
    // dev-info page select, then the capture's info page (base4.jsonl 5.802/5.808)
    EXPECT_EQ(ex(sim, "020440010301"), ReplayUsbTransport::from_hex("07024000"));
    EXPECT_EQ(ex(sim, "0103400c07"),
              ReplayUsbTransport::from_hex("010e40080f0a05000031323334350000"));
    EXPECT_EQ(ex(sim, "0103400284"), ReplayUsbTransport::from_hex("010440088002"));
    EXPECT(sim.violations().empty());
}

PAKON_TEST(lamp_ready_raises_service_until_acked) {
    SimConfig c = fast();
    c.lamp_ready_delay = 20ms;
    SimDevice sim(c);
    ex(sim, "020740048fe8ff1800"); // lamp temperature init starts the warm-up clock
    EXPECT_EQ(ex(sim, "0103400102"), ReplayUsbTransport::from_hex("0103400800"));
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(ex(sim, "030110"), ReplayUsbTransport::from_hex("03041088aaaa"));
    EXPECT_EQ(ex(sim, "0103400102"), ReplayUsbTransport::from_hex("0103408802"));
    EXPECT_EQ(ex(sim, "02054002060002"), ReplayUsbTransport::from_hex("07024000"));
    EXPECT_EQ(ex(sim, "0103400102"), ReplayUsbTransport::from_hex("0103400800"));
    EXPECT_EQ(ex(sim, "0103400183"), ReplayUsbTransport::from_hex("010340080a"));
}

PAKON_TEST(led_current_above_the_ceiling_is_a_violation_and_clamped) {
    SimDevice sim(fast());
    ex(sim, "020440018001");         // visible only: R4 G20 B20 IR0
    ex(sim, "02084005810600030009"); // capture B=6 R=3 G=9: fine
    EXPECT(sim.violations().empty());
    ex(sim, "0208400581060005000f"); // R=5 > 4
    EXPECT_EQ(sim.violations().size(), 1u);
    EXPECT_EQ(sim.led_currents()[2], 4);
}

PAKON_TEST(stream_runs_only_while_the_acquire_bit_is_set_and_overflows_unread) {
    SimDevice sim(fast());
    ex(sim, "0206440382040300");
    ex(sim, "0206440382050604");
    ex(sim, "0206440382065307");
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(sim.streamed_bytes(), 0u);
    ex(sim, "0206440382006300"); // acquire
    std::this_thread::sleep_for(60ms);
    ex(sim, "0206440382006200"); // idle
    const auto produced = sim.streamed_bytes();
    EXPECT(produced > 100000);
    EXPECT(sim.lost_bytes() > 0); // nobody read: the FX2 FIFO overflowed
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(sim.streamed_bytes(), produced); // stopped on idle
}

PAKON_TEST(forbidden_addresses_and_eeprom_writes_are_violations) {
    SimDevice sim(fast());
    ex(sim, "040346000c");
    EXPECT_EQ(sim.violations().size(), 1u);
    EXPECT(!sim.control_write(0xA2, 0, 0x1234).has_value());
    EXPECT(!sim.control_write(0xA4, 0x00A4, 0x1234).has_value());
    EXPECT_EQ(sim.violations().size(), 3u);
}

int main() { return pakon::test::run_all(); }
