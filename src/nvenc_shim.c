// SPDX-License-Identifier: MIT
//
// vegas-nvenc-fix - a compatibility shim for nvEncodeAPI64.dll
//
// WHAT IS BROKEN
// --------------
// VEGAS Pro's MAGIX AVC/AAC MP4 renderer (mxavcaacplug.dll) drives NVENC as an
// SDK 7.1 client: it passes function-list version 0x71020007 and 7.1-era
// version stamps on every API struct. Recent NVIDIA driver branches reject
// those stamps. Measured on driver 616.92:
//
//   nvEncOpenEncodeSessionEx, struct ver 7.1  -> NV_ENC_ERR_INVALID_VERSION
//   nvEncOpenEncodeSessionEx, struct ver 13.1 -> NV_ENC_SUCCESS
//
// and crossing the two version fields shows that only NV_ENC_*::version is
// checked - NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS::apiVersion is not:
//
//   struct 13.1 + api 7.1 -> SUCCESS     struct 7.1 + api 13.1 -> INVALID_VERSION
//
// So VEGAS cannot open an encode session at all. It never reaches a preset
// call, which is why the preset list in Custom Settings is empty and why the
// render aborts as "Error 0x80660008 (message missing)".
//
// Behind that sits a second, independent break: SDK 13.x also removed the
// legacy encode presets (NV_ENC_PRESET_DEFAULT/HP/HQ/BD/LOW_LATENCY_*/
// LOSSLESS_*), and mxavcaacplug.dll references only those GUIDs. Repairing the
// version stamps alone would just move the failure one step later.
//
// WHAT THIS DOES
// --------------
// This DLL is a drop-in proxy. Placed next to vegas220.exe it is found ahead of
// %SystemRoot%\System32 by the standard loader search order, forwards every
// call to the real driver DLL, and repairs two things:
//
//   1. VERSION UPLIFT. Every NVENC struct begins with a uint32_t version. On
//      the way in the shim overwrites it with the stamp this build's header
//      defines, and restores the caller's original on the way out so the
//      caller's memory is left exactly as it was handed over. This is safe
//      because NVENC structs are fixed size across SDK revisions - the
//      function list is 317 pointer slots in SDK 8.0, 11.1 and 13.1 alike -
//      so new fields are carved out of trailing reserved space that an older
//      client has already zeroed, and zero is the documented default.
//
//   2. PRESET TRANSLATION. A removed legacy preset GUID is mapped to the
//      P1..P7 preset and tuning info that NVIDIA's own migration guide calls
//      equivalent, and fetched via nvEncGetEncodePresetConfigEx. The legacy
//      GUIDs are also re-advertised by the preset enumerators and rewritten in
//      nvEncInitializeEncoder.
//
// The genuine driver function is tried first wherever a fallback exists, so on
// a driver that still accepts the old stamps the shim changes nothing and is
// safe to leave installed across driver updates and rollbacks.
//
// This does not patch, modify or redistribute any VEGAS or NVIDIA binary.

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>

#include "nvEncodeAPI.h"
#include "nvenc_deprecated_presets.h"
#include "nvenc_preset_map.h"

#define VNF_VERSION "1.1.0"

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
    case NV_ENC_SUCCESS:                 return "SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE:    return "ERR_NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE:  return "ERR_UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_DEVICE:      return "ERR_INVALID_DEVICE";
    case NV_ENC_ERR_INVALID_PARAM:       return "ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL:        return "ERR_INVALID_CALL";
    case NV_ENC_ERR_UNSUPPORTED_PARAM:   return "ERR_UNSUPPORTED_PARAM";
    case NV_ENC_ERR_INVALID_VERSION:     return "ERR_INVALID_VERSION";
    case NV_ENC_ERR_UNIMPLEMENTED:       return "ERR_UNIMPLEMENTED";
    case NV_ENC_ERR_INVALID_PTR:         return "ERR_INVALID_PTR";
    case NV_ENC_ERR_OUT_OF_MEMORY:       return "ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_NOT_ENOUGH_BUFFER:   return "ERR_NOT_ENOUGH_BUFFER";
    case NV_ENC_ERR_NEED_MORE_INPUT:     return "ERR_NEED_MORE_INPUT";
    case NV_ENC_ERR_ENCODER_BUSY:        return "ERR_ENCODER_BUSY";
    case NV_ENC_ERR_GENERIC:             return "ERR_GENERIC";
    default:                             return "ERR_other";
    }
}

