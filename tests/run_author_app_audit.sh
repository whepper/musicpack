#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Packaging audit for the standalone MusicPack Author .app.
#
# Verifies the bundle contains musicpack, musicpack-sonic and a relocatable
# ONNX Runtime, and that no dependency resolves to Homebrew or an external
# path. Skips (passes) when the .app has not been built on this machine; the
# authoritative gate runs inside scripts/build-author-macos.sh.
#
# Usage: tests/run_author_app_audit.sh [path-to-MusicPack Author.app]

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${1:-$ROOT/author/src-tauri/target/release/bundle/macos/MusicPack Author.app}"

if [ ! -d "$APP" ]; then
  echo "skip: no bundled app at $APP (run scripts/build-author-macos.sh first)"
  exit 0
fi

exec "$ROOT/scripts/audit-author-macos.sh" "$APP"
