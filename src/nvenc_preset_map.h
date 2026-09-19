// SPDX-License-Identifier: MIT
//
// Legacy-preset -> (modern preset, tuning info) mapping.
//
// Source: NVIDIA "NVENC Preset Migration Guide", Video Codec SDK v11.1,
// last updated 2021-10-26. Table 1 (HEVC) and Table 2 (H.264).
//
// NVIDIA's tables are indexed by (old preset, old RC mode, resolution, GPU
// architecture). This shim is invoked from nvEncGetEncodePresetConfig(), which
// receives only the codec and the preset - no RC mode and no frame size - so a
// single representative row per (codec, preset) pair has to be chosen.
//
// The rows used here are:
//   * the Ampere/Turing preset column. Driver branches that removed the legacy
//     presets also dropped Maxwell and Pascal support entirely, so any GPU that
//     can hit this bug is Turing or newer.
//   * the 1080p row, with the baseline RC mode for that preset family
//     (VBR for the quality presets, CBR for low-latency, CQP for lossless).
//
// Note that H.264 and HEVC genuinely differ - HEVC "HQ" maps to P6 while H.264
// "HQ" maps to P4 - so the table is keyed by codec as well as preset.
//
// Deliberately NOT replicated from NVIDIA's tables: the multipass, GOP length,
// IDR period and slice-mode columns. Those describe how to reproduce an old
// preset's full behaviour from scratch. The host application here already sets
// its own rate control, GOP and bitrate on the NV_ENC_CONFIG it gets back, so
// forcing those fields would override the user's render settings. This shim
// translates preset identity only and leaves encoding decisions to the host.

#ifndef NVENC_PRESET_MAP_H
#define NVENC_PRESET_MAP_H

#include "nvEncodeAPI.h"
#include "nvenc_deprecated_presets.h"

typedef struct {
    const GUID        *legacy;   // preset GUID removed in SDK 13.x
    const GUID        *modern;   // P1..P7 replacement
    int                pnum;     // 1..7, carried explicitly for logging: the
                                 // P1..P7 GUIDs are separate objects, not an
                                 // array, so it cannot be derived by pointer
                                 // arithmetic
    NV_ENC_TUNING_INFO tuning;   // tuning info the replacement needs
    const char        *name;     // for logging
} vnf_preset_row;

// ---- H.264 (NVIDIA Table 2, Ampere/Turing, 1080p) ------------------------
static const vnf_preset_row VNF_MAP_H264[] = {
    { &VNF_NV_ENC_PRESET_HP_GUID,                  &NV_ENC_PRESET_P2_GUID, 2, NV_ENC_TUNING_INFO_HIGH_QUALITY, "HP" },
    { &VNF_NV_ENC_PRESET_DEFAULT_GUID,             &NV_ENC_PRESET_P3_GUID, 3, NV_ENC_TUNING_INFO_HIGH_QUALITY, "DEFAULT" },
    { &VNF_NV_ENC_PRESET_HQ_GUID,                  &NV_ENC_PRESET_P4_GUID, 4, NV_ENC_TUNING_INFO_HIGH_QUALITY, "HQ" },
    { &VNF_NV_ENC_PRESET_BD_GUID,                  &NV_ENC_PRESET_P4_GUID, 4, NV_ENC_TUNING_INFO_HIGH_QUALITY, "BD" },
    { &VNF_NV_ENC_PRESET_LOW_LATENCY_HP_GUID,      &NV_ENC_PRESET_P2_GUID, 2, NV_ENC_TUNING_INFO_LOW_LATENCY,  "LOW_LATENCY_HP" },
    { &VNF_NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID, &NV_ENC_PRESET_P3_GUID, 3, NV_ENC_TUNING_INFO_LOW_LATENCY,  "LOW_LATENCY_DEFAULT" },
    { &VNF_NV_ENC_PRESET_LOW_LATENCY_HQ_GUID,      &NV_ENC_PRESET_P4_GUID, 4, NV_ENC_TUNING_INFO_LOW_LATENCY,  "LOW_LATENCY_HQ" },
    { &VNF_NV_ENC_PRESET_LOSSLESS_HP_GUID,         &NV_ENC_PRESET_P2_GUID, 2, NV_ENC_TUNING_INFO_LOSSLESS,     "LOSSLESS_HP" },
    { &VNF_NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID,    &NV_ENC_PRESET_P3_GUID, 3, NV_ENC_TUNING_INFO_LOSSLESS,     "LOSSLESS_DEFAULT" },
};

// ---- HEVC (NVIDIA Table 1, Ampere/Turing, 1080p) -------------------------
static const vnf_preset_row VNF_MAP_HEVC[] = {
    { &VNF_NV_ENC_PRESET_HP_GUID,                  &NV_ENC_PRESET_P1_GUID, 1, NV_ENC_TUNING_INFO_HIGH_QUALITY, "HP" },
    { &VNF_NV_ENC_PRESET_DEFAULT_GUID,             &NV_ENC_PRESET_P5_GUID, 5, NV_ENC_TUNING_INFO_HIGH_QUALITY, "DEFAULT" },
    { &VNF_NV_ENC_PRESET_HQ_GUID,                  &NV_ENC_PRESET_P6_GUID, 6, NV_ENC_TUNING_INFO_HIGH_QUALITY, "HQ" },
    { &VNF_NV_ENC_PRESET_BD_GUID,                  &NV_ENC_PRESET_P5_GUID, 5, NV_ENC_TUNING_INFO_HIGH_QUALITY, "BD" },
    { &VNF_NV_ENC_PRESET_LOW_LATENCY_HP_GUID,      &NV_ENC_PRESET_P2_GUID, 2, NV_ENC_TUNING_INFO_LOW_LATENCY,  "LOW_LATENCY_HP" },
    { &VNF_NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID, &NV_ENC_PRESET_P3_GUID, 3, NV_ENC_TUNING_INFO_LOW_LATENCY,  "LOW_LATENCY_DEFAULT" },
    { &VNF_NV_ENC_PRESET_LOW_LATENCY_HQ_GUID,      &NV_ENC_PRESET_P4_GUID, 4, NV_ENC_TUNING_INFO_LOW_LATENCY,  "LOW_LATENCY_HQ" },
    { &VNF_NV_ENC_PRESET_LOSSLESS_HP_GUID,         &NV_ENC_PRESET_P3_GUID, 3, NV_ENC_TUNING_INFO_LOSSLESS,     "LOSSLESS_HP" },
    { &VNF_NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID,    &NV_ENC_PRESET_P5_GUID, 5, NV_ENC_TUNING_INFO_LOSSLESS,     "LOSSLESS_DEFAULT" },
};

#define VNF_MAP_COUNT ((int)(sizeof(VNF_MAP_H264) / sizeof(VNF_MAP_H264[0])))

// Picks the table for a codec. Legacy presets only ever applied to H.264 and
// HEVC; anything else (AV1) falls back to the H.264 table so that a stray
// legacy GUID still resolves to something sane rather than failing.
static __inline const vnf_preset_row *vnf_table_for_codec(const GUID *codec)
{
    if (IsEqualGUID(codec, &NV_ENC_CODEC_HEVC_GUID)) return VNF_MAP_HEVC;
    return VNF_MAP_H264;
}

static __inline const vnf_preset_row *vnf_find_row(const GUID *codec, const GUID *preset)
{
    const vnf_preset_row *tbl = vnf_table_for_codec(codec);
    for (int i = 0; i < VNF_MAP_COUNT; i++)
        if (IsEqualGUID(preset, tbl[i].legacy)) return &tbl[i];
    return NULL;
}

#endif // NVENC_PRESET_MAP_H
