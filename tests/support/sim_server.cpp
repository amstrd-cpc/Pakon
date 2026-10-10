// pakon_sim_server: the simulated F-135+ (sim_device.hpp) behind the TCP
// protocol of pakon-tlx-macos's pkusb.dll, so the unmodified OEM stack
// (TLXClientDemo/tlx.dll/TLB.dll under Wine, pkusb.dll in place of the
// kernel driver) can be run against the simulator - never a real scanner.
//
// Wire protocol (pkusb.c pk_call_on / read_worker, pakonusb.py handle):
//   request  u32 code, outsz, inlen, odlen; then inlen + odlen bytes
//   reply    u32 ok, n; then n bytes (ok = 0: the call failed)
//   code 0x222090  bulk OUT 0x01 + bulk IN 0x81: one PPB exchange
//   code 0x222059  EP0: 10-byte struct direction, type, recipient, -,
//                  bRequest, -, wValue u16, wIndex u16
//   code READ_EP6  0xFFFFFFFF: image bytes; exactly outsz or none
//
// The session is logged in the capture-corpus schema (cmd/rsp/ep0/ep6)
// for tools/compare_sessions.py. The simulator itself enforces the
// safety rules (forbidden addresses, LED ceilings, EEPROM allow-list);
// its violations are printed on exit.
//
// Usage: pakon_sim_server [--port 5140] [--log session.jsonl]
//                         [--film LEAD,LINES] [--speed X] [--lamp-ms N]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "support/sim_device.hpp"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::system_clock;

constexpr std::uint32_t kIoctlVendor = 0x222059;
constexpr std::uint32_t kIoctlCommand = 0x222090;
constexpr std::uint32_t kReadEp6 = 0xFFFFFFFF;

std::atomic<bool> g_quit{false};

class Log {
public:
    explicit Log(const std::string& path) {
        if (!path.empty()) {
            file_ = std::fopen(path.c_str(), "w");
        }
        write(R"j({"d":"meta","label":"sim","bridge":"pakon_sim_server (tests/support/sim_server.cpp)","clock":"host wall clock","scope":"PPB commands and replies, vendor control requests, EP6 byte counts"})j");
    }
    ~Log() {
        if (file_) {
            std::fclose(file_);
        }
    }
    void event(const std::string& body) {
        const double t = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
        write(std::format("{{\"t\":{:.6f},{}}}", t, body));
    }

private:
    void write(const std::string& line) {
        std::lock_guard lock(mutex_);
        if (file_) {
            std::fprintf(file_, "%s\n", line.c_str());
            std::fflush(file_);
        }
    }
    std::mutex mutex_;
    std::FILE* file_{nullptr};
};

std::string hex(std::span<const std::uint8_t> bytes) {
    std::string s;
    for (const auto b : bytes) {
        s += std::format("{:02x}", b);
    }
    return s;
}

