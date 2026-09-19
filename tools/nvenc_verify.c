// nvenc_verify - end-to-end proof that the shim repairs the legacy preset path.
//
// Usage: nvenc_verify.exe [path\to\nvEncodeAPI64.dll]
//   With no argument it loads the system DLL, which is expected to FAIL on a
//   driver that dropped the legacy presets. Point it at the built shim and the
//   same sequence is expected to PASS.
//
// The test deliberately mirrors what VEGAS does:
//   nvEncGetEncodePresetConfig(H.264, NV_ENC_PRESET_HQ_GUID)
//     -> nvEncInitializeEncoder(presetGUID = NV_ENC_PRESET_HQ_GUID)
//     -> encode real frames and read back a bitstream.

#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "nvEncodeAPI.h"
#include "../src/nvenc_deprecated_presets.h"

#define WIDTH   640
#define HEIGHT  360
#define FRAMES  10

typedef NVENCSTATUS(NVENCAPI *PFN_CREATE_INSTANCE)(NV_ENCODE_API_FUNCTION_LIST *);

static int g_failures = 0;

static void check(const char *what, NVENCSTATUS st)
{
    if (st == NV_ENC_SUCCESS) {
        printf("  [ ok ] %s\n", what);
    } else {
        printf("  [FAIL] %s  (status %d)\n", what, (int)st);
        g_failures++;
    }
}

