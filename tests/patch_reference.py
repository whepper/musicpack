#!/usr/bin/env python3
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
"""Patch the pristine reference CMakeLists so only the encoder builds.

The upstream r475 build tree (git 05d97a5) cannot configure on a modern
CI runner without help:

  * mpcgain/mpcchap require libreplaygain/libcuefile and hard-fail when
    they are absent;
  * mpcdec, mpc2sv8, mpccut (and wavcmp) declare add_executable twice
    under if(MSVC), which CMake rejects as duplicate targets on Windows;
  * the encoder only needs libmpcpsy, libmpcenc and include.

Commenting out the other subdirectories keeps configure green everywhere
while leaving the mpcenc target (and its static-lib dependencies) intact.

Usage: patch_reference.py <reference-source-dir>
"""

import os
import re
import sys

KEEP = ("libmpcpsy", "libmpcenc", "mpcenc", "include")

ASINH_SHIM = re.compile(
    r"#ifdef _MSC_VER\n"
    r"static double\n"
    r"asinh \( double x \)\n"
    r"\{\n"
    r"    return x >= 0  \?  log \(sqrt \(x\*x\+1\) \+ x\)  :  -log \(sqrt \(x\*x\+1\) - x\);\n"
    r"\}\n"
    r"#endif\n",
    re.MULTILINE,
)


def patch_file(path, repl, label):
    """Apply a list of (old, new) replacements to a source file.

    `old` may be a str (exact match) or a compiled regex. Each replacement is
    skipped if its `new` is already present, so re-running is safe.
    """
    if not os.path.isfile(path):
        print("warning: no file at %s (skipping %s)" % (path, label), file=sys.stderr)
        return
    with open(path, "r", encoding="utf-8") as f:
        txt = f.read()
    for old, new in repl:
        if hasattr(old, "sub"):
            if old.search(txt):
                txt = old.sub(new, txt)
        elif old in txt:
            txt = txt.replace(old, new)
    with open(path, "w", encoding="utf-8") as f:
        f.write(txt)
    print("patched %s (%s)" % (path, label))