// Logs the first time a given call site is reached, and thereafter only on
// failure, so that per-frame entry points cannot flood the log.
#define VNF_TRACE(status, fmt, ...)                                            \
    do {                                                                       \
        static LONG _seen = 0;                                                 \
        if ((status) != NV_ENC_SUCCESS || InterlockedExchange(&_seen, 1) == 0) \
            vnf_log(1, fmt, __VA_ARGS__);                                      \
    } while (0)

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
static int                  g_disabled;
static int                  g_augment_enum = 1;
static int                  g_uplift       = 1;

// The driver hands out identical function pointers for every requested API
// version (verified for SDK 7.0 through 13.1), so one cached current-version
// table serves every client in the process.
static NV_ENCODE_API_FUNCTION_LIST g_real;
static int g_have_real;

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
    if (GetEnvironmentVariableW(L"VEGAS_NVENC_FIX_UPLIFT", buf, 64) > 0 && buf[0] == L'0')
        g_uplift = 0;

    WCHAR host[MAX_PATH] = L"?";
    GetModuleFileNameW(NULL, host, MAX_PATH);
    vnf_log(1, "--- vegas-nvenc-fix " VNF_VERSION " loaded ---");
    vnf_log(1, "host process  : %S", host);
    vnf_log(1, "built against : NVENC API %u.%u", NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
    if (g_disabled) vnf_log(1, "VEGAS_NVENC_FIX_DISABLE=1 -> pure pass-through");
    if (!g_uplift)  vnf_log(1, "VEGAS_NVENC_FIX_UPLIFT=0 -> version uplift disabled");

    // Load the genuine driver DLL by absolute System32 path. Resolving by name
    // would find this proxy again; resolving by full path cannot.
    WCHAR path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 20) { vnf_log(1, "FATAL: GetSystemDirectory failed"); return TRUE; }
    wcscat_s(path, MAX_PATH, L"\\nvEncodeAPI64.dll");

    WCHAR self[MAX_PATH] = L"";
    GetModuleFileNameW((HMODULE)&__ImageBase, self, MAX_PATH);
    if (_wcsicmp(self, path) == 0) {
        vnf_log(1, "FATAL: shim is installed as the system DLL - refusing to self-load");
        return TRUE;
    }

    g_real_dll = LoadLibraryW(path);
    if (!g_real_dll) {
        vnf_log(1, "FATAL: LoadLibrary(%S) failed, err=%lu", path, GetLastError());
        return TRUE;
    }
    vnf_log(1, "real driver   : %S", path);

    g_real_create      = (PFN_CREATE_INSTANCE)GetProcAddress(g_real_dll, "NvEncodeAPICreateInstance");
    g_real_max_version = (PFN_MAX_VERSION)GetProcAddress(g_real_dll, "NvEncodeAPIGetMaxSupportedVersion");
    for (int i = 0; i < VNF_TOOL_COUNT; i++)
        g_vnf_tool_fns[i] = (void *)GetProcAddress(g_real_dll, g_tool_names[i]);

    if (!g_real_create) { vnf_log(1, "FATAL: NvEncodeAPICreateInstance missing"); return TRUE; }

    // Take a private, current-version function list. This is the table the shim
    // calls through, so the driver always sees a client version it accepts,
    // whatever the host asked for.
    memset(&g_real, 0, sizeof(g_real));
    g_real.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = g_real_create(&g_real);
    if (st != NV_ENC_SUCCESS) {
        vnf_log(1, "FATAL: private CreateInstance(current) -> %s", vnf_status(st));
        return TRUE;
    }
    g_have_real = 1;
    vnf_log(1, "private table : NVENC %u.%u acquired",
            NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
    return TRUE;
}

static void vnf_ensure_init(void)
{
    InitOnceExecuteOnce(&g_init_once, vnf_init, NULL, NULL);
}

