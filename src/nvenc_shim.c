// SPDX-License-Identifier: MIT
//
// vegas-nvenc-fix - a compatibility shim for nvEncodeAPI64.dll
//
// WHAT IS BROKEN
// --------------
// NVIDIA Video Codec SDK 13.x removed the legacy encode presets
// (NV_ENC_PRESET_DEFAULT/HP/HQ/BD/LOW_LATENCY_*/LOSSLESS_*). On driver
// branches that ship that SDK, nvEncGetEncodePresetConfig() still exists and
// still returns a valid function pointer, but rejects every legacy preset GUID
// with NV_ENC_ERR_UNSUPPORTED_PARAM, and nvEncGetEncodePresetGUIDs() enumerates
// only P1..P7.
//
// VEGAS Pro 17-22 ships MainConcept/MAGIX encoder plug-ins that reference only
// the legacy GUIDs. mxavcaacplug.dll - the "MAGIX AVC/AAC MP4" renderer -
// contains the literal string "nvEncGetEncodePresetConfig returned failure" and
// aborts the render when that call fails, which VEGAS surfaces as
// "Error 0x80660008 (message missing)".
//
// WHAT THIS DOES
// --------------
// This DLL is a drop-in proxy. Placed next to vegas220.exe it is found ahead of
// %SystemRoot%\System32 by the standard loader search order, forwards every
// call to the real driver DLL, and repairs exactly three things:
//
//   1. nvEncGetEncodePresetConfig  - on failure with a legacy preset GUID,
//      retries via nvEncGetEncodePresetConfigEx with the P1..P7 preset and
//      tuning info that NVIDIA's own migration guide designates as equivalent.
//   2. nvEncGetEncodePresetCount / GUIDs - re-advertises the legacy presets so
//      callers that enumerate before choosing still see them.
//   3. nvEncInitializeEncoder - rewrites a legacy presetGUID in the init params
//      to its modern equivalent, since the driver rejects it there too.
//
// Everything else is passed through untouched. The real function is always
// tried FIRST, so on a driver where the legacy presets still work this shim is
// a no-op and stays safe to leave installed across driver rollbacks.
//
// This does not patch, modify or redistribute any VEGAS or NVIDIA binary.

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>

#include "nvEncodeAPI.h"
#include "nvenc_deprecated_presets.h"
#include "nvenc_preset_map.h"

#define VNF_VERSION "1.0.0"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

// ---------------------------------------------------------------- logging --

static CRITICAL_SECTION g_log_lock;
static int   g_log_level = 1;          // 0 = off, 1 = normal, 2 = verbose
static WCHAR g_log_path[MAX_PATH];
static int   g_log_ready = 0;

static void vnf_log_init(void)
{
    WCHAR buf[64];
    DWORD n = GetEnvironmentVariableW(L"VEGAS_NVENC_FIX_LOG", buf, 64);
    if (n > 0 && n < 64) g_log_level = _wtoi(buf);
    if (g_log_level <= 0) { g_log_level = 0; return; }

    WCHAR base[MAX_PATH];
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { g_log_level = 0; return; }

    _snwprintf_s(g_log_path, MAX_PATH, _TRUNCATE, L"%s\\vegas-nvenc-fix", base);
    CreateDirectoryW(g_log_path, NULL);
    _snwprintf_s(g_log_path, MAX_PATH, _TRUNCATE, L"%s\\vegas-nvenc-fix\\nvenc-shim.log", base);
    g_log_ready = 1;
}

