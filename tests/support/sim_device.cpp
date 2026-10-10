#include "support/sim_device.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <format>

#include "pakon/eeprom/eeprom.hpp"

namespace pakon::sim {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint8_t kHost = 0x10;
constexpr std::uint8_t kPicl = 0x40;
constexpr std::uint8_t kPicm = 0x44;

// Optics model (choices, not evidence): dark code per channel at A/D
// offset 0 and slope per offset step — tuned so the OEM dark servo
// (target 300 ± 32, step trunc(-(m-300)·3/112)) converges in a few
// steps, like the captured ramp; LED efficiency in codes per current
// step at full on-time and gain 1.0 — R saturates near current 4, G near
// 8, B near 5, IR near 6, inside the 0x44 ceilings like the captured
// B=6 R=3 G=9.
constexpr std::array<double, 3> kDarkBase{2270, 2300, 2250};
constexpr std::array<double, 3> kDarkSlope{33, 35, 31};
constexpr std::array<double, 4> kLedEff{17000, 7000, 12000, 6000}; // R G B IR
constexpr std::array<double, 3> kFilmT{0.45, 0.22, 0.11};          // C-41 base

double gain_factor(std::uint16_t code) {
    const double c = std::min<double>(code, 63);
    return 1.0 / (1.0 - c / 75.6); // inverse of TLB@0x100201d0
}

std::uint32_t hash32(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return static_cast<std::uint32_t>(x);
}

void put16(std::vector<std::uint8_t>& v, std::size_t at, std::uint16_t x) {
    v[at] = static_cast<std::uint8_t>(x & 0xFF);
    v[at + 1] = static_cast<std::uint8_t>(x >> 8);
}

void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) {
        v[at + i] = static_cast<std::uint8_t>(x >> (8 * i));
    }
}

void seal(std::vector<std::uint8_t>& img, std::size_t base, std::uint32_t length) {
    put32(img, base, length);
    put32(img, base + 4,
          eeprom::crc32(std::span<const std::uint8_t>(img).subspan(base + 8, length - 8)));
}

} // namespace

std::vector<std::uint8_t> make_eeprom(const EepromSpec& spec) {
    std::vector<std::uint8_t> img(eeprom::kSize, 0xFF);
    if (spec.blank) {
        return img;
    }
    for (const std::size_t base : {std::size_t{0x000}, std::size_t{0x400}}) {
        std::fill(img.begin() + static_cast<std::ptrdiff_t>(base),
                  img.begin() + static_cast<std::ptrdiff_t>(base + 398), 0x00);
        put32(img, base + 0x08, 400);
        put32(img, base + 0x0C, 1351);
        put32(img, base + 0x10, spec.serial);
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t k = 0; k < 3; ++k) {
                put16(img, base + 0x14 + 6 * i + 2 * k, spec.base[i][k]);
            }
        }
        // NegMatrix diagonal + constants, PosMatrix 0.25 diagonal (plausible
        // floats; the scan path does not use them).
        const float neg[30] = {0.29f, 0, 0, 0, 0, 0, 0, 0, 0, 166.f,
                               0, 0.29f, 0, 0, 0, 0, 0, 0, 0, 430.f,
                               0, 0, 0.32f, 0, 0, 0, 0, 0, 0, 638.f};
        std::memcpy(img.data() + base + 0x26, neg, sizeof neg);
        for (std::size_t r = 0; r < 3; ++r) {
            const float q = 0.25f;
            std::memcpy(img.data() + base + 0x9E + 4 * (r * 10 + r), &q, 4);
        }
        seal(img, base, 398);
    }
    for (const std::size_t base : {std::size_t{0x800}, std::size_t{0xA00}}) {
        std::fill(img.begin() + static_cast<std::ptrdiff_t>(base),
                  img.begin() + static_cast<std::ptrdiff_t>(base + 36), 0x00);
        for (std::size_t w = 0; w < 12; ++w) {
            put16(img, base + 0x08 + 2 * w, spec.adjust);
        }
        seal(img, base, 36);
    }
    if (spec.corrupt_primary_a || spec.corrupt_all) {
        img[0x0A5] ^= 0x48; // the single-byte fault seen on unit 16402
    }
    if (spec.corrupt_all) {
        img[0x4A5] ^= 0x48;
        img[0x810] ^= 0x01;
        img[0xA10] ^= 0x01;
    }
    return img;
}