bool recv_all(int fd, void* p, std::size_t n) {
    auto* c = static_cast<char*>(p);
    while (n > 0) {
        const auto r = ::recv(fd, c, n, 0);
        if (r <= 0) {
            return false;
        }
        c += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

bool send_all(int fd, const void* p, std::size_t n) {
    const auto* c = static_cast<const char*>(p);
    while (n > 0) {
        const auto r = ::send(fd, c, n, MSG_NOSIGNAL);
        if (r <= 0) {
            return false;
        }
        c += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

bool reply(int fd, bool ok, std::span<const std::uint8_t> data) {
    const std::uint32_t rep[2] = {ok ? 1u : 0u, ok ? static_cast<std::uint32_t>(data.size()) : 0u};
    return send_all(fd, rep, sizeof rep) &&
           (!ok || data.empty() || send_all(fd, data.data(), data.size()));
}

// Drains EP6 continuously (the FX2 FIFO is tiny) into a host buffer that
// READ_EP6 serves from - what pakonusb.py's reader thread does.
class ImageBuffer {
public:
    explicit ImageBuffer(pakon::sim::SimDevice& sim) : sim_(sim) {
        thread_ = std::thread([this] { run(); });
    }
    ~ImageBuffer() {
        stop_ = true;
        thread_.join();
    }
    ImageBuffer(const ImageBuffer&) = delete;
    ImageBuffer& operator=(const ImageBuffer&) = delete;

    // A new acquire window: drop what the previous one left behind, so
    // line 0 of the new window is the first byte served.
    void new_window() {
        std::lock_guard lock(mutex_);
        buffer_.clear();
    }

    std::vector<std::uint8_t> take(std::size_t want, std::chrono::milliseconds deadline) {
        const auto end = std::chrono::steady_clock::now() + deadline;
        while (std::chrono::steady_clock::now() < end && !g_quit) {
            {
                std::lock_guard lock(mutex_);
                if (buffer_.size() >= want) {
                    const auto n = static_cast<std::ptrdiff_t>(want);
                    std::vector<std::uint8_t> out(buffer_.begin(), buffer_.begin() + n);
                    buffer_.erase(buffer_.begin(), buffer_.begin() + n);
                    return out;
                }
            }
            std::this_thread::sleep_for(1ms);
        }
        return {};
    }

private:
    void run() {
        constexpr std::size_t kSlots = 8;
        constexpr std::size_t kBytes = 20480;
        auto pipe = sim_.open_bulk_in(0x86, kSlots);
        if (!pipe) {
            std::fprintf(stderr, "sim_server: open_bulk_in: %s\n", pipe.error().message.c_str());
            return;
        }
        std::vector<std::vector<std::uint8_t>> bufs(kSlots, std::vector<std::uint8_t>(kBytes));
        std::size_t outstanding = 0;
        for (std::size_t i = 0; i < kSlots; ++i) {
            if ((*pipe)->submit(i, bufs[i])) {
                ++outstanding;
            }
        }
        bool aborted = false;
        while (outstanding > 0) {
            if (stop_ && !aborted) {
                (*pipe)->abort();
                aborted = true;
            }
            auto c = (*pipe)->wait(50ms);
            if (!c || !*c) {
                continue;
            }
            const auto done = **c;
            --outstanding;
            if (done.bytes > 0 && !done.aborted) {
                std::lock_guard lock(mutex_);
                const auto& b = bufs[done.slot];
                buffer_.insert(buffer_.end(), b.begin(),
                               b.begin() + static_cast<std::ptrdiff_t>(done.bytes));
            }
            if (!aborted && !done.aborted && !done.failed && (*pipe)->submit(done.slot, bufs[done.slot])) {
                ++outstanding;
            }
        }
    }

    pakon::sim::SimDevice& sim_;
    std::mutex mutex_;
    std::deque<std::uint8_t> buffer_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

struct Server {
    pakon::sim::SimDevice& sim;
    ImageBuffer& image;
    Log& log;
    std::mutex command_mutex;
};

// FPGA control write (PICM 0x82 sub0) with the acquire bit set.
bool acquire_on(std::span<const std::uint8_t> f) {
    return f.size() == 8 && f[0] == 0x02 && f[2] == 0x44 && f[4] == 0x82 && f[5] == 0x00 &&
           (f[6] & 0x01) != 0;
}

void serve(Server& s, int fd) {
    for (;;) {
        std::uint32_t hdr[4];
        if (!recv_all(fd, hdr, sizeof hdr)) {
            break;
        }
        const auto [code, outsz, inlen, odlen] = std::tuple{hdr[0], hdr[1], hdr[2], hdr[3]};
        std::vector<std::uint8_t> in(inlen), od(odlen);
        if ((inlen && !recv_all(fd, in.data(), inlen)) ||
            (odlen && !recv_all(fd, od.data(), odlen))) {
            break;
        }
        bool ok = true;
        if (code == kReadEp6) {
            const auto out = s.image.take(outsz, 2000ms);
            s.log.event(std::format("\"d\":\"ep6\",\"n\":{}", out.size()));
            ok = reply(fd, true, out);
        } else if (code == kIoctlCommand) {
            std::lock_guard lock(s.command_mutex);
            s.log.event(std::format("\"d\":\"cmd\",\"hex\":\"{}\"", hex(in)));
            if (acquire_on(in)) {
                s.image.new_window();
            }
            auto r = s.sim.command_exchange(in);
            if (r) {
                s.log.event(std::format("\"d\":\"rsp\",\"hex\":\"{}\"", hex(*r)));
                const auto n = std::min<std::size_t>(r->size(), outsz ? outsz : r->size());
                ok = reply(fd, true, std::span<const std::uint8_t>(r->data(), n));
            } else {
                ok = reply(fd, false, {});
            }
        } else if (code == kIoctlVendor && in.size() >= 10) {
            std::lock_guard lock(s.command_mutex);
            const bool inbound = in[0] != 0;
            const std::uint8_t req = in[4];
            const auto value = static_cast<std::uint16_t>(in[6] | (in[7] << 8));
            const auto index = static_cast<std::uint16_t>(in[8] | (in[9] << 8));
            if (inbound) {
                auto r = s.sim.control_read(req, value, index, static_cast<std::uint16_t>(outsz));
                s.log.event(std::format(
                    "\"d\":\"ep0\",\"req\":{},\"wValue\":{},\"wIndex\":{},\"dir\":\"in\",\"n\":{}",
                    req, value, index, r ? r->size() : 0));
                ok = r ? reply(fd, true, *r) : reply(fd, false, {});
            } else {
                auto r = s.sim.control_write(req, value, index);
                s.log.event(std::format(
                    "\"d\":\"ep0\",\"req\":{},\"wValue\":{},\"wIndex\":{},\"dir\":\"out\",\"n\":0{}",
                    req, value, index, r ? "" : ",\"blocked\":true"));
                ok = reply(fd, r.has_value(), {});
            }
        } else {
            std::fprintf(stderr, "sim_server: unsupported code 0x%08x (in %u, out %u)\n", code,
                         inlen, outsz);
            ok = reply(fd, false, {});
        }
        if (!ok) {
            break;
        }
    }
    ::close(fd);
}

} // namespace

int main(int argc, char** argv) {
    int port = 5140;
    std::string log_path;
    pakon::sim::SimConfig cfg;
    cfg.film = pakon::sim::FilmModel{300, 1500};
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i], v = argv[i + 1];
        if (a == "--port") {
            port = std::stoi(v);
        } else if (a == "--log") {
            log_path = v;
        } else if (a == "--speed") {
            cfg.speed = std::stod(v);
        } else if (a == "--lamp-ms") {
            cfg.lamp_ready_delay = std::chrono::milliseconds(std::stoi(v));
        } else if (a == "--film") {
            const auto comma = v.find(',');
            cfg.film = pakon::sim::FilmModel{std::stoul(v.substr(0, comma)),
                                             std::stoul(v.substr(comma + 1))};
        } else {
            std::fprintf(stderr, "usage: pakon_sim_server [--port N] [--log F] [--film L,N] "
                                 "[--speed X] [--lamp-ms N]\n");
            return 2;
        }
    }
    std::signal(SIGINT, [](int) { g_quit = true; });
    std::signal(SIGTERM, [](int) { g_quit = true; });

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    const int yes = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // local only
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(listener, 4) != 0) {
        std::perror("sim_server: bind/listen");
        return 1;
    }
    pakon::sim::SimDevice sim(cfg);
    ImageBuffer image(sim);
    Log log(log_path);
    Server server{sim, image, log, {}};
    std::fprintf(stderr, "sim_server: simulated F-135+ on 127.0.0.1:%d\n", port);

    while (!g_quit) {
        timeval tv{0, 200000};
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        if (::select(listener + 1, &set, nullptr, nullptr, &tv) <= 0) {
            continue;
        }
        const int fd = ::accept(listener, nullptr, nullptr);
        if (fd < 0) {
            continue;
        }
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
        std::fprintf(stderr, "sim_server: client connected\n");
        // Detached: a client may block in recv forever; the process ends
        // with _Exit below, never by unwinding under a live client.
        std::thread([&server, fd] { serve(server, fd); }).detach();
    }
    ::close(listener);
    const auto v = sim.violations();
    const auto u = sim.unknown_writes();
    std::fprintf(stderr, "sim_server: %zu frames, %zu safety violations, %zu unknown writes\n",
                 sim.frames().size(), v.size(), u.size());
    for (const auto& s : v) {
        std::fprintf(stderr, "  violation: %s\n", s.c_str());
    }
    for (const auto& s : u) {
        std::fprintf(stderr, "  unknown write: %s\n", s.c_str());
    }
    std::fflush(stderr);
    std::_Exit(v.empty() ? 0 : 3);
}
