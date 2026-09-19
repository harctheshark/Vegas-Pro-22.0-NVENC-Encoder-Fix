// SPDX-License-Identifier: MIT
//
// nvcuda.dll proxy - a DIAGNOSTIC, not part of the fix.
//
// WHY THIS EXISTS
// ---------------
// VEGAS's mxavcaacplug.dll reaches NVENC through CUDA. Its string table shows
// the selection logic it inherited from the NVIDIA sample code:
//
//     cuInit error:0x%x
//     cuDeviceGetCount error:0x%x
//     cuDeviceGet error:0x%x
//     cuDeviceComputeCapability error:0x%x
//     cuCtxCreate error:0x%x
//     cuCtxPopCurrent error:0x%x
//     cuCtxDestroy error:0x%x
//     Invalid Device Id = %d
//     GPU %d does not have NVENC capabilities exiting
//
// Every one of those runs BEFORE the first NVENC call, so an nvEncodeAPI64.dll
// shim cannot see any of it. On the machine this was written for, the NVENC
// shim records a capability probe that succeeds - OpenEncodeSessionEx then
// DestroyEncoder - after which the render makes no NVENC call at all. The
// reason has to be in the CUDA layer or in the host's own logic, and this
// proxy is how the CUDA layer becomes visible.
//
// It forwards all 769 exports: 756 through register-exact assembly tail jumps
// (generated, since most signatures are undocumented) and 13 through C wrappers
// that log. It changes no behaviour - every call is passed straight through and
// the real result returned.
//
// Install it only while diagnosing, and remove it afterwards.

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>

#include "generated/nvcuda_names.h"

#define VNC_VERSION "1.0.0"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;

// ---------------------------------------------------------------- logging --

static CRITICAL_SECTION g_log_lock;
static WCHAR g_log_path[MAX_PATH];
static int   g_log_ready;
static int   g_log_level = 1;

static void vnc_log(int level, const char *fmt, ...)
{
    if (!g_log_ready || level > g_log_level) return;

    char line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int off = _snprintf_s(line, sizeof(line), _TRUNCATE, "%02d:%02d:%02d.%03d [%5lu] ",
                          st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                          GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line + off, sizeof(line) - off, _TRUNCATE, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_log_lock);
    HANDLE h = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, line, (DWORD)strlen(line), &w, NULL);
        WriteFile(h, "\r\n", 2, &w, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_log_lock);
}

// CUDA result codes the plugin's paths can realistically produce.
static const char *cu_err(CUresult r)
{
    switch (r) {
    case 0:   return "CUDA_SUCCESS";
    case 1:   return "ERROR_INVALID_VALUE";
    case 2:   return "ERROR_OUT_OF_MEMORY";
    case 3:   return "ERROR_NOT_INITIALIZED";
    case 4:   return "ERROR_DEINITIALIZED";
    case 100: return "ERROR_NO_DEVICE";
    case 101: return "ERROR_INVALID_DEVICE";
    case 200: return "ERROR_INVALID_IMAGE";
    case 201: return "ERROR_INVALID_CONTEXT";
    case 205: return "ERROR_MAP_FAILED";
    case 214: return "ERROR_NVLINK_UNCORRECTABLE";
    case 215: return "ERROR_JIT_COMPILER_NOT_FOUND";
    case 216: return "ERROR_UNSUPPORTED_PTX_VERSION";
    case 218: return "ERROR_INVALID_PTX";
    case 219: return "ERROR_INVALID_GRAPHICS_CONTEXT";
    case 222: return "ERROR_UNSUPPORTED_DEVSIDE_SYNC";
    case 300: return "ERROR_INVALID_SOURCE";
    case 301: return "ERROR_FILE_NOT_FOUND";
    case 304: return "ERROR_OPERATING_SYSTEM";
    case 400: return "ERROR_INVALID_HANDLE";
    case 500: return "ERROR_NOT_FOUND";
    case 600: return "ERROR_NOT_READY";
    case 999: return "ERROR_UNKNOWN";
    default:  return "CUDA_ERROR_<other>";
    }
}

#define VNC_TRACE(r, fmt, ...)                                                 \
    do {                                                                       \
        static LONG _seen = 0;                                                 \
        if ((r) != 0 || InterlockedExchange(&_seen, 1) == 0)                   \
            vnc_log(1, fmt, __VA_ARGS__);                                      \
    } while (0)

// ------------------------------------------------------------- real nvcuda --

void *g_vnc_fns[VNC_FORWARD_COUNT];      // referenced by the generated thunks

static HMODULE g_real;