int main(int argc, char **argv)
{
    char dllPath[MAX_PATH];
    if (argc > 1) {
        snprintf(dllPath, sizeof(dllPath), "%s", argv[1]);
    } else {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        snprintf(dllPath, sizeof(dllPath), "%s\\nvEncodeAPI64.dll", sys);
    }

    printf("=== nvenc_verify ===\n");
    printf("DLL under test: %s\n\n", dllPath);

    HMODULE lib = LoadLibraryA(dllPath);
    if (!lib) { printf("FATAL: LoadLibrary failed (err %lu)\n", GetLastError()); return 2; }

    PFN_CREATE_INSTANCE create =
        (PFN_CREATE_INSTANCE)GetProcAddress(lib, "NvEncodeAPICreateInstance");
    if (!create) { printf("FATAL: NvEncodeAPICreateInstance not exported\n"); return 2; }

    NV_ENCODE_API_FUNCTION_LIST api;
    memset(&api, 0, sizeof(api));
    api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = create(&api);
    if (st != NV_ENC_SUCCESS) { printf("FATAL: CreateInstance -> %d\n", (int)st); return 2; }

    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL flOut;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                 D3D11_SDK_VERSION, &dev, &flOut, &ctx))) {
        printf("FATAL: D3D11CreateDevice failed\n"); return 2;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op;
    memset(&op, 0, sizeof(op));
    op.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    op.device     = dev;
    op.apiVersion = NVENCAPI_VERSION;

    void *enc = NULL;
    st = api.nvEncOpenEncodeSessionEx(&op, &enc);
    if (st != NV_ENC_SUCCESS) { printf("FATAL: OpenEncodeSessionEx -> %d\n", (int)st); return 2; }

    // --- 1. the call that VEGAS fails on --------------------------------
    printf("[1] legacy preset config (the call VEGAS makes)\n");
    NV_ENC_PRESET_CONFIG pc;
    memset(&pc, 0, sizeof(pc));
    pc.version           = NV_ENC_PRESET_CONFIG_VER;
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    st = api.nvEncGetEncodePresetConfig(enc, NV_ENC_CODEC_H264_GUID,
                                        VNF_NV_ENC_PRESET_HQ_GUID, &pc);
    check("nvEncGetEncodePresetConfig(H.264, NV_ENC_PRESET_HQ_GUID)", st);
    if (st != NV_ENC_SUCCESS) {
        printf("\n  Legacy presets are unavailable through this DLL.\n");
        printf("  RESULT: BROKEN (%d failure(s))\n", g_failures);
        return 1;
    }

    // --- 2. preset enumeration ------------------------------------------
    printf("\n[2] preset enumeration\n");
    uint32_t count = 0;
    st = api.nvEncGetEncodePresetCount(enc, NV_ENC_CODEC_H264_GUID, &count);
    check("nvEncGetEncodePresetCount", st);
    printf("         presets advertised: %u\n", count);

    GUID *guids = (GUID *)calloc(count ? count : 1, sizeof(GUID));
    uint32_t got = 0;
    st = api.nvEncGetEncodePresetGUIDs(enc, NV_ENC_CODEC_H264_GUID, guids, count, &got);
    check("nvEncGetEncodePresetGUIDs", st);
    int sawLegacy = 0;
    for (uint32_t i = 0; i < got; i++)
        if (IsEqualGUID(&guids[i], &VNF_NV_ENC_PRESET_HQ_GUID)) sawLegacy = 1;
    printf("         returned %u GUIDs, legacy HQ present: %s\n", got, sawLegacy ? "yes" : "no");
    free(guids);

    // --- 3. initialise with a legacy preset -----------------------------
    printf("\n[3] encoder initialisation with the legacy preset\n");
    // Keep the test deterministic: no B-frames and no lookahead means every
    // nvEncEncodePicture call returns a frame instead of buffering.
    pc.presetCfg.frameIntervalP              = 1;
    pc.presetCfg.rcParams.enableLookahead    = 0;

    NV_ENC_INITIALIZE_PARAMS init;
    memset(&init, 0, sizeof(init));
    init.version       = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID    = NV_ENC_CODEC_H264_GUID;
    init.presetGUID    = VNF_NV_ENC_PRESET_HQ_GUID;   // legacy GUID on purpose
    init.encodeWidth   = WIDTH;
    init.encodeHeight  = HEIGHT;
    init.darWidth      = WIDTH;
    init.darHeight     = HEIGHT;
    init.frameRateNum  = 30;
    init.frameRateDen  = 1;
    init.enablePTD     = 1;
    init.encodeConfig  = &pc.presetCfg;
    init.tuningInfo    = NV_ENC_TUNING_INFO_UNDEFINED;

    st = api.nvEncInitializeEncoder(enc, &init);
    check("nvEncInitializeEncoder(presetGUID = NV_ENC_PRESET_HQ_GUID)", st);
    if (st != NV_ENC_SUCCESS) { printf("\n  RESULT: BROKEN (%d failure(s))\n", g_failures); return 1; }

    // --- 4. encode real frames ------------------------------------------
    printf("\n[4] encoding %d frames\n", FRAMES);
    NV_ENC_CREATE_INPUT_BUFFER inb;
    memset(&inb, 0, sizeof(inb));
    inb.version   = NV_ENC_CREATE_INPUT_BUFFER_VER;
    inb.width     = WIDTH;
    inb.height    = HEIGHT;
    inb.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    st = api.nvEncCreateInputBuffer(enc, &inb);
    check("nvEncCreateInputBuffer", st);

    NV_ENC_CREATE_BITSTREAM_BUFFER outb;
    memset(&outb, 0, sizeof(outb));
    outb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    st = api.nvEncCreateBitstreamBuffer(enc, &outb);
    check("nvEncCreateBitstreamBuffer", st);
    if (g_failures) { printf("\n  RESULT: BROKEN\n"); return 1; }

    size_t total = 0;
    int encoded = 0;
    for (int f = 0; f < FRAMES; f++) {
        NV_ENC_LOCK_INPUT_BUFFER lk;
        memset(&lk, 0, sizeof(lk));
        lk.version     = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lk.inputBuffer = inb.inputBuffer;
        if (api.nvEncLockInputBuffer(enc, &lk) != NV_ENC_SUCCESS) { g_failures++; break; }

        // Moving vertical bar, so the frames are not identical and the encoder
        // actually has residual to compress.
        uint8_t *y = (uint8_t *)lk.bufferDataPtr;
        for (int r = 0; r < HEIGHT; r++)
            for (int c = 0; c < WIDTH; c++)
                y[r * lk.pitch + c] = (uint8_t)(((c + f * 16) % WIDTH < 64) ? 235 : 16);
        uint8_t *uv = y + (size_t)lk.pitch * HEIGHT;
        memset(uv, 128, (size_t)lk.pitch * (HEIGHT / 2));
        api.nvEncUnlockInputBuffer(enc, inb.inputBuffer);

        NV_ENC_PIC_PARAMS pic;
        memset(&pic, 0, sizeof(pic));
        pic.version         = NV_ENC_PIC_PARAMS_VER;
        pic.inputBuffer     = inb.inputBuffer;
        pic.outputBitstream = outb.bitstreamBuffer;
        pic.bufferFmt       = NV_ENC_BUFFER_FORMAT_NV12;
        pic.pictureStruct   = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputWidth      = WIDTH;
        pic.inputHeight     = HEIGHT;
        pic.inputPitch      = lk.pitch;
        pic.frameIdx        = f;
        pic.inputTimeStamp  = f;

        NVENCSTATUS e = api.nvEncEncodePicture(enc, &pic);
        if (e == NV_ENC_ERR_NEED_MORE_INPUT) continue;
        if (e != NV_ENC_SUCCESS) { printf("  [FAIL] nvEncEncodePicture frame %d -> %d\n", f, (int)e); g_failures++; break; }

        NV_ENC_LOCK_BITSTREAM lb;
        memset(&lb, 0, sizeof(lb));
        lb.version         = NV_ENC_LOCK_BITSTREAM_VER;
        lb.outputBitstream = outb.bitstreamBuffer;
        if (api.nvEncLockBitstream(enc, &lb) == NV_ENC_SUCCESS) {
            total += lb.bitstreamSizeInBytes;
            encoded++;
            api.nvEncUnlockBitstream(enc, outb.bitstreamBuffer);
        } else { g_failures++; break; }
    }

    // Flush.
    NV_ENC_PIC_PARAMS eos;
    memset(&eos, 0, sizeof(eos));
    eos.version         = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags  = NV_ENC_PIC_FLAG_EOS;
    api.nvEncEncodePicture(enc, &eos);

    printf("  [ ok ] encoded %d frame(s), %zu bytes of H.264 bitstream\n", encoded, total);
    if (encoded == 0 || total == 0) { printf("  [FAIL] no bitstream produced\n"); g_failures++; }

    api.nvEncDestroyBitstreamBuffer(enc, outb.bitstreamBuffer);
    api.nvEncDestroyInputBuffer(enc, inb.inputBuffer);
    api.nvEncDestroyEncoder(enc);

    printf("\n=== RESULT: %s ===\n", g_failures ? "BROKEN" : "WORKING");
    return g_failures ? 1 : 0;
}