void *vnf_resolve_tool(unsigned index)
{
    vnf_ensure_init();
    if (index >= VNF_TOOL_COUNT) return NULL;
    return g_vnf_tool_fns[index];
}

// ------------------------------------------------------- version uplifting --

typedef struct { uint32_t *slot; uint32_t saved; } vnf_ver_save;

static __inline void vnf_ver_set(vnf_ver_save *s, void *structPtr, uint32_t current)
{
    if (!structPtr || !g_uplift) { s->slot = NULL; return; }
    s->slot  = (uint32_t *)structPtr;     // version is always the first member
    s->saved = *s->slot;
    *s->slot = current;
}

static __inline void vnf_ver_restore(const vnf_ver_save *s)
{
    if (s->slot) *s->slot = s->saved;
}

// --------------------------------------------------------------- the hooks --

static NVENCSTATUS NVENCAPI vnf_OpenEncodeSessionEx(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS *p,
                                                    void **encoder)
{
    vnf_ver_save v;
    vnf_ver_set(&v, p, NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER);
    uint32_t was = v.slot ? v.saved : 0;
    NVENCSTATUS st = g_real.nvEncOpenEncodeSessionEx(p, encoder);
    vnf_ver_restore(&v);

    VNF_TRACE(st, "OpenEncodeSessionEx: struct ver 0x%08X -> 0x%08X : %s",
              was, (unsigned)NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER, vnf_status(st));
    return st;
}

static NVENCSTATUS NVENCAPI vnf_GetEncodeCaps(void *e, GUID codec, NV_ENC_CAPS_PARAM *caps, int *val)
{
    vnf_ver_save v;
    vnf_ver_set(&v, caps, NV_ENC_CAPS_PARAM_VER);
    NVENCSTATUS st = g_real.nvEncGetEncodeCaps(e, codec, caps, val);
    vnf_ver_restore(&v);
    VNF_TRACE(st, "GetEncodeCaps -> %s", vnf_status(st));
    return st;
}

static NVENCSTATUS NVENCAPI vnf_GetEncodePresetConfig(void *encoder, GUID encodeGUID,
                                                      GUID presetGUID,
                                                      NV_ENC_PRESET_CONFIG *presetConfig)
{
    vnf_ver_save vo, vi;
    vnf_ver_set(&vo, presetConfig, NV_ENC_PRESET_CONFIG_VER);
    vnf_ver_set(&vi, presetConfig ? &presetConfig->presetCfg : NULL, NV_ENC_CONFIG_VER);

    // Give the driver first refusal: where the legacy presets still work this
    // succeeds and nothing is translated.
    NVENCSTATUS st = g_real.nvEncGetEncodePresetConfig(encoder, encodeGUID, presetGUID, presetConfig);

    if (st != NV_ENC_SUCCESS) {
        const vnf_preset_row *row = vnf_find_row(&encodeGUID, &presetGUID);
        if (row && g_real.nvEncGetEncodePresetConfigEx) {
            NVENCSTATUS st2 = g_real.nvEncGetEncodePresetConfigEx(encoder, encodeGUID,
                                                                  *row->modern, row->tuning,
                                                                  presetConfig);
            vnf_log(1, "preset %s (%s) -> P%d tuning=%d : %s",
                    row->name,
                    IsEqualGUID(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID) ? "HEVC" : "H.264",
                    row->pnum, (int)row->tuning, vnf_status(st2));
            st = st2;
        } else {
            vnf_log(1, "GetEncodePresetConfig -> %s (no mapping for that preset)", vnf_status(st));
        }
    }

    vnf_ver_restore(&vi);
    vnf_ver_restore(&vo);
    return st;
}

