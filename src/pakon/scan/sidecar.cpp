#include "pakon/scan/sidecar.hpp"

#include <format>

namespace pakon::scan {
namespace {

std::string geometry(const Geometry& g) {
    return std::format(
        R"({{"start":{},"end":{},"resample":{},"binning":{},"ir":{},"pixels":{},"channels":{}}})",
        g.start, g.end, g.resample, g.binning, g.ir, g.pixels(), g.channels());
}

std::string duties(const Duties& d) {
    return std::format(R"({{"r":{:.6f},"g":{:.6f},"b":{:.6f},"ir":{:.6f}}})", d.r, d.g, d.b, d.ir);
}

std::string reference(const LineAverage& a) {
    std::string s = std::format(R"({{"lines":{},"pixels":{},"planes":[)", a.lines, a.pixels);
    for (std::size_t c = 0; c < a.planes.size(); ++c) {
        s += c ? ",[" : "[";
        for (std::size_t p = 0; p < a.planes[c].size(); ++p) {
            s += (p ? "," : "") + std::format("{:.2f}", a.planes[c][p]);
        }
        s += "]";
    }
    return s + "]}";
}

std::string optional_line(const std::optional<std::size_t>& v) {
    return v ? std::to_string(*v) : "null";
}

} // namespace

std::string sidecar_json(const ScanResult& r) {
    const auto& u = r.unit;
    const auto& b = u.base[base_index(r.mode.base)];
    const auto& c = r.corrections;
    const auto cal = calibration_geometry(r.mode, b.offset, false);
    const auto& img = r.kind == ScanKind::first_light ? r.white : r.film;
    std::string s = "{\n";
    s += "\"schema\":\"pakon-scan/1\",\n";
    s += std::format("\"mode\":\"{}\",\"kind\":\"{}\",\n", mode_name(r.mode),
                     r.kind == ScanKind::first_light ? "first_light" : "film");
    s += std::format(
        "\"unit\":{{\"serial\":{},\"scanner_type\":{},\"hardware_version\":{},\"offset\":{},"
        "\"motor_speed\":{},\"motor_speed_ir\":{},\"adjust\":[{},{},{},{}],\"fallback\":{}}},\n",
        u.serial, u.scanner_type, u.hardware_version, b.offset, b.motor_speed, b.motor_speed_ir,
        b.adjust[0], b.adjust[1], b.adjust[2], b.adjust[3], u.uses_fallback());
    s += std::format("\"geometry\":{{\"calibration\":{},\"film\":{}}},\n", geometry(cal),
                     geometry(r.film_geometry));
    s += std::format("\"integration\":{},\"line_time_us\":{:.3f},\"motor_rate\":{},\n",
                     integration(r.mode), line_time_us(integration(r.mode)), r.motor_rate);
    s += std::format(
        "\"corrections\":{{\"ad_offsets\":[{},{},{}],\"ad_gains\":[{},{},{}],"
        "\"led_currents\":{{\"r\":{},\"g\":{},\"b\":{},\"ir\":{}}},\"on_time\":{},"
        "\"film_on_time\":{},\n",
        c.offsets[0], c.offsets[1], c.offsets[2], c.gains[0], c.gains[1], c.gains[2],
        c.currents.r, c.currents.g, c.currents.b, c.currents.ir, duties(c.duties),
        duties(r.film_duties));
    s += std::format("  \"dark\":{},\n  \"white\":{}}},\n", reference(c.dark), reference(c.white));
    s += std::format(
        "\"image\":{{\"rows\":{},\"samples_per_row\":{},\"film_start_line\":{},"
        "\"film_end_line\":{},\"film_end\":\"{}\",\"line_period_ms\":{:.4f}}},\n",
        img.rows(), img.geometry.samples_per_row, optional_line(r.film_start_line),
        optional_line(r.film_end_line), to_string(r.film_end), r.line_period_ms);
    s += std::format(
        "\"stream\":{{\"bytes_in\":{},\"overflow_events\":{},\"transfer_errors\":{},"
        "\"resyncs\":{}}}\n",
        r.stream.bytes_in, r.stream.overflow_events, r.stream.transfer_errors, r.resyncs);
    return s + "}\n";
}

} // namespace pakon::scan
