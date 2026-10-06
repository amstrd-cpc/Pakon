#pragma once

// PPB bus addresses.
//
// Source: pakon-reference/docs/ppb-protocol.md § "Bus participants"
// ([DOCUMENTED] from the OEM driver's address constants and observed in
// captures) and docs/scanner-family.md.
//
//   Address   Device                        Present on
//   0x10      HOST (the bridge itself)      all models
//   0x20/0x22 PICL / PICL bootloader        F-135
//   0x24/0x26 PICM / PICM bootloader        F-135
//   0x40/0x42 PICL+ / bootloader            F-135+
//   0x44/0x46 PICM+ / bootloader            F-135+
//
// SAFETY: the bootloader addresses (0x22/0x26/0x42/0x46) answer on the
// same command channel as everything else; type-4 packets to them with
// command bytes 0x0C–0x0F erase flash rows (a real unit lost a motor
// firmware row). Writes to unknown bus addresses can hit the I2C EEPROMs
// (0xA2 boot personality, 0xA4 per-unit EEPROM — the shifted addresses)
// and have erased a real unit's personality. Never send to anything not
// listed as a known controller here.

#include <cstdint>

namespace pakon::protocol {

inline constexpr std::uint8_t kAddrHost = 0x10;

// F-135 (base) controllers
inline constexpr std::uint8_t kAddrPicl = 0x20;
inline constexpr std::uint8_t kAddrPiclBoot = 0x22;
inline constexpr std::uint8_t kAddrPicm = 0x24;
inline constexpr std::uint8_t kAddrPicmBoot = 0x26;

// F-135+ controllers
inline constexpr std::uint8_t kAddrPiclPlus = 0x40;
inline constexpr std::uint8_t kAddrPiclPlusBoot = 0x42;
inline constexpr std::uint8_t kAddrPicmPlus = 0x44;
inline constexpr std::uint8_t kAddrPicmPlusBoot = 0x46;

// The two controller address sets are the central F-135 vs F-135+
// difference: "the same commands, sent to different addresses"
// (ppb-protocol.md) [INFERRED] from paired address constants.
struct ControllerAddresses {
    std::uint8_t light;          // PICL or PICL+
    std::uint8_t light_bootloader;
    std::uint8_t motor;          // PICM or PICM+
    std::uint8_t motor_bootloader;
};

inline constexpr ControllerAddresses kF135{
    kAddrPicl, kAddrPiclBoot, kAddrPicm, kAddrPicmBoot};

inline constexpr ControllerAddresses kF135Plus{
    kAddrPiclPlus, kAddrPiclPlusBoot, kAddrPicmPlus, kAddrPicmPlusBoot};

// Bus addresses that are safe to *read/poll* (documented controllers).
// Bootloaders are excluded from ordinary use entirely.
inline constexpr std::uint8_t kKnownControllerAddresses[] = {
    kAddrHost, kAddrPicl, kAddrPicm, kAddrPiclPlus, kAddrPicmPlus,
};

// I2C EEPROM addresses as they appear on the shifted PPB bus
// (per-unit-data-and-safety.md: "0xA2 = boot EEPROM at 0x51,
// 0xA4 = per-unit EEPROM at 0x52"). NEVER write to these.
inline constexpr std::uint8_t kAddrBootEeprom = 0xA2;
inline constexpr std::uint8_t kAddrPerUnitEeprom = 0xA4;

} // namespace pakon::protocol
