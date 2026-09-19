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

// ---- The mapping actually used -------------------------------------------
//
// NVIDIA's migration tables exist to REPRODUCE a legacy preset's old
// behaviour. That was the right target while the goal was compatibility, and
// it is why an earlier version of this file mapped "High quality" to P4 and
// spread the rest over P2..P5, differing per codec.
//
// It is the wrong target now. The host is working again, so the only thing
// these names have to do is describe what the user is going to get. NVENC
// separates the two axes cleanly, and the mapping below follows that split:
//
//     preset P1..P7  = speed / quality, P1 fastest, P7 best
//     tuning info    = what the encode is FOR
//
// So each legacy family keeps its tuning, and the three words the host puts in
// its dropdown - "high performance", "default", "high quality" - select the
// point on the P scale they claim to select:
//
//     high performance -> P1      fastest
//     default          -> P4      balanced, NVIDIA's own middle
//     high quality     -> P6      slower, visibly better at a given bitrate
//     BD               -> P5      between default and high quality
//
// Both codecs use the same numbers deliberately. NVIDIA's tables map the same
// legacy name to different P values for H.264 and HEVC, which was correct for
// reproducing old output but would mean "High quality" quietly meaning two
// different things depending on the format chosen. Accuracy of the label wins.
//
// P6 rather than P7 for high quality is the user's call and a sensible one:
// P7 costs noticeably more encode time for a small gain, and at a generous
// bitrate the difference is hard to see. Changing it is a one-line edit here.

// ---- H.264 ---------------------------------------------------------------
static const vnf_preset_row VNF_MAP_H264[] = {
    { &VNF_NV_ENC_PRESET_HP_GUID,                  &NV_ENC_PRESET_P1_GUID, 1, NV_ENC_TUNING_INFO_HIGH_QUALITY, "HP" },
    { &VNF_NV_ENC_PRESET_DEFAULT_GUID,             &NV_ENC_PRESET_P4_GUID, 4, NV_ENC_TUNING_INFO_HIGH_QUALITY, "DEFAULT" },
    { &VNF_NV_ENC_PRESET_HQ_GUID,                  &NV_ENC_PRESET_P6_GUID, 6, NV_ENC_TUNING_INFO_HIGH_QUALITY, "HQ" },
    { &VNF_NV_ENC_PRESET_BD_GUID,                  &NV_ENC_PRESET_P5_GUID, 5, NV_ENC_TUNING_INFO_HIGH_QUALITY, "BD" },
    { &VNF_NV_ENC_PRESET_LOW_LATENCY_HP_GUID,      &NV_ENC_PRESET_P1_GUID, 1, NV_ENC_TUNING_INFO_LOW_LATENCY,  "LOW_LATENCY_HP" },
    { &VNF_NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID, &NV_ENC_PRESET_P4_GUID, 4, NV_ENC_TUNING_INFO_LOW_LATENCY,  "LOW_LATENCY_DEFAULT" },
    { &VNF_NV_ENC_PRESET_LOW_LATENCY_HQ_GUID,      &NV_ENC_PRESET_P6_GUID, 6, NV_ENC_TUNING_INFO_LOW_LATENCY,  "LOW_LATENCY_HQ" },
    { &VNF_NV_ENC_PRESET_LOSSLESS_HP_GUID,         &NV_ENC_PRESET_P1_GUID, 1, NV_ENC_TUNING_INFO_LOSSLESS,     "LOSSLESS_HP" },
    { &VNF_NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID,    &NV_ENC_PRESET_P4_GUID, 4, NV_ENC_TUNING_INFO_LOSSLESS,     "LOSSLESS_DEFAULT" },
};

// ---- HEVC (same numbers, so a name means one thing) ----------------------
static const vnf_preset_row VNF_MAP_HEVC[] = {
    { &VNF_NV_ENC_PRESET_HP_GUID,                  &NV_ENC_PRESET_P1_GUID, 1, NV_ENC_TUNING_INFO_HIGH_QUALITY, "HP" },
    { &VNF_NV_ENC_PRESET_DEFAULT_GUID,             &NV_ENC_PRESET_P4_GUID, 4, NV_ENC_TUNING_INFO_HIGH_QUALITY, "DEFAULT" },
    { &VNF_NV_ENC_PRESET_HQ_GUID,                  &NV_ENC_PRESET_P6_GUID, 6, NV_ENC_TUNING_INFO_HIGH_QUALITY, "HQ" },
    { &VNF_NV_ENC_PRESET_BD_GUID,                  &NV_ENC_PRESET_P5_GUID, 5, NV_ENC_TUNING_INFO_HIGH_QUALITY, "BD" },
    { &VNF_NV_ENC_PRESET_LOW_LATENCY_HP_GUID,      &NV_ENC_PRESET_P1_GUID, 1, NV_ENC_TUNING_INFO_LOW_LATENCY,  "LOW_LATENCY_HP" },
    { &VNF_NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID, &NV_ENC_PRESET_P4_GUID, 4, NV_ENC_TUNING_INFO_LOW_LATENCY,  "LOW_LATENCY_DEFAULT" },
    { &VNF_NV_ENC_PRESET_LOW_LATENCY_HQ_GUID,      &NV_ENC_PRESET_P6_GUID, 6, NV_ENC_TUNING_INFO_LOW_LATENCY,  "LOW_LATENCY_HQ" },
    { &VNF_NV_ENC_PRESET_LOSSLESS_HP_GUID,         &NV_ENC_PRESET_P1_GUID, 1, NV_ENC_TUNING_INFO_LOSSLESS,     "LOSSLESS_HP" },
    { &VNF_NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID,    &NV_ENC_PRESET_P4_GUID, 4, NV_ENC_TUNING_INFO_LOSSLESS,     "LOSSLESS_DEFAULT" },
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