typedef CUresult(*PFN_cuInit)(unsigned int);
typedef CUresult(*PFN_cuDeviceGetCount)(int *);
typedef CUresult(*PFN_cuDeviceGet)(CUdevice *, int);
typedef CUresult(*PFN_cuDeviceGetName)(char *, int, CUdevice);
typedef CUresult(*PFN_cuDeviceComputeCapability)(int *, int *, CUdevice);
typedef CUresult(*PFN_cuCtxCreate)(CUcontext *, unsigned int, CUdevice);
typedef CUresult(*PFN_cuCtxDestroy)(CUcontext);
typedef CUresult(*PFN_cuCtxPopCurrent)(CUcontext *);
typedef CUresult(*PFN_cuCtxPushCurrent)(CUcontext);

static struct {
    PFN_cuInit                    init;
    PFN_cuDeviceGetCount          getCount;
    PFN_cuDeviceGet               get;
    PFN_cuDeviceGetName           getName;
    PFN_cuDeviceComputeCapability compCap;
    PFN_cuCtxCreate               ctxCreate, ctxCreate2;
    PFN_cuCtxDestroy              ctxDestroy, ctxDestroy2;
    PFN_cuCtxPopCurrent           ctxPop, ctxPop2;
    PFN_cuCtxPushCurrent          ctxPush, ctxPush2;
} R;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK vnc_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;

    InitializeCriticalSection(&g_log_lock);

    WCHAR base[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
        _snwprintf_s(g_log_path, MAX_PATH, _TRUNCATE, L"%s\\vegas-nvenc-fix", base);
        CreateDirectoryW(g_log_path, NULL);
        _snwprintf_s(g_log_path, MAX_PATH, _TRUNCATE,
                     L"%s\\vegas-nvenc-fix\\cuda-shim.log", base);
        g_log_ready = 1;
    }

    WCHAR host[MAX_PATH] = L"?";
    GetModuleFileNameW(NULL, host, MAX_PATH);
    vnc_log(1, "--- nvcuda proxy " VNC_VERSION " loaded ---");
    vnc_log(1, "host process : %S", host);

    WCHAR path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n >= MAX_PATH - 16) { vnc_log(1, "FATAL: GetSystemDirectory"); return TRUE; }
    wcscat_s(path, MAX_PATH, L"\\nvcuda.dll");

    WCHAR self[MAX_PATH] = L"";
    GetModuleFileNameW((HMODULE)&__ImageBase, self, MAX_PATH);
    if (_wcsicmp(self, path) == 0) {
        vnc_log(1, "FATAL: proxy installed as the system DLL - refusing to self-load");
        return TRUE;
    }

    g_real = LoadLibraryW(path);
    if (!g_real) { vnc_log(1, "FATAL: LoadLibrary(%S) err=%lu", path, GetLastError()); return TRUE; }
    vnc_log(1, "real nvcuda  : %S", path);

    for (int i = 0; i < VNC_FORWARD_COUNT; i++)
        g_vnc_fns[i] = (void *)GetProcAddress(g_real, g_vnc_names[i]);

    R.init       = (PFN_cuInit)GetProcAddress(g_real, "cuInit");
    R.getCount   = (PFN_cuDeviceGetCount)GetProcAddress(g_real, "cuDeviceGetCount");
    R.get        = (PFN_cuDeviceGet)GetProcAddress(g_real, "cuDeviceGet");
    R.getName    = (PFN_cuDeviceGetName)GetProcAddress(g_real, "cuDeviceGetName");
    R.compCap    = (PFN_cuDeviceComputeCapability)GetProcAddress(g_real, "cuDeviceComputeCapability");
    R.ctxCreate  = (PFN_cuCtxCreate)GetProcAddress(g_real, "cuCtxCreate");
    R.ctxCreate2 = (PFN_cuCtxCreate)GetProcAddress(g_real, "cuCtxCreate_v2");
    R.ctxDestroy = (PFN_cuCtxDestroy)GetProcAddress(g_real, "cuCtxDestroy");
    R.ctxDestroy2= (PFN_cuCtxDestroy)GetProcAddress(g_real, "cuCtxDestroy_v2");
    R.ctxPop     = (PFN_cuCtxPopCurrent)GetProcAddress(g_real, "cuCtxPopCurrent");
    R.ctxPop2    = (PFN_cuCtxPopCurrent)GetProcAddress(g_real, "cuCtxPopCurrent_v2");
    R.ctxPush    = (PFN_cuCtxPushCurrent)GetProcAddress(g_real, "cuCtxPushCurrent");
    R.ctxPush2   = (PFN_cuCtxPushCurrent)GetProcAddress(g_real, "cuCtxPushCurrent_v2");
    return TRUE;
}

static void ensure(void) { InitOnceExecuteOnce(&g_once, vnc_init, NULL, NULL); }

void *vnc_resolve(unsigned idx)
{
    ensure();
    return (idx < VNC_FORWARD_COUNT) ? g_vnc_fns[idx] : NULL;
}

// ----------------------------------------------------- intercepted exports --
// Device selection and context lifetime are logged on every call: they are
// infrequent, and they are exactly what the plugin reports failures about.

