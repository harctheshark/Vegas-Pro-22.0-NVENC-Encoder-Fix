// nvenc_probe71 - what does the driver actually do for an SDK 7.1 client?
//
// VEGAS's mxavcaacplug.dll requests NVENC API version 7.1 (function list
// version 0x71020007). nvEncGetEncodePresetConfigEx did not exist until SDK
// 10, so the questions this answers are:
//
//   1. Which function-list slots does the driver populate for a 7.1 client?
//      In particular, is nvEncGetEncodePresetConfigEx there at all?
//   2. Does the driver still honour the legacy presets for an old client?
//   3. If the 7.1 table has no ...ConfigEx, can the pointer taken from a
//      current-version table be called with the 7.1 client's own structs?
//
// Answer 3 is what decides whether the shim can repair a 7.1 caller.

#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdint.h>

#include "nvEncodeAPI.h"
#include "../src/nvenc_deprecated_presets.h"

typedef NVENCSTATUS(NVENCAPI *PFN_CREATE_INSTANCE)(NV_ENCODE_API_FUNCTION_LIST *);

// SDK 7.1 constants, derived the same way the headers do:
//   NVENCAPI_VERSION      = major | (minor << 24)      = 7 | (1<<24)
//   NVENCAPI_STRUCT_VERSION(n) = VERSION | (n<<16) | (0x7<<28)
// Struct indices are taken from the SDK 8.0 header (the oldest published), and
// FUNCTION_LIST index 2 is confirmed correct because 0x71020007 is exactly the
// value VEGAS sends.
#define V71_API      ((uint32_t)(7u | (1u << 24)))
#define V71_STRUCT(n) ((uint32_t)(V71_API | ((n) << 16) | (0x7u << 28)))
#define V71_FUNCTION_LIST_VER   V71_STRUCT(2)
#define V71_OPEN_SESSION_VER    V71_STRUCT(1)
#define V71_PRESET_CONFIG_VER   (V71_STRUCT(4) | (1u << 31))
#define V71_CONFIG_VER          (V71_STRUCT(6) | (1u << 31))

static const char *st_name(NVENCSTATUS s)
{
    switch (s) {
    case NV_ENC_SUCCESS:               return "SUCCESS";
    case NV_ENC_ERR_INVALID_PARAM:     return "ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_VERSION:   return "ERR_INVALID_VERSION";
    case NV_ENC_ERR_UNSUPPORTED_PARAM: return "ERR_UNSUPPORTED_PARAM";
    case NV_ENC_ERR_INVALID_CALL:      return "ERR_INVALID_CALL";
    case NV_ENC_ERR_UNIMPLEMENTED:     return "ERR_UNIMPLEMENTED";
    case NV_ENC_ERR_INVALID_PTR:       return "ERR_INVALID_PTR";
    default:                           return "ERR_other";
    }
}

static int count_slots(const NV_ENCODE_API_FUNCTION_LIST *fl)
{
    // Skip version+reserved (first 8 bytes), walk the pointer array.
    void *const *p = (void *const *)((const char *)fl + 8);
    int total = (int)((sizeof(*fl) - 8) / sizeof(void *));
    int n = 0;
    for (int i = 0; i < total; i++) if (p[i]) n++;
    return n;
}

