#!/usr/bin/env bash
# Bootstrap the MusicPack Sonic research environment (macOS / Linux).
#
# Creates a CPython 3.11 venv (see requirements-openl3.txt for why 3.11),
# installs the pinned OpenL3/TensorFlow stack, and installs the patched
# openl3 build from the local wheels/ tree.
#
# Usage:
#   research/sonic/bootstrap_env.sh
#   research/sonic/.venv/bin/python research/sonic/benchmark.py --help
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -x .venv/bin/python ]; then
    uv venv --python 3.11 .venv
fi

python3 patch_openl3.py wheels

uv pip install --python .venv/bin/python pytest -r requirements-openl3.txt
uv pip install --python .venv/bin/python wheels/openl3-0.4.0

.venv/bin/python - <<'PY'
import openl3, tensorflow as tf
print("openl3", openl3.__version__)
print("tensorflow", tf.__version__)
PY

echo "research environment ready at $(pwd)/.venv"
