#include "pakon/process/frames.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace pakon::process {

namespace {

std::vector<double> numbers(const Json& j) {
    std::vector<double> out;
    if (const auto* a = j.array()) {
        out.reserve(a->size());
        for (const auto& v : *a) {
            out.push_back(v.number().value_or(0.0));
        }
    }
    return out;
}

} // namespace

Result<ScanInfo> scan_info_from_sidecar(const Json& s) {
    ScanInfo info;
    if (s["schema"].string() != std::optional<std::string>("pakon-scan/1") ||
        s["kind"].string() != std::optional<std::string>("film")) {
        return failure<ScanInfo>(ErrorKind::image_bad_input,
                                 "sidecar is not a pakon-scan/1 film window");
    }
    info.mode = s["mode"].string().value_or("");
    const auto& img = s["image"];
    info.film_start = static_cast<std::size_t>(img["film_start_line"].number().value_or(0));
    if (const auto e = img["film_end_line"].number()) {
        info.film_end = static_cast<std::size_t>(*e);
    }
    info.motor_rate = s["motor_rate"].number().value_or(0);
    info.line_period_ms = img["line_period_ms"].number().value_or(0);
    const auto& film = s["geometry"]["film"];
    const auto& cal = s["geometry"]["calibration"];
    info.pixels = static_cast<std::size_t>(film["pixels"].number().value_or(0));
    info.ir = film["ir"].boolean().value_or(false);
    const double shift = film["start"].number().value_or(0) - cal["start"].number().value_or(0);
    info.reference_shift = shift > 0 ? static_cast<std::size_t>(shift) : 0;
    info.dark_green = numbers(s["corrections"]["dark"]["planes"][1]);
    info.white_green = numbers(s["corrections"]["white"]["planes"][1]);
    if (info.pixels == 0 || info.motor_rate <= 0 || info.line_period_ms <= 0 ||
        info.dark_green.size() < info.pixels + info.reference_shift ||
        info.white_green.size() < info.pixels + info.reference_shift) {
        return failure<ScanInfo>(ErrorKind::image_bad_input,
                                 "sidecar lacks the film geometry, motor rate, line period or "
                                 "the dark/white references");
    }
    return info;
}

Result<std::vector<LineStats>> line_profile(PakrawReader& raw, const ScanInfo& info) {
    const std::size_t channels = info.ir ? 4 : 3;
    if (raw.samples_per_row() != info.pixels * channels) {
        return failure<std::vector<LineStats>>(
            ErrorKind::image_bad_input,
            std::format("{} samples per row, sidecar says {} pixels x {}", raw.samples_per_row(),
                        info.pixels, channels));
    }
    std::vector<std::size_t> idx;
    std::vector<double> dark, span;
    for (std::size_t p = info.pixels / 10; p < info.pixels - info.pixels / 10; p += 2) {
        idx.push_back(p);
        const double d = info.dark_green[p + info.reference_shift];
        dark.push_back(d);
        span.push_back(std::max(1.0, info.white_green[p + info.reference_shift] - d));
    }
    std::vector<std::uint16_t> row(raw.samples_per_row());
    std::vector<double> t(idx.size());
    std::vector<LineStats> out;
    out.reserve(static_cast<std::size_t>(raw.rows()));
    for (std::uint64_t r = 0; r < raw.rows(); ++r) {
        if (auto rr = raw.read_row(r, row); !rr) {
            return rr.error();
        }
        double sum = 0;
        for (std::size_t k = 0; k < idx.size(); ++k) {
            t[k] = (row[3 * idx[k] + 1] - dark[k]) / span[k];
            sum += t[k];
        }
        const auto n = t.size();
        std::nth_element(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(n / 10), t.end());
        const double p10 = t[n / 10];
        std::nth_element(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(9 * n / 10), t.end());
        const double p90 = t[9 * n / 10];
        out.push_back({static_cast<float>(sum / static_cast<double>(n)), static_cast<float>(p10),
                       static_cast<float>(p90)});
    }
    return out;
}