static NVENCSTATUS NVENCAPI vnf_GetEncodePresetConfigEx(void *encoder, GUID encodeGUID,
                                                        GUID presetGUID,
                                                        NV_ENC_TUNING_INFO tuningInfo,
                                                        NV_ENC_PRESET_CONFIG *presetConfig)
{
    const vnf_preset_row *row = vnf_find_row(&encodeGUID, &presetGUID);
    if (row) {
        if (tuningInfo == NV_ENC_TUNING_INFO_UNDEFINED) tuningInfo = row->tuning;
        presetGUID = *row->modern;
    }
    vnf_ver_save vo, vi;
    vnf_ver_set(&vo, presetConfig, NV_ENC_PRESET_CONFIG_VER);
    vnf_ver_set(&vi, presetConfig ? &presetConfig->presetCfg : NULL, NV_ENC_CONFIG_VER);
    NVENCSTATUS st = g_real.nvEncGetEncodePresetConfigEx(encoder, encodeGUID, presetGUID,
                                                         tuningInfo, presetConfig);
    vnf_ver_restore(&vi);
    vnf_ver_restore(&vo);
    VNF_TRACE(st, "GetEncodePresetConfigEx -> %s", vnf_status(st));
    return st;
}

static NVENCSTATUS NVENCAPI vnf_GetEncodePresetCount(void *encoder, GUID encodeGUID,
                                                     uint32_t *encodePresetGUIDCount)
{
    NVENCSTATUS st = g_real.nvEncGetEncodePresetCount(encoder, encodeGUID, encodePresetGUIDCount);
    if (st != NV_ENC_SUCCESS || !g_augment_enum || !encodePresetGUIDCount) return st;
    if (!IsEqualGUID(&encodeGUID, &NV_ENC_CODEC_H264_GUID) &&
        !IsEqualGUID(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID)) return st;

    *encodePresetGUIDCount += VNF_MAP_COUNT;
    VNF_TRACE(st, "GetEncodePresetCount -> %u (driver + %d legacy)",
              *encodePresetGUIDCount, VNF_MAP_COUNT);
    return st;
}

static NVENCSTATUS NVENCAPI vnf_GetEncodePresetGUIDs(void *encoder, GUID encodeGUID,
                                                     GUID *presetGUIDs, uint32_t guidArraySize,
                                                     uint32_t *encodePresetGUIDCount)
{
    NVENCSTATUS st = g_real.nvEncGetEncodePresetGUIDs(encoder, encodeGUID, presetGUIDs,
                                                      guidArraySize, encodePresetGUIDCount);
    if (st != NV_ENC_SUCCESS || !g_augment_enum || !presetGUIDs || !encodePresetGUIDCount)
        return st;
    if (!IsEqualGUID(&encodeGUID, &NV_ENC_CODEC_H264_GUID) &&
        !IsEqualGUID(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID)) return st;

    const vnf_preset_row *tbl = vnf_table_for_codec(&encodeGUID);
    uint32_t n = *encodePresetGUIDCount;
    for (int i = 0; i < VNF_MAP_COUNT && n < guidArraySize; i++)
        presetGUIDs[n++] = *tbl[i].legacy;

    VNF_TRACE(st, "GetEncodePresetGUIDs -> %u of %u slots", n, guidArraySize);
    *encodePresetGUIDCount = n;
    return st;
}

static NVENCSTATUS NVENCAPI vnf_InitializeEncoder(void *encoder, NV_ENC_INITIALIZE_PARAMS *p)
{
    const vnf_preset_row *row = NULL;
    if (p) {
        row = vnf_find_row(&p->encodeGUID, &p->presetGUID);
        if (row) {
            p->presetGUID = *row->modern;
            if (p->tuningInfo == NV_ENC_TUNING_INFO_UNDEFINED) p->tuningInfo = row->tuning;
        } else if (g_uplift && p->tuningInfo == NV_ENC_TUNING_INFO_UNDEFINED) {
            // A pre-SDK-10 client never set tuningInfo, and zero is not a valid
            // value for encoding. High quality is what the old presets targeted.
            p->tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
        }
    }

    vnf_ver_save vo, vc;
    vnf_ver_set(&vo, p, NV_ENC_INITIALIZE_PARAMS_VER);
    vnf_ver_set(&vc, (p && p->encodeConfig) ? p->encodeConfig : NULL, NV_ENC_CONFIG_VER);
    NVENCSTATUS st = g_real.nvEncInitializeEncoder(encoder, p);
    vnf_ver_restore(&vc);
    vnf_ver_restore(&vo);

    vnf_log(1, "InitializeEncoder: %ux%u preset=%s tuning=%d : %s",
            p ? p->encodeWidth : 0, p ? p->encodeHeight : 0,
            row ? row->name : "(modern)", p ? (int)p->tuningInfo : -1, vnf_status(st));
    return st;
}

