// Simulated scans against a device that streams on its own clock.
//
// These two cases are written against the runner as it stands at this
// commit and are EXPECTED TO FAIL (CTest WILL_FAIL) — they demonstrate
// the root causes before the fix:
//   F1 the image stream is not drained while the command block runs
//      (one synchronous read at a time, started after ~40 frames), so
//      the device FIFO overflows and lines are lost;
//   F2 windows are host-terminated (the device streams until the host
//      writes idle), so a quiescence end condition never fires.

#include <atomic>
#include <chrono>
#include <thread>

#include "pakon/image/completion.hpp"
#include "pakon/ppb/client.hpp"
#include "pakon/scan/runner.hpp"
#include "pakon/scan/transport.hpp"
#include "support/sim_device.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace std::chrono_literals;
using namespace pakon;

// ppb::Client owns its transport; forward to a simulator the test owns.
class Forward final : public usb::IUsbTransport {
public:
    explicit Forward(sim::SimDevice& s) : sim_(s) {}
    Result<std::vector<std::uint8_t>> command_exchange(std::span<const std::uint8_t> f) override {
        return sim_.command_exchange(f);
    }
    Result<std::vector<std::uint8_t>> bulk_read(std::uint8_t ep, std::size_t n) override {
        return sim_.bulk_read(ep, n);
    }
    Result<std::vector<std::uint8_t>> control_read(std::uint8_t r, std::uint16_t v,
                                                   std::uint16_t i, std::uint16_t n) override {
        return sim_.control_read(r, v, i, n);
    }
    VoidResult control_write(std::uint8_t r, std::uint16_t v, std::uint16_t i) override {
        return sim_.control_write(r, v, i);
    }
    const usb::DeviceInfo& device_info() const override { return sim_.device_info(); }

private:
    sim::SimDevice& sim_;
};

// Cancels the run after 5 s: an old-runner window that cannot end (no
// rows after lost data, or no quiescence) must fail, not hang the suite.
class Watchdog {
public:
    explicit Watchdog(scan::CancelToken& token)
        : thread_([this, &token] {
              for (int i = 0; i < 100 && !done_; ++i) {
                  std::this_thread::sleep_for(50ms);
              }
              token.request();
          }) {}
    ~Watchdog() {
        done_ = true;
        thread_.join();
    }

private:
    std::atomic<bool> done_{false};
    std::thread thread_;
};

sim::SimConfig config() {
    sim::SimConfig c;
    c.lamp_ready_delay = 100ms;
    return c;
}

} // namespace

PAKON_TEST(f1_no_line_is_lost_while_the_window_streams) {
    sim::SimDevice device(config());
    ppb::Client client(std::make_unique<Forward>(device));
    scan::PpbCommandChannel commands(client);
    scan::CancelToken cancel;
    scan::UsbImageSource images(client.transport(), &cancel);

    scan::ScanRunnerConfig cfg;
    cfg.calibration_completion = std::make_shared<image::RowBudgetCompletion>(40);
    cfg.transport_completion = std::make_shared<image::RowBudgetCompletion>(40);
    cfg.cancel = &cancel;
    auto runner = scan::ScanRunner::create(cfg);
    EXPECT(runner.has_value());
    bool ok = false;
    {
        Watchdog watchdog(cancel);
        if (runner) {
            ok = (*runner)->run(commands, images).has_value();
        }
    }
    EXPECT(ok);
    EXPECT_EQ(device.lost_bytes(), 0u);
}

PAKON_TEST(f2_a_film_window_ends_while_the_device_still_streams) {
    sim::SimDevice device(config());
    ppb::Client client(std::make_unique<Forward>(device));
    scan::PpbCommandChannel commands(client);
    scan::CancelToken cancel;
    scan::UsbImageSource images(client.transport(), &cancel);

    scan::ScanRunnerConfig cfg;
    cfg.calibration_completion = std::make_shared<image::RowBudgetCompletion>(40);
    cfg.transport_completion = std::make_shared<image::NoProgressCompletion>(2);
    cfg.cancel = &cancel;
    auto runner = scan::ScanRunner::create(cfg);
    EXPECT(runner.has_value());
    bool ok = false;
    {
        // A window that only ends on device quiescence never ends.
        Watchdog watchdog(cancel);
        if (runner) {
            ok = (*runner)->run(commands, images).has_value();
        }
    }
    EXPECT(ok);
    EXPECT(!device.acquiring());
}

int main() { return pakon::test::run_all(); }
