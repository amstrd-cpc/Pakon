// Logging stand-in for the OEM PakonIMAu.dll (Windows XP, 32-bit).
//
// Purpose: record exactly how the OEM stack (TLB.dll) drives the Pakon
// image pipeline — call order, argument values and the memory they point
// to — so this project can drive the same DLL (docs/OEM_RE.md §13). It
// changes nothing: every call is logged and forwarded unchanged to the
// original, renamed PakonIMAu_real.dll in the same directory.
//
// The 18 exports are __cdecl (plain `ret`, caller cleans the stack;
// Ghidra 12.1.4 of PakonIMAu.dll MD5 eea9dcf7…512242d), so each proxy
// takes 16 DWORDs and passes all 16 on: reading slots beyond the real
// argument count only reads the caller's stack, and the caller pops.
//
// XP: no C runtime (custom entry point, /GS-, kernel32/user32 only),
// subsystem 5.01. Output next to the DLL: PakonIMAu_proxy.log (text) and
// PakonIMAu_proxy.bin (records: u32 magic 'PKPX', u32 seq, u32 arg,
// u32 phase 0=before/1=after, u32 address, u32 length, bytes).

#include <windows.h>

namespace {

constexpr int kArgs = 16;
constexpr DWORD kDumpBytes = 4096;

HMODULE g_self = nullptr;
HMODULE g_real = nullptr;
CRITICAL_SECTION g_lock;
HANDLE g_log = INVALID_HANDLE_VALUE;
HANDLE g_bin = INVALID_HANDLE_VALUE;
DWORD g_seq = 0;
char g_dir[MAX_PATH];

void write(HANDLE h, const void* p, DWORD n) {
    DWORD done = 0;
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, p, n, &done, nullptr);
    }
}

void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        write(g_log, buf, static_cast<DWORD>(n));
    }
}

HANDLE open_out(const char* name) {
    char path[MAX_PATH];
    lstrcpyA(path, g_dir);
    lstrcatA(path, name);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, nullptr, FILE_END);
    }
    return h;
}

bool init_locked() {
    if (g_real) {
        return true;
    }
    g_log = open_out("PakonIMAu_proxy.log");
    g_bin = open_out("PakonIMAu_proxy.bin");
    // The original, renamed. Windows Explorer hides extensions by default,
    // so a rename may produce "PakonIMAu_real.dll.dll": accept both.
    // LOAD_WITH_ALTERED_SEARCH_PATH: its own dependencies (KODAKCMS.dll,
    // xerces-c_2_2_0.dll, ekjpegi.dll, MSVCR71.dll) are looked for in its
    // directory first, whatever process loaded us (error 126 otherwise,
    // first VM run 2026-10-14).
    static const char* const kNames[] = {"PakonIMAu_real.dll", "PakonIMAu_real.dll.dll"};
    logf("=== proxy start, dir %s\r\n", g_dir);
    for (const char* name : kNames) {
        char path[MAX_PATH];
        lstrcpyA(path, g_dir);
        lstrcatA(path, name);
        const DWORD attr = GetFileAttributesA(path);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            logf("    %s: not there\r\n", name);
            continue;
        }
        g_real = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        logf("    %s: %s (error %lu)\r\n", name, g_real ? "loaded" : "NOT LOADED",
             g_real ? 0UL : GetLastError());
        if (g_real) {
            break;
        }
    }
    return g_real != nullptr;
}

// The readable span starting at p, up to kDumpBytes.
DWORD readable(const void* p) {
    if (reinterpret_cast<DWORD>(p) < 0x10000) {
        return 0;
    }
    DWORD n = kDumpBytes;
    while (n >= 16 && IsBadReadPtr(p, n)) {
        n /= 2;
    }
    return n >= 16 ? n : 0;
}

