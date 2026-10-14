#include "pakon/process/positive.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace pakon::process {

namespace {

constexpr std::size_t kBins = 4000; // density 0..4 in 0.001 steps
constexpr double kBinWidth = 0.001;
constexpr double kMinT = 1e-4;

// Flat-field factor per channel and film pixel: t = (raw - dark) * k.
struct FlatField {
    std::array<std::vector<double>, 3> dark;
    std::array<std::vector<double>, 3> scale;

    explicit FlatField(const ScanInfo& info) {
        for (std::size_t c = 0; c < 3; ++c) {
            dark[c].resize(info.pixels);
            scale[c].resize(info.pixels);
            const double boost = info.film_on_time[c] > 0 && info.calibration_on_time[c] > 0
                                     ? info.calibration_on_time[c] / info.film_on_time[c]
                                     : 1.0;
            for (std::size_t p = 0; p < info.pixels; ++p) {
                const std::size_t q = p + info.reference_shift;
                const double d = q < info.dark[c].size() ? info.dark[c][q] : 0.0;
                const double w = q < info.white[c].size() ? info.white[c][q] : 65535.0;
                dark[c][p] = d;
                scale[c][p] = boost / std::max(1.0, w - d);
            }
        }
    }
    double t(std::size_t c, std::size_t p, std::uint16_t v) const {
        return std::max(kMinT, (v - dark[c][p]) * scale[c][p]);
    }
};

VoidResult check(const PakrawReader& raw, const ScanInfo& info) {
    const std::size_t channels = info.ir ? 4 : 3;
    if (raw.samples_per_row() != info.pixels * channels) {
        return void_failure(ErrorKind::image_bad_input,
                            std::format("{} samples per row, sidecar says {} pixels x {}",
                                        raw.samples_per_row(), info.pixels, channels));
    }
    for (std::size_t c = 0; c < 3; ++c) {
        if (info.dark[c].size() < info.pixels + info.reference_shift ||
            info.white[c].size() < info.pixels + info.reference_shift) {
            return void_failure(ErrorKind::image_bad_input,
                                "sidecar lacks the R/G/B dark and white references");
        }
    }
    return {};
}

double percentile(const std::vector<std::uint64_t>& hist, double q) {
    std::uint64_t total = 0;
    for (const auto n : hist) {
        total += n;
    }
    if (total == 0) {
        return 0.0;
    }
    const auto target = static_cast<std::uint64_t>(q * static_cast<double>(total));
    std::uint64_t seen = 0;
    for (std::size_t b = 0; b < hist.size(); ++b) {
        seen += hist[b];
        if (seen > target) {
            return (static_cast<double>(b) + 0.5) * kBinWidth;
        }
    }
    return static_cast<double>(hist.size()) * kBinWidth;
}

double srgb(double y) {
    y = std::clamp(y, 0.0, 1.0);
    return y <= 0.0031308 ? 12.92 * y : 1.055 * std::pow(y, 1.0 / 2.4) - 0.055;
}

} // namespace

Result<PositiveParams> measure_positive(PakrawReader& raw, const ScanInfo& info,
                                        const FrameSplit& split, const PositiveOptions& opt) {
    if (auto c = check(raw, info); !c) {
        return c.error();
    }
    const FlatField ff(info);
    PositiveParams pp;
    pp.range_decades = opt.range_decades;
    std::vector<std::uint16_t> row(raw.samples_per_row());
    const std::size_t from = info.pixels / 10;
    const std::size_t to = info.pixels - info.pixels / 10;

    // Base: mean transmission over the gap lines (inner half of each run,
    // away from the frame edges).
    std::array<double, 3> sum{};
    std::size_t n = 0;
    for (const auto& g : split.gaps) {
        const std::size_t len = g.last - g.first;
        for (std::size_t r = g.first + len / 4; r < g.last - len / 4 && r < raw.rows(); r += 2) {
            if (auto rr = raw.read_row(r, row); !rr) {
                return rr.error();
            }
            for (std::size_t p = from; p < to; p += 4) {
                for (std::size_t c = 0; c < 3; ++c) {
                    sum[c] += ff.t(c, p, row[3 * p + c]);
                }
                ++n;
            }
            ++pp.base_lines;
        }
    }
    if (n == 0) {
        return failure<PositiveParams>(ErrorKind::image_bad_input,
                                       "no bare-base gap lines to measure the film base on");
    }
    for (std::size_t c = 0; c < 3; ++c) {
        pp.base[c] = sum[c] / static_cast<double>(n);
    }

    // Densities above base over every picture.
    std::array<std::vector<std::uint64_t>, 3> hist;
    for (auto& h : hist) {
        h.assign(kBins, 0);
    }
    for (const auto& f : split.frames) {
        if (f.kind != FrameKind::picture) {
            continue;
        }
        for (std::size_t r = f.first; r < f.last && r < raw.rows(); r += 4) {
            if (auto rr = raw.read_row(r, row); !rr) {
                return rr.error();
            }
            for (std::size_t p = 0; p < info.pixels; p += 4) {
                for (std::size_t c = 0; c < 3; ++c) {
                    const double d = -std::log10(ff.t(c, p, row[3 * p + c]) / pp.base[c]);
                    const auto b = static_cast<std::size_t>(std::clamp(d / kBinWidth, 0.0,
                                                                       static_cast<double>(kBins - 1)));
                    ++hist[c][b];
                }
            }
        }
    }
    double span = 0;
    for (std::size_t c = 0; c < 3; ++c) {
        pp.d_low[c] = percentile(hist[c], opt.low_percentile);
        pp.d_mid[c] = percentile(hist[c], 0.5);
        pp.d_high[c] = std::max(pp.d_low[c] + 0.05, percentile(hist[c], opt.high_percentile));
        span += pp.d_high[c] - pp.d_low[c];
    }
    pp.span = span / 3.0;
    double mid = 0;
    for (std::size_t c = 0; c < 3; ++c) {
        mid += (pp.d_mid[c] - pp.d_low[c]) / pp.span;
    }
    pp.mid_x = mid / 3.0;
    pp.line_shift = static_cast<int>(std::lround(kRowSpacingPerPitch * split.pitch));
    pp.per_frame_exposure = opt.per_frame_exposure;
    pp.max_exposure_shift = opt.max_exposure_shift;
    return pp;
}

