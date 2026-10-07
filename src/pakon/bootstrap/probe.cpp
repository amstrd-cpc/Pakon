// Read-only bootstrap probe — see bootstrap/probe.hpp for the evidence
// and safety rules governing every request issued here.

#include "pakon/bootstrap/probe.hpp"

#include <algorithm>
#include <format>

namespace pakon::bootstrap {

Result<Personality> parse_personality(std::span<const std::uint8_t> raw) {
    if (raw.size() != kPersonalityLength) {
        return failure<Personality>(
            ErrorKind::usb_short_transfer,
            std::format("personality read returned {} bytes, expected {}",
                        raw.size(), kPersonalityLength));
    }
    Personality personality;
    std::copy(raw.begin(), raw.end(), personality.bytes.begin());
    return personality;
}

Result<ProbeReport> probe(usb::IUsbTransport& transport) {
    // The single I/O this module may perform: the documented read-only
    // stage-1 personality read (probe.hpp). Any future probe step must be
    // evidence-backed and added under the same rule.
    auto raw = transport.control_read(kPersonalityRequest, kPersonalityValue,
                                      kPersonalityIndex, kPersonalityLength);
    if (!raw) {
        return raw.error();
    }
    auto personality = parse_personality(*raw);
    if (!personality) {
        return personality.error();
    }
    return ProbeReport{transport.device_info(), *personality};
}

std::string hex_string(std::span<const std::uint8_t> bytes) {
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            out += ' ';
        }
        out += std::format("{:02x}", bytes[i]);
    }
    return out;
}

} // namespace pakon::bootstrap
