#!/bin/bash
# Generates src/nvenc_deprecated_presets.h from the last NVENC header that still
# declared the legacy preset GUIDs (SDK 11.1, via FFmpeg/nv-codec-headers, MIT).
#
# The values are extracted mechanically rather than transcribed, so the generated
# header is verifiable against upstream at any time by re-running this script.
set -euo pipefail

OUT_DIR="$(cd "$(dirname "$0")/../src" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

TAG="n11.1.5.3"
URL="https://raw.githubusercontent.com/FFmpeg/nv-codec-headers/${TAG}/include/ffnvcodec/nvEncodeAPI.h"

echo "Fetching ${TAG} header ..."
curl -sSL -m 60 -o "$TMP/old.h" "$URL"

NAMES="NV_ENC_PRESET_DEFAULT_GUID NV_ENC_PRESET_HP_GUID NV_ENC_PRESET_HQ_GUID NV_ENC_PRESET_BD_GUID NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID NV_ENC_PRESET_LOW_LATENCY_HQ_GUID NV_ENC_PRESET_LOW_LATENCY_HP_GUID NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID NV_ENC_PRESET_LOSSLESS_HP_GUID"

{
  echo "// SPDX-License-Identifier: MIT"
  echo "//"
  echo "// GENERATED FILE - do not edit by hand."
  echo "// Produced by scripts/gen_deprecated_presets.sh from nv-codec-headers ${TAG}"
  echo "// (NVIDIA Video Codec SDK 11.1), the last revision that still declared the"
  echo "// legacy encode-preset GUIDs. NVIDIA removed them in SDK 13.x."
  echo "//"
  echo "// These constants are pure interface identifiers: the shim needs them only to"
  echo "// RECOGNISE preset GUIDs that old applications still pass in."
  echo
  echo "#ifndef NVENC_DEPRECATED_PRESETS_H"
  echo "#define NVENC_DEPRECATED_PRESETS_H"
  echo
  echo "#include <guiddef.h>"
  echo

  for n in $NAMES; do
    awk -v want="$n" '
      $0 ~ ("static const GUID[ \t]+" want "[ \t]*=") {
        getline line
        n = 0
        while (match(line, /0x[0-9a-fA-F]+/)) {
          tok = substr(line, RSTART, RLENGTH)
          n++; p[n] = tok
          line = substr(line, RSTART + RLENGTH)
        }
        if (n != 11) { printf "#error \"failed to parse %s\"\n", want; exit 1 }
        printf "static const GUID VNF_%s =\n", want
        printf "    { %s, %s, %s, { %s, %s, %s, %s, %s, %s, %s, %s } };\n",
               p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]
        found = 1
        exit 0
      }
      END { if (!found) { printf "#error \"missing %s\"\n", want; exit 1 } }
    ' "$TMP/old.h"
  done

  echo
  echo "#endif // NVENC_DEPRECATED_PRESETS_H"
} > "$OUT_DIR/nvenc_deprecated_presets.h"

echo "Wrote $OUT_DIR/nvenc_deprecated_presets.h"
grep -c "^static const GUID" "$OUT_DIR/nvenc_deprecated_presets.h"