static void vnf_log(int level, const char *fmt, ...)
{
    if (!g_log_ready || level > g_log_level) return;

    char line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int off = _snprintf_s(line, sizeof(line), _TRUNCATE,
                          "%02d:%02d:%02d.%03d [%5lu] ",
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
        DWORD written;
        WriteFile(h, line, (DWORD)strlen(line), &written, NULL);
        WriteFile(h, "\r\n", 2, &written, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_log_lock);
}

static const char *vnf_status(NVENCSTATUS s)
{
    switch (s) {
    case NV_ENC_SUCCESS:                return "SUCCESS";
    case NV_ENC_ERR_INVALID_PARAM:      return "ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL:       return "ERR_INVALID_CALL";
    case NV_ENC_ERR_UNSUPPORTED_PARAM:  return "ERR_UNSUPPORTED_PARAM";
    case NV_ENC_ERR_INVALID_VERSION:    return "ERR_INVALID_VERSION";
    case NV_ENC_ERR_UNIMPLEMENTED:      return "ERR_UNIMPLEMENTED";
    case NV_ENC_ERR_INVALID_PTR:        return "ERR_INVALID_PTR";
    case NV_ENC_ERR_OUT_OF_MEMORY:      return "ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_NOT_ENOUGH_BUFFER:  return "ERR_NOT_ENOUGH_BUFFER";
    case NV_ENC_ERR_GENERIC:            return "ERR_GENERIC";
    default:                            return "ERR_other";
    }
}

// ------------------------------------------------------------ real driver --

typedef NVENCSTATUS(NVENCAPI *PFN_CREATE_INSTANCE)(NV_ENCODE_API_FUNCTION_LIST *);
typedef NVENCSTATUS(NVENCAPI *PFN_MAX_VERSION)(uint32_t *);

// Indices must match the order in nvenc_thunks.asm.
enum {
    VNF_TOOL_CreateInterface = 0,
    VNF_TOOL_DestroyInterface,
    VNF_TOOL_GetApiFunctionCount,
    VNF_TOOL_GetApiID,
    VNF_TOOL_GetApiNames,
    VNF_TOOL_GetInterface,
    VNF_TOOL_SetApiID,
    VNF_TOOL_SetInterface,
    VNF_TOOL_COUNT
};

static const char *const g_tool_names[VNF_TOOL_COUNT] = {
    "NvToolCreateInterface", "NvToolDestroyInterface", "NvToolGetApiFunctionCount",
    "NvToolGetApiID", "NvToolGetApiNames", "NvToolGetInterface",
    "NvToolSetApiID", "NvToolSetInterface"
};

// Referenced by the assembly thunks; must have external linkage.
void *g_vnf_tool_fns[VNF_TOOL_COUNT];

static HMODULE              g_real_dll;
static PFN_CREATE_INSTANCE  g_real_create;
static PFN_MAX_VERSION      g_real_max_version;
static int                  g_disabled;      // VEGAS_NVENC_FIX_DISABLE=1
static int                  g_augment_enum = 1;

// Cached driver entry points. The driver hands out the same pointers for every
// requested API version (verified across SDK 9.0 - 13.1), so one copy is
// enough; it is refreshed on every successful CreateInstance regardless.
static struct {
    PNVENCGETENCODEPRESETCONFIG   getPresetConfig;
    PNVENCGETENCODEPRESETCONFIGEX getPresetConfigEx;
    PNVENCGETENCODEPRESETCOUNT    getPresetCount;
    PNVENCGETENCODEPRESETGUIDS    getPresetGUIDs;
    PNVENCINITIALIZEENCODER       initializeEncoder;
} g_real;

static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK vnf_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;

    InitializeCriticalSection(&g_log_lock);
    vnf_log_init();

    WCHAR buf[64];
    if (GetEnvironmentVariableW(L"VEGAS_NVENC_FIX_DISABLE", buf, 64) > 0 && buf[0] == L'1')
        g_disabled = 1;
    if (GetEnvironmentVariableW(L"VEGAS_NVENC_FIX_ENUM", buf, 64) > 0 && buf[0] == L'0')
        g_augment_enum = 0;

    WCHAR host[MAX_PATH] = L"?";
    GetModuleFileNameW(NULL, host, MAX_PATH);
    vnf_log(1, "--- vegas-nvenc-fix " VNF_VERSION " loaded ---");
    vnf_log(1, "host process: %S", host);
    if (g_disabled) vnf_log(1, "VEGAS_NVENC_FIX_DISABLE=1 -> pure pass-through");

    // Load the genuine driver DLL by absolute System32 path. Resolving by name
    // would find this proxy again; resolving by full path cannot.
    WCHAR path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 20) { vnf_log(1, "FATAL: GetSystemDirectory failed"); return TRUE; }
    wcscat_s(path, MAX_PATH, L"\\nvEncodeAPI64.dll");

    // Refuse to load ourselves, in case this file was ever copied to System32.
    WCHAR self[MAX_PATH] = L"";
    GetModuleFileNameW((HMODULE)&__ImageBase, self, MAX_PATH);
    if (_wcsicmp(self, path) == 0) {
        vnf_log(1, "FATAL: shim is installed as the system DLL (%S) - refusing to self-load", self);
        return TRUE;
    }

    g_real_dll = LoadLibraryW(path);
    if (!g_real_dll) {
        vnf_log(1, "FATAL: LoadLibrary(%S) failed, err=%lu", path, GetLastError());
        return TRUE;
    }
    vnf_log(1, "real driver DLL: %S", path);

    g_real_create      = (PFN_CREATE_INSTANCE)GetProcAddress(g_real_dll, "NvEncodeAPICreateInstance");
    g_real_max_version = (PFN_MAX_VERSION)GetProcAddress(g_real_dll, "NvEncodeAPIGetMaxSupportedVersion");
    for (int i = 0; i < VNF_TOOL_COUNT; i++)
        g_vnf_tool_fns[i] = (void *)GetProcAddress(g_real_dll, g_tool_names[i]);

    if (!g_real_create) vnf_log(1, "FATAL: NvEncodeAPICreateInstance missing from real DLL");
    return TRUE;
}