static NVENCSTATUS NVENCAPI vnf_ReconfigureEncoder(void *e, NV_ENC_RECONFIGURE_PARAMS *p)
{
    vnf_ver_save vo, vi, vc;
    vnf_ver_set(&vo, p, NV_ENC_RECONFIGURE_PARAMS_VER);
    vnf_ver_set(&vi, p ? &p->reInitEncodeParams : NULL, NV_ENC_INITIALIZE_PARAMS_VER);
    vnf_ver_set(&vc, (p && p->reInitEncodeParams.encodeConfig)
                         ? p->reInitEncodeParams.encodeConfig : NULL, NV_ENC_CONFIG_VER);
    NVENCSTATUS st = g_real.nvEncReconfigureEncoder(e, p);
    vnf_ver_restore(&vc); vnf_ver_restore(&vi); vnf_ver_restore(&vo);
    VNF_TRACE(st, "ReconfigureEncoder -> %s", vnf_status(st));
    return st;
}

// Entry points whose only need is a current version stamp.
#define VNF_SIMPLE_HOOK(hook, fn, type, ver)                                   \
    static NVENCSTATUS NVENCAPI hook(void *e, type *p)                         \
    {                                                                          \
        vnf_ver_save v;                                                        \
        vnf_ver_set(&v, p, ver);                                               \
        NVENCSTATUS st = g_real.fn(e, p);                                      \
        vnf_ver_restore(&v);                                                   \
        VNF_TRACE(st, #fn " -> %s", vnf_status(st));                           \
        return st;                                                             \
    }

VNF_SIMPLE_HOOK(vnf_CreateInputBuffer,       nvEncCreateInputBuffer,       NV_ENC_CREATE_INPUT_BUFFER,     NV_ENC_CREATE_INPUT_BUFFER_VER)
VNF_SIMPLE_HOOK(vnf_CreateBitstreamBuffer,   nvEncCreateBitstreamBuffer,   NV_ENC_CREATE_BITSTREAM_BUFFER, NV_ENC_CREATE_BITSTREAM_BUFFER_VER)
VNF_SIMPLE_HOOK(vnf_CreateMVBuffer,          nvEncCreateMVBuffer,          NV_ENC_CREATE_MV_BUFFER,        NV_ENC_CREATE_MV_BUFFER_VER)
VNF_SIMPLE_HOOK(vnf_EncodePicture,           nvEncEncodePicture,           NV_ENC_PIC_PARAMS,              NV_ENC_PIC_PARAMS_VER)
VNF_SIMPLE_HOOK(vnf_LockBitstream,           nvEncLockBitstream,           NV_ENC_LOCK_BITSTREAM,          NV_ENC_LOCK_BITSTREAM_VER)
VNF_SIMPLE_HOOK(vnf_LockInputBuffer,         nvEncLockInputBuffer,         NV_ENC_LOCK_INPUT_BUFFER,       NV_ENC_LOCK_INPUT_BUFFER_VER)
VNF_SIMPLE_HOOK(vnf_MapInputResource,        nvEncMapInputResource,        NV_ENC_MAP_INPUT_RESOURCE,      NV_ENC_MAP_INPUT_RESOURCE_VER)
VNF_SIMPLE_HOOK(vnf_RegisterResource,        nvEncRegisterResource,        NV_ENC_REGISTER_RESOURCE,       NV_ENC_REGISTER_RESOURCE_VER)
VNF_SIMPLE_HOOK(vnf_RegisterAsyncEvent,      nvEncRegisterAsyncEvent,      NV_ENC_EVENT_PARAMS,            NV_ENC_EVENT_PARAMS_VER)
VNF_SIMPLE_HOOK(vnf_UnregisterAsyncEvent,    nvEncUnregisterAsyncEvent,    NV_ENC_EVENT_PARAMS,            NV_ENC_EVENT_PARAMS_VER)
VNF_SIMPLE_HOOK(vnf_GetSequenceParams,       nvEncGetSequenceParams,       NV_ENC_SEQUENCE_PARAM_PAYLOAD,  NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER)
VNF_SIMPLE_HOOK(vnf_RunMotionEstimationOnly, nvEncRunMotionEstimationOnly, NV_ENC_MEONLY_PARAMS,           NV_ENC_MEONLY_PARAMS_VER)

// ----------------------------------------------------------------- exports --

NVENCSTATUS NVENCAPI NvEncodeAPIGetMaxSupportedVersion(uint32_t *version)
{
    vnf_ensure_init();
    if (!g_real_max_version) return NV_ENC_ERR_INVALID_CALL;
    NVENCSTATUS st = g_real_max_version(version);
    VNF_TRACE(st, "GetMaxSupportedVersion -> %u.%u",
              version ? (*version >> 4) : 0, version ? (*version & 0xF) : 0);
    return st;
}

NVENCSTATUS NVENCAPI NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST *functionList)
{
    vnf_ensure_init();
    if (!g_real_create) return NV_ENC_ERR_INVALID_CALL;
    if (!functionList)  return NV_ENC_ERR_INVALID_PTR;

    const uint32_t asked = functionList->version;

    if (g_disabled || !g_have_real) {
        NVENCSTATUS st = g_real_create(functionList);
        vnf_log(1, "CreateInstance(0x%08X) pass-through -> %s", asked, vnf_status(st));
        return st;
    }

    // Hand back the current-version table rather than the one the caller asked
    // for. The layout is identical - NV_ENCODE_API_FUNCTION_LIST is the same
    // size in every SDK revision - and every slot is populated, so an old
    // caller gains the entry points it needs rather than a partially filled
    // table it would then drive with stamps the driver rejects.
    memcpy(functionList, &g_real, sizeof(*functionList));
    functionList->version = asked;      // leave the caller's own stamp alone

    vnf_log(1, "CreateInstance(0x%08X = SDK %u.%u) -> table uplifted to %u.%u",
            asked, asked & 0xFF, (asked >> 24) & 0x0F,
            NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);

    functionList->nvEncOpenEncodeSessionEx     = vnf_OpenEncodeSessionEx;
    functionList->nvEncGetEncodeCaps           = vnf_GetEncodeCaps;
    functionList->nvEncGetEncodePresetConfig   = vnf_GetEncodePresetConfig;
    functionList->nvEncGetEncodePresetConfigEx = vnf_GetEncodePresetConfigEx;
    functionList->nvEncGetEncodePresetCount    = vnf_GetEncodePresetCount;
    functionList->nvEncGetEncodePresetGUIDs    = vnf_GetEncodePresetGUIDs;
    functionList->nvEncInitializeEncoder       = vnf_InitializeEncoder;
    functionList->nvEncReconfigureEncoder      = vnf_ReconfigureEncoder;
    functionList->nvEncCreateInputBuffer       = vnf_CreateInputBuffer;
    functionList->nvEncCreateBitstreamBuffer   = vnf_CreateBitstreamBuffer;
    functionList->nvEncCreateMVBuffer          = vnf_CreateMVBuffer;
    functionList->nvEncEncodePicture           = vnf_EncodePicture;
    functionList->nvEncLockBitstream           = vnf_LockBitstream;
    functionList->nvEncLockInputBuffer         = vnf_LockInputBuffer;
    functionList->nvEncMapInputResource        = vnf_MapInputResource;
    functionList->nvEncRegisterResource        = vnf_RegisterResource;
    functionList->nvEncRegisterAsyncEvent      = vnf_RegisterAsyncEvent;
    functionList->nvEncUnregisterAsyncEvent    = vnf_UnregisterAsyncEvent;
    functionList->nvEncGetSequenceParams       = vnf_GetSequenceParams;
    functionList->nvEncRunMotionEstimationOnly = vnf_RunMotionEstimationOnly;

    return NV_ENC_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    // Deliberately does no work: the real DLL imports ole32 and winmm, so
    // loading it here would mean nested LoadLibrary under the loader lock.
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
