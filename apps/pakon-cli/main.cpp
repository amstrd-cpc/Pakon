// pakon-cli - command-line diagnostics for the Pakon F-X35 scanner stack.
//
// Commands (Phase 1-4 scope + bootstrap probe + diagnostics + scan):
//   list                 enumerate attached Pakon scanners (no I/O sent)
//   attrib               read-only PnP/driver attribution report
//                        (property queries only - no device I/O)
//   descriptors          read-only USB descriptor/topology discovery for
//                        the booted F135 (standard GET_DESCRIPTOR reads +
//                        WinUSB interface queries; no vendor requests)
//   probe                cold-device read-only bootstrap probe: one
//                        documented vendor control read (stage-1
//                        personality), no bulk traffic, no writes
//   identify             open PPB session, presence probes, module info
//   status               identify + status polls and read-only registers
//   scan                 capture a scan session - explicit opt-in only:
//                        --dry-run prints the plan with NO device opened;
//                        --live-scan executes over one warm session
//                        (scan_cli.hpp carries the full safety rules)
//
// Options:
//   --log <level>        off|error|warn|info|debug|trace (default info;
//                        use trace for full packet hex dumps)

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "scan_cli.hpp"
#include "pakon/bootstrap/probe.hpp"
#include "pakon/logging/logger.hpp"
#include "pakon/scanner/scanner.hpp"
#include "pakon/usb/descriptors.hpp"
#include "pakon/usb/driver_attrib.hpp"
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
        "pakon-cli - Pakon F-X35 scanner tools\n"
        "\n"
        "usage: pakon-cli [--log LEVEL] <command>\n"
        "\n"
        "commands:\n"
        "  list       list attached Pakon scanners (enumeration only)\n"
        "  attrib     report PnP/driver attribution for attached scanners\n"
        "             (read-only property queries: hardware/compatible IDs,\n"
        "             service, class + class GUID, descriptions, bound INF;\n"
        "             no device is opened, no USB traffic is sent)\n"
        "  descriptors report the booted F135's full USB topology\n"
        "             (device + configuration descriptor bytes, every\n"
        "             interface/alt setting/endpoint, referenced strings;\n"
        "             standard GET_DESCRIPTOR reads + WinUSB queries only -\n"
        "             no vendor request, no reset, no reconfiguration;\n"
        "             see docs/F135_TOPOLOGY.md)\n"
        "  probe      read-only bootstrap probe: stage-1 personality read\n"
        "             (works on cold devices; the only I/O it sends is one\n"
        "             documented vendor control read - see docs/BOOTSTRAP.md)\n"
        "  identify   open the PPB session and identify the scanner model\n"
        "  status     identify, then poll status registers (read-only)\n"
        "\n");
    pakon::cli::print_scan_usage(stdout);
    std::puts(
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
                    device.is_cold() ? "  (cold/bootstrap - firmware not loaded)"
                                     : "  (operational)");
        std::printf("Serial: %s\n",
                    device.serial_number ? device.serial_number->c_str() : "n/a");
        if (!device.hardware_id.empty()) {
            std::printf("Hardware ID: %s\n", device.hardware_id.c_str());
        }
        if (!device.instance_id.empty()) {
            std::printf("Device instance: %s\n", device.instance_id.c_str());
        }
        if (device.has_device_interface()) {
            std::printf("Device path: %s\n", device.device_path.c_str());
        } else {
            std::puts("Device path: (none - no function-driver interface; "
                      "discovered via PnP, not openable)");
        }

        if (device.interfaces.empty()) {
            std::printf("Interfaces: unavailable%s%s\n",
                        device.interface_note.empty() ? "" : " - ",
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

// Read-only PnP/driver attribution (docs/BOOT_CHAIN.md sec. 6): property
// queries over the present device list - no device is opened, no USB
// traffic is sent, no driver state is modified.
int cmd_attrib() {
    if (!pakon::usb::driver_attribution_supported()) {
        std::fprintf(stderr,
                     "error: driver attribution requires Windows (PnP "
                     "property queries); see docs/BOOT_CHAIN.md\n");
        return 1;
    }

    const auto devices = pakon::usb::collect_driver_attributions();
    if (devices.empty()) {
        std::puts("No Pakon F-X35 devices detected.");
        std::puts("(Read-only PnP query: no device opened, no USB traffic "
                  "sent.)");
        return 0;
    }

    const auto shown = [](const std::string& value) -> const char* {
        return value.empty() ? "(not set)" : value.c_str();
    };
    for (const auto& device : devices) {
        const char* kind =
            device.kind == pakon::usb::PakonHardwareId::cold_f235
                ? "cold F235 (bootstrap)"
                : device.kind == pakon::usb::PakonHardwareId::warm_f135
                      ? "warm F135 (operational)"
                      : "unknown";
        std::puts("Pakon device attribution (read-only PnP property queries)");
        std::printf("  Instance:        %s\n", device.instance_id.c_str());
        std::printf("  Identity:        VID %04x  PID %04x  (%s)\n",
                    device.vendor_id, device.product_id, kind);
        std::puts("  Present:         yes (enumerated under DIGCF_PRESENT)");
        std::puts("  Hardware IDs:");
        if (device.hardware_ids.empty()) {
            std::puts("    (not set)");
        } else {
            for (const auto& id : device.hardware_ids) {
                std::printf("    %s\n", id.c_str());
            }
        }
        std::puts("  Compatible IDs:");
        if (device.compatible_ids.empty()) {
            std::puts("    (none)");
        } else {
            for (const auto& id : device.compatible_ids) {
                std::printf("    %s\n", id.c_str());
            }
        }
        std::printf("  Service:         %s\n",
                    device.service.empty()
                        ? "(none - no function driver, Code 28)"
                        : device.service.c_str());
        std::printf("  Class:           %s  %s\n", shown(device.class_name),
                    shown(device.class_guid));
        std::printf("  DeviceDesc:      %s\n", shown(device.device_desc));
        std::printf("  FriendlyName:    %s\n", shown(device.friendly_name));
        std::printf("  BusReported:     %s\n", shown(device.bus_reported_desc));
        std::printf("  Driver INF:      %s\n", shown(device.driver_inf));
        std::printf("  Driver provider: %s\n", shown(device.driver_provider));
        std::puts("");
    }
    std::puts("(Read-only PnP query: no device opened, no USB traffic sent.)");
    return 0;
}

// bmAttributes low bits -> transfer type name (USB spec encoding).
const char* transfer_name(std::uint8_t attributes) {
    const std::uint8_t type = attributes & 0x03;
    return type == 0 ? "control"
         : type == 1 ? "isochronous"
         : type == 2 ? "bulk"
                     : "interrupt";
}

void print_hex_bytes(const std::vector<std::uint8_t>& bytes) {
    for (std::size_t offset = 0; offset < bytes.size(); offset += 16) {
        std::printf("    %04zx:", offset);
        for (std::size_t i = offset; i < bytes.size() && i < offset + 16;
             ++i) {
            std::printf(" %02x", bytes[i]);
        }
        std::puts("");
    }
}

// One topology section: every interface -> alternate setting -> endpoint
// with direction, transfer type, packet size (raw + low 11 bits) and
// raw bInterval (the encoding differs per transfer type and is not
// decoded here - raw bytes only, per the project's no-invention rule).
void print_topology(const std::vector<pakon::usb::DescriptorInterface>& interfaces,
                    const char* title) {
    std::puts(title);
    if (interfaces.empty()) {
        std::puts("  (none reported)");
        return;
    }
    for (const auto& iface : interfaces) {
        for (const auto& alt : iface.alt_settings) {
            std::printf(
                "  interface %u alt %u: class %02x subclass %02x "
                "protocol %02x  iInterface %u  endpoints %zu\n",
                alt.interface_number, alt.alternate_setting, alt.class_code,
                alt.subclass_code, alt.protocol_code, alt.string_index,
                alt.endpoints.size());
            for (const auto& endpoint : alt.endpoints) {
                std::printf(
                    "    EP 0x%02x %-3s %-11s wMaxPacket 0x%04x (%u)  "
                    "bInterval %u\n",
                    endpoint.address,
                    (endpoint.address & 0x80) ? "IN" : "OUT",
                    transfer_name(endpoint.attributes), endpoint.w_max_packet,
                    endpoint.w_max_packet & 0x07FFu, endpoint.interval);
            }
        }
    }
}

// Read-only F135 descriptor/topology discovery (docs/F135_TOPOLOGY.md).
// Standard GET_DESCRIPTOR reads (device/configuration/string) plus local
// WinUSB queries only: no vendor control request (0xA0/0xA3/0xA4/0xA9
// never touched), no firmware upload, no USB reset, no configuration
// change - the device is left exactly as found.
int cmd_descriptors() {
    if (!pakon::usb::descriptor_scan_supported()) {
        std::fprintf(stderr,
                     "error: descriptor discovery requires Windows "
                     "(WinUSB descriptor queries); "
                     "see docs/F135_TOPOLOGY.md\n");
        return 1;
    }

    const auto report = pakon::usb::scan_pakon_descriptors();
    if (!report.error.empty()) {
        std::fprintf(stderr, "error: %s\n", report.error.c_str());
        return 1;
    }

    std::puts("Read-only USB descriptor discovery (booted 0F05:F135 runtime)");
    std::puts("Traffic: standard GET_DESCRIPTOR (device/configuration/string)");
    std::puts("        + local WinUSB descriptor queries. No vendor request");
    std::puts("        (0xA0/0xA3/0xA4/0xA9 untouched), no firmware upload,");
    std::puts("        no reset, no configuration change.");
    std::puts("");
    std::puts("Device");
    std::printf("  instance:       %s\n", report.instance_id.c_str());
    std::printf("  hardware id:    %s\n", report.hardware_id.c_str());
    std::printf("  interface path: %s\n", report.device_path.c_str());
    std::printf("  open:           %s\n", report.open_mode.c_str());
    if (report.link_speed_known) {
        std::printf("  link speed:     %s\n", report.link_speed.c_str());
    }
    std::puts("  configuration:  active (implied - WinUsb_Initialize "
              "succeeded;\n                  the WinUSB API exposes no "
              "bConfigurationValue read)");
    std::puts("");

    if (report.device_descriptor_read) {
        std::puts("DEVICE DESCRIPTOR (18 bytes, GET_DESCRIPTOR(DEVICE))");
        std::printf("  bcdUSB %04x  class %02x/%02x/%02x  bMaxPacketSize0 %u\n",
                    report.bcd_usb, report.device_class,
                    report.device_sub_class, report.device_protocol,
                    report.b_max_packet_size0);
        std::printf("  idVendor %04x  idProduct %04x  bcdDevice %04x\n",
                    report.id_vendor, report.id_product, report.bcd_device);
        std::printf("  iManufacturer %u  iProduct %u  iSerialNumber %u  "
                    "bNumConfigurations %u\n",
                    report.i_manufacturer, report.i_product, report.i_serial,
                    report.b_num_configurations);
        std::puts("  raw:");
        print_hex_bytes(report.device_descriptor);
    } else {
        std::printf("DEVICE DESCRIPTOR: unavailable (%s)\n",
                    report.device_descriptor_error.c_str());
    }
    std::puts("");

    if (report.config_descriptor_read) {
        std::printf(
            "CONFIGURATION DESCRIPTOR (%zu bytes, "
            "GET_DESCRIPTOR(CONFIGURATION))\n",
            report.config_descriptor.size());
        const unsigned power_unit =
            report.device_descriptor_read && report.bcd_usb >= 0x0300 ? 8u
                                                                      : 2u;
        std::printf("  bNumInterfaces %u  bConfigurationValue %u  "
                    "iConfiguration %u\n",
                    report.b_num_interfaces, report.b_configuration_value,
                    report.i_configuration);
        std::printf("  bmAttributes 0x%02x  bMaxPower %u (= %u mA at %u mA "
                    "units)\n",
                    report.bm_attributes, report.max_power,
                    report.max_power * power_unit, power_unit);
        if (report.b_num_configurations > 1) {
            std::puts("  note: bNumConfigurations > 1 - only configuration 0");
            std::puts("        was read; selecting another configuration would");
            std::puts("        reconfigure the device and is NOT done here.");
        }
        if (!report.config_parse_error.empty()) {
            std::printf("  parse: first anomaly - %s\n",
                        report.config_parse_error.c_str());
        }
        std::puts("  raw:");
        print_hex_bytes(report.config_descriptor);
    } else {
        std::printf("CONFIGURATION DESCRIPTOR: unavailable (%s)\n",
                    report.config_descriptor_error.c_str());
    }
    std::puts("");

    print_topology(report.interfaces,
                   "INTERFACES (parsed from the raw configuration "
                   "descriptor)");
    std::puts("");
    print_topology(report.winusb_interfaces,
                   "INTERFACES (WinUSB query: QueryInterfaceSettings / "
                   "QueryPipe - cross-check)");
    if (!report.winusb_walk_error.empty()) {
        std::printf("  walk note: %s\n", report.winusb_walk_error.c_str());
    }
    std::puts("");

    std::puts("STRING DESCRIPTORS (GET_DESCRIPTOR(STRING), referenced "
              "indices only)");
    if (report.strings.empty()) {
        std::puts("  (no string indices referenced)");
    }
    for (const auto& entry : report.strings) {
        if (entry.read) {
            std::printf("  index %u (lang 0x%04x): \"%s\"\n", entry.index,
                        entry.language_id, entry.text.c_str());
        } else {
            std::printf("  index %u: unavailable (%s)\n", entry.index,
                        entry.error.c_str());
        }
    }
    std::puts("");
    std::puts("(Read-only: standard descriptor reads + WinUSB queries only -");
    std::puts(" no vendor control request, no firmware upload, no USB reset,");
    std::puts(" no configuration change; the device was left as found.)");
    std::puts("Interpretation and evidence labels: docs/F135_TOPOLOGY.md");
    return 0;
}

int cmd_probe() {
    // Explicit cold_ok: the probe's whole purpose is the cold stage-1
    // loader. It performs exactly one read-only vendor control request
    // (bootstrap::probe - pinned in tests/bootstrap/probe_test.cpp); no
    // bulk traffic, no control writes, no PPB frames. Evidence and safety
    // analysis: docs/BOOTSTRAP.md.
    auto transport = pakon::usb::open_first(/*cold_ok=*/true);
    if (!transport) {
        print_error(transport.error());
        return 1;
    }

    const auto& info = (*transport)->device_info();
    std::printf("State: %s\n", info.is_cold()
                    ? "cold/bootstrap (0f05:f235) - firmware not loaded"
                    : "warm/operational");
    std::printf("VID: %04x  PID: %04x\n", info.vendor_id, info.product_id);
    if (!info.hardware_id.empty()) {
        std::printf("Hardware ID: %s\n", info.hardware_id.c_str());
    }
    if (!info.instance_id.empty()) {
        std::printf("Device instance: %s\n", info.instance_id.c_str());
    }

    const auto report = pakon::bootstrap::probe(**transport);
    if (!report) {
        print_error(report.error());
        std::puts("hint: the stage-1 personality read (vendor IN 0xA9, "
                  "wIndex 0) is the only I/O this command performs - record "
                  "this output as bootstrap evidence; interpretation guide: "
                  "docs/BOOTSTRAP.md");
        return 1;
    }

    const auto& personality = report->personality.bytes;
    std::puts("Personality: 8-byte C0 record (stage-1 loader, vendor IN "
              "0xA9, wValue 0, wIndex 0)");
    std::printf("  raw:   %s\n",
                pakon::bootstrap::hex_string(personality).c_str());
    std::string ascii;
    for (const auto byte : personality) {
        ascii += (byte >= 0x20 && byte < 0x7f) ? static_cast<char>(byte) : '.';
    }
    std::printf("  ascii: %s\n", ascii.c_str());
    std::puts("Note: the C0 record's byte layout is not documented in this "
              "repository - raw bytes only, no decoding applied "
              "(docs/BOOTSTRAP.md).");
    std::puts("Probe sent exactly one vendor control read: no bulk traffic, "
              "no writes. Firmware upload is not implemented (evidence gaps: "
              "docs/BOOTSTRAP.md).");
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

    // Module-info id: only the capture-evidenced 5-byte ASCII window
    // (offset 5..9) renders as text; otherwise show the raw payload so
    // no string is ever fabricated from binary.
    const auto module_text = [](const auto& module) -> std::string {
        if (!module) {
            return "n/a";
        }
        const auto id = module->printable();
        return id.empty() ? module->hex() + " (raw)" : id;
    };

    std::printf("Model: %s\n", pakon::scanner::to_string(identity->model).data());
    std::printf("Light controller: 0x%02x (%s), module: %s\n",
                identity->addresses.light,
                identity->light_present ? "present" : "absent",
                module_text(identity->light_module).c_str());
    std::printf("Motor controller: 0x%02x (%s), module: %s\n",
                identity->addresses.motor,
                identity->motor_present ? "present" : "absent",
                module_text(identity->motor_module).c_str());
    if (identity->bridge_info) {
        std::printf("Bridge info (HOST reg 0x03): %02x %02x\n",
                    (*identity->bridge_info)[0], (*identity->bridge_info)[1]);
    }
    // The OEM log's own version line ("Version USB 0x03,0x0F ... Lamp
    // 0x05,0x0A ... Motor 0x05,0x06", psiref Logs) for a direct
    // comparison: bridge = (hi, lo) of HOST 0x03, controllers = (b[2],
    // b[1]) of the dev-info page (docs/OEM_RE.md §4.1).
    if (identity->bridge_info && identity->light_module && identity->motor_module) {
        const auto lamp = identity->light_module->firmware();
        const auto motor = identity->motor_module->firmware();
        std::printf("Version USB 0x%02X,0x%02X  Lamp 0x%02X,0x%02X  Motor 0x%02X,0x%02X\n",
                    (*identity->bridge_info)[1], (*identity->bridge_info)[0], lamp.first,
                    lamp.second, motor.first, motor.second);
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

// The live-scan session opener wired into cli::run_scan_command.
//
// Reuses the EXISTING working sequence - usb::open_first(cold_ok=false)
// (warm device only: the cold/bootstrap path is probe's, not ours),
// Scanner::connect (the documented open handshake that identify and
// status already perform), identify() - and hands over ONE session:
// command frames (bulk 0x01/0x81) and image reads (bulk 0x86) both
// ride this transport, so no second connection ever competes with it.
// No cold boot, no firmware reload, no re-initialisation beyond the
// handshake identify/status already do today.
pakon::Result<pakon::cli::LiveScanSession> open_live_session() {
    auto transport = pakon::usb::open_first(/*cold_ok=*/false);
    if (!transport) {
        return transport.error();
    }
    auto scanner = pakon::scanner::Scanner::connect(std::move(*transport));
    if (!scanner) {
        return scanner.error();
    }
    auto identity = (*scanner)->identify();
    if (!identity) {
        return identity.error();
    }
    pakon::cli::LiveScanSession session;
    session.scanner = std::move(*scanner);
    session.identity = std::move(*identity);
    session.client = &session.scanner->client();
    return session;
}

} // namespace

int main(int argc, char** argv) {
    std::string_view command;
    std::string_view log_level = "info";
    std::vector<std::string> scan_args;

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
        } else if (command == "scan") {
            // Everything after `scan` belongs to the scan command's own
            // argument validation (scan_cli.cpp); it decides usage
            // errors before any device entry point can run.
            scan_args.emplace_back(arg);
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
    if (command == "attrib") {
        return cmd_attrib();
    }
    if (command == "descriptors") {
        return cmd_descriptors();
    }
    if (command == "probe") {
        return cmd_probe();
    }
    if (command == "identify" || command == "status") {
        return run_scanner_command(command);
    }
    if (command == "scan") {
        // Explicit opt-in only: run_scan_command opens the scanner
        // solely on the --live-scan path (scan_cli.hpp). The default
        // invocation and --dry-run are decided in validation, before
        // the opener exists - no USB traffic is possible there.
        pakon::cli::ScanCliDeps deps;
        deps.open_session = &open_live_session;
        return pakon::cli::run_scan_command(scan_args, deps, stdout, stderr);
    }

    std::fprintf(stderr, "error: unknown command '%s'\n",
                 std::string(command).c_str());
    print_usage();
    return 2;
}