// --- the asynchronous pipe -------------------------------------------------

class SimPipe final : public usb::IBulkInPipe {
public:
    explicit SimPipe(SimDevice& device) : device_(device) {}
    ~SimPipe() override {
        abort();
        std::scoped_lock lock(device_.mutex_);
        device_.pipe_open_ = false;
        device_.completions_.clear();
    }

    VoidResult submit(std::size_t slot, std::span<std::uint8_t> buffer) override {
        std::scoped_lock lock(device_.mutex_);
        device_.pending_.push_back({slot, buffer, 0});
        // Bytes the device already holds go to the new read first.
        while (!device_.fifo_.empty() && !device_.pending_.empty()) {
            auto& head = device_.pending_.front();
            const std::size_t n =
                std::min(device_.fifo_.size(), head.buffer.size() - head.filled);
            std::copy_n(device_.fifo_.begin(), n, head.buffer.begin() +
                                                      static_cast<std::ptrdiff_t>(head.filled));
            device_.fifo_.erase(device_.fifo_.begin(),
                                device_.fifo_.begin() + static_cast<std::ptrdiff_t>(n));
            head.filled += n;
            const bool idle = (device_.control_ & 1) == 0;
            const std::size_t before = device_.pending_.size();
            device_.complete_head_locked(idle);
            if (device_.pending_.size() == before) {
                break; // head still waiting for more bytes
            }
        }
        return {};
    }

    Result<std::optional<usb::PipeCompletion>> wait(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(device_.mutex_);
        device_.completion_cv_.wait_for(lock, timeout,
                                        [&] { return !device_.completions_.empty(); });
        if (device_.completions_.empty()) {
            return std::optional<usb::PipeCompletion>{};
        }
        auto c = std::move(device_.completions_.front());
        device_.completions_.pop_front();
        return std::optional<usb::PipeCompletion>{std::move(c)};
    }

    void abort() override {
        std::scoped_lock lock(device_.mutex_);
        while (!device_.pending_.empty()) {
            usb::PipeCompletion c;
            c.slot = device_.pending_.front().slot;
            c.aborted = true;
            device_.pending_.pop_front();
            device_.completions_.push_back(std::move(c));
        }
        device_.completion_cv_.notify_all();
    }

private:
    SimDevice& device_;
};

// --- device ----------------------------------------------------------------

SimDevice::SimDevice(SimConfig config) : config_(std::move(config)) {
    info_.vendor_id = 0x0F05;
    info_.product_id = 0xF135;
    info_.serial_number = "010-203-04";
    eeprom_ = config_.eeprom.empty() ? make_eeprom() : config_.eeprom;
    if (config_.speed <= 0) {
        config_.speed = 1.0;
    }
    producer_ = std::thread([this] { producer_loop(); });
}

SimDevice::~SimDevice() {
    {
        std::scoped_lock lock(mutex_);
        quit_ = true;
    }
    stream_cv_.notify_all();
    producer_.join();
}

std::vector<std::uint8_t> SimDevice::ack(std::uint8_t address, std::uint8_t status) const {
    return {0x07, 0x02, address, status};
}

std::vector<std::uint8_t> SimDevice::read_reply(std::uint8_t address, std::uint8_t flags,
                                                std::span<const std::uint8_t> payload) const {
    std::vector<std::uint8_t> r{0x01, static_cast<std::uint8_t>(2 + payload.size()), address,
                                flags};
    r.insert(r.end(), payload.begin(), payload.end());
    return r;
}

bool SimDevice::event_pending_locked() const { return picl_status_ != 0 || picm_status_ != 0; }

void SimDevice::service_tick_locked() {
    if (temp_init_ && !lamp_stable_) {
        const auto delay = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(config_.lamp_ready_delay.count() /
                                                      config_.speed));
        if (Clock::now() - temp_init_at_ >= delay) {
            lamp_stable_ = true;
            picl_status_ |= 0x02; // OEM_RE.md §8: lamp ready event
        }
    }
}