Result<std::vector<std::uint16_t>> develop_frame(PakrawReader& raw, const ScanInfo& info,
                                                 const PositiveParams& pp, const Frame& f,
                                                 Feed feed) {
    if (auto c = check(raw, info); !c) {
        return c.error();
    }
    const FlatField ff(info);
    const std::size_t w = f.last - f.first;
    const std::size_t h = info.pixels;
    const std::size_t spr = raw.samples_per_row();
    const auto s = static_cast<std::size_t>(std::max(0, pp.line_shift));

    // The frame plus s lines either side (R and B come from neighbours).
    const std::size_t lo = f.first >= s ? f.first - s : 0;
    const std::size_t hi = std::min<std::size_t>(static_cast<std::size_t>(raw.rows()), f.last + s);
    std::vector<std::uint16_t> rows((hi - lo) * spr);
    for (std::size_t r = lo; r < hi; ++r) {
        if (auto rr = raw.read_row(r, std::span<std::uint16_t>(rows).subspan((r - lo) * spr, spr)); !rr) {
            return rr.error();
        }
    }
    const auto sample = [&](std::size_t line, std::ptrdiff_t shift, std::size_t p, std::size_t c) {
        const auto want = static_cast<std::ptrdiff_t>(line) + shift;
        const auto clamped = std::clamp<std::ptrdiff_t>(want, static_cast<std::ptrdiff_t>(lo),
                                                        static_cast<std::ptrdiff_t>(hi) - 1);
        return rows[static_cast<std::size_t>(clamped - static_cast<std::ptrdiff_t>(lo)) * spr + 3 * p + c];
    };
    const std::array<std::ptrdiff_t, 3> shift{-static_cast<std::ptrdiff_t>(s), 0,
                                              static_cast<std::ptrdiff_t>(s)};
    const auto x_of = [&](std::size_t line, std::size_t p, std::size_t c) {
        const double d = -std::log10(ff.t(c, p, sample(line, shift[c], p, c)) / pp.base[c]);
        return (d - pp.d_mid[c]) / pp.span + pp.mid_x;
    };

    // Per-frame exposure: the 99.5 % of the brightest channel to 1.
    double exposure = 0;
    if (pp.per_frame_exposure) {
        std::vector<double> xs;
        for (std::size_t r = f.first; r < f.last; r += 4) {
            for (std::size_t p = h / 20; p < h - h / 20; p += 4) {
                xs.push_back(std::max({x_of(r, p, 0), x_of(r, p, 1), x_of(r, p, 2)}));
            }
        }
        if (!xs.empty()) {
            const auto k = static_cast<std::ptrdiff_t>(0.995 * static_cast<double>(xs.size() - 1));
            std::nth_element(xs.begin(), xs.begin() + k, xs.end());
            exposure = std::clamp(1.0 - xs[static_cast<std::size_t>(k)], -pp.max_exposure_shift,
                                  pp.max_exposure_shift);
        }
    }

    const double floor = std::pow(10.0, -pp.range_decades);
    std::vector<std::uint16_t> out(w * h * 3);
    for (std::size_t r = 0; r < w; ++r) {
        const std::size_t x_out = picture_col(r, w, feed);
        for (std::size_t p = 0; p < h; ++p) {
            const std::size_t y_out = picture_row(p, h, feed);
            for (std::size_t c = 0; c < 3; ++c) {
                const double x = std::clamp(x_of(f.first + r, p, c) + exposure, 0.0, 1.0);
                const double y = (std::pow(10.0, pp.range_decades * (x - 1.0)) - floor) / (1.0 - floor);
                out[(y_out * w + x_out) * 3 + c] =
                    static_cast<std::uint16_t>(std::lround(srgb(y) * 65535.0));
            }
        }
    }
    return out;
}

} // namespace pakon::process
