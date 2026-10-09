// Offline tests for the window reader: explicit completion policies,
// device-signalled end-of-stream, quiescence, and the truncation
// policy at the stream tail.

#include <memory>
#include <vector>

#include "pakon/image/completion.hpp"
#include "pakon/image/reader.hpp"
#include "support/synthetic_image.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace pakon;
using namespace pakon::image;
using namespace pakon::test;

// 8192 samples = 16384 B per row, chosen so the 128 KiB alignment
// window flushes exactly 8 rows on lock (16384 * 8 == kAlignScanBytes)
// — the initial row budget is met by that first flush instead of being
// blown past in one burst. Still well above the 1000-sample sync floor.
constexpr std::size_t kPeriod = 8192;

} // namespace

PAKON_TEST(reader_requires_an_explicit_completion_policy) {
    auto no_policy = ImageReceiver::create(nullptr);
    EXPECT(!no_policy.has_value());
    if (!no_policy) {
        EXPECT_EQ(no_policy.error().kind, ErrorKind::image_no_completion_policy);
    }
    auto zero_read = ImageReceiver::create(std::make_shared<const RowBudgetCompletion>(1), 0);
    EXPECT(!zero_read.has_value());
}

PAKON_TEST(row_budget_completes_at_the_budget) {
    auto source = SyntheticImageSource::once(encode_rows(make_rows(kPeriod, 40)));
    auto receiver =
        ImageReceiver::create(std::make_shared<const RowBudgetCompletion>(8), kPeriod * 2);
    EXPECT(receiver.has_value());
    if (!receiver) {
        return;
    }
    RawImage image;
    auto report = (*receiver)->run(source, image);
    EXPECT(report.has_value());
    if (report) {
        EXPECT_EQ(report->rows, 8u);
        EXPECT_EQ(report->bytes_received, 8 * kPeriod * 2);
        EXPECT_EQ(report->truncated_tail_bytes, 0u);
        EXPECT(std::string(report->completion).find("row-budget") != std::string::npos);
    }
    EXPECT_EQ(image.rows(), 8u);
    EXPECT_EQ(image.geometry.samples_per_row, kPeriod);
    // Pixel fidelity of the framed window.
    if (image.rows() == 8u) {
        EXPECT(image.samples[0] == row_sample(0, 0));
        EXPECT(image.samples[7 * kPeriod + 100] == row_sample(7, 100));
    }
}

PAKON_TEST(no_progress_completes_after_the_idle_limit) {
    // Stream of 20 rows, then the device stays alive but silent.
    SyntheticImageSource source(ImageTail::idle_forever);
    source.append_data(encode_rows(make_rows(kPeriod, 20)));
    auto receiver =
        ImageReceiver::create(std::make_shared<const NoProgressCompletion>(3), kPeriod * 2);
    EXPECT(receiver.has_value());
    if (!receiver) {
        return;
    }
    RawImage image;
    auto report = (*receiver)->run(source, image);
    EXPECT(report.has_value());
    if (report) {
        EXPECT_EQ(report->rows, 20u);
        EXPECT(std::string(report->completion).find("no-progress") != std::string::npos);
    }
}

PAKON_TEST(source_end_completes_when_the_device_stops) {
    auto source = SyntheticImageSource::once(encode_rows(make_rows(kPeriod, 12)));
    auto receiver = ImageReceiver::create(std::make_shared<const SourceEndCompletion>());
    EXPECT(receiver.has_value());
    if (!receiver) {
        return;
    }
    RawImage image;
    auto report = (*receiver)->run(source, image);
    EXPECT(report.has_value());
    if (report) {
        EXPECT_EQ(report->rows, 12u);
        EXPECT(std::string(report->completion).find("source-end") != std::string::npos);
    }
}

PAKON_TEST(any_completion_completes_on_the_first_member) {
    auto source = SyntheticImageSource::once(encode_rows(make_rows(kPeriod, 40)));
    auto any = std::make_shared<const AnyCompletion>(
        std::vector<std::shared_ptr<const ICompletionPolicy>>{
            std::make_shared<const RowBudgetCompletion>(10),
            std::make_shared<const SourceEndCompletion>()});
    auto receiver = ImageReceiver::create(any, kPeriod * 2);
    EXPECT(receiver.has_value());
    if (!receiver) {
        return;
    }
    RawImage image;
    auto report = (*receiver)->run(source, image);
    EXPECT(report.has_value());
    if (report) {
        EXPECT_EQ(report->rows, 10u);
    }
}

PAKON_TEST(device_cut_tail_is_dropped_and_reported) {
    // The device stops mid-row: the complete rows survive, the partial
    // row is dropped and its size reported.
    auto bytes = encode_rows(make_rows(kPeriod, 10));
    const auto good = bytes.size();
    bytes.resize(good + kPeriod); // half a row more
    auto source = SyntheticImageSource::once(std::move(bytes));
    auto receiver = ImageReceiver::create(std::make_shared<const SourceEndCompletion>());
    EXPECT(receiver.has_value());
    if (!receiver) {
        return;
    }
    RawImage image;
    auto report = (*receiver)->run(source, image);
    EXPECT(report.has_value());
    if (report) {
        EXPECT_EQ(report->rows, 10u);
        EXPECT_EQ(report->truncated_tail_bytes, kPeriod);
    }
    EXPECT_EQ(image.rows(), 10u);
}

PAKON_TEST(short_stream_without_device_end_is_an_error) {
    // A row budget that the stream cannot meet, with the source going
    // quiet instead of ending: completion fires on quiescence, and the
    // pending partial row is a truncation error (no padding, no silent
    // loss).
    auto bytes = encode_rows(make_rows(kPeriod, 10));
    bytes.resize(bytes.size() + 100);
    SyntheticImageSource source(ImageTail::idle_forever);
    source.append_data(std::move(bytes));
    auto receiver =
        ImageReceiver::create(std::make_shared<const NoProgressCompletion>(2), kPeriod * 2);
    EXPECT(receiver.has_value());
    if (!receiver) {
        return;
    }
    RawImage image;
    auto report = (*receiver)->run(source, image);
    EXPECT(!report.has_value());
    if (!report) {
        EXPECT_EQ(report.error().kind, ErrorKind::image_truncated);
    }
}

PAKON_TEST(transport_errors_propagate) {
    // A source that fails (e.g. USB error) surfaces the error; the
    // reader never converts it into completion.
    class FailingSource final : public IImageSource {
    public:
        Result<ImageChunk> read(std::size_t) override {
            return failure<ImageChunk>(ErrorKind::usb_io_failed, "device gone");
        }
    };
    FailingSource source;
    auto receiver = ImageReceiver::create(std::make_shared<const SourceEndCompletion>());
    EXPECT(receiver.has_value());
    if (!receiver) {
        return;
    }
    RawImage image;
    auto report = (*receiver)->run(source, image);
    EXPECT(!report.has_value());
    if (!report) {
        EXPECT_EQ(report.error().kind, ErrorKind::usb_io_failed);
    }
}

int main() { return pakon::test::run_all(); }