Result<std::vector<std::uint8_t>>
SimDevice::command_exchange(std::span<const std::uint8_t> frame) {
    Result<std::vector<std::uint8_t>> reply = std::vector<std::uint8_t>{};
    {
        std::scoped_lock lock(mutex_);
        frames_.emplace_back(frame.begin(), frame.end());
        const std::size_t index = exchange_count_++;
        if (dead_) {
            reply = failure<std::vector<std::uint8_t>>(ErrorKind::usb_io_failed,
                                                       "sim: device gone (sticky fault)");
        } else if (config_.fault_at_frame && config_.fault_at_frame->first == index) {
            if (config_.fault_sticky) {
                dead_ = true;
            }
            switch (config_.fault_at_frame->second) {
            case FaultKind::transport_error:
                reply = failure<std::vector<std::uint8_t>>(ErrorKind::usb_io_failed,
                                                           "sim: injected transport error");
                break;
            case FaultKind::timeout:
                reply = failure<std::vector<std::uint8_t>>(ErrorKind::usb_timeout,
                                                           "sim: injected timeout");
                break;
            case FaultKind::bad_status:
                if (frame.size() >= 3 && frame[0] == 0x01) {
                    reply = std::vector<std::uint8_t>{0x01, 0x02, frame[2], 0x02};
                } else if (frame.size() >= 3 && frame[0] == 0x03) {
                    reply = std::vector<std::uint8_t>{0x03, 0x02, frame[2], 0x02};
                } else {
                    reply = ack(frame.size() >= 3 ? frame[2] : 0, 0x02);
                }
                break;
            }
        } else {
            service_tick_locked();
            reply = handle_locked(frame);
        }
    }
    stream_cv_.notify_all();
    if (config_.command_latency.count() > 0) {
        std::this_thread::sleep_for(std::chrono::duration<double, std::micro>(
            config_.command_latency.count() / config_.speed));
    }
    return reply;
}

Result<std::vector<std::uint8_t>> SimDevice::handle_locked(std::span<const std::uint8_t> frame) {
    if (frame.size() < 3 || frame[0] == 0x00) {
        if (!frame.empty() && frame[0] == 0x00) {
            violations_.push_back("type byte 0 sent - wedges the FX2 bridge");
        }
        return failure<std::vector<std::uint8_t>>(ErrorKind::usb_timeout,
                                                  "sim: no reply to malformed frame");
    }
    const std::uint8_t type = frame[0];
    const std::uint8_t address = frame[2];
    if (frame[1] + 2u != frame.size()) {
        return ack(address, 0x02);
    }
    if (address == 0x22 || address == 0x26 || address == 0x42 || address == 0x46 ||
        address == 0xA2 || address == 0xA4) {
        violations_.push_back(std::format("frame to forbidden address 0x{:02x}", address));
    }
    const bool known = address == kHost || address == kPicl || address == kPicm;

    switch (type) {
    case 0x03: { // READ_STATUS
        if (address == kHost) {
            const auto st = static_cast<std::uint8_t>((event_pending_locked() ? 0x80 : 0x00) |
                                                      (config_.host_poll_six_bytes ? 0x08 : 0));
            if (config_.host_poll_six_bytes) {
                return std::vector<std::uint8_t>{0x03, 0x04, kHost, st, 0xAA, 0xAA};
            }
            return std::vector<std::uint8_t>{0x03, 0x03, kHost, st, 0xAA};
        }
        if (!known) {
            return std::vector<std::uint8_t>{0x03, 0x02, address, 0x01};
        }
        const bool ev = address == kPicl ? picl_status_ != 0 : picm_status_ != 0;
        return std::vector<std::uint8_t>{0x03, 0x02, address,
                                         static_cast<std::uint8_t>(0x08 | (ev ? 0x80 : 0))};
    }
    case 0x01: { // READ [addr][count][reg]
        if (frame.size() != 5) {
            return read_reply(address, 0x02, {});
        }
        if (!known) {
            return read_reply(address, 0x01, {});
        }
        bool ok = true;
        auto payload = read_register(address, frame[4], frame[3], ok);
        if (!ok) {
            return read_reply(address, 0x02, {});
        }
        const bool ev = address == kPicl   ? picl_status_ != 0
                        : address == kPicm ? picm_status_ != 0
                                           : event_pending_locked();
        return read_reply(address, static_cast<std::uint8_t>(0x08 | (ev ? 0x80 : 0)), payload);
    }
    case 0x04: // CMD [addr][0][reg]
        if (frame.size() != 5 || frame[3] != 0) {
            return ack(address, 0x02);
        }
        if (!known) {
            return ack(address, 0x01);
        }
        command(address, frame[4]);
        return ack(address, 0x00);
    case 0x02: { // WRITE [addr][count][reg][payload]
        if (frame.size() < 5 || frame[3] + 5u != frame.size()) {
            return ack(address, 0x02);
        }
        if (!known) {
            return ack(address, 0x01);
        }
        write_register(address, frame[4], frame.subspan(5));
        return ack(address, 0x00);
    }
    default:
        return ack(address, 0x02);
    }
}

