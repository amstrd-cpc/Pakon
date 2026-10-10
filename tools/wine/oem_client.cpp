// oem_client.exe - drives the OEM COM server (tlx.dll -> TLB.dll) the way
// TLXClientDemo.exe does, from a script, for the dynamic RE runs under
// Wine against tests/support/sim_server.cpp (docs/OEM_RE.md §11). It is
// never pointed at a real scanner.
//
// Every interface, vtable slot and parameter type comes from tlx.dll's
// type library at run time (DispCallFunc on the declaring interface), so
// nothing here depends on a guessed vtable.
//
//   oem_client init                      InitializeScanner(1, 20000)
//   oem_client scan RES COLOR CONTROL    + ScanPictures(RES, COLOR, 0, 0,
//                                          CONTROL, "1000")
// RES 0/1/2 = Base 4/8/16; COLOR 1 = negative; CONTROL e.g. 0 or 8 (IR).
//
// Build: i686-w64-mingw32-g++ -std=c++20 -O2 -static -o oem_client.exe
//        oem_client.cpp -lole32 -loleaut32 -luuid

#include <windows.h>
#include <oleauto.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

const CLSID kTlxMainClass = {0xEA82986B, 0xE47C, 0x4C0F,
                             {0x97, 0xEA, 0xFB, 0x50, 0xED, 0x21, 0x6D, 0x2E}};

double g_t0 = 0;
double now() { return GetTickCount() / 1000.0 - g_t0; }

// Last Awake(operation, status) seen, for the wait loops.
volatile LONG g_last_op = -1;
volatile LONG g_last_status = -1;
volatile LONG g_awake_count = 0;

std::string narrow(const wchar_t* w) {
    std::string s;
    for (; w && *w; ++w) {
        s += static_cast<char>(*w < 128 ? *w : '?');
    }
    return s;
}

void awake(LONG op, LONG status) {
    g_last_op = op;
    g_last_status = status;
    InterlockedIncrement(&g_awake_count);
    std::printf("[%8.3f] Awake(operation %ld, status %ld)\n", now(), static_cast<long>(op),
                static_cast<long>(status));
    std::fflush(stdout);
}

// ICallBackClient implementation. Two vtables, chosen from the type
// library: Awake at slot 7 for an IDispatch-derived interface, slot 3 for
// an IUnknown-derived one. Invoke also routes the Awake DISPID.
IID g_cb_iid{};
bool g_cb_dispatch_based = true;
DISPID g_awake_dispid = 0;

struct CallBack {
    void** vtbl;
    LONG refs{1};
};

