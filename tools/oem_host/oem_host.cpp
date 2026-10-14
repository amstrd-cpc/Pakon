// 32-bit host for the OEM PakonIMAu.dll (Windows 10/11 x64 via WoW64).
//
// Stage 2 of the image pipeline (docs/OEM_RE.md §13): drive the OEM's own
// Ansel / ColorAdjust code with our data. The DLL and its data stay where
// the user installed them (<oem dir>: PakonIMAu.dll, KODAKCMS, xerces,
// ekjpegi, MSVC 7.1 runtimes, Config\, anselinstalldir\); nothing of them
// is in this repository.
//
// Calls, layouts and argument values come from logging TLB.dll's calls in
// the XP VM (tools/oem_proxy, runs of 2026-10-14):
//   PIBegin(install_dir, 0, 0, settings*, 0, 0, 0, romm, rpd, srgb,
//           ColRevLut1, ColRevLutS6) -- settings +0x60/+0x68/+0x70 carry the
//           unit's EEPROM ColNeg offsets (173, 447, 623 on serial 17373);
//   PIColorAdjustPlanar(desc*) -- desc {in u16 planar, out u8, w, h, ...,
//           +0x10..+0x28 user adjustments, +0x48 mode, +0x4c toning,
//           +0x50 saturation profile}.
//
// Commands:
//   coloradjust <oem dir> <settings.bin> <desc.bin> <in.raw> <w> <h> <out.raw>
//       run PIColorAdjustPlanar once on a 16-bit planar image; desc.bin is
//       a captured descriptor whose buffer pointers are replaced.

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using PIBeginFn = int(__cdecl*)(const wchar_t*, int, int, void*, int, int, int, const wchar_t*,
                                const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*);
using PIEndFn = int(__cdecl*)();
using PIColorAdjustFn = int(__cdecl*)(void*);

std::vector<unsigned char> read_file(const char* path) {
    std::vector<unsigned char> out;
    if (FILE* f = std::fopen(path, "rb")) {
        unsigned char buf[1 << 16];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
            out.insert(out.end(), buf, buf + n);
        }
        std::fclose(f);
    }
    return out;
}

bool write_file(const char* path, const void* p, size_t n) {
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        return false;
    }
    const bool ok = std::fwrite(p, 1, n, f) == n;
    std::fclose(f);
    return ok;
}

struct Oem {
    HMODULE dll = nullptr;
    PIBeginFn begin = nullptr;
    PIEndFn end = nullptr;
    PIColorAdjustFn color_adjust = nullptr;
};

bool load(const std::wstring& dir, Oem& oem) {
    SetDllDirectoryW(dir.c_str());
    oem.dll = LoadLibraryExW((dir + L"\\PakonIMAu.dll").c_str(), nullptr,
                             LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!oem.dll) {
        std::fprintf(stderr, "LoadLibrary PakonIMAu.dll failed: %lu\n", GetLastError());
        return false;
    }
    oem.begin = reinterpret_cast<PIBeginFn>(GetProcAddress(oem.dll, "PIBegin"));
    oem.end = reinterpret_cast<PIEndFn>(GetProcAddress(oem.dll, "PIEnd"));
    oem.color_adjust = reinterpret_cast<PIColorAdjustFn>(GetProcAddress(oem.dll, "PIColorAdjustPlanar"));
    return oem.begin && oem.end && oem.color_adjust;
}

int begin(const Oem& oem, const std::wstring& dir, std::vector<unsigned char>& settings) {
    const int r = oem.begin(dir.c_str(), 0, 0, settings.data(), 0, 0, 0,
                            L"\\Config\\ColorCorrection\\romm.pf",
                            L"\\Config\\ColorCorrection\\rpd.pf",
                            L"\\Config\\ColorCorrection\\srgb.pf",
                            L"\\Config\\ColorCorrection\\ColRevLut1.pf",
                            L"\\Config\\ColorCorrection\\ColRevLutS6.lut");
    std::printf("PIBegin -> %d\n", r);
    return r;
}

