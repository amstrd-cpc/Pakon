// pakon-cli — command-line diagnostics for the Pakon F-X35 scanner stack.
//
// Commands (Phase 1-4 scope):
//   list                 enumerate attached Pakon scanners (no I/O sent)
//   identify             open PPB session, presence probes, module info
//   status               identify + status polls and read-only registers
//
// Options:
//   --log <level>        off|error|warn|info|debug|trace (default info;
//                        use trace for full packet hex dumps)

#include <cstdio>
#include <string>
#include <string_view>

#include "pakon/logging/logger.hpp"
#include "pakon/scanner/scanner.hpp"
#include "pakon/usb/transport.hpp"

namespace {

using pakon::ErrorKind;

void print_error(const pakon::Error& error) {
    std::fprintf(stderr, "error: %s: %s\n",
                 std::string(pakon::to_string(error.kind)).c_str(),
                 error.message.c_str());
}

void print_usage() {
    std::puts(
        "pakon-cli — Pakon F-X35 scanner tools\n"
        "\n"
        "usage: pakon-cli [--log LEVEL] <command>\n"
        "\n"
        "commands:\n"
        "  list       list attached Pakon scanners (enumeration only)\n"
        "  identify   open the PPB session and identify the scanner model\n"
        "  status     identify, then poll status registers (read-only)\n"
        "\n"
        "log levels: off, error, warn, info, debug, trace\n"
        "            (trace prints full TX/RX packet hex dumps)\n");
}

int cmd_list() {
    const auto devices = pakon::usb::enumerate_pakon();
    if (devices.empty()) {
#ifndef _WIN32
        std::puts("No devices: this build has no native USB backend (the WinUSB");
        std::puts("backend is Windows-only; see docs/USB.md).");
#else
        std::puts("No Pakon F-X35 devices detected.");
        std::puts("(Enumeration is read-only; no commands are sent to any device.)");
#endif
        return 0;
    }

    for (const auto& device : devices) {
        std::printf("Pakon F-X35 detected\n\n");
        std::printf("VID: %04x\n", device.vendor_id);
        std::printf("PID: %04x%s\n", device.product_id,
                    device.is_cold() ? "  (cold/bootstrap — firmware not loaded)"
                                     : "  (operational)");
        std::printf("Serial: %s\n",
                    device.serial_number ? device.serial_number->c_str() : "n/a");
        std::printf("Device path: %s\n", device.device_path.c_str());

        if (device.interfaces.empty()) {
            std::printf("Interfaces: unavailable%s%s\n",
                        device.interface_note.empty() ? "" : " — ",
                        device.interface_note.c_str());
            std::printf("\n"
                        "Note: F-135 and F-135+ are indistinguishable by USB "
                        "descriptors; the model is determined later by PPB "
                        "presence probes.\n");
            continue;
        }

        std::puts("Interfaces:");
        for (const auto& iface : device.interfaces) {
            std::printf("  interface %u alt %u class %02x subclass %02x "
                        "protocol %02x\n",
                        iface.number, iface.alternate_setting, iface.class_code,
                        iface.subclass_code, iface.protocol_code);
            for (const auto& endpoint : iface.endpoints) {
                std::printf("    endpoint 0x%02x %s %s max_packet=%u\n",
                            endpoint.address,
                            endpoint.is_in() ? "IN " : "OUT",
                            endpoint.is_bulk() ? "bulk" : "other",
                            endpoint.max_packet_size);
            }
        }
        std::printf("\n"
                    "Documented command channel: bulk OUT 0x01 / bulk IN 0x81.\n"
                    "The image stream uses a separate bulk IN endpoint (number "
                    "read from the descriptors above).\n");
    }
    return 0;
}

int run_scanner_command(std::string_view command) {
    auto transport = pakon::usb::open_first();
    if (!transport) {
        print_error(transport.error());
        return 1;
    }

    auto scanner = pakon::scanner::Scanner::connect(std::move(*transport));
    if (!scanner) {
        print_error(scanner.error());
        return 1;
    }

    auto identity = (*scanner)->identify();
    if (!identity) {
        print_error(identity.error());
        return 1;
    }

    std::printf("Model: %s\n", pakon::scanner::to_string(identity->model).data());
    std::printf("Light controller: 0x%02x (%s), module: %s\n",
                identity->addresses.light,
                identity->light_present ? "present" : "absent",
                identity->light_module
                    ? identity->light_module->printable().c_str()
                    : "n/a");
    std::printf("Motor controller: 0x%02x (%s), module: %s\n",
                identity->addresses.motor,
                identity->motor_present ? "present" : "absent",
                identity->motor_module
                    ? identity->motor_module->printable().c_str()
                    : "n/a");
    if (identity->bridge_info) {
        std::printf("Bridge info (HOST reg 0x03): %02x %02x\n",
                    (*identity->bridge_info)[0], (*identity->bridge_info)[1]);
    }

    if (command == "identify") {
        (*scanner)->disconnect();
        return 0;
    }

    auto status = (*scanner)->status();
    if (!status) {
        print_error(status.error());
        return 1;
    }

    std::printf("Status:\n");
    std::printf("  HOST poll:      0x%02x\n", status->host_poll);
    std::printf("  Light poll:     0x%02x\n", status->light_poll);
    std::printf("  Motor poll:     0x%02x\n", status->motor_poll);
    if (status->ccd_status) {
        std::printf("  CCD status:     0x%02x\n", *status->ccd_status);
    }
    if (status->light_status) {
        std::printf("  Light status:   %02x %02x\n", (*status->light_status)[0],
                    (*status->light_status)[1]);
    }
    if (status->temperature) {
        std::printf("  Temperature:    %02x %02x %02x %02x\n",
                    (*status->temperature)[0], (*status->temperature)[1],
                    (*status->temperature)[2], (*status->temperature)[3]);
    }

    (*scanner)->disconnect();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string_view command;
    std::string_view log_level = "info";

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--log") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --log requires a level\n");
                return 2;
            }
            log_level = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (command.empty()) {
            command = arg;
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }

    const auto level = pakon::log::parse_level(log_level);
    if (!level) {
        std::fprintf(stderr, "error: unknown log level '%s'\n",
                     std::string(log_level).c_str());
        return 2;
    }
    pakon::log::Logger::instance().set_level(*level);

    if (command.empty()) {
        print_usage();
        return 2;
    }

    if (command == "list") {
        return cmd_list();
    }
    if (command == "identify" || command == "status") {
        return run_scanner_command(command);
    }

    std::fprintf(stderr, "error: unknown command '%s'\n",
                 std::string(command).c_str());
    print_usage();
    return 2;
}
