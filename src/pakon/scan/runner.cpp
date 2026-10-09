#include "pakon/scan/runner.hpp"

#include <format>

#include "pakon/logging/logger.hpp"
#include "pakon/protocol/scan_commands.hpp"

namespace pakon::scan {

Result<std::unique_ptr<ScanRunner>> ScanRunner::create(ScanRunnerConfig config) {
    if (!config.calibration_completion || !config.transport_completion) {
        return failure<std::unique_ptr<ScanRunner>>(
            ErrorKind::image_no_completion_policy,
            "both scan windows need an explicit completion policy");
    }
    if (config.read_bytes == 0) {
        return failure<std::unique_ptr<ScanRunner>>(
            ErrorKind::image_no_completion_policy,
            "read size of 0 bytes would never make progress");
    }
    auto runner = std::unique_ptr<ScanRunner>(new ScanRunner());
    runner->config_ = std::move(config);
    const auto& a = runner->config_.addresses;
    const auto& p = runner->config_.plan;
    runner->init_frames_ = init_plan(a, p);
    runner->service_frames_ = service_plan(a);
    runner->calibration_frames_ = calibration_plan(a, p);
    runner->transport_frames_ = transport_plan(a, p);
    runner->teardown_frames_ = teardown_plan(a, p);
    return runner;
}

VoidResult ScanRunner::run_plan(ICommandChannel& commands,
                                const std::vector<ppb::Frame>& frames,
                                const char* phase) {
    for (const auto& frame : frames) {
        auto reply = commands.exchange(frame);
        if (!reply) {
            return void_failure(reply.error().kind,
                                std::format("{}: {}", phase, reply.error().message));
        }
        // Abort on anything that is not the documented success for the
        // request type — never retry silently.
        const bool ok =
            frame.type == ppb::FrameType::read ? ppb::is_read_success(*reply)
                                               : ppb::is_success(reply->status);
        if (!ok) {
            return void_failure(
                ErrorKind::ppb_bad_status,
                std::format("{}: frame to bus {:02x} reg {:02x} rejected with status {}",
                            phase, frame.data[0], frame.data[2],
                            ppb::to_string(reply->status)));
        }
    }
    return {};
}

Result<ScanResult> ScanRunner::run(ICommandChannel& commands, image::IImageSource& images) {
    ScanResult result;
    const auto& a = config_.addresses;

    auto fault = [&](const Error& error) -> Result<ScanResult> {
        if (auto f = session_.handle(ScanEvent::fault); !f) {
            return failure<ScanResult>(
                ErrorKind::scanner_unexpected_state,
                std::format("session fault handling failed: {}", f.error().message));
        }
        log::Logger::instance().log(log::Level::error, "scan session failed: {}",
                                    error.message);
        return failure<ScanResult>(error.kind, error.message);
    };
    auto fail_plan = [&](VoidResult r) -> Result<ScanResult> {
        return fault(Error{r.error().kind, r.error().message});
    };

    if (auto s = session_.handle(ScanEvent::start); !s) {
        return fault(s.error());
    }
    if (auto r = run_plan(commands, init_frames_, "init"); !r) {
        return fail_plan(r);
    }
    if (auto s = session_.handle(ScanEvent::init_done); !s) {
        return fault(s.error());
    }

    // ready: poll the light service register until the device asks for
    // service (payload 0x02; capture corpus: idle replies carry 00).
    while (true) {
        auto reply = commands.exchange(protocol::scan::read_service_status(a.light));
        if (!reply) {
            return fault(reply.error());
        }
        if (!ppb::is_read_success(*reply)) {
            return fault(Error{ErrorKind::ppb_bad_status,
                               std::format("service poll rejected with flags {}",
                                           ppb::to_string(reply->status))});
        }
        if (protocol::scan::service_requested(*reply)) {
            break;
        }
    }
    if (auto s = session_.handle(ScanEvent::service_requested); !s) {
        return fault(s.error());
    }
    if (auto r = run_plan(commands, service_frames_, "service"); !r) {
        return fail_plan(r);
    }
    if (auto s = session_.handle(ScanEvent::service_done); !s) {
        return fault(s.error());
    }

    // calibrating: send the window's command block, then drain W1
    // until its policy completes. (The OEM interleaves the block with
    // the stream; the reference does not establish that the ordering
    // matters, so the replay is sequential.)
    if (auto r = run_plan(commands, calibration_frames_, "calibration"); !r) {
        return fail_plan(r);
    }
    {
        auto receiver =
            image::ImageReceiver::create(config_.calibration_completion, config_.read_bytes);
        if (!receiver) {
            return fault(receiver.error());
        }
        auto report = (*receiver)->run(images, result.calibration);
        if (!report) {
            return fault(report.error());
        }
        result.calibration_report = *report;
        log::Logger::instance().log(
            log::Level::info,
            "calibration window: {} rows, {} bytes, completion: {} ({})", report->rows,
            report->bytes_received, report->completion,
            result.calibration.geometry.samples_per_row);
    }
    if (auto s = session_.handle(ScanEvent::calibration_done); !s) {
        return fault(s.error());
    }

    // preparing_transport → transporting.
    if (auto r = run_plan(commands, transport_frames_, "transport"); !r) {
        return fail_plan(r);
    }
    if (auto s = session_.handle(ScanEvent::transport_prepared); !s) {
        return fault(s.error());
    }
    {
        auto receiver =
            image::ImageReceiver::create(config_.transport_completion, config_.read_bytes);
        if (!receiver) {
            return fault(receiver.error());
        }
        auto report = (*receiver)->run(images, result.film);
        if (!report) {
            return fault(report.error());
        }
        result.transport_report = *report;
        log::Logger::instance().log(
            log::Level::info,
            "film window: {} rows, {} bytes, completion: {} ({})", report->rows,
            report->bytes_received, report->completion,
            result.film.geometry.samples_per_row);
    }
    if (auto s = session_.handle(ScanEvent::completion_signalled); !s) {
        return fault(s.error());
    }
    if (auto s = session_.handle(ScanEvent::teardown_ready); !s) {
        return fault(s.error());
    }

    // tearing_down → complete.
    if (auto r = run_plan(commands, teardown_frames_, "teardown"); !r) {
        return fail_plan(r);
    }
    if (auto s = session_.handle(ScanEvent::teardown_done); !s) {
        return fault(s.error());
    }
    return result;
}

} // namespace pakon::scan