int main(void)
{
    char sys[MAX_PATH], path[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    snprintf(path, sizeof(path), "%s\\nvEncodeAPI64.dll", sys);
    HMODULE lib = LoadLibraryA(path);
    if (!lib) { printf("FATAL: load %s\n", path); return 2; }
    PFN_CREATE_INSTANCE create =
        (PFN_CREATE_INSTANCE)GetProcAddress(lib, "NvEncodeAPICreateInstance");

    printf("=== nvenc_probe71 ===\n");
    printf("sizeof(NV_ENCODE_API_FUNCTION_LIST) = %zu bytes (%zu pointer slots)\n\n",
           sizeof(NV_ENCODE_API_FUNCTION_LIST),
           (sizeof(NV_ENCODE_API_FUNCTION_LIST) - 8) / sizeof(void *));

    // --- [1] the 7.1 table ------------------------------------------------
    printf("--- [1] function list for an SDK 7.1 client (0x%08X) ---\n", V71_FUNCTION_LIST_VER);
    NV_ENCODE_API_FUNCTION_LIST fl71;
    memset(&fl71, 0, sizeof(fl71));
    fl71.version = V71_FUNCTION_LIST_VER;
    NVENCSTATUS st = create(&fl71);
    printf("  CreateInstance            -> %s\n", st_name(st));
    if (st != NV_ENC_SUCCESS) return 1;
    printf("  non-NULL slots            : %d\n", count_slots(&fl71));
    printf("  nvEncGetEncodePresetConfig   : %p\n", (void *)fl71.nvEncGetEncodePresetConfig);
    printf("  nvEncGetEncodePresetConfigEx : %p  %s\n", (void *)fl71.nvEncGetEncodePresetConfigEx,
           fl71.nvEncGetEncodePresetConfigEx ? "" : "  <-- ABSENT for a 7.1 client");
    printf("  nvEncGetEncodePresetCount    : %p\n", (void *)fl71.nvEncGetEncodePresetCount);
    printf("  nvEncInitializeEncoder       : %p\n", (void *)fl71.nvEncInitializeEncoder);

    // --- [1b] 7.1 session open BEFORE any newer instance exists -----------
    // Order matters: the driver appears to latch a process-wide client version
    // at NvEncodeAPICreateInstance. This test must run before the 13.1 table is
    // created, or it measures the latch rather than 7.1 support.
    {
        ID3D11Device *d0 = NULL; ID3D11DeviceContext *c0 = NULL;
        D3D_FEATURE_LEVEL f0;
        if (SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                        D3D11_SDK_VERSION, &d0, &f0, &c0))) {
            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS o;
            memset(&o, 0, sizeof(o));
            o.version    = V71_OPEN_SESSION_VER;
            o.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
            o.device     = d0;
            o.apiVersion = V71_API;
            void *e0 = NULL;
            NVENCSTATUS s0 = fl71.nvEncOpenEncodeSessionEx(&o, &e0);
            printf("\n--- [1b] 7.1 session open, no 13.1 instance created yet ---\n");
            printf("  nvEncOpenEncodeSessionEx (7.1 stamps) -> %s\n", st_name(s0));
            if (s0 == NV_ENC_SUCCESS) {
                NV_ENC_PRESET_CONFIG p0;
                memset(&p0, 0, sizeof(p0));
                p0.version           = V71_PRESET_CONFIG_VER;
                p0.presetCfg.version = V71_CONFIG_VER;
                NVENCSTATUS s1 = fl71.nvEncGetEncodePresetConfig(e0, NV_ENC_CODEC_H264_GUID,
                                                                 VNF_NV_ENC_PRESET_HQ_GUID, &p0);
                printf("  getPresetConfig(HQ) on that session   -> %s\n", st_name(s1));
                fl71.nvEncDestroyEncoder(e0);
            }
        }
    }

    // --- [2] a current-version table, for comparison ----------------------
    printf("\n--- [2] function list for a current (13.1) client ---\n");
    NV_ENCODE_API_FUNCTION_LIST flNow;
    memset(&flNow, 0, sizeof(flNow));
    flNow.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    st = create(&flNow);
    printf("  CreateInstance            -> %s\n", st_name(st));
    printf("  non-NULL slots            : %d\n", count_slots(&flNow));
    printf("  nvEncGetEncodePresetConfigEx : %p\n", (void *)flNow.nvEncGetEncodePresetConfigEx);
    printf("  same PresetConfig pointer as 7.1? %s\n",
           fl71.nvEncGetEncodePresetConfig == flNow.nvEncGetEncodePresetConfig ? "yes" : "NO");

    // --- [3] open a 7.1 session -------------------------------------------
    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL flo;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                 D3D11_SDK_VERSION, &dev, &flo, &ctx))) {
        printf("\nFATAL: D3D11CreateDevice\n"); return 2;
    }

    printf("\n--- [3] lowest client API version the driver will open a session for ---\n");
    struct { int maj, min; } vers[] = {
        {7,0},{7,1},{8,0},{8,1},{9,0},{9,1},{10,0},{11,0},{11,1},
        {12,0},{12,1},{12,2},{13,0},{13,1}
    };
    void *enc = NULL;
    uint32_t okApi = 0, okStructVer = 0;
    for (int i = 0; i < (int)(sizeof(vers)/sizeof(vers[0])); i++) {
        uint32_t api = (uint32_t)(vers[i].maj | (vers[i].min << 24));
        uint32_t sv  = api | (1u << 16) | (0x7u << 28);   // STRUCT_VERSION(1)
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS o;
        memset(&o, 0, sizeof(o));
        o.version    = sv;
        o.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        o.device     = dev;
        o.apiVersion = api;
        void *e = NULL;
        NVENCSTATUS s = fl71.nvEncOpenEncodeSessionEx(&o, &e);
        printf("  api %2d.%-2d (struct 0x%08X) -> %s\n", vers[i].maj, vers[i].min, sv, st_name(s));
        if (s == NV_ENC_SUCCESS) {
            if (!enc) { enc = e; okApi = api; okStructVer = sv; }
            else fl71.nvEncDestroyEncoder(e);
        }
    }
    if (!enc) { printf("\n  no client version could open a session - cannot continue\n"); return 1; }
    printf("\n  lowest accepted: api 0x%08X, struct 0x%08X\n", okApi, okStructVer);
    printf("  VEGAS asks for api 0x%08X (7.1) -> %s\n", V71_API,
           okApi == V71_API ? "ACCEPTED" : "REJECTED, the shim must raise it");

    // --- [4] preset enumeration as seen by a 7.1 client -------------------
    printf("\n--- [4] presets enumerated for a 7.1 client (H.264) ---\n");
    uint32_t cnt = 0;
    st = fl71.nvEncGetEncodePresetCount(enc, NV_ENC_CODEC_H264_GUID, &cnt);
    printf("  nvEncGetEncodePresetCount -> %s, count=%u\n", st_name(st), cnt);

    // --- [5] legacy preset through the 7.1 table --------------------------
    printf("\n--- [5] legacy preset via the 7.1 table (what VEGAS does) ---\n");
    NV_ENC_PRESET_CONFIG pc;
    memset(&pc, 0, sizeof(pc));
    pc.version           = V71_PRESET_CONFIG_VER;
    pc.presetCfg.version = V71_CONFIG_VER;
    st = fl71.nvEncGetEncodePresetConfig(enc, NV_ENC_CODEC_H264_GUID,
                                         VNF_NV_ENC_PRESET_HQ_GUID, &pc);
    printf("  getPresetConfig(HQ) with 7.1 struct versions -> %s\n", st_name(st));

    // --- [6] THE DECIDING TEST --------------------------------------------
    // Can the current-version ...ConfigEx pointer be driven with the 7.1
    // client's own struct versions? If yes, the shim can repair a 7.1 caller.
    printf("\n--- [6] borrow ...ConfigEx and feed it 7.1 structs ---\n");
    PNVENCGETENCODEPRESETCONFIGEX ex = flNow.nvEncGetEncodePresetConfigEx;
    if (!ex) {
        printf("  no ...ConfigEx available at all\n");
    } else {
        memset(&pc, 0, sizeof(pc));
        pc.version           = V71_PRESET_CONFIG_VER;
        pc.presetCfg.version = V71_CONFIG_VER;
        st = ex(enc, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
                NV_ENC_TUNING_INFO_HIGH_QUALITY, &pc);
        printf("  ...ConfigEx(P4, HQ) with 7.1 struct versions -> %s\n", st_name(st));
        if (st == NV_ENC_SUCCESS)
            printf("     gopLength=%d rcMode=0x%X  => the repair path WORKS for 7.1\n",
                   pc.presetCfg.gopLength, (unsigned)pc.presetCfg.rcParams.rateControlMode);

        // And with current struct versions, for contrast.
        memset(&pc, 0, sizeof(pc));
        pc.version           = NV_ENC_PRESET_CONFIG_VER;
        pc.presetCfg.version = NV_ENC_CONFIG_VER;
        st = ex(enc, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
                NV_ENC_TUNING_INFO_HIGH_QUALITY, &pc);
        printf("  ...ConfigEx(P4, HQ) with 13.1 struct versions -> %s\n", st_name(st));
    }

    // --- [7] which field does the driver actually reject? -----------------
    // op.version is the struct version; op.apiVersion is the client's API
    // version. Crossing them shows which one must be rewritten.
    printf("\n--- [7] struct version vs apiVersion, crossed ---\n");
    struct { const char *label; uint32_t sv, api; } cross[] = {
        { "struct 13.1, api 13.1 (control)", NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER, NVENCAPI_VERSION },
        { "struct 13.1, api  7.1          ", NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER, V71_API },
        { "struct  7.1, api 13.1          ", V71_OPEN_SESSION_VER,                     NVENCAPI_VERSION },
        { "struct  7.1, api  7.1          ", V71_OPEN_SESSION_VER,                     V71_API },
    };
    for (int i = 0; i < 4; i++) {
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS o;
        memset(&o, 0, sizeof(o));
        o.version    = cross[i].sv;
        o.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        o.device     = dev;
        o.apiVersion = cross[i].api;
        void *e = NULL;
        NVENCSTATUS s = fl71.nvEncOpenEncodeSessionEx(&o, &e);
        printf("  %s -> %s\n", cross[i].label, st_name(s));
        if (s == NV_ENC_SUCCESS) fl71.nvEncDestroyEncoder(e);
    }

    // --- [8] can a 13.1 session be driven end to end? ---------------------
    // If the shim raises the version, everything downstream must still work.
    printf("\n--- [8] legacy preset through a raised session ---\n");
    NV_ENC_PRESET_CONFIG pc2;
    memset(&pc2, 0, sizeof(pc2));
    pc2.version           = NV_ENC_PRESET_CONFIG_VER;
    pc2.presetCfg.version = NV_ENC_CONFIG_VER;
    st = ex(enc, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
            NV_ENC_TUNING_INFO_HIGH_QUALITY, &pc2);
    printf("  ...ConfigEx(P4,HQ) on the raised session -> %s\n", st_name(st));

    fl71.nvEncDestroyEncoder(enc);
    printf("\n=== done ===\n");
    return 0;
}
