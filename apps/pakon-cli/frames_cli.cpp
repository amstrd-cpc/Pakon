#include "frames_cli.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "pakon/image/tiff.hpp"
#include "pakon/process/frames.hpp"
#include "pakon/process/json.hpp"
#include "pakon/process/pakraw.hpp"

namespace pakon::cli {

namespace {

namespace fs = std::filesystem;

std::string frames_json(const process::FrameSplit& s, const std::string& raw) {
    std::string j = std::format(
        "{{\"schema\":\"pakon-frames/1\",\"source\":\"{}\",\"pitch_seed\":{:.1f},\"pitch\":{:.1f},"
        "\"base\":{:.4f},\"gaps_on_grid\":{},\n\"gaps\":[",
        fs::path(raw).filename().string(), s.pitch_seed, s.pitch, s.base, s.gaps_on_grid);
    for (std::size_t i = 0; i < s.gaps.size(); ++i) {
        j += std::format("{}[{},{}]", i ? "," : "", s.gaps[i].first, s.gaps[i].last);
    }
    j += "],\n\"frames\":[\n";
    for (std::size_t i = 0; i < s.frames.size(); ++i) {
        const auto& f = s.frames[i];
        j += std::format("{}  {{\"index\":{},\"kind\":\"{}\",\"first_line\":{},\"end_line\":{},"
                         "\"leading_gap_seen\":{},\"trailing_gap_seen\":{}}}",
                         i ? ",\n" : "", i + 1, process::to_string(f.kind), f.first, f.last,
                         f.leading_gap_seen, f.trailing_gap_seen);
    }
    return j + "\n]}\n";
}

// One frame, rows [first, last): RGB transposed so width = travel, plus
// the IR lane when present. Samples as scanned (linear negative).
VoidResult write_frame(process::PakrawReader& raw, const process::ScanInfo& info,
                       const process::Frame& f, const std::string& rgb_path,
                       const std::string& ir_path) {
    const std::size_t w = f.last - f.first;
    const std::size_t h = info.pixels;
    std::vector<std::uint16_t> rgb(w * h * 3);
    std::vector<std::uint16_t> ir(info.ir ? w * h : 0);
    std::vector<std::uint16_t> row(raw.samples_per_row());
    for (std::size_t r = 0; r < w; ++r) {
        if (auto rr = raw.read_row(f.first + r, row); !rr) {
            return rr;
        }
        for (std::size_t p = 0; p < h; ++p) {
            for (std::size_t c = 0; c < 3; ++c) {
                rgb[(p * w + r) * 3 + c] = row[3 * p + c];
            }
            if (info.ir) {
                ir[p * w + r] = row[3 * h + p];
            }
        }
    }
    if (auto t = image::write_tiff16(rgb_path, static_cast<std::uint32_t>(w),
                                     static_cast<std::uint32_t>(h), 3, rgb);
        !t) {
        return t;
    }
    if (info.ir) {
        return image::write_tiff16(ir_path, static_cast<std::uint32_t>(w),
                                   static_cast<std::uint32_t>(h), 1, ir);
    }
    return {};
}

} // namespace

void print_frames_usage(std::FILE* out) {
    std::fprintf(out,
                 "  frames <prefix> [--out-dir DIR] [--detect-only]\n"
                 "             split a scanned film window into its pictures (offline:\n"
                 "             reads <prefix>.film.pakraw + <prefix>.scan.json, opens no\n"
                 "             device); writes <prefix>.frameNN.tiff (linear 16-bit raw\n"
                 "             negative), .frameNN.ir.tiff in IR modes, <prefix>.frames.json\n");
}

int run_frames_command(const std::vector<std::string>& args, std::FILE* out, std::FILE* err) {
    std::string prefix;
    std::string out_dir;
    bool detect_only = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--out-dir" && i + 1 < args.size()) {
            out_dir = args[++i];
        } else if (args[i] == "--detect-only") {
            detect_only = true;
        } else if (prefix.empty() && !args[i].starts_with("--")) {
            prefix = args[i];
        } else {
            std::fprintf(err, "error: frames: unexpected argument '%s'\n", args[i].c_str());
            print_frames_usage(err);
            return 2;
        }
    }
    if (prefix.empty()) {
        print_frames_usage(err);
        return 2;
    }
    const std::string raw_path = prefix + ".film.pakraw";
    const std::string side_path = prefix + ".scan.json";

    std::ifstream sf(side_path, std::ios::binary);
    if (!sf) {
        std::fprintf(err, "error: frames: cannot read %s\n", side_path.c_str());
        return 1;
    }
    std::stringstream text;
    text << sf.rdbuf();
    auto json = process::parse_json(text.str());
    if (!json) {
        std::fprintf(err, "error: frames: %s: %s\n", side_path.c_str(), json.error().message.c_str());
        return 1;
    }
    auto info = process::scan_info_from_sidecar(*json);
    if (!info) {
        std::fprintf(err, "error: frames: %s: %s\n", side_path.c_str(), info.error().message.c_str());
        return 1;
    }
    auto raw = process::PakrawReader::open(raw_path);
    if (!raw) {
        std::fprintf(err, "error: frames: %s\n", raw.error().message.c_str());
        return 1;
    }
    auto profile = process::line_profile(**raw, *info);
    if (!profile) {
        std::fprintf(err, "error: frames: %s\n", profile.error().message.c_str());
        return 1;
    }
    const auto split = process::find_frames(*profile, *info);
    std::fprintf(out,
                 "frames: %s, %llu lines, film %zu..%s; pitch seed %.0f, fitted %.1f lines; "
                 "base %.4f; %zu gaps (%zu on the grid)\n",
                 info->mode.c_str(), static_cast<unsigned long long>((*raw)->rows()),
                 info->film_start,
                 info->film_end ? std::to_string(*info->film_end).c_str() : "end",
                 split.pitch_seed, split.pitch, split.base, split.gaps.size(),
                 split.gaps_on_grid);
    std::size_t picture = 0;
    for (const auto& f : split.frames) {
        const bool pic = f.kind == process::FrameKind::picture;
        std::fprintf(out, "  %s lines %zu..%zu (%zu)%s\n",
                     pic ? std::format("frame {:2}:", ++picture).c_str()
                         : std::format("{:<9}", std::string(process::to_string(f.kind)) + ":").c_str(),
                     f.first, f.last, f.last - f.first,
                     f.leading_gap_seen && f.trailing_gap_seen ? "" : "  [edge predicted]");
    }

    fs::path base = fs::path(prefix);
    if (!out_dir.empty()) {
        std::error_code ec;
        fs::create_directories(out_dir, ec);
        base = fs::path(out_dir) / fs::path(prefix).filename();
    }
    const std::string stem = base.string();
    int code = 0;
    if (std::FILE* f = std::fopen((stem + ".frames.json").c_str(), "w")) {
        const auto j = frames_json(split, raw_path);
        std::fwrite(j.data(), 1, j.size(), f);
        std::fclose(f);
        std::fprintf(out, "written: %s.frames.json\n", stem.c_str());
    } else {
        std::fprintf(err, "error: frames: cannot write %s.frames.json\n", stem.c_str());
        code = 1;
    }
    if (detect_only) {
        return code;
    }
    std::size_t n = 0;
    for (std::size_t i = 0; i < split.frames.size(); ++i) {
        if (split.frames[i].kind != process::FrameKind::picture) {
            continue; // blank or fogged film: listed in frames.json only
        }
        ++n;
        const auto rgb = std::format("{}.frame{:02}.tiff", stem, n);
        const auto ir = std::format("{}.frame{:02}.ir.tiff", stem, n);
        if (auto w = write_frame(**raw, *info, split.frames[i], rgb, ir); !w) {
            std::fprintf(err, "error: frames: %s: %s\n", rgb.c_str(), w.error().message.c_str());
            code = 1;
            continue;
        }
        std::fprintf(out, "written: %s%s\n", rgb.c_str(), info->ir ? " (+ .ir.tiff)" : "");
    }
    return code;
}

} // namespace pakon::cli