def main():
    if len(sys.argv) != 2:
        print("usage: patch_reference.py <reference-source-dir>", file=sys.stderr)
        return 1
    top = sys.argv[1]
    path = os.path.join(top, "CMakeLists.txt")
    if not os.path.isfile(path):
        print("no CMakeLists.txt at %s" % path, file=sys.stderr)
        return 1

    with open(path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    out = []
    for line in lines:
        m = re.match(r"^add_subdirectory\((\w+)\)\s*$", line)
        if m and m.group(1) not in KEEP:
            out.append("# " + line.rstrip("\n") + "  # disabled: not needed by mpcenc\n")
        else:
            out.append(line)

    with open(path, "w", encoding="utf-8") as f:
        f.writelines(out)

    # Match the main build's Release optimization so encoder outputs are
    # comparable. GCC 14+ promotes -Wincompatible-pointer-types
    # to an error, which the reference code triggers (ans.c, mpcenc.c).
    with open(path, "r", encoding="utf-8") as f:
        txt = f.read()
    txt = txt.replace(
        'set(CMAKE_C_FLAGS "-O3 -Wall -fomit-frame-pointer -pipe")',
        'set(CMAKE_C_FLAGS "-O3 -Wall -fomit-frame-pointer -pipe -Wno-error=incompatible-pointer-types -ffp-contract=off")',
    )
    # The modernized encoder pins FP semantics with -ffp-contract=off
    # (GCC/Clang) and /fp:precise (MSVC; VS2022's /fp:precise defaults to
    # fp_contract(off), so no contractions) so its scalar and SIMD paths are
    # provably identical. The pristine reference build must use the same
    # policy so the live-mode comparison stays meaningful. The canonical
    # local manifest remains a separate -O0 fallback and is unaffected.
    fp_pin = 'if(MSVC)\n  set(CMAKE_C_FLAGS "/fp:precise")\nendif(MSVC)\n'
    if 'set(CMAKE_C_FLAGS "/fp:precise")' not in txt:
        marker = "endif(NOT MSVC)\n"
        if marker in txt:
            txt = txt.replace(marker, marker + "\n" + fp_pin, 1)
        else:
            txt += "\n" + fp_pin
    with open(path, "w", encoding="utf-8") as f:
        f.write(txt)

    # MSVC-only source fixes. These are upstream bugs that the modernized
    # tree already fixes (renamed table, removed the asinh shim); on MSVC
    # they are hard errors, on GCC/Clang they are harmless redefinitions.
    patch_file(
        os.path.join(top, "libmpcenc", "bitstream.c"),
        [
            ("static const mpc_uint8_t log2[32]", "static const mpc_uint8_t mpc_log2[32]"),
            ("static const mpc_uint8_t log2_lost[32]", "static const mpc_uint8_t mpc_log2_lost[32]"),
            ("log2_lost[max - 1]", "mpc_log2_lost[max - 1]"),
            ("log2[max - 1]", "mpc_log2[max - 1]"),
        ],
        "log2/log2_lost -> mpc_log2/mpc_log2_lost",
    )
    patch_file(
        os.path.join(top, "libmpcpsy", "psy_tab.c"),
        [
            (ASINH_SHIM, ""),
            (
                "m->Max_Band = (int) ( m->BandWidth * 64. / m->SampleFreq );",
                "m->Max_Band = m->SampleFreq != 0. ? (int)(m->BandWidth * 64. / m->SampleFreq) : 0;",
            ),
        ],
        "remove asinh shim; guard zero sample rate",
    )
    patch_file(
        os.path.join(top, "libmpcenc", "analy_filter.c"),
        [
            (
                "c1 = Ci_opt - 8;\n    c2 = Ci_opt + 128;",
                "c1 = Ci_opt;\n    c2 = Ci_opt + 136;",
            ),
            (
                "c1 += 8, c2 += 8;\n        *y++ = EXPR (c1,x1) + EXPR (c2,x2);",
                "*y++ = EXPR (c1,x1) + EXPR (c2,x2);\n        c1 += 8, c2 += 8;",
            ),
            (
                "c1 = Ci_opt + 384 - 8;\n    c2 = Ci_opt + 256;",
                "c1 = Ci_opt + 384;\n    c2 = Ci_opt + 264;",
            ),
        ],
        "keep Vectoring pointers in bounds",
    )
    patch_file(
        os.path.join(top, "libmpcenc", "encode_sv7.c"),
        [("int k = 0, idx = 1, cnt = 0, sng;", "int k = 0, cnt = 0, sng;\n            unsigned int idx = 1;")],
        "make bitstream index shifts unsigned",
    )
    patch_file(
        os.path.join(top, "mpcenc", "mpcenc.h"),
        [
            ("QuantizeSubband                 ( unsigned int* qu_output", "QuantizeSubband                 ( mpc_int16_t* qu_output"),
            ("QuantizeSubbandWithNoiseShaping ( unsigned int* qu_output", "QuantizeSubbandWithNoiseShaping ( mpc_int16_t* qu_output"),
        ],
        "match quantizer output declarations to definitions",
    )
    patch_file(
        os.path.join(top, "common", "fastmath.c"),
        [
            ("const float  tabatan2", "float  tabatan2"),
            ("const float  tabcos", "float  tabcos"),
            ("const float  tabsqrt_ex", "float  tabsqrt_ex"),
            ("const float  tabsqrt_m", "float  tabsqrt_m"),
        ],
        "make runtime-initialized fastmath tables writable",
    )
    patch_file(
        os.path.join(top, "include", "mpc", "mpcmath.h"),
        [
            ("extern const float  tabatan2", "extern float  tabatan2"),
            ("extern const float  tabcos", "extern float  tabcos"),
            ("extern const float  tabsqrt_ex", "extern float  tabsqrt_ex"),
            ("extern const float  tabsqrt_m", "extern float  tabsqrt_m"),
        ],
        "declare runtime-initialized fastmath tables writable",
    )

    print("patched %s (encoder-only Release build)" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