HRESULT STDMETHODCALLTYPE cb_qi(CallBack* self, REFIID riid, void** out) {
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, g_cb_iid) ||
        (g_cb_dispatch_based && IsEqualIID(riid, IID_IDispatch))) {
        *out = self;
        InterlockedIncrement(&self->refs);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE cb_addref(CallBack* self) {
    return static_cast<ULONG>(InterlockedIncrement(&self->refs));
}
ULONG STDMETHODCALLTYPE cb_release(CallBack* self) {
    return static_cast<ULONG>(InterlockedDecrement(&self->refs));
}
HRESULT STDMETHODCALLTYPE cb_gtic(CallBack*, UINT* n) {
    *n = 0;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE cb_gti(CallBack*, UINT, LCID, ITypeInfo**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE cb_gidon(CallBack*, REFIID, LPOLESTR* names, UINT n, LCID,
                                   DISPID* ids) {
    for (UINT i = 0; i < n; ++i) {
        ids[i] = (lstrcmpiW(names[i], L"Awake") == 0) ? g_awake_dispid : DISPID_UNKNOWN;
    }
    return S_OK;
}
HRESULT STDMETHODCALLTYPE cb_invoke(CallBack*, DISPID id, REFIID, LCID, WORD, DISPPARAMS* p,
                                    VARIANT*, EXCEPINFO*, UINT*) {
    if (id == g_awake_dispid && p && p->cArgs == 2) {
        VARIANT a, b;
        VariantInit(&a);
        VariantInit(&b);
        VariantChangeType(&a, &p->rgvarg[1], 0, VT_I4); // arguments arrive reversed
        VariantChangeType(&b, &p->rgvarg[0], 0, VT_I4);
        awake(a.lVal, b.lVal);
        return S_OK;
    }
    return DISP_E_MEMBERNOTFOUND;
}
HRESULT STDMETHODCALLTYPE cb_awake(CallBack*, LONG op, LONG status) {
    awake(op, status);
    return S_OK;
}

void* g_vtbl_dispatch[] = {reinterpret_cast<void*>(cb_qi),     reinterpret_cast<void*>(cb_addref),
                           reinterpret_cast<void*>(cb_release), reinterpret_cast<void*>(cb_gtic),
                           reinterpret_cast<void*>(cb_gti),    reinterpret_cast<void*>(cb_gidon),
                           reinterpret_cast<void*>(cb_invoke), reinterpret_cast<void*>(cb_awake)};
void* g_vtbl_unknown[] = {reinterpret_cast<void*>(cb_qi), reinterpret_cast<void*>(cb_addref),
                          reinterpret_cast<void*>(cb_release), reinterpret_cast<void*>(cb_awake)};

ITypeInfo* find_type(ITypeLib* lib, const wchar_t* name) {
    for (UINT i = 0; i < lib->GetTypeInfoCount(); ++i) {
        BSTR n = nullptr;
        lib->GetDocumentation(static_cast<INT>(i), &n, nullptr, nullptr, nullptr);
        const bool match = n && lstrcmpW(n, name) == 0;
        SysFreeString(n);
        if (match) {
            ITypeInfo* ti = nullptr;
            lib->GetTypeInfo(i, &ti);
            return ti;
        }
    }
    return nullptr;
}

void pump(DWORD ms) {
    const DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
}

// Calls a method by name: finds the interface that declares it in the
// type library, queries the object for that interface and calls the
// vtable slot with DispCallFunc, coercing each argument to the declared
// type. Out-parameters are allocated and printed.
struct Caller {
    IUnknown* object;
    ITypeLib* lib;

    HRESULT call(const wchar_t* name, std::vector<VARIANT> args) {
        for (UINT t = 0; t < lib->GetTypeInfoCount(); ++t) {
            ITypeInfo* ti = nullptr;
            TYPEKIND kind{};
            lib->GetTypeInfoType(t, &kind);
            if ((kind != TKIND_INTERFACE && kind != TKIND_DISPATCH) ||
                FAILED(lib->GetTypeInfo(t, &ti))) {
                continue;
            }
            TYPEATTR* ta = nullptr;
            ti->GetTypeAttr(&ta);
            if (kind == TKIND_DISPATCH) {
                // A dual interface: its vtable half is the -1 implemented type.
                HREFTYPE href = 0;
                ITypeInfo* vt = nullptr;
                const bool dual = (ta->wTypeFlags & TYPEFLAG_FDUAL) != 0 &&
                                  SUCCEEDED(ti->GetRefTypeOfImplType(static_cast<UINT>(-1), &href)) &&
                                  SUCCEEDED(ti->GetRefTypeInfo(href, &vt));
                ti->ReleaseTypeAttr(ta);
                ti->Release();
                if (!dual) {
                    continue;
                }
                ti = vt;
                ti->GetTypeAttr(&ta);
            }
            for (UINT f = 0; f < ta->cFuncs; ++f) {
                FUNCDESC* fd = nullptr;
                ti->GetFuncDesc(f, &fd);
                BSTR n = nullptr;
                UINT got = 0;
                ti->GetNames(fd->memid, &n, 1, &got);
                const bool match = n && lstrcmpW(n, name) == 0;
                SysFreeString(n);
                if (match) {
                    const IID iid = ta->guid;
                    const HRESULT hr = invoke(name, iid, fd, args);
                    ti->ReleaseFuncDesc(fd);
                    ti->ReleaseTypeAttr(ta);
                    ti->Release();
                    return hr;
                }
                ti->ReleaseFuncDesc(fd);
            }
            ti->ReleaseTypeAttr(ta);
            ti->Release();
        }
        std::printf("  %s: not in the type library\n", narrow(name).c_str());
        return E_FAIL;
    }

    HRESULT invoke(const wchar_t* name, const IID& iid, FUNCDESC* fd, std::vector<VARIANT>& args) {
        void* itf = nullptr;
        HRESULT hr = object->QueryInterface(iid, &itf);
        if (FAILED(hr)) {
            std::printf("  %s: QueryInterface 0x%08lx\n", narrow(name).c_str(),
                        static_cast<unsigned long>(hr));
            return hr;
        }
        const int nparams = fd->cParams;
        std::vector<VARIANT> store(static_cast<std::size_t>(nparams));
        std::vector<VARIANT> argv(static_cast<std::size_t>(nparams));
        std::vector<VARIANTARG*> ptrs(static_cast<std::size_t>(nparams));
        std::vector<VARTYPE> types(static_cast<std::size_t>(nparams));
        std::size_t next_in = 0;
        for (int i = 0; i < nparams; ++i) {
            auto& st = store[static_cast<std::size_t>(i)];
            auto& a = argv[static_cast<std::size_t>(i)];
            VariantInit(&st);
            VariantInit(&a);
            const ELEMDESC& ed = fd->lprgelemdescParam[i];
            VARTYPE vt = ed.tdesc.vt;
            const bool pointer = vt == VT_PTR && ed.tdesc.lptdesc;
            if (pointer) {
                vt = ed.tdesc.lptdesc->vt;
            }
            // [out] from the IDL when present; otherwise a pointer to a
            // plain type. A pointer to a user-defined type is an interface.
            const USHORT flags = ed.paramdesc.wParamFlags;
            const bool out = (flags & PARAMFLAG_FOUT) != 0 ||
                             (!(flags & PARAMFLAG_FIN) && pointer && vt != VT_USERDEFINED &&
                              vt != VT_UNKNOWN && vt != VT_DISPATCH);
            if (vt == VT_USERDEFINED) {
                vt = VT_I4; // enums and interface pointers are 4 bytes on i686
            }
            if (out) {
                st.vt = vt; // out-parameter
                a.vt = VT_I4;
                a.lVal = static_cast<LONG>(reinterpret_cast<INT_PTR>(&st.lVal));
            } else if (next_in < args.size()) {
                if (args[next_in].vt == VT_UNKNOWN) {
                    a = args[next_in++];
                    a.vt = VT_I4;
                    a.lVal = static_cast<LONG>(reinterpret_cast<INT_PTR>(args[next_in - 1].punkVal));
                } else {
                    VariantChangeType(&a, &args[next_in++], 0, vt);
                }
            }
            types[static_cast<std::size_t>(i)] = a.vt;
            ptrs[static_cast<std::size_t>(i)] = &a;
        }
        VARIANT result;
        VariantInit(&result);
        hr = DispCallFunc(itf, fd->oVft, CC_STDCALL, VT_ERROR, static_cast<UINT>(nparams),
                          types.data(), ptrs.data(), &result);
        const HRESULT ret = SUCCEEDED(hr) ? static_cast<HRESULT>(result.scode) : hr;
        std::printf("[%8.3f] %s -> 0x%08lx", now(), narrow(name).c_str(),
                    static_cast<unsigned long>(ret));
        for (int i = 0; i < nparams; ++i) {
            const VARIANT& v = store[static_cast<std::size_t>(i)];
            if (v.vt == VT_EMPTY) {
                continue;
            }
            if (v.vt == VT_BSTR) {
                std::printf(" [%d]=\"%s\"", i, narrow(v.bstrVal).c_str());
            } else if (v.vt == VT_I2 || v.vt == VT_BOOL || v.vt == VT_UI2) {
                std::printf(" [%d]=%d", i, static_cast<int>(v.iVal));
            } else {
                std::printf(" [%d]=%ld", i, static_cast<long>(v.lVal));
            }
        }
        std::printf("\n");
        std::fflush(stdout);
        static_cast<IUnknown*>(itf)->Release();
        return ret;
    }
};

VARIANT i4(long v) {
    VARIANT x;
    VariantInit(&x);
    x.vt = VT_I4;
    x.lVal = v;
    return x;
}

VARIANT str(const wchar_t* s) {
    VARIANT x;
    VariantInit(&x);
    x.vt = VT_BSTR;
    x.bstrVal = SysAllocString(s);
    return x;
}

VARIANT unk(IUnknown* u) {
    VARIANT x;
    VariantInit(&x);
    x.vt = VT_UNKNOWN;
    x.punkVal = u;
    return x;
}

// Waits for Awake(op, End/Complete) or a timeout.
bool wait_for(LONG op, DWORD ms) {
    const DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end) {
        pump(50);
        if (g_last_op == op && (g_last_status == 3000 || g_last_status == 2000)) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    g_t0 = GetTickCount() / 1000.0;
    const std::string mode = argc > 1 ? argv[1] : "init";
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::printf("CoInitializeEx 0x%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }
    ITypeLib* lib = nullptr;
    hr = LoadTypeLibEx(L"tlx.dll", REGKIND_NONE, &lib);
    if (FAILED(hr)) {
        std::printf("LoadTypeLibEx(tlx.dll) 0x%08lx - run from the COM server directory\n",
                    static_cast<unsigned long>(hr));
        return 1;
    }
    if (ITypeInfo* cb = find_type(lib, L"ICallBackClient")) {
        TYPEATTR* ta = nullptr;
        cb->GetTypeAttr(&ta);
        g_cb_iid = ta->guid;
        g_cb_dispatch_based = ta->typekind == TKIND_DISPATCH ||
                              (ta->wTypeFlags & (TYPEFLAG_FDUAL | TYPEFLAG_FDISPATCHABLE)) != 0;
        for (UINT i = 0; i < ta->cFuncs; ++i) {
            FUNCDESC* fd = nullptr;
            cb->GetFuncDesc(i, &fd);
            BSTR n = nullptr;
            UINT got = 0;
            cb->GetNames(fd->memid, &n, 1, &got);
            if (n && lstrcmpiW(n, L"Awake") == 0) {
                g_awake_dispid = fd->memid;
                std::printf("ICallBackClient: typekind %d flags 0x%x, Awake memid %ld, "
                            "vtable offset %d\n",
                            static_cast<int>(ta->typekind), ta->wTypeFlags,
                            static_cast<long>(fd->memid), fd->oVft);
            }
            SysFreeString(n);
            cb->ReleaseFuncDesc(fd);
        }
        cb->ReleaseTypeAttr(ta);
    } else {
        std::printf("ICallBackClient not found in the type library\n");
    }

    IUnknown* object = nullptr;
    hr = CoCreateInstance(kTlxMainClass, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown,
                          reinterpret_cast<void**>(&object));
    if (FAILED(hr)) {
        std::printf("CoCreateInstance(TLXMainClass) 0x%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }
    Caller c{object, lib};

    static CallBack callback{g_cb_dispatch_based ? g_vtbl_dispatch : g_vtbl_unknown};
    c.call(L"CBAdvise", {unk(reinterpret_cast<IUnknown*>(&callback))});

    c.call(L"InitializeScanner", {i4(1), i4(20000)});
    const bool init_ok = wait_for(0, 120000);
    std::printf("[%8.3f] initialize %s\n", now(), init_ok ? "completed" : "TIMED OUT");
    c.call(L"GetInitializeWarnings", {});
    c.call(L"GetAndClearLastError", {i4(0)});
    c.call(L"GetScannerInfo000", {});

    if (mode == "scan" && argc >= 5) {
        const long res = std::atol(argv[2]);
        const long color = std::atol(argv[3]);
        const long control = std::strtol(argv[4], nullptr, 0);
        c.call(L"ScanPictures", {i4(res), i4(color), i4(0), i4(0), i4(control), str(L"1000")});
        const bool scan_ok = wait_for(34, 600000);
        std::printf("[%8.3f] scan %s\n", now(), scan_ok ? "completed" : "TIMED OUT");
        c.call(L"GetAndClearLastError", {i4(0)});
        c.call(L"GetRollCountScanGroup", {});
    }
    std::printf("[%8.3f] %ld Awake calls\n", now(), static_cast<long>(g_awake_count));
    std::fflush(stdout);
    // The server's worker threads may still hold the callback: leave
    // without tearing COM down under them.
    ExitProcess(0);
}
