#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# OPTIONAL: bootstrap the Discogs-EffNet comparator environment.
#
# Essentia is AGPL-3.0 and the MTG models are CC BY-NC-SA 4.0 — this is
# EVALUATION-ONLY and never a MusicPack dependency. It needs its own venv
# because essentia-tensorflow pulls numpy 2.x, which conflicts with the
# openl3/TensorFlow 2.15 (numpy<2) environment.
#
# Usage:
#   research/sonic/bootstrap_essentia.sh
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -x .venv-essentia/bin/python ]; then
    uv venv --python 3.11 .venv-essentia
fi

uv pip install --python .venv-essentia/bin/python pytest -r requirements-essentia.txt soundfile

.venv-essentia/bin/python -c "import essentia; print('essentia', essentia.__version__)"
echo "discogs comparator environment ready at $(pwd)/.venv-essentia"
