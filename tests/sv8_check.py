#!/usr/bin/env python3
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
"""Inspect a Musepack file's block structure to prove it is Musepack SV8.

SV8 streams are a sequence of packet blocks after the 4-byte magic "MPCK":

    <2-byte key> <size varint> <payload>

The size varint (see mpc_bits_get_size / encodeSize) encodes the total block
length *including* the 2-byte key and the varint itself; the payload is that
total minus the header. The "SH" (stream header) block's payload begins with
a 4-byte big-endian CRC followed by the stream-version byte, which must be 8
for SV8.

The first block ("SH") carries the stream version; the "EI" (encoder info)
block carries the encoder version as <7-bit profile><1-bit PNS><8-bit
major><8-bit minor><8-bit build>, all bit-packed MSB-first.

This tool has two modes:

  --assert <file>   Assert the file is a valid SV8 stream: "MPCK" magic,
                    an "SH" block with stream version 8, no SV7 "MP+" magic,
                    and an "SE" (end of stream) block at the end. Exits 0 on
                    success, 1 on failure with a reason on stderr.
  --assert <file> --expect-ei X.Y.Z
                    Also assert the EI block reports encoder version X.Y.Z.
  --diff <a> <b>    Compare two files block by block (keys + payloads) and
                    report which blocks differ. Used to prove that a version
                    bump changed only the "EI" metadata bytes, not the audio
                    payload.

Kept deliberately small and self-contained (stdlib only): it exists to gate
the format invariant, not to reimplement the decoder.
"""

import sys

MAGIC_SV8 = b"MPCK"
MAGIC_SV7 = b"MP+"


class StreamError(Exception):
    pass


def read_varint(data, pos):
    value = 0
    n = 0
    while True:
        if pos >= len(data):
            raise StreamError("truncated size varint")
        b = data[pos]
        pos += 1
        n += 1
        value = (value << 7) | (b & 0x7F)
        if not (b & 0x80):
            return value, n, pos


def parse_blocks(data):
    """Yield (key, payload, header_len) for each block after the magic."""
    pos = 0
    while pos < len(data):
        if pos + 2 > len(data):
            raise StreamError("truncated block key")
        key = data[pos:pos + 2]
        pos += 2
        total, vlen, pos = read_varint(data, pos)
        payload_len = total - (2 + vlen)
        if payload_len < 0 or pos + payload_len > len(data):
            raise StreamError("block %r size %d overruns stream" % (key, total))
        payload = data[pos:pos + payload_len]
        pos += payload_len
        yield key, payload, 2 + vlen
        if key == b"SE":
            return
    raise StreamError("no SE (end of stream) block found")


def load_blocks(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == MAGIC_SV7:
        raise StreamError("file starts with SV7 magic 'MP+' (not SV8)")
    if data[:4] != MAGIC_SV8:
        raise StreamError("file does not start with SV8 magic 'MPCK'")
    return data[4:], list(parse_blocks(data[4:]))


def extract_sh_version(blocks):
    for key, payload, _ in blocks:
        if key == b"SH":
            if len(payload) < 5:
                raise StreamError("SH block payload too short")
            return payload[4]
    raise StreamError("no SH (stream header) block found")


def extract_ei_version(blocks):
    """Return (major, minor, build) from the EI block, or None if absent."""
    for key, payload, _ in blocks:
        if key == b"EI":
            # 7-bit profile + 1-bit PNS + 8+8+8-bit version, MSB-first.
            bits = "".join(format(b, "08b") for b in payload)
            if len(bits) < 24:
                raise StreamError("EI block payload too short")
            major = int(bits[8:16], 2)
            minor = int(bits[16:24], 2)
            build = int(bits[24:32], 2) if len(bits) >= 32 else 0
            return major, minor, build
    return None


def mode_assert(path, expect_ei=None):
    body, blocks = load_blocks(path)
    version = extract_sh_version(blocks)
    if version != 8:
        raise StreamError("SH stream version is %d, expected 8" % version)
    if blocks[-1][0] != b"SE":
        raise StreamError("stream does not end with an SE block")
    ei = extract_ei_version(blocks)
    if expect_ei is not None:
        if ei is None:
            raise StreamError("expected EI encoder version %s, but no EI block found"
                              % expect_ei)
        got = "%d.%d.%d" % ei
        if got != expect_ei:
            raise StreamError("EI encoder version is %s, expected %s"
                              % (got, expect_ei))
    print("ok: %s is Musepack SV8 (magic MPCK, stream version %d%s)" % (
        path, version,
        ", encoder %d.%d.%d" % ei if ei else ""))
    return 0


def mode_diff(path_a, path_b):
    _, blocks_a = load_blocks(path_a)
    _, blocks_b = load_blocks(path_b)
    a = {k: p for k, p, _ in blocks_a}
    b = {k: p for k, p, _ in blocks_b}
    keys = [k for k, _, _ in blocks_a]
    different = []
    for k in keys:
        pa = a.get(k)
        pb = b.get(k)
        if pa != pb:
            different.append(k)
    print("blocks differing: %s" % (", ".join(
        sorted(k.decode("latin1") for k in different)) or "(none)"))
    for k in different:
        pa = a.get(k)
        pb = b.get(k)
        print("  %r: a=%s b=%s" % (k, pa.hex() if pa is not None else None,
                                   pb.hex() if pb is not None else None))
    return 0 if len(different) == 0 else 2


def main(argv):
    if len(argv) == 2 and argv[0] == "--assert":
        try:
            return mode_assert(argv[1])
        except StreamError as e:
            print("FAIL: %s" % e, file=sys.stderr)
            return 1
    if len(argv) == 4 and argv[0] == "--assert" and argv[2] == "--expect-ei":
        try:
            return mode_assert(argv[1], expect_ei=argv[3])
        except StreamError as e:
            print("FAIL: %s" % e, file=sys.stderr)
            return 1
    if len(argv) == 3 and argv[0] == "--diff":
        try:
            return mode_diff(argv[1], argv[2])
        except StreamError as e:
            print("FAIL: %s" % e, file=sys.stderr)
            return 1
    print("usage: sv8_check.py --assert <file> | --diff <file-a> <file-b>",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
