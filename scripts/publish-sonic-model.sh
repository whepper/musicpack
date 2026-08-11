#!/usr/bin/env bash
# Publish the pinned Sonic model artifact to a GitHub release.
#
# The production analyzer (musicpack-sonic) consumes an immutable,
# SHA-256-verified post-frontend ONNX artifact. End users never build it:
# MusicPack Author downloads it from the release asset below.
#
#   asset URL:
#     https://github.com/whepper/musicpack/releases/download/sonic-model-openl3-v1/openl3_post.onnx
#
# Usage:
#   scripts/publish-sonic-model.sh [path/to/openl3_post.onnx]
#
# With no argument, the artifact is produced from the pinned OpenL3 H5 via
# the reproducible conversion script (requires the research venv + the H5).
# The script refuses to publish an artifact that does not match the pinned
# SHA-256 (sonic/sonic_profile.h / SONIC_PROFILE_ONNX_SHA256).
#
# Requirements: gh CLI (authenticated), and either the artifact or the
# research venv + research/sonic/models/openl3_audio_mel256_music.h5.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PIN="3b4b7dac370d8f3dc6c3b22613269301ce7d4540d857f8d434f67bfb1a9ca986"
RELEASE="sonic-model-openl3-v1"
ASSET="openl3_post.onnx"

ARTIFACT="${1:-}"

if [ -z "$ARTIFACT" ] || [ ! -f "$ARTIFACT" ]; then
  H5="$ROOT/research/sonic/models/openl3_audio_mel256_music.h5"
  PY="$ROOT/research/sonic/.venv/bin/python"
  if [ ! -f "$H5" ] || [ ! -x "$PY" ]; then
    echo "error: pass a built $ASSET, or provide the research H5 + venv" >&2
    exit 1
  fi
  OUT="$(mktemp -t sonic-model.XXXXXX)"
  "$PY" "$ROOT/research/sonic/convert_openl3.py" "$H5" "$OUT"
  ARTIFACT="$OUT"
fi

SHA="$(shasum -a 256 "$ARTIFACT" | awk '{print $1}')"
SIZE="$(stat -f%z "$ARTIFACT")"

echo "artifact: $ARTIFACT"
echo "size:     $SIZE bytes"
echo "sha256:   $SHA"

if [ "$SHA" != "$PIN" ]; then
  echo "error: artifact SHA-256 does not match the pinned value" >&2
  echo "  pinned: $PIN" >&2
  exit 1
fi

command -v gh >/dev/null || { echo "error: gh CLI is required" >&2; exit 1; }

# Create the release if it does not exist yet (idempotent), then upload the
# asset. The release is a model-asset release, not a code version tag.
if ! gh release view "$RELEASE" >/dev/null 2>&1; then
  gh release create "$RELEASE" \
    --title "Sonic model — $ASSET" \
    --notes "Immutable OpenL3 post-frontend ONNX artifact for the \
musicpack-sonic-openl3-v1 profile. SHA-256 $PIN."
fi

gh release upload "$RELEASE" "$ARTIFACT#${ASSET}" --clobber

echo
echo "Published: https://github.com/whepper/musicpack/releases/download/$RELEASE/$ASSET"
echo "sha256: $SHA"
