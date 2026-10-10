// Read-only EEPROM: allow-list, CRC, the OEM primary/backup fallback,
// the fallback-to-capture path, motor-rate derivation, and the exact
// traffic of a read against the simulator (no 0xA2, no write-select).

#include <string>
#include <vector>

#include "pakon/eeprom/eeprom.hpp"
#include "support/sim_device.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace pakon;
using eeprom::CopySource;

} // namespace

PAKON_TEST(crc32_matches_the_zlib_check_value) {
    const std::string s = "123456789";
    const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
    EXPECT_EQ(eeprom::crc32(std::span<const std::uint8_t>(p, s.size())), 0xCBF43926u);
}

PAKON_TEST(allow_list_admits_only_read_select_and_reads) {
    using eeprom::Direction;
    EXPECT(eeprom::is_allowed({Direction::out, 0xA4, 0x00A5, 0x1234}));
    EXPECT(eeprom::is_allowed({Direction::in, 0xA9, 0x0808, 0x1234}));
    EXPECT(!eeprom::is_allowed({Direction::out, 0xA2, 0x0000, 0x1234}));
    EXPECT(!eeprom::is_allowed({Direction::out, 0xA2, 0x0000, 0x0000}));
    EXPECT(!eeprom::is_allowed({Direction::out, 0xA4, 0x00A4, 0x1234})); // write-select
    EXPECT(!eeprom::is_allowed({Direction::out, 0xA4, 0x00A3, 0x1234})); // other chip
    EXPECT(!eeprom::is_allowed({Direction::in, 0xA9, 0x2000, 0x1234}));
    EXPECT(!eeprom::is_allowed({Direction::out, 0xA9, 0x0000, 0x1234}));
}

PAKON_TEST(parse_reads_this_units_fields_from_the_primary) {
    sim::EepromSpec spec;
    const auto img = sim::make_eeprom(spec);
    const auto cal = eeprom::parse(img);
    EXPECT(cal.section_a == CopySource::primary);
    EXPECT(cal.section_b == CopySource::primary);
    EXPECT(!cal.uses_fallback());
    EXPECT_EQ(cal.warnings, 0u);
    EXPECT_EQ(cal.scanner_type, 1351u);
    EXPECT_EQ(cal.serial, 17373u);
    EXPECT_EQ(cal.hardware_version, 400u);
    EXPECT_EQ(cal.base[0].offset, 28);
    EXPECT_EQ(cal.base[0].motor_speed, 25100);
    EXPECT_EQ(cal.base[0].motor_speed_ir, 18800);
    EXPECT_EQ(cal.base[2].offset, 59);
    EXPECT_EQ(cal.base[1].adjust[2], 1000);
}

PAKON_TEST(bad_primary_falls_back_to_the_backup_like_the_oem) {
    sim::EepromSpec spec;
    spec.corrupt_primary_a = true; // unit 16402's single flipped byte
    const auto cal = eeprom::parse(sim::make_eeprom(spec));
    EXPECT(cal.section_a == CopySource::backup);
    EXPECT_EQ(cal.warnings, 0u); // the OEM warns only when both copies fail
    EXPECT_EQ(cal.base[0].motor_speed, 25100);
    EXPECT(!cal.notes.empty());
}

PAKON_TEST(both_copies_bad_uses_the_marked_capture_fallback) {
    sim::EepromSpec spec;
    spec.corrupt_all = true;
    const auto cal = eeprom::parse(sim::make_eeprom(spec));
    EXPECT(cal.uses_fallback());
    EXPECT(cal.section_a == CopySource::fallback);
    EXPECT_EQ(cal.warnings & eeprom::kWarnChecksumBad, eeprom::kWarnChecksumBad);
    EXPECT_EQ(cal.base[0].motor_speed, 25726); // unit 16402, flagged
    EXPECT_EQ(cal.serial, 16402u);

    spec = {};
    spec.blank = true;
    const auto blank = eeprom::parse(sim::make_eeprom(spec));
    EXPECT(blank.uses_fallback());
    EXPECT_EQ(blank.warnings & eeprom::kWarnBlank, eeprom::kWarnBlank);
}

PAKON_TEST(motor_rate_is_speed_times_adjust_over_1000_clamped) {
    eeprom::BaseParams b{30, 25726, 19278, {1000, 1000, 1000, 1000}};
    EXPECT_EQ(eeprom::motor_rate(b, false), 25726); // the captured base4 0x647E
    EXPECT_EQ(eeprom::motor_rate(b, true), 19278);  // the captured base4-IR 0x4B4E
    b.adjust = {1050, 1000, 990, 1000};
    EXPECT_EQ(eeprom::motor_rate(b, false), 27012);
    EXPECT_EQ(eeprom::motor_rate(b, true), 19085);
    b.adjust = {2000, 0, 0, 0}; // clamped to 1100 / 900
    EXPECT_EQ(eeprom::motor_rate(b, false), 28298);
    EXPECT_EQ(eeprom::motor_rate(b, true), 17350);
    b.motor_speed = 10;
    EXPECT_EQ(eeprom::motor_rate(b, false), 1000); // the OEM's floor
}

PAKON_TEST(read_sections_issues_only_allowed_requests_and_matches_the_chip) {
    sim::SimConfig cfg;
    sim::EepromSpec spec;
    spec.corrupt_primary_a = true;
    cfg.eeprom = sim::make_eeprom(spec);
    sim::SimDevice sim(cfg);
    auto image = eeprom::read_sections(sim);
    EXPECT(image.has_value());
    if (image) {
        const auto cal = eeprom::parse(*image);
        EXPECT(cal.section_a == CopySource::backup);
        EXPECT_EQ(cal.base[1].motor_speed, 11200);
    }
    EXPECT(sim.violations().empty());
    EXPECT(sim.unknown_writes().empty());
    // OEM read pattern: A primary (8 + 12×32 + 6), A backup (same),
    // B primary (8 + 28): 2 requests per chunk.
    EXPECT_EQ(sim.control_requests(), 2u * (14 + 14 + 2));
}

int main() { return pakon::test::run_all(); }