__declspec(dllexport) CUresult cuInit(unsigned int flags)
{
    ensure();
    if (!R.init) return 3;
    CUresult r = R.init(flags);
    vnc_log(1, "cuInit(0x%X) -> %s", flags, cu_err(r));
    return r;
}

__declspec(dllexport) CUresult cuDeviceGetCount(int *count)
{
    ensure();
    if (!R.getCount) return 3;
    CUresult r = R.getCount(count);
    vnc_log(1, "cuDeviceGetCount -> %s, count=%d", cu_err(r), count ? *count : -1);
    return r;
}

__declspec(dllexport) CUresult cuDeviceGet(CUdevice *dev, int ordinal)
{
    ensure();
    if (!R.get) return 3;
    CUresult r = R.get(dev, ordinal);
    // An out-of-range ordinal here is the "Invalid Device Id" path in the host.
    vnc_log(1, "cuDeviceGet(ordinal=%d) -> %s, device=%d",
            ordinal, cu_err(r), dev ? *dev : -1);
    return r;
}

__declspec(dllexport) CUresult cuDeviceGetName(char *name, int len, CUdevice dev)
{
    ensure();
    if (!R.getName) return 3;
    CUresult r = R.getName(name, len, dev);
    vnc_log(1, "cuDeviceGetName(device=%d) -> %s, \"%s\"", dev, cu_err(r),
            (r == 0 && name) ? name : "");
    return r;
}

__declspec(dllexport) CUresult cuDeviceComputeCapability(int *major, int *minor, CUdevice dev)
{
    ensure();
    if (!R.compCap) return 3;
    CUresult r = R.compCap(major, minor, dev);
    vnc_log(1, "cuDeviceComputeCapability(device=%d) -> %s, %d.%d", dev, cu_err(r),
            major ? *major : -1, minor ? *minor : -1);
    return r;
}

__declspec(dllexport) CUresult cuCtxCreate(CUcontext *ctx, unsigned int flags, CUdevice dev)
{
    ensure();
    if (!R.ctxCreate) return 3;
    CUresult r = R.ctxCreate(ctx, flags, dev);
    vnc_log(1, "cuCtxCreate(flags=0x%X, device=%d) -> %s, ctx=%p",
            flags, dev, cu_err(r), ctx ? *ctx : NULL);
    return r;
}

__declspec(dllexport) CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev)
{
    ensure();
    if (!R.ctxCreate2) return 3;
    CUresult r = R.ctxCreate2(ctx, flags, dev);
    vnc_log(1, "cuCtxCreate_v2(flags=0x%X, device=%d) -> %s, ctx=%p",
            flags, dev, cu_err(r), ctx ? *ctx : NULL);
    return r;
}

__declspec(dllexport) CUresult cuCtxDestroy(CUcontext ctx)
{
    ensure();
    if (!R.ctxDestroy) return 3;
    CUresult r = R.ctxDestroy(ctx);
    vnc_log(1, "cuCtxDestroy(ctx=%p) -> %s", ctx, cu_err(r));
    return r;
}

__declspec(dllexport) CUresult cuCtxDestroy_v2(CUcontext ctx)
{
    ensure();
    if (!R.ctxDestroy2) return 3;
    CUresult r = R.ctxDestroy2(ctx);
    vnc_log(1, "cuCtxDestroy_v2(ctx=%p) -> %s", ctx, cu_err(r));
    return r;
}

__declspec(dllexport) CUresult cuCtxPopCurrent(CUcontext *ctx)
{
    ensure();
    if (!R.ctxPop) return 3;
    CUresult r = R.ctxPop(ctx);
    VNC_TRACE(r, "cuCtxPopCurrent -> %s, ctx=%p", cu_err(r), ctx ? *ctx : NULL);
    return r;
}

__declspec(dllexport) CUresult cuCtxPopCurrent_v2(CUcontext *ctx)
{
    ensure();
    if (!R.ctxPop2) return 3;
    CUresult r = R.ctxPop2(ctx);
    VNC_TRACE(r, "cuCtxPopCurrent_v2 -> %s, ctx=%p", cu_err(r), ctx ? *ctx : NULL);
    return r;
}

__declspec(dllexport) CUresult cuCtxPushCurrent(CUcontext ctx)
{
    ensure();
    if (!R.ctxPush) return 3;
    CUresult r = R.ctxPush(ctx);
    VNC_TRACE(r, "cuCtxPushCurrent(ctx=%p) -> %s", ctx, cu_err(r));
    return r;
}

__declspec(dllexport) CUresult cuCtxPushCurrent_v2(CUcontext ctx)
{
    ensure();
    if (!R.ctxPush2) return 3;
    CUresult r = R.ctxPush2(ctx);
    VNC_TRACE(r, "cuCtxPushCurrent_v2(ctx=%p) -> %s", ctx, cu_err(r));
    return r;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