FrameSplit find_frames(std::span<const LineStats> profile, const ScanInfo& info) {
    FrameSplit fs;
    const std::size_t start = std::min(info.film_start, profile.size());
    const std::size_t end = std::min(info.film_end.value_or(profile.size()), profile.size());
    fs.pitch_seed = kFramePitchSecondsAtRate25676 * 25676.0 / info.motor_rate /
                    (info.line_period_ms / 1000.0);
    fs.pitch = fs.pitch_seed;
    if (end <= start + 1) {
        return fs;
    }

    // Base transmission: median mean of the uniform lines in the film.
    std::vector<double> uniform;
    for (std::size_t i = start; i < end; ++i) {
        const auto& l = profile[i];
        if (l.mean > 0 && (l.p90 - l.p10) / l.mean < 0.2f) {
            uniform.push_back(l.mean);
        }
    }
    if (uniform.empty()) {
        return fs;
    }
    std::nth_element(uniform.begin(), uniform.begin() + static_cast<std::ptrdiff_t>(uniform.size() / 2),
                     uniform.end());
    fs.base = uniform[uniform.size() / 2];

    // Gap runs.
    std::vector<GapRun> runs;
    for (std::size_t i = start; i < end;) {
        const auto is_gap = [&](std::size_t k) {
            return profile[k].p10 >= 0.85 * fs.base && profile[k].p90 <= 1.15 * fs.base;
        };
        if (!is_gap(i)) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < end && is_gap(j)) {
            ++j;
        }
        if (!runs.empty() && static_cast<double>(i - runs.back().last) < 0.03 * fs.pitch_seed) {
            runs.back().last = j;
        } else {
            runs.push_back({i, j});
        }
        i = j;
    }
    for (const auto& r : runs) {
        if (r.last - r.first >= 8) {
            fs.gaps.push_back(r);
        }
    }
    const auto centre = [](const GapRun& g) { return 0.5 * static_cast<double>(g.first + g.last); };

    // Fit phase and pitch to the gaps.
    double anchor = fs.gaps.empty() ? static_cast<double>(start) : centre(fs.gaps.front());
    double pitch = fs.pitch_seed;
    std::vector<std::pair<long, double>> best_hits;
    for (const auto& a : fs.gaps) {
        for (int k = -50; k <= 50; ++k) {
            const double p = fs.pitch_seed * (1.0 + k / 1000.0);
            std::vector<std::pair<long, double>> hits;
            for (const auto& g : fs.gaps) {
                const long n = std::lround((centre(g) - centre(a)) / p);
                if (std::abs(centre(g) - (centre(a) + static_cast<double>(n) * p)) <= 0.04 * p) {
                    hits.emplace_back(n, centre(g));
                }
            }
            if (hits.size() > best_hits.size()) {
                best_hits = std::move(hits);
                anchor = centre(a);
                pitch = p;
            }
        }
    }
    if (best_hits.size() >= 2) {
        double mn = 0, mc = 0;
        for (const auto& [n, c] : best_hits) {
            mn += static_cast<double>(n);
            mc += c;
        }
        mn /= static_cast<double>(best_hits.size());
        mc /= static_cast<double>(best_hits.size());
        double num = 0, den = 0;
        for (const auto& [n, c] : best_hits) {
            num += (static_cast<double>(n) - mn) * (c - mc);
            den += (static_cast<double>(n) - mn) * (static_cast<double>(n) - mn);
        }
        if (den > 0) {
            pitch = num / den;
            anchor = mc - pitch * mn;
        }
    }
    fs.pitch = pitch;
    fs.gaps_on_grid = best_hits.size();

    // Grid gaps, snapped to the runs seen.
    struct GridGap {
        double at;
        std::optional<GapRun> run;
    };
    std::vector<GridGap> grid;
    const long n0 = static_cast<long>(std::floor((static_cast<double>(start) - anchor) / pitch)) - 1;
    const long n1 = static_cast<long>(std::ceil((static_cast<double>(end) - anchor) / pitch)) + 1;
    for (long n = n0; n <= n1; ++n) {
        const double g = anchor + static_cast<double>(n) * pitch;
        GridGap gg{g, std::nullopt};
        double best = 0.06 * pitch;
        for (const auto& r : fs.gaps) {
            if (std::abs(centre(r) - g) <= best) {
                best = std::abs(centre(r) - g);
                gg.run = r;
            }
        }
        grid.push_back(gg);
    }
    for (std::size_t k = 0; k + 1 < grid.size(); ++k) {
        const auto& a = grid[k];
        const auto& b = grid[k + 1];
        double lo = a.run ? static_cast<double>(a.run->last) : a.at;
        double hi = b.run ? static_cast<double>(b.run->first) : b.at;
        lo = std::max(lo, static_cast<double>(start));
        hi = std::min(hi, static_cast<double>(end));
        if (hi - lo >= 0.8 * pitch) {
            fs.frames.push_back({static_cast<std::size_t>(lo), static_cast<std::size_t>(hi),
                                 a.run.has_value(), b.run.has_value()});
        }
    }
    return fs;
}

} // namespace pakon::process