void SimDevice::command(std::uint8_t address, std::uint8_t reg) {
    if (address == kPicl && reg == 0x8A) { // second half of bDrvResetFifos
        fifo_.clear();
        line_pos_ = line_.size(); // the next byte starts a fresh line
        return;
    }
    if (address == kPicl && reg == 0x92) { dx_running_ = false; return; }
    if (address == kPicm && (reg == 0xA0 || reg == 0xA1)) { going_ = rate_ > 0; return; }
    if (address == kPicm && reg == 0xA2) { going_ = false; return; }
    if (reg == 0x00) { return; }                     // presence probe
    if (address == kHost && reg == 0x85) { return; } // HostReset
    unknown_writes_.push_back(std::format("CMD 0x{:02x} reg 0x{:02x}", address, reg));
}

void SimDevice::set_control(std::uint16_t value) {
    const bool was = (control_ & 1) != 0;
    control_ = value;
    const bool now = (control_ & 1) != 0;
    if (was && !now) {
        // Acquisition stopped: a partly filled read completes short and
        // the next acquisition starts on a line boundary.
        complete_head_locked(true);
        line_pos_ = line_.size();
    }
    if (!was && now) {
        last_tick_ = Clock::now();
        budget_ = 0;
        line_pos_ = line_.size();
    }
}

void SimDevice::write_register(std::uint8_t address, std::uint8_t reg,
                               std::span<const std::uint8_t> p) {
    const auto u16 = [&](std::size_t i) {
        return static_cast<std::uint16_t>(p[i] | (p[i + 1] << 8));
    };
    if (address == kHost) {
        if ((reg == 0x8F || reg == 0x84) && p.size() == 1) { return; }
    } else if (address == kPicl) {
        switch (reg) {
        case 0x80: if (p.size() == 1) { lamp_mask_ = p[0] & 3; return; } break;
        case 0x81:
            if (p.size() == 5) {
                // Board 0x44 ceilings, IR state from the lamp mask
                // (TLB@0x100203c0): [B, IR, R, 0, G].
                const bool ir = (lamp_mask_ & 2) != 0;
                const std::array<std::uint8_t, 5> ceil =
                    ir ? std::array<std::uint8_t, 5>{24, 8, 8, 0, 24}
                       : std::array<std::uint8_t, 5>{20, 0, 4, 0, 20};
                for (std::size_t i = 0; i < 5; ++i) {
                    if (p[i] > ceil[i]) {
                        violations_.push_back(std::format(
                            "LED current slot {} = {} above the ceiling {} (IR {})", i, p[i],
                            ceil[i], ir));
                    }
                    currents_[i] = std::min(p[i], ceil[i]);
                }
                return;
            }
            break;
        case 0x82:
            if (p.size() == 12) {
                for (std::size_t i = 0; i < 6; ++i) { ontime_[i] = u16(2 * i); }
                return;
            }
            break;
        case 0x06:
            if (p.size() == 2) {
                const std::uint8_t st = p[1];
                picl_status_ = static_cast<std::uint8_t>(picl_status_ & ~st);
                return;
            }
            break;
        case 0x03: if (p.size() == 1) { info_page_[0] = p[0] == 1; return; } break;
        case 0x87: if (p.size() == 2) { return; } break;
        case 0x89: if (p.size() == 1) { resample_ = p[0] == 1; return; } break;
        case 0x8B: case 0x8C: case 0x8D: case 0x8F:
            if (p.size() == 4) {
                if (!temp_init_) {
                    temp_init_ = true;
                    temp_init_at_ = Clock::now();
                }
                return;
            }
            break;
        case 0xD0:
            if (p.size() == 1) {
                if (p[0] != 0x00) { violations_.push_back("TEC 0xD0 written != 00"); }
                return;
            }
            break;
        case 0xD1:
            if (p.size() == 1) {
                if (p[0] != 0x01) { violations_.push_back("TEC 0xD1 written != 01"); }
                return;
            }
            break;
        case 0x91: if (p.size() == 3) { dx_running_ = true; return; } break;
        default: break;
        }
    } else if (address == kPicm) {
        switch (reg) {
        case 0x82:
            if (p.size() == 3) {
                const std::uint16_t v = u16(1);
                switch (p[0]) {
                case 0: set_control(v); return;
                case 4: pixel_start_ = v; return;
                case 5: pixel_end_ = v; return;
                case 6: integration_ = v; return;
                case 9: leds_ = v; return;
                case 1: case 2: case 3: case 10: case 11: return;
                default: break;
                }
            }
            break;
        case 0x84:
            if (p.size() == 3) {
                const std::uint16_t v = u16(1);
                if (p[0] <= 1) { return; }
                if (p[0] <= 4) {
                    if (v > 0x3F) { violations_.push_back("A/D gain above 0x3F"); }
                    gains_[p[0] - 2] = v;
                    return;
                }
                if (p[0] <= 7) {
                    const int mag = v & 0xFF;
                    offsets_[p[0] - 5] = (v & 0x100) ? -mag : mag;
                    return;
                }
            }
            break;
        case 0x97: if (p.size() == 1) { return; } break;
        case 0x03: if (p.size() == 1) { info_page_[1] = p[0] == 1; return; } break;
        case 0xA5: if (p.size() == 2) { rate_ = u16(0); return; } break;
        default: break;
        }
    }
    std::string bytes;
    for (auto b : p) { bytes += std::format("{:02x}", b); }
    unknown_writes_.push_back(std::format("WRITE 0x{:02x} reg 0x{:02x} {}", address, reg, bytes));
}

