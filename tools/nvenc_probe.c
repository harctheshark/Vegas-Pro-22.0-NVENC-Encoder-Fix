// nvenc_probe - interrogates the installed NVENC driver to establish exactly
// which parts of the legacy preset API still work.
//
// This is the diagnostic instrument behind the shim: it answers, on real
// hardware rather than by assumption,
//   1. what API version the driver supports,
//   2. whether nvEncGetEncodePresetConfig is still populated in the function list,
//   3. what the driver returns when it is called with a deprecated preset GUID,
//   4. whether the replacement path (nvEncGetEncodePresetConfigEx + P1..P7) works.

#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdint.h>

#include "nvEncodeAPI.h"
#include "../src/nvenc_deprecated_presets.h"

typedef NVENCSTATUS(NVENCAPI *PFN_CREATE_INSTANCE)(NV_ENCODE_API_FUNCTION_LIST *);
typedef NVENCSTATUS(NVENCAPI *PFN_MAX_VERSION)(uint32_t *);

static const char *status_name(NVENCSTATUS s)
{
    switch (s) {
    case NV_ENC_SUCCESS:                     return "NV_ENC_SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE:        return "ERR_NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE:      return "ERR_UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE:   return "ERR_INVALID_ENCODERDEVICE";
    case NV_ENC_ERR_INVALID_DEVICE:          return "ERR_INVALID_DEVICE";
    case NV_ENC_ERR_DEVICE_NOT_EXIST:        return "ERR_DEVICE_NOT_EXIST";
    case NV_ENC_ERR_INVALID_PTR:             return "ERR_INVALID_PTR";
    case NV_ENC_ERR_INVALID_EVENT:           return "ERR_INVALID_EVENT";
    case NV_ENC_ERR_INVALID_PARAM:           return "ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL:            return "ERR_INVALID_CALL";
    case NV_ENC_ERR_OUT_OF_MEMORY:           return "ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_ENCODER_NOT_INITIALIZED: return "ERR_ENCODER_NOT_INITIALIZED";
    case NV_ENC_ERR_UNSUPPORTED_PARAM:       return "ERR_UNSUPPORTED_PARAM";
    case NV_ENC_ERR_LOCK_BUSY:               return "ERR_LOCK_BUSY";
    case NV_ENC_ERR_NOT_ENOUGH_BUFFER:       return "ERR_NOT_ENOUGH_BUFFER";
    case NV_ENC_ERR_INVALID_VERSION:         return "ERR_INVALID_VERSION";
    case NV_ENC_ERR_MAP_FAILED:              return "ERR_MAP_FAILED";
    case NV_ENC_ERR_NEED_MORE_INPUT:         return "ERR_NEED_MORE_INPUT";
    case NV_ENC_ERR_ENCODER_BUSY:            return "ERR_ENCODER_BUSY";
    case NV_ENC_ERR_EVENT_NOT_REGISTERD:     return "ERR_EVENT_NOT_REGISTERD";
    case NV_ENC_ERR_GENERIC:                 return "ERR_GENERIC";
    case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY: return "ERR_INCOMPATIBLE_CLIENT_KEY";
    case NV_ENC_ERR_UNIMPLEMENTED:           return "ERR_UNIMPLEMENTED";
    case NV_ENC_ERR_RESOURCE_REGISTER_FAILED:return "ERR_RESOURCE_REGISTER_FAILED";
    case NV_ENC_ERR_RESOURCE_NOT_REGISTERED: return "ERR_RESOURCE_NOT_REGISTERED";
    case NV_ENC_ERR_RESOURCE_NOT_MAPPED:     return "ERR_RESOURCE_NOT_MAPPED";
    default:                                 return "ERR_<unknown>";
    }
}

struct named_guid { const char *name; const GUID *guid; };

static const struct named_guid g_deprecated[] = {
    { "NV_ENC_PRESET_DEFAULT_GUID",             &VNF_NV_ENC_PRESET_DEFAULT_GUID },
    { "NV_ENC_PRESET_HP_GUID",                  &VNF_NV_ENC_PRESET_HP_GUID },
    { "NV_ENC_PRESET_HQ_GUID",                  &VNF_NV_ENC_PRESET_HQ_GUID },
    { "NV_ENC_PRESET_BD_GUID",                  &VNF_NV_ENC_PRESET_BD_GUID },
    { "NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID", &VNF_NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID },
    { "NV_ENC_PRESET_LOW_LATENCY_HQ_GUID",      &VNF_NV_ENC_PRESET_LOW_LATENCY_HQ_GUID },
    { "NV_ENC_PRESET_LOW_LATENCY_HP_GUID",      &VNF_NV_ENC_PRESET_LOW_LATENCY_HP_GUID },
    { "NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID",    &VNF_NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID },
    { "NV_ENC_PRESET_LOSSLESS_HP_GUID",         &VNF_NV_ENC_PRESET_LOSSLESS_HP_GUID },
};