std::wstring widen(const char* s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

int cmd_coloradjust(int argc, char** argv) {
    if (argc != 9) {
        std::fprintf(stderr, "usage: oem_host coloradjust <oem dir> <settings.bin> <desc.bin> "
                             "<in.raw> <w> <h> <out.raw>\n");
        return 2;
    }
    const std::wstring dir = widen(argv[2]);
    auto settings = read_file(argv[3]);
    auto desc = read_file(argv[4]);
    auto in = read_file(argv[5]);
    const unsigned w = static_cast<unsigned>(std::atoi(argv[6]));
    const unsigned h = static_cast<unsigned>(std::atoi(argv[7]));
    if (settings.size() < 0x80 || desc.size() < 0x60 || in.size() != size_t{w} * h * 3 * 2) {
        std::fprintf(stderr, "bad inputs: settings %zu, desc %zu, image %zu (want %u)\n",
                     settings.size(), desc.size(), in.size(), w * h * 6);
        return 1;
    }
    Oem oem;
    if (!load(dir, oem) || begin(oem, dir, settings) != 0) {
        return 1;
    }
    std::vector<unsigned char> out(size_t{w} * h * 3);
    auto* d = reinterpret_cast<DWORD*>(desc.data());
    d[0] = reinterpret_cast<DWORD>(in.data());
    d[1] = reinterpret_cast<DWORD>(out.data());
    d[2] = w;
    d[3] = h;
    const DWORD t0 = GetTickCount();
    const int r = oem.color_adjust(desc.data());
    std::printf("PIColorAdjustPlanar -> %d (%lu ms)\n", r, GetTickCount() - t0);
    oem.end();
    return write_file(argv[8], out.data(), out.size()) ? r : 1;
}

// replay <oem dir> <settings.bin> <function> <spec.txt>
//   Calls one PakonIMAu export with 16 DWORD arguments described by the
//   spec (cdecl, so unused trailing slots are harmless), one per line:
//     <i> val <hex>             literal
//     <i> blob <file> [bytes]   file contents in a fresh buffer (at least
//                               `bytes` long), pointer passed
//     <i> buf <bytes>           zeroed buffer
//     <i> ref <j> <offset>      pointer into argument j's buffer
//     poke <i> <offset> <hex>   write a DWORD into argument i's buffer
//                               (e.g. a buffer pointer inside a descriptor)
//     pokeref <i> <offset> <j>  write argument j's buffer address there
//     out <i> <bytes> <file>    after the call, write that buffer
//   Several calls can share buffers by naming one spec per call is not
//   needed: buffers live for this one call only.
int cmd_replay(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: oem_host replay <oem dir> <settings.bin> <function> <spec>\n");
        return 2;
    }
    const std::wstring dir = widen(argv[2]);
    auto settings = read_file(argv[3]);
    Oem oem;
    if (!load(dir, oem) || begin(oem, dir, settings) != 0) {
        return 1;
    }
    using Fn16 = DWORD(__cdecl*)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                                 DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
    auto fn = reinterpret_cast<Fn16>(GetProcAddress(oem.dll, argv[4]));
    if (!fn) {
        std::fprintf(stderr, "no export %s\n", argv[4]);
        return 1;
    }
    std::vector<unsigned char> bufs[16];
    DWORD a[16] = {};
    struct Out {
        int i;
        size_t n;
        std::string file;
    };
    std::vector<Out> outs;
    FILE* spec = std::fopen(argv[5], "r");
    if (!spec) {
        std::fprintf(stderr, "cannot read %s\n", argv[5]);
        return 1;
    }
    char line[1024];
    while (std::fgets(line, sizeof line, spec)) {
        char kind[16] = {}, s1[700] = {}, s2[64] = {}, s3[700] = {};
        int i = -1;
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (std::sscanf(line, "poke %d %s %s", &i, s1, s2) == 3) {
            *reinterpret_cast<DWORD*>(bufs[i].data() + std::strtoul(s1, nullptr, 0)) =
                std::strtoul(s2, nullptr, 16);
            continue;
        }
        if (std::sscanf(line, "pokeref %d %s %s", &i, s1, s2) == 3) {
            *reinterpret_cast<DWORD*>(bufs[i].data() + std::strtoul(s1, nullptr, 0)) =
                a[std::atoi(s2)];
            continue;
        }
        if (std::sscanf(line, "out %d %s %699s", &i, s1, s3) == 3) {
            outs.push_back({i, std::strtoul(s1, nullptr, 0), s3});
            continue;
        }
        if (std::sscanf(line, "%d %15s %699s %63s", &i, kind, s1, s2) < 3 || i < 0 || i > 15) {
            std::fprintf(stderr, "bad spec line: %s", line);
            return 1;
        }
        if (std::strcmp(kind, "val") == 0) {
            a[i] = std::strtoul(s1, nullptr, 16);
        } else if (std::strcmp(kind, "blob") == 0) {
            bufs[i] = read_file(s1);
            const size_t want = s2[0] ? std::strtoul(s2, nullptr, 0) : 0;
            if (bufs[i].empty()) {
                std::fprintf(stderr, "cannot read %s\n", s1);
                return 1;
            }
            if (bufs[i].size() < want) {
                bufs[i].resize(want);
            }
            a[i] = reinterpret_cast<DWORD>(bufs[i].data());
        } else if (std::strcmp(kind, "buf") == 0) {
            bufs[i].assign(std::strtoul(s1, nullptr, 0), 0);
            a[i] = reinterpret_cast<DWORD>(bufs[i].data());
        } else if (std::strcmp(kind, "ref") == 0) {
            a[i] = a[std::atoi(s1)] + std::strtoul(s2, nullptr, 0);
        }
    }
    std::fclose(spec);
    const DWORD t0 = GetTickCount();
    const DWORD r = fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11],
                       a[12], a[13], a[14], a[15]);
    std::printf("%s -> %08lx (%lu ms)\n", argv[4], r, GetTickCount() - t0);
    for (const auto& o : outs) {
        const size_t n = std::min(o.n, bufs[o.i].size());
        write_file(o.file.c_str(), bufs[o.i].data(), n);
        std::printf("written %s (%zu bytes)\n", o.file.c_str(), n);
    }
    oem.end();
    return 0;
}