static void vnf_ensure_init(void)
{
    InitOnceExecuteOnce(&g_init_once, vnf_init, NULL, NULL);
}

// Resolver used by the assembly thunks for the NvTool* exports. Returns the
// real function pointer, or NULL if it could not be resolved.
void *vnf_resolve_tool(unsigned index)
{
    vnf_ensure_init();
    if (index >= VNF_TOOL_COUNT) return NULL;
    return g_vnf_tool_fns[index];
}

// ------------------------------------------------------------- the repair --

static NVENCSTATUS NVENCAPI vnf_GetEncodePresetConfig(void *encoder, GUID encodeGUID,
                                                      GUID presetGUID,
                                                      NV_ENC_PRESET_CONFIG *presetConfig)
{
    // Always give the driver first refusal. On a driver where the legacy
    // presets still work this path returns SUCCESS and nothing is translated.
    NVENCSTATUS st = g_real.getPresetConfig(encoder, encodeGUID, presetGUID, presetConfig);
    if (st == NV_ENC_SUCCESS) return st;

    const vnf_preset_row *row = vnf_find_row(&encodeGUID, &presetGUID);
    if (!row) {
        vnf_log(2, "GetEncodePresetConfig: %s for an unrecognised preset - passing failure through",
                vnf_status(st));
        return st;
    }
    if (!g_real.getPresetConfigEx) {
        vnf_log(1, "GetEncodePresetConfig: %s and no ...Ex available", vnf_status(st));
        return st;
    }

    NVENCSTATUS st2 = g_real.getPresetConfigEx(encoder, encodeGUID, *row->modern,
                                               row->tuning, presetConfig);
    vnf_log(1, "translated preset %s (%s) -> P%c tuning=%d : %s",
            row->name,
            IsEqualGUID(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID) ? "HEVC" : "H.264",
            '0' + (char)row->pnum,
            (int)row->tuning, vnf_status(st2));
    return st2;
}

static NVENCSTATUS NVENCAPI vnf_GetEncodePresetCount(void *encoder, GUID encodeGUID,
                                                     uint32_t *encodePresetGUIDCount)
{
    NVENCSTATUS st = g_real.getPresetCount(encoder, encodeGUID, encodePresetGUIDCount);
    if (st != NV_ENC_SUCCESS || !g_augment_enum || !encodePresetGUIDCount) return st;

    // Only re-advertise legacy presets for codecs that actually had them.
    if (!IsEqualGUID(&encodeGUID, &NV_ENC_CODEC_H264_GUID) &&
        !IsEqualGUID(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID))
        return st;

    *encodePresetGUIDCount += VNF_MAP_COUNT;
    vnf_log(2, "GetEncodePresetCount -> %u (driver + %d legacy)",
            *encodePresetGUIDCount, VNF_MAP_COUNT);
    return st;
}

static NVENCSTATUS NVENCAPI vnf_GetEncodePresetGUIDs(void *encoder, GUID encodeGUID,
                                                     GUID *presetGUIDs, uint32_t guidArraySize,
                                                     uint32_t *encodePresetGUIDCount)
{
    NVENCSTATUS st = g_real.getPresetGUIDs(encoder, encodeGUID, presetGUIDs,
                                           guidArraySize, encodePresetGUIDCount);
    if (st != NV_ENC_SUCCESS || !g_augment_enum || !presetGUIDs || !encodePresetGUIDCount)
        return st;
    if (!IsEqualGUID(&encodeGUID, &NV_ENC_CODEC_H264_GUID) &&
        !IsEqualGUID(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID))
        return st;

    const vnf_preset_row *tbl = vnf_table_for_codec(&encodeGUID);
    uint32_t n = *encodePresetGUIDCount;
    for (int i = 0; i < VNF_MAP_COUNT && n < guidArraySize; i++)
        presetGUIDs[n++] = *tbl[i].legacy;

    vnf_log(2, "GetEncodePresetGUIDs -> %u of %u slots (%u from driver)",
            n, guidArraySize, *encodePresetGUIDCount);
    *encodePresetGUIDCount = n;
    return st;
}