std::vector<std::uint8_t> SimDevice::read_register(std::uint8_t address, std::uint8_t reg,
                                                   std::uint8_t count, bool& ok) {
    std::vector<std::uint8_t> out;
    auto want = [&](std::size_t n) {
        if (count != n) { ok = false; }
    };
    if (address == kHost && reg == 0x03) {
        want(2);
        out = {0x0F, 0x03};
    } else if (reg == 0x07 && (address == kPicl || address == kPicm)) {
        want(12);
        const bool page = info_page_[address == kPicm ? 1 : 0];
        if (page) { // the capture's info page: (b[2], b[1]) = firmware version
            out = address == kPicl
                      ? std::vector<std::uint8_t>{0x0F, 0x0A, 0x05, 0, 0, '1', '2', '3', '4', '5', 0, 0}
                      : std::vector<std::uint8_t>{0x10, 0x06, 0x05, 0, 0, '1', '2', '3', '4', '5', 0, 0};
        } else { // the lab unit's answer without the page select
            out = address == kPicl
                      ? std::vector<std::uint8_t>{0x82, 0x02, 0xD4, 0x01, 0x04, 0xC0, 0x21, 0x02, 0, 0, 0x92, 0}
                      : std::vector<std::uint8_t>{0x02, 0x20, 0x00, 0xA0, 0x00, 0x8C, 0x08, 0, 0, 0x20, 0, 0};
        }
    } else if (address == kPicl && reg == 0x02) {
        want(1);
        out = {picl_status_};
    } else if (address == kPicm && reg == 0x02) {
        want(1);
        out = {picm_status_};
    } else if (address == kPicl && reg == 0x83) {
        want(1);
        out = {static_cast<std::uint8_t>((lamp_stable_ ? 0x02 : 0) | (temp_init_ ? 0x08 : 0))};
    } else if (address == kPicl && reg == 0x84) {
        want(2);
        out = {0x80, 0x02};
    } else if (address == kPicl && reg == 0x88) {
        want(4);
        out = {0x82, 0x02, 0xD2, 0x01};
    } else if (address == kPicl && reg == 0x90) {
        want(30);
        out.assign(30, 0);
        out[0] = static_cast<std::uint8_t>(dx_counter_ & 0xFF);
        out[1] = static_cast<std::uint8_t>(dx_counter_ >> 8);
        out[2] = 1;
        out[3] = dx_flags_;
        ++dx_counter_;
    } else {
        ok = false;
        unknown_writes_.push_back(
            std::format("READ 0x{:02x} reg 0x{:02x} x{}", address, reg, count));
    }
    return out;
}

