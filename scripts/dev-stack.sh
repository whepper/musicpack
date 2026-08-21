#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# One-command start of the MusicPack Docker test stack on macOS.
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p docker/library

case "${1:-up}" in
  up)
    docker compose -f docker/compose.yaml up -d --build server
    echo "Web UI:    http://localhost:8080"
    echo "Health:    http://localhost:8080/api/v1/health"
    echo "Library:   $(pwd)/docker/library (drop .mpack packages here, then restart)"
    for i in $(seq 1 30); do
      curl -fsS http://localhost:8080/api/v1/health >/dev/null 2>&1 && break
      sleep 1
    done
    docker compose -f docker/compose.yaml exec server musicpack-server scan --library /data/library || true
    ;;
  down)  docker compose -f docker/compose.yaml down ;;
  logs)  docker compose -f docker/compose.yaml logs -f server ;;
  tests) docker compose -f docker/compose.yaml run --rm tests ;;
  shell) docker compose -f docker/compose.yaml exec server bash || \
         docker compose -f docker/compose.yaml run --rm --entrypoint bash server ;;
  *) echo "usage: $0 [up|down|logs|tests|shell]" >&2; exit 2 ;;
esac