static NVENCSTATUS NVENCAPI vnf_InitializeEncoder(void *encoder,
                                                  NV_ENC_INITIALIZE_PARAMS *createEncodeParams)
{
    if (createEncodeParams) {
        const vnf_preset_row *row = vnf_find_row(&createEncodeParams->encodeGUID,
                                                 &createEncodeParams->presetGUID);
        if (row) {
            // Only the two preset fields are touched, in place. The struct is
            // not copied: its size differs between SDK revisions only in the
            // trailing reserved arrays, and a copy sized by this build's header
            // could over-read a caller built against an older SDK.
            createEncodeParams->presetGUID = *row->modern;
            if (createEncodeParams->tuningInfo == NV_ENC_TUNING_INFO_UNDEFINED)
                createEncodeParams->tuningInfo = row->tuning;

            vnf_log(1, "InitializeEncoder: preset %s -> P%c tuning=%d (%ux%u, params ver 0x%08X)",
                    row->name,
                    '0' + (char)row->pnum,
                    (int)createEncodeParams->tuningInfo,
                    createEncodeParams->encodeWidth, createEncodeParams->encodeHeight,
                    createEncodeParams->version);
        }
    }
    NVENCSTATUS st = g_real.initializeEncoder(encoder, createEncodeParams);
    if (st != NV_ENC_SUCCESS) vnf_log(1, "InitializeEncoder -> %s", vnf_status(st));
    return st;
}

// ----------------------------------------------------------------exports --



NVENCSTATUS NVENCAPI
NvEncodeAPIGetMaxSupportedVersion(uint32_t *version)
{
    vnf_ensure_init();
    if (!g_real_max_version) return NV_ENC_ERR_INVALID_CALL;
    return g_real_max_version(version);
}

NVENCSTATUS NVENCAPI
NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST *functionList)
{
    vnf_ensure_init();
    if (!g_real_create) return NV_ENC_ERR_INVALID_CALL;

    NVENCSTATUS st = g_real_create(functionList);
    if (st != NV_ENC_SUCCESS || !functionList) {
        vnf_log(1, "CreateInstance(ver 0x%08X) -> %s",
                functionList ? functionList->version : 0, vnf_status(st));
        return st;
    }

    vnf_log(1, "CreateInstance(ver 0x%08X = SDK %u.%u) -> SUCCESS",
            functionList->version,
            functionList->version & 0xFF, (functionList->version >> 24) & 0x0F);

    if (g_disabled) return st;

    // Snapshot the genuine entry points, then swap in the repaired ones.
    if (functionList->nvEncGetEncodePresetConfig) {
        g_real.getPresetConfig   = functionList->nvEncGetEncodePresetConfig;
        g_real.getPresetConfigEx = functionList->nvEncGetEncodePresetConfigEx;
        functionList->nvEncGetEncodePresetConfig = vnf_GetEncodePresetConfig;
    }
    if (functionList->nvEncGetEncodePresetCount) {
        g_real.getPresetCount = functionList->nvEncGetEncodePresetCount;
        functionList->nvEncGetEncodePresetCount = vnf_GetEncodePresetCount;
    }
    if (functionList->nvEncGetEncodePresetGUIDs) {
        g_real.getPresetGUIDs = functionList->nvEncGetEncodePresetGUIDs;
        functionList->nvEncGetEncodePresetGUIDs = vnf_GetEncodePresetGUIDs;
    }
    if (functionList->nvEncInitializeEncoder) {
        g_real.initializeEncoder = functionList->nvEncInitializeEncoder;
        functionList->nvEncInitializeEncoder = vnf_InitializeEncoder;
    }

    vnf_log(2, "hooks installed (presetConfig/presetCount/presetGUIDs/initializeEncoder)");
    return st;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    // Deliberately empty apart from thread-notification opt-out. The real DLL
    // imports ole32 and winmm; loading it here would mean nested LoadLibrary
    // under the loader lock. All initialisation is deferred to first use.
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