// --- EP0 -------------------------------------------------------------------

VoidResult SimDevice::control_write(std::uint8_t request, std::uint16_t value,
                                    std::uint16_t index) {
    std::scoped_lock lock(mutex_);
    ++control_count_;
    if (request == eeprom::kRequestWrite) {
        violations_.push_back(std::format("EEPROM write request 0xA2 (wValue 0x{:04x})", value));
        return void_failure(ErrorKind::usb_io_failed, "sim: 0xA2 refused");
    }
    if (request == eeprom::kRequestSelect) {
        if ((value & 1) == 0) {
            violations_.push_back(std::format("EEPROM write-select 0x{:04x}", value));
            return void_failure(ErrorKind::usb_io_failed, "sim: write-select refused");
        }
        eeprom_selected_ = index == eeprom::kIndex && value == eeprom::kSelectRead;
        return {};
    }
    unknown_writes_.push_back(std::format("EP0 OUT 0x{:02x}", request));
    return void_failure(ErrorKind::usb_io_failed, "sim: unexpected vendor OUT");
}

Result<std::vector<std::uint8_t>> SimDevice::control_read(std::uint8_t request,
                                                          std::uint16_t value,
                                                          std::uint16_t index,
                                                          std::uint16_t length) {
    std::scoped_lock lock(mutex_);
    ++control_count_;
    if (request == eeprom::kRequestRead && index == eeprom::kIndex) {
        if (!eeprom_selected_) {
            violations_.push_back("EEPROM read without a read-select");
        }
        if (length > eeprom::kMaxChunk || value + length > eeprom_.size()) {
            return failure<std::vector<std::uint8_t>>(ErrorKind::usb_io_failed,
                                                      "sim: EEPROM read out of range");
        }
        return std::vector<std::uint8_t>(eeprom_.begin() + value,
                                         eeprom_.begin() + value + length);
    }
    if (request == 0xA9 && index == 0 && length == 8) { // stage-1 personality
        return std::vector<std::uint8_t>{0xC0, 0x05, 0x0F, 0x35, 0xF2, 0x07, 0xAA, 0x04};
    }
    unknown_writes_.push_back(std::format("EP0 IN 0x{:02x}", request));
    return failure<std::vector<std::uint8_t>>(ErrorKind::usb_io_failed,
                                              "sim: unexpected vendor IN");
}

// --- EP6 -------------------------------------------------------------------

Result<std::unique_ptr<usb::IBulkInPipe>> SimDevice::open_bulk_in(std::uint8_t endpoint,
                                                                  std::size_t) {
    std::scoped_lock lock(mutex_);
    if (endpoint != 0x86 || pipe_open_) {
        return failure<std::unique_ptr<usb::IBulkInPipe>>(ErrorKind::usb_open_failed,
                                                          "sim: image pipe unavailable");
    }
    pipe_open_ = true;
    return std::unique_ptr<usb::IBulkInPipe>(new SimPipe(*this));
}

