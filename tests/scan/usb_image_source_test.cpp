// Pin the live image adapter (scan::UsbImageSource): bulk reads must
// target endpoint 0x86 with the requested length passed through, read
// deadlines and empty reads must map to idle ticks (never completion,
// never errors), and non-timeout failures must propagate. No hardware
// is touched — the transport here is a local fake.

#include <cstdint>
#include <vector>

#include "pakon/scan/transport.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace pakon;

class FakeTransport final : public usb::IUsbTransport {
public:
    struct Read {
        bool fail{false};
        ErrorKind kind{ErrorKind::usb_io_failed};
        std::vector<std::uint8_t> bytes;
    };

    std::vector<Read> script;
    std::size_t index{0};
    std::uint8_t last_endpoint{0};
    std::size_t last_max{0};

    Result<std::vector<std::uint8_t>>
    command_exchange(std::span<const std::uint8_t>) override {
        return failure<std::vector<std::uint8_t>>(ErrorKind::usb_not_supported,
                                                   "not under test");
    }

    Result<std::vector<std::uint8_t>> bulk_read(std::uint8_t endpoint,
                                                std::size_t max_length) override {
        last_endpoint = endpoint;
        last_max = max_length;
        if (index >= script.size()) {
            return failure<std::vector<std::uint8_t>>(ErrorKind::usb_io_failed,
                                                      "script exhausted");
        }
        const auto& read = script[index++];
        if (read.fail) {
            return failure<std::vector<std::uint8_t>>(read.kind, "scripted failure");
        }
        return read.bytes;
    }

    Result<std::vector<std::uint8_t>>
    control_read(std::uint8_t, std::uint16_t, std::uint16_t, std::uint16_t) override {
        return failure<std::vector<std::uint8_t>>(ErrorKind::usb_not_supported,
                                                  "not under test");
    }

    Result<void> control_write(std::uint8_t, std::uint16_t, std::uint16_t) override {
        return void_failure(ErrorKind::usb_not_supported, "not under test");
    }

    const usb::DeviceInfo& device_info() const override { return info_; }

private:
    usb::DeviceInfo info_{};
};

} // namespace

PAKON_TEST(image_reads_target_endpoint_0x86) {
    FakeTransport transport;
    transport.script.push_back({false, {}, {0x11, 0x22, 0x33}});
    scan::UsbImageSource source(transport);

    auto chunk = source.read(20480);
    EXPECT(chunk.has_value());
    if (chunk) {
        EXPECT_EQ(chunk->bytes, std::vector<std::uint8_t>({0x11, 0x22, 0x33}));
        EXPECT_EQ(chunk->timed_out, false);
        EXPECT_EQ(chunk->end_of_stream, false);
    }
    EXPECT_EQ(transport.last_endpoint, 0x86);
    EXPECT_EQ(transport.last_max, 20480u);
}

PAKON_TEST(deadline_and_empty_reads_are_idle_ticks) {
    // usb_timeout: the device isn't answering — an idle tick, not an
    // error and not completion (quiescence policy decides that).
    FakeTransport timed_out;
    timed_out.script.push_back({true, ErrorKind::usb_timeout, {}});
    scan::UsbImageSource a(timed_out);
    auto idle = a.read(4096);
    EXPECT(idle.has_value());
    if (idle) {
        EXPECT(idle->bytes.empty());
        EXPECT_EQ(idle->timed_out, true);
        EXPECT_EQ(idle->end_of_stream, false);
    }

    // A successful zero-length read is the same thing.
    FakeTransport empty;
    empty.script.push_back({false, {}, {}});
    scan::UsbImageSource b(empty);
    auto zero = b.read(4096);
    EXPECT(zero.has_value());
    if (zero) {
        EXPECT_EQ(zero->timed_out, true);
        EXPECT_EQ(zero->end_of_stream, false);
    }
}

PAKON_TEST(non_timeout_failures_propagate) {
    FakeTransport broken;
    broken.script.push_back({true, ErrorKind::usb_io_failed, {}});
    scan::UsbImageSource source(broken);
    auto chunk = source.read(4096);
    EXPECT(!chunk.has_value());
    if (!chunk) {
        EXPECT_EQ(chunk.error().kind, ErrorKind::usb_io_failed);
    }
}

PAKON_TEST(cancel_is_observed_before_the_pipe_is_touched) {
    // Ctrl+C during a window: the token is checked BEFORE the bulk
    // read, so an interrupt never becomes an in-flight transfer, an
    // idle tick, or part of the completion count — it is an error
    // (ErrorKind::cancelled) that propagates to the runner's teardown.
    FakeTransport transport;
    transport.script.push_back({false, {}, {0x11}});
    scan::CancelToken token;
    token.request();
    scan::UsbImageSource source(transport, &token);

    auto chunk = source.read(4096);
    EXPECT(!chunk.has_value());
    if (!chunk) {
        EXPECT_EQ(chunk.error().kind, ErrorKind::cancelled);
    }
    EXPECT_EQ(transport.index, 0u);        // no bulk read was issued
    EXPECT_EQ(transport.last_endpoint, 0); // transport untouched

    // With the token clear the same adapter behaves exactly as before.
    token.reset();
    auto ok = source.read(4096);
    EXPECT(ok.has_value());
    if (ok) {
        EXPECT_EQ(ok->bytes, std::vector<std::uint8_t>({0x11}));
        EXPECT_EQ(ok->timed_out, false);
    }
    EXPECT_EQ(transport.index, 1u);
}

int main() { return pakon::test::run_all(); }
