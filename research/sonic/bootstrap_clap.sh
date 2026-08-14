#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Bootstrap the LAION-CLAP (decision-gate) research environment.
#
# CLAP needs PyTorch, which conflicts with openl3's numpy<2 / TF 2.15 stack,
# so it gets its own CPython 3.11 venv. The checkpoint is Apache-2.0 and is
# downloaded at runtime into models/ (gitignored, SHA-256 verified).
#
# Usage:
#   research/sonic/bootstrap_clap.sh
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -x .venv-clap/bin/python ]; then
    uv venv --python 3.11 .venv-clap
fi

uv pip install --python .venv-clap/bin/python pytest -r requirements-clap.txt

.venv-clap/bin/python - <<'PY'
import torch, transformers
print("torch", torch.__version__, "| transformers", transformers.__version__)
PY
echo "CLAP research environment ready at $(pwd)/.venv-clap"