// script <oem dir> <settings.bin> <script.txt>
//   Several calls in one session, sharing named buffers (Ansel keeps roll
//   and scene state between calls). One statement per line:
//     buf <name> blob <file> [bytes]   file contents (padded to bytes)
//     buf <name> zero <bytes>
//     poke <name> <offset> <hex>       DWORD into a buffer
//     pokeref <name> <offset> <name2>  address of name2 into a buffer
//     call <export> <arg>...           up to 16 args: hex literal or @name
//     save <name> <bytes> <file>
//   Paths must not contain spaces (run from the data directory).
int cmd_script(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: oem_host script <oem dir> <settings.bin> <script>\n");
        return 2;
    }
    const std::wstring dir = widen(argv[2]);
    auto settings = read_file(argv[3]);
    Oem oem;
    if (!load(dir, oem) || begin(oem, dir, settings) != 0) {
        return 1;
    }
    using Fn16 = DWORD(__cdecl*)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                                 DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
    struct Named {
        std::string name;
        std::vector<unsigned char> data;
    };
    std::vector<Named*> named;
    const auto find = [&](const std::string& n) -> Named* {
        for (auto* b : named) {
            if (b->name == n) {
                return b;
            }
        }
        return nullptr;
    };
    FILE* f = std::fopen(argv[4], "r");
    if (!f) {
        std::fprintf(stderr, "cannot read %s\n", argv[4]);
        return 1;
    }
    char line[2048];
    int lineno = 0;
    int rc = 0;
    while (rc == 0 && std::fgets(line, sizeof line, f)) {
        ++lineno;
        std::vector<std::string> t;
        for (char* p = std::strtok(line, " \t\r\n"); p; p = std::strtok(nullptr, " \t\r\n")) {
            t.emplace_back(p);
        }
        if (t.empty() || t[0][0] == '#') {
            continue;
        }
        const auto fail = [&](const char* why) {
            std::fprintf(stderr, "line %d: %s\n", lineno, why);
            rc = 1;
        };
        if (t[0] == "buf" && t.size() >= 4) {
            auto* b = find(t[1]);
            if (!b) {
                b = new Named{t[1], {}};
                named.push_back(b);
            }
            if (t[2] == "blob") {
                b->data = read_file(t[3].c_str());
                if (b->data.empty()) {
                    fail("cannot read blob");
                    continue;
                }
                if (t.size() >= 5 && b->data.size() < std::strtoul(t[4].c_str(), nullptr, 0)) {
                    b->data.resize(std::strtoul(t[4].c_str(), nullptr, 0));
                }
            } else {
                b->data.assign(std::strtoul(t[3].c_str(), nullptr, 0), 0);
            }
        } else if ((t[0] == "poke" || t[0] == "pokeref") && t.size() == 4) {
            auto* b = find(t[1]);
            const size_t off = std::strtoul(t[2].c_str(), nullptr, 0);
            if (!b || off + 4 > b->data.size()) {
                fail("bad poke");
                continue;
            }
            DWORD v = 0;
            if (t[0] == "poke") {
                v = std::strtoul(t[3].c_str(), nullptr, 16);
            } else if (auto* r = find(t[3])) {
                v = reinterpret_cast<DWORD>(r->data.data());
            } else {
                fail("unknown buffer");
                continue;
            }
            *reinterpret_cast<DWORD*>(b->data.data() + off) = v;
        } else if (t[0] == "call" && t.size() >= 2) {
            auto fn = reinterpret_cast<Fn16>(GetProcAddress(oem.dll, t[1].c_str()));
            if (!fn) {
                fail("unknown export");
                continue;
            }
            DWORD a[16] = {};
            for (size_t i = 2; i < t.size() && i - 2 < 16; ++i) {
                if (t[i][0] == '@') {
                    auto* b = find(t[i].substr(1));
                    if (!b) {
                        fail("unknown buffer");
                        break;
                    }
                    a[i - 2] = reinterpret_cast<DWORD>(b->data.data());
                } else {
                    a[i - 2] = std::strtoul(t[i].c_str(), nullptr, 16);
                }
            }
            if (rc) {
                continue;
            }
            const DWORD t0 = GetTickCount();
            const DWORD r = fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10],
                               a[11], a[12], a[13], a[14], a[15]);
            std::printf("%s -> %08lx (%lu ms)\n", t[1].c_str(), r, GetTickCount() - t0);
        } else if (t[0] == "save" && t.size() == 4) {
            auto* b = find(t[1]);
            if (!b) {
                fail("unknown buffer");
                continue;
            }
            const size_t n = std::min<size_t>(std::strtoul(t[2].c_str(), nullptr, 0), b->data.size());
            write_file(t[3].c_str(), b->data.data(), n);
            std::printf("saved %s (%zu bytes)\n", t[3].c_str(), n);
        } else {
            fail("unknown statement");
        }
    }
    std::fclose(f);
    oem.end();
    for (auto* b : named) {
        delete b;
    }
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "script") == 0) {
        return cmd_script(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "coloradjust") == 0) {
        return cmd_coloradjust(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "replay") == 0) {
        return cmd_replay(argc, argv);
    }
    std::fprintf(stderr, "usage: oem_host coloradjust ... | replay ...\n");
    return 2;
}