static const struct named_guid g_modern[] = {
    { "P1", &NV_ENC_PRESET_P1_GUID }, { "P2", &NV_ENC_PRESET_P2_GUID },
    { "P3", &NV_ENC_PRESET_P3_GUID }, { "P4", &NV_ENC_PRESET_P4_GUID },
    { "P5", &NV_ENC_PRESET_P5_GUID }, { "P6", &NV_ENC_PRESET_P6_GUID },
    { "P7", &NV_ENC_PRESET_P7_GUID },
};

static void print_guid(const GUID *g)
{
    printf("{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
           (unsigned long)g->Data1, g->Data2, g->Data3,
           g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
           g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static const char *lookup_name(const GUID *g)
{
    for (int i = 0; i < (int)(sizeof(g_deprecated) / sizeof(g_deprecated[0])); i++)
        if (IsEqualGUID(g, g_deprecated[i].guid)) return g_deprecated[i].name;
    for (int i = 0; i < (int)(sizeof(g_modern) / sizeof(g_modern[0])); i++)
        if (IsEqualGUID(g, g_modern[i].guid)) return g_modern[i].name;
    return "(unrecognised)";
}

int main(void)
{
    printf("=== NVENC legacy-preset probe ===\n");
    printf("built against NVENC API %u.%u\n\n",
           NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);

    // Load the real driver DLL explicitly from System32 so the probe reports on
    // the driver even when a shim is installed alongside it.
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\nvEncodeAPI64.dll", sys);

    HMODULE lib = LoadLibraryA(path);
    if (!lib) { printf("FATAL: cannot load %s (err %lu)\n", path, GetLastError()); return 1; }
    printf("loaded %s\n", path);

    PFN_MAX_VERSION max_ver =
        (PFN_MAX_VERSION)GetProcAddress(lib, "NvEncodeAPIGetMaxSupportedVersion");
    PFN_CREATE_INSTANCE create =
        (PFN_CREATE_INSTANCE)GetProcAddress(lib, "NvEncodeAPICreateInstance");
    printf("  NvEncodeAPIGetMaxSupportedVersion : %s\n", max_ver ? "present" : "MISSING");
    printf("  NvEncodeAPICreateInstance         : %s\n", create ? "present" : "MISSING");
    if (!create) return 1;

    if (max_ver) {
        uint32_t v = 0;
        NVENCSTATUS s = max_ver(&v);
        printf("  driver max supported version      : %u.%u  (raw 0x%08X, status %s)\n",
               v >> 4, v & 0xf, v, status_name(s));
    }

    printf("\n--- [1] NvEncodeAPICreateInstance across API versions ---\n");
    struct { const char *label; uint32_t ver; } versions[] = {
        { " 9.0", (uint32_t)( 9u                  | (2u << 16) | (0x7u << 28)) },
        { "10.0", (uint32_t)(10u                  | (2u << 16) | (0x7u << 28)) },
        { "11.0", (uint32_t)(11u                  | (2u << 16) | (0x7u << 28)) },
        { "11.1", (uint32_t)(11u | (1u << 24)     | (2u << 16) | (0x7u << 28)) },
        { "12.0", (uint32_t)(12u                  | (2u << 16) | (0x7u << 28)) },
        { "12.1", (uint32_t)(12u | (1u << 24)     | (2u << 16) | (0x7u << 28)) },
        { "12.2", (uint32_t)(12u | (2u << 24)     | (2u << 16) | (0x7u << 28)) },
        { "13.0", (uint32_t)(13u                  | (2u << 16) | (0x7u << 28)) },
        { "13.1", (uint32_t)(13u | (1u << 24)     | (2u << 16) | (0x7u << 28)) },
    };
    for (int i = 0; i < (int)(sizeof(versions) / sizeof(versions[0])); i++) {
        NV_ENCODE_API_FUNCTION_LIST fl;
        memset(&fl, 0, sizeof(fl));
        fl.version = versions[i].ver;
        NVENCSTATUS s = create(&fl);
        printf("  SDK %s (0x%08X) -> %-16s", versions[i].label, versions[i].ver, status_name(s));
        if (s == NV_ENC_SUCCESS)
            printf(" cfg=%p cfgEx=%p init=%p openEx=%p",
                   (void *)fl.nvEncGetEncodePresetConfig,
                   (void *)fl.nvEncGetEncodePresetConfigEx,
                   (void *)fl.nvEncInitializeEncoder,
                   (void *)fl.nvEncOpenEncodeSessionEx);
        printf("\n");
    }

    // From here on use the current version for the functional tests.
    NV_ENCODE_API_FUNCTION_LIST api;
    memset(&api, 0, sizeof(api));
    api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = create(&api);
    if (st != NV_ENC_SUCCESS) { printf("\nFATAL: create instance failed: %s\n", status_name(st)); return 1; }

    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl_out;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &dev, &fl_out, &ctx);
    if (FAILED(hr)) { printf("\nFATAL: D3D11CreateDevice failed 0x%08lX\n", (unsigned long)hr); return 1; }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op;
    memset(&op, 0, sizeof(op));
    op.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    op.device     = dev;
    op.apiVersion = NVENCAPI_VERSION;

    void *enc = NULL;
    st = api.nvEncOpenEncodeSessionEx(&op, &enc);
    if (st != NV_ENC_SUCCESS) { printf("\nFATAL: open session: %s\n", status_name(st)); return 1; }
    printf("\nencode session opened (D3D11)\n");

    printf("\n--- [2] presets the driver ENUMERATES for H.264 ---\n");
    uint32_t count = 0;
    st = api.nvEncGetEncodePresetCount(enc, NV_ENC_CODEC_H264_GUID, &count);
    printf("  nvEncGetEncodePresetCount -> %s, count=%u\n", status_name(st), count);
    if (st == NV_ENC_SUCCESS && count) {
        GUID *guids = (GUID *)calloc(count, sizeof(GUID));
        uint32_t got = 0;
        st = api.nvEncGetEncodePresetGUIDs(enc, NV_ENC_CODEC_H264_GUID, guids, count, &got);
        printf("  nvEncGetEncodePresetGUIDs -> %s, got=%u\n", status_name(st), got);
        for (uint32_t i = 0; i < got; i++) {
            printf("    ");
            print_guid(&guids[i]);
            printf("  %s\n", lookup_name(&guids[i]));
        }
        free(guids);
    }

    printf("\n--- [3] nvEncGetEncodePresetConfig with DEPRECATED GUIDs (H.264) ---\n");
    printf("    (this is the exact call VEGAS makes)\n");
    for (int i = 0; i < (int)(sizeof(g_deprecated) / sizeof(g_deprecated[0])); i++) {
        NV_ENC_PRESET_CONFIG pc;
        memset(&pc, 0, sizeof(pc));
        pc.version           = NV_ENC_PRESET_CONFIG_VER;
        pc.presetCfg.version = NV_ENC_CONFIG_VER;
        st = api.nvEncGetEncodePresetConfig(enc, NV_ENC_CODEC_H264_GUID, *g_deprecated[i].guid, &pc);
        printf("  %-42s -> %s\n", g_deprecated[i].name, status_name(st));
    }

    printf("\n--- [4] nvEncGetEncodePresetConfigEx with P1..P7 (H.264, HIGH_QUALITY) ---\n");
    for (int i = 0; i < (int)(sizeof(g_modern) / sizeof(g_modern[0])); i++) {
        NV_ENC_PRESET_CONFIG pc;
        memset(&pc, 0, sizeof(pc));
        pc.version           = NV_ENC_PRESET_CONFIG_VER;
        pc.presetCfg.version = NV_ENC_CONFIG_VER;
        st = api.nvEncGetEncodePresetConfigEx(enc, NV_ENC_CODEC_H264_GUID, *g_modern[i].guid,
                                              NV_ENC_TUNING_INFO_HIGH_QUALITY, &pc);
        printf("  %-4s -> %-16s  rcMode=0x%X gopLength=%d\n", g_modern[i].name, status_name(st),
               (unsigned)pc.presetCfg.rcParams.rateControlMode, pc.presetCfg.gopLength);
    }

    printf("\n--- [5] same, for HEVC ---\n");
    for (int i = 0; i < (int)(sizeof(g_deprecated) / sizeof(g_deprecated[0])); i++) {
        NV_ENC_PRESET_CONFIG pc;
        memset(&pc, 0, sizeof(pc));
        pc.version           = NV_ENC_PRESET_CONFIG_VER;
        pc.presetCfg.version = NV_ENC_CONFIG_VER;
        st = api.nvEncGetEncodePresetConfig(enc, NV_ENC_CODEC_HEVC_GUID, *g_deprecated[i].guid, &pc);
        printf("  %-42s -> %s\n", g_deprecated[i].name, status_name(st));
    }

    api.nvEncDestroyEncoder(enc);
    printf("\n=== probe complete ===\n");
    return 0;
}
