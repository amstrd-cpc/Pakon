#include "pakon/image/reader.hpp"

#include <format>

namespace pakon::image {

namespace {

void append_rows(RawImage& image, const std::vector<std::vector<std::uint16_t>>& rows) {
    for (const auto& row : rows) {
        image.samples.insert(image.samples.end(), row.begin(), row.end());
    }
}

} // namespace

Result<std::unique_ptr<ImageReceiver>>
ImageReceiver::create(std::shared_ptr<const ICompletionPolicy> policy,
                      std::size_t read_bytes) {
    if (!policy) {
        return failure<std::unique_ptr<ImageReceiver>>(
            ErrorKind::image_no_completion_policy,
            "an image window without a completion policy would never end; "
            "supply one explicitly (see image/completion.hpp)");
    }
    if (read_bytes == 0) {
        return failure<std::unique_ptr<ImageReceiver>>(
            ErrorKind::image_no_completion_policy,
            "read size of 0 bytes would never make progress");
    }
    return std::unique_ptr<ImageReceiver>(
        new ImageReceiver(std::move(policy), read_bytes));
}

Result<WindowReport> ImageReceiver::run(IImageSource& source, RawImage& out) {
    RowFramer framer;
    WindowStats stats;
    bool saw_eos = false;

    while (true) {
        auto chunk = source.read(read_bytes_);
        if (!chunk) {
            return chunk.error();
        }
        if (!chunk->bytes.empty()) {
            stats.idle_ticks = 0;
            stats.bytes_received += chunk->bytes.size();
            auto rows = framer.feed(chunk->bytes);
            if (!rows) {
                return rows.error();
            }
            append_rows(out, *rows);
            stats.rows_completed += rows->size();
        } else {
            ++stats.idle_ticks;
        }
        if (chunk->end_of_stream) {
            saw_eos = true;
            stats.source_ended = true;
        }
        if (policy_->check(stats) == Completion::complete) {
            break;
        }
    }

    WindowReport report;
    report.bytes_received = stats.bytes_received;
    report.completion = policy_->label();

    // Finalise the framer. A partial row the DEVICE left behind when it
    // stopped feeding the endpoint is real (end-of-roll can land
    // mid-row); it is dropped and reported, not padded. Any other
    // truncation fails the window.
    auto tail = framer.flush();
    if (!tail) {
        return tail.error();
    }
    append_rows(out, tail->rows);
    if (tail->leftover_bytes > 0) {
        if (!saw_eos) {
            return failure<WindowReport>(
                ErrorKind::image_truncated,
                std::format("{} bytes left after the last complete row ({} rows "
                            "framed) — the stream ended short",
                            tail->leftover_bytes, framer.rows_emitted()));
        }
        report.truncated_tail_bytes = tail->leftover_bytes;
    }

    if (const auto sync = framer.sync()) {
        out.geometry.samples_per_row = static_cast<std::uint32_t>(sync->period_samples);
    }
    report.rows = framer.rows_emitted();
    return report;
}

} // namespace pakon::image