Result<std::vector<std::uint8_t>> SimDevice::bulk_read(std::uint8_t endpoint,
                                                       std::size_t max_length) {
    // The legacy synchronous path (one read at a time): drains the device
    // FIFO, waiting up to the old 2 s pipe deadline.
    if (endpoint != 0x86) {
        return failure<std::vector<std::uint8_t>>(ErrorKind::usb_io_failed, "sim: bad endpoint");
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(2000);
    while (Clock::now() < deadline) {
        {
            std::scoped_lock lock(mutex_);
            if (!fifo_.empty()) {
                const std::size_t n = std::min(max_length, fifo_.size());
                std::vector<std::uint8_t> out(fifo_.begin(),
                                              fifo_.begin() + static_cast<std::ptrdiff_t>(n));
                fifo_.erase(fifo_.begin(), fifo_.begin() + static_cast<std::ptrdiff_t>(n));
                return out;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return failure<std::vector<std::uint8_t>>(ErrorKind::usb_timeout, "sim: no image data");
}

std::size_t SimDevice::line_samples_locked() const {
    const std::size_t raw = pixel_end_ > pixel_start_ ? pixel_end_ - pixel_start_ : 0;
    const std::size_t px = resample_ ? raw * 3 / 4 : raw;
    return px * ((control_ & 0x100) ? 4 : 3);
}

double SimDevice::line_time_us_locked() const {
    return std::max<double>(integration_, 100) * 0.48; // OEM_RE.md §9
}

void SimDevice::complete_head_locked(bool short_ok) {
    if (pending_.empty()) {
        return;
    }
    auto& head = pending_.front();
    if (head.filled == head.buffer.size() || (short_ok && head.filled > 0)) {
        usb::PipeCompletion c;
        c.slot = head.slot;
        c.bytes = head.filled;
        if (config_.stream_fault_after_bytes && streamed_ >= *config_.stream_fault_after_bytes) {
            c.failed = true;
            c.error = "sim: injected stream fault";
        }
        pending_.pop_front();
        completions_.push_back(std::move(c));
        completion_cv_.notify_all();
    }
}

void SimDevice::deliver_locked(std::span<const std::uint8_t> bytes) {
    streamed_ += bytes.size();
    while (!bytes.empty() && !pending_.empty()) {
        auto& head = pending_.front();
        const std::size_t n = std::min(bytes.size(), head.buffer.size() - head.filled);
        std::copy_n(bytes.begin(), n,
                    head.buffer.begin() + static_cast<std::ptrdiff_t>(head.filled));
        head.filled += n;
        bytes = bytes.subspan(n);
        complete_head_locked(false);
    }
    for (const auto b : bytes) {
        if (fifo_.size() >= config_.device_fifo_bytes) {
            ++lost_;
        } else {
            fifo_.push_back(b);
        }
    }
}

void SimDevice::produce_line_locked() {
    const std::size_t samples = line_samples_locked();
    const bool ir = (control_ & 0x100) != 0;
    const std::size_t px = ir ? samples / 4 : samples / 3;
    const bool binned = (control_ & 2) != 0;
    const double span = binned ? 1030.0 : 2060.0;
    const bool vis = (lamp_mask_ & 1) != 0;
    const bool irlit = (lamp_mask_ & 2) != 0;

    // Film in the gate: lines move only while the drive runs.
    if (going_ && rate_ > 0) {
        ++travel_lines_;
        if (config_.film) {
            const auto& f = *config_.film;
            if (travel_lines_ == f.lead_lines) {
                picl_status_ |= 0x20;
                dx_flags_ = 0x10;
            }
            if (travel_lines_ == f.lead_lines + f.film_lines) {
                picl_status_ |= 0x20;
                dx_flags_ = 0;
            }
        }
    }
    const bool film_in = config_.film && travel_lines_ >= config_.film->lead_lines &&
                         travel_lines_ < config_.film->lead_lines + config_.film->film_lines;

    const double base = ontime_[5] == 0 ? 1.0 : ontime_[5];
    // currents/ontime slots: [B, IR, R, 0, G]
    const std::array<double, 4> light{
        vis ? kLedEff[0] * currents_[2] * std::min(1.0, ontime_[2] / base) : 0.0,
        vis ? kLedEff[1] * currents_[4] * std::min(1.0, ontime_[4] / base) : 0.0,
        vis ? kLedEff[2] * currents_[0] * std::min(1.0, ontime_[0] / base) : 0.0,
        irlit ? kLedEff[3] * currents_[1] * std::min(1.0, ontime_[1] / base) : 0.0};
    std::array<double, 3> dark{};
    for (std::size_t c = 0; c < 3; ++c) {
        dark[c] = std::max(kDarkBase[c] + kDarkSlope[c] * offsets_[c], 0.0);
    }
    const double row = 0.6 + 0.4 * std::sin(static_cast<double>(travel_lines_) / 53.0);

    line_.resize(samples * 2);
    const auto put = [&](std::size_t i, double v, bool marker) {
        std::uint32_t s = static_cast<std::uint32_t>(std::clamp(v, 0.0, 65535.0));
        const std::uint32_t noise = hash32((lines_ << 20) ^ i) & 1u;
        s = marker ? (s | 1u) : ((s & ~1u) | noise);
        line_[2 * i] = static_cast<std::uint8_t>(s & 0xFF);
        line_[2 * i + 1] = static_cast<std::uint8_t>(s >> 8);
    };
    for (std::size_t p = 0; p < px; ++p) {
        const double x = pixel_start_ + (resample_ ? p * 4.0 / 3.0 : static_cast<double>(p));
        const bool masked = x < 18.0;
        const double xn = x / span;
        const double profile = 1.0 - 0.15 * (2 * xn - 1) * (2 * xn - 1);
        const double pattern = film_in ? row * (0.7 + 0.3 * static_cast<double>((p / 37) % 2)) : 1.0;
        for (std::size_t c = 0; c < 3; ++c) {
            const double t = film_in ? kFilmT[c] * pattern : 1.0;
            const double lit = masked ? 0.0 : light[c] * gain_factor(gains_[c]) * profile * t;
            put(3 * p + c, dark[c] + lit, p == 0 && c == 0);
        }
        if (ir) {
            const double lit =
                masked ? 0.0 : light[3] * gain_factor(gains_[1]) * profile * (film_in ? 0.8 : 1.0);
            put(3 * px + p, dark[1] + lit, false);
        }
    }
    line_pos_ = 0;
    ++lines_;
}

void SimDevice::producer_loop() {
    std::unique_lock lock(mutex_);
    while (!quit_) {
        if ((control_ & 1) == 0) {
            stream_cv_.wait_for(lock, std::chrono::milliseconds(2));
            continue;
        }
        const auto now = Clock::now();
        const double line_bytes = std::max<double>(static_cast<double>(line_samples_locked()) * 2.0, 2.0);
        const double rate = line_bytes / (line_time_us_locked() * 1e-6) * config_.speed;
        budget_ += std::chrono::duration<double>(now - last_tick_).count() * rate;
        // At most 20 ms of backlog: when the host machine cannot generate
        // lines at the configured rate the simulated device just runs
        // slower (never a burst that holds the lock for long).
        budget_ = std::min(budget_, rate * 0.02);
        last_tick_ = now;
        std::size_t emitted = 0;
        while (budget_ >= 512.0 && (control_ & 1) != 0 && emitted < 64 * 1024) {
            emitted += 512;
            std::uint8_t packet[512];
            std::size_t n = 0;
            while (n < sizeof packet) {
                if (line_pos_ >= line_.size()) {
                    produce_line_locked();
                }
                const std::size_t take = std::min(sizeof packet - n, line_.size() - line_pos_);
                std::memcpy(packet + n, line_.data() + line_pos_, take);
                line_pos_ += take;
                n += take;
            }
            deliver_locked(std::span<const std::uint8_t>(packet, n));
            budget_ -= 512.0;
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        lock.lock();
    }
}

// --- observation -------------------------------------------------------------

std::vector<std::vector<std::uint8_t>> SimDevice::frames() const {
    std::scoped_lock lock(mutex_);
    return frames_;
}
std::vector<std::string> SimDevice::violations() const {
    std::scoped_lock lock(mutex_);
    return violations_;
}
std::vector<std::string> SimDevice::unknown_writes() const {
    std::scoped_lock lock(mutex_);
    return unknown_writes_;
}
std::uint64_t SimDevice::lost_bytes() const {
    std::scoped_lock lock(mutex_);
    return lost_;
}
std::uint64_t SimDevice::streamed_bytes() const {
    std::scoped_lock lock(mutex_);
    return streamed_;
}
std::uint64_t SimDevice::lines_produced() const {
    std::scoped_lock lock(mutex_);
    return lines_;
}
bool SimDevice::acquiring() const {
    std::scoped_lock lock(mutex_);
    return (control_ & 1) != 0;
}
bool SimDevice::lamp_lit() const {
    std::scoped_lock lock(mutex_);
    return lamp_mask_ != 0;
}
bool SimDevice::motor_running() const {
    std::scoped_lock lock(mutex_);
    return going_;
}
std::array<std::uint8_t, 5> SimDevice::led_currents() const {
    std::scoped_lock lock(mutex_);
    return currents_;
}
std::array<int, 3> SimDevice::ad_offsets() const {
    std::scoped_lock lock(mutex_);
    return offsets_;
}
std::array<std::uint16_t, 3> SimDevice::ad_gains() const {
    std::scoped_lock lock(mutex_);
    return gains_;
}
std::uint16_t SimDevice::motor_rate() const {
    std::scoped_lock lock(mutex_);
    return rate_;
}
std::size_t SimDevice::control_requests() const {
    std::scoped_lock lock(mutex_);
    return control_count_;
}

} // namespace pakon::sim