void dump(DWORD seq, int arg, DWORD phase, const void* p) {
    const DWORD n = readable(p);
    if (n == 0) {
        return;
    }
    const DWORD head[6] = {0x5850'4B50u, seq, static_cast<DWORD>(arg), phase,
                           reinterpret_cast<DWORD>(p), n};
    write(g_bin, head, sizeof head);
    write(g_bin, p, n);
    // A short text preview: first 32 bytes hex, and the string if it is one.
    const auto* b = static_cast<const unsigned char*>(p);
    logf("    a%d%s @%08lx [%lu]:", arg, phase ? "'" : "", reinterpret_cast<DWORD>(p), n);
    for (int i = 0; i < 32; ++i) {
        logf(" %02x", b[i]);
    }
    if (b[1] == 0 && b[0] >= 0x20 && b[0] < 0x7f) {
        char s[200];
        int k = 0;
        for (int i = 0; i + 1 < static_cast<int>(n) && k < 199; i += 2) {
            if (b[i] == 0 && b[i + 1] == 0) {
                break;
            }
            s[k++] = b[i + 1] == 0 && b[i] >= 0x20 && b[i] < 0x7f ? static_cast<char>(b[i]) : '?';
        }
        s[k] = 0;
        logf("  L\"%s\"", s);
    }
    logf("\r\n");
}

using Fn = DWORD(__cdecl*)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                           DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);

DWORD forward(const char* name, const DWORD* a) {
    EnterCriticalSection(&g_lock);
    const bool ok = init_locked();
    const DWORD seq = ++g_seq;
    Fn fn = ok ? reinterpret_cast<Fn>(GetProcAddress(g_real, name)) : nullptr;
    logf("#%lu %s tid %lu tick %lu\r\n  args:", seq, name, GetCurrentThreadId(), GetTickCount());
    for (int i = 0; i < kArgs; ++i) {
        logf(" %08lx", a[i]);
    }
    logf("\r\n");
    for (int i = 0; i < kArgs; ++i) {
        dump(seq, i, 0, reinterpret_cast<const void*>(a[i]));
    }
    LeaveCriticalSection(&g_lock);
    if (!fn) {
        return 0xFFFFFFFFu;
    }
    // Not under the lock: the OEM may call from several threads and the
    // real pipeline is long-running.
    const DWORD r = fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11],
                       a[12], a[13], a[14], a[15]);
    EnterCriticalSection(&g_lock);
    logf("#%lu %s -> %08lx tick %lu\r\n", seq, name, r, GetTickCount());
    for (int i = 0; i < kArgs; ++i) {
        dump(seq, i, 1, reinterpret_cast<const void*>(a[i]));
    }
    FlushFileBuffers(g_log);
    LeaveCriticalSection(&g_lock);
    return r;
}

} // namespace

#define PROXY(name)                                                                            \
    extern "C" DWORD __cdecl proxy_##name(DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4,     \
                                          DWORD a5, DWORD a6, DWORD a7, DWORD a8, DWORD a9,     \
                                          DWORD a10, DWORD a11, DWORD a12, DWORD a13,           \
                                          DWORD a14, DWORD a15) {                               \
        const DWORD a[kArgs] = {a0, a1, a2, a3, a4, a5, a6, a7,                                 \
                                a8, a9, a10, a11, a12, a13, a14, a15};                          \
        return forward(#name, a);                                                               \
    }

PROXY(PIBegin)
PROXY(PIEnd)
PROXY(PISaveFilePlanar_8)
PROXY(PIFileSpecsPlanar_8)
PROXY(PIFileOpenPlanar)
PROXY(PIColorAdjustPlanar)
PROXY(PIRotatePlanar)
PROXY(PIScaleAndRotatePlanar)
PROXY(PIColorCorrectColNegPlanarScan)
PROXY(PIColorCorrectColNegPlanarSave)
PROXY(PIColorCorrectColRevPlanar)
PROXY(PIAnselStartNewRoll)
PROXY(PIAnselAddScene)
PROXY(PIAnselEndRoll)
PROXY(PIAnselAnalyzeRoll)
PROXY(PIAnselAnalyzeScene)
PROXY(PIAnselColorSceneBalancePlanar)
PROXY(PIAnselDeleteRoll)
PROXY(PIAnselDeleteScene)

extern "C" BOOL WINAPI ProxyEntry(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = self;
        InitializeCriticalSection(&g_lock);
        GetModuleFileNameA(self, g_dir, MAX_PATH);
        for (int i = lstrlenA(g_dir); i > 0; --i) {
            if (g_dir[i - 1] == '\\') {
                g_dir[i] = 0;
                break;
            }
        }
        DisableThreadLibraryCalls(self);
    }
    return TRUE;
}
