# MPAK v1 — single-file `.mpak` physical container

Status: **normative for the MPAK v1 format freeze**. Version 1.

MPAK is the single-file physical representation of a MusicPack `.mpack`
package. It carries exactly the same logical model as the directory bundle
defined in `specs/musicpack-v1.md`: the manifest remains the sole semantic
authority, and MPAK is a *storage backend*, nothing more. As anticipated by
the logical spec ("a future packed single-file `MPAK` representation will be
a different *storage backend* ... deliberately contains no offsets, packing,
or container details"), this document specifies precisely those offsets,
packing, and container details — and changes nothing about the logical
model.

```text
manifest  = what the album IS      (semantic authority, storage-independent)
INDX      = physical acceleration  (derived, rebuildable, optional)
DATA      = member bytes           (self-identifying, byte-exact)
TAIL      = fixity & identity      (optional in v1)
```

---

## 1. Design goals and invariant

- **Byte-exact member storage**: every member is stored bit-for-bit
  unchanged; no recompression, transcoding, normalization, or
  repacketization. This preserves bit-exact comparability against the
  directory bundle and keeps MPAK codec-independent.
- **Early index for random access**: an `INDX` block immediately follows
  the fixed header, enabling O(log n) member lookup from a single HTTP
  Range prefix fetch.
- **Sequential parsing without the index**: everything INDX knows is
  re-derivable by scanning DATA blocks from byte 0. The index must never be
  a prerequisite for reading, recovery, or verification of semantics.
- **One authority, one duplication rule**: the manifest is unchanged from
  v1 (authoritative); INDX duplicates `{path, size, sha256}` per member for
  physical-layer verification; TAIL adds physical completeness/identity.
  INDX↔manifest disagreement is always a *consistency error*, never a
  warning.
- **Deterministic builds**: identical logical inputs → byte-identical
  `.mpak`, assertable by hashing the file (mirrors the encoder `compat`
  discipline).

Core invariant:

> **INDX is an acceleration structure, not a prerequisite for sequential
> parsing or recovery.**

---

## 2. Physical layout

```text
┌───────────────────────────┐
│ MPAK header   (16 bytes)  │  ┌ fixed preamble: magic, version, flags
├───────────────────────────┤  │
│ INDX block                │  ← physical acceleration (RECOMMENDED)
├───────────────────────────┤  │
│ MANF block                │  ← exact canonical manifest.json bytes (REQUIRED)
├───────────────────────────┤  │
│ DATA block (per member)   │  ← zero or more, manifest traversal order
│   ...                     │  │
├───────────────────────────┤  │
│ TAIL block                │  ← fixity, identity, backward INDX pointer (OPTIONAL)
└───────────────────────────┘
```

Block order in v1 (normative for writers): `header, INDX, MANF, DATA…,
TAIL`. Readers MUST NOT depend on INDX position; they MUST locate every
block type by scanning. In particular, a conforming reader accepts an INDX
anywhere before TAIL, and a file with INDX absent is well-formed.

### 2.1 MPAK header (fixed 16 bytes)

| Offset | Size | Value                                      |
|--------|------|--------------------------------------------|
| 0–3    | 4    | ASCII `"MPAK"` (`4D 50 41 4B`)             |
| 4      | 1    | major version, u8                          |
| 5      | 1    | minor version, u8                          |
| 6–7    | 2    | flags, big-endian u16 (see §3.6)           |
| 8–15   | 8    | reserved, big-endian u64, MUST be zero     |

- **Magic**: byte 0 MUST be `M`. No prefixes are tolerated before the
  magic (MPAK explicitly does not inherit SV8's ID3v2-prefix tolerance).
- **Versioning**: unknown major → reject cleanly. For a *known* major with
  an unknown higher minor, the reader proceeds: it reads known block types
  and skips unknown ones (§3.5), and reports a downgrade-compatibility
  *warning* (not an error). Unknown minor semantics are therefore
  additive-only within a major.
- **Reserved**: must be zero in v1; readers MUST NOT reject non-zero
  reserved bytes at the *warning* level — treat as non-fatal (forward
  compatibility pressure relief), but writers MUST emit zero.

### 2.2 Block framing

Every block is framed uniformly:

| Field    | Offset | Size | Encoding                                       |
|----------|--------|------|------------------------------------------------|
| type     | 0      | 4    | 1-byte ASCII codes (see §3.4)                  |
| length   | 4      | 8    | big-endian u64, payload length in bytes         |
| crc16    | 12     | 2    | big-endian CRC-16 over bytes 0–11 (type+length)|
| payload  | 14     | n    | exactly `length` bytes                          |

- **Total overhead: 14 bytes per block.** Skip = `14 + length`.
- **length** is *payload-only* (does NOT include the 14-byte header). This
  deliberately rejects SV8's self-including base-128 varint (`encodeSize`
  with `addCodeSize`), which exists only to save bytes at codec-packet
  granularity and complicates writers; at package granularity the saving is
  noise.
- **CRC-16**: polynomial `0x8005`, initial value `0xFFFF`, no reflection,
  xorout `0x0000` ("CRC-16/BUYPASS"). Verifying it MUST precede
  interpreting `length` on that block. (Rationale: distinguishes framing
  corruption from payload corruption and enables rescan confidence —
  SV8's single SH-CRC-32 is too weak for archival; per-block CRC-32 is
  redundant given member-level SHA-256.)
- **Bit-level addressing is not used**: all offsets and lengths are
  byte-granular (again rejecting SV8 practice).

### 2.3 Block types in v1

```text
INDX   physical acceleration index (RECOMMENDED)
MANF   canonical manifest.json bytes (REQUIRED)
DATA   one member object (any asset kind)
TAIL   fixity & identity trailer (OPTIONAL)
```

Member role taxonomy (AUDI/ARTW/SONC/…) is explicitly **not** used. Role
is manifest semantics; the container stores one generic object type.

### 2.4 Type characters

- v1 assignments use uppercase A–Z only.
- **Private/experimental namespace**: types containing any lowercase
  letter (a–z), digit (0–9), or non-ASCII byte are reserved for private
  use and MUST be skipped by conforming readers. Only all-uppercase
  4-character codes may enter the public registry via a spec revision.
- This avoids SV8's 676-code A–Z constraint while keeping hex-dump
  readability (RIFF/ISOBMFF-proven).

Skip rule: unknown block types MUST be skipped using the declared length
(after CRC-16 passes). Readers refusing to skip (e.g. simple extractors)
MUST at least tolerate-and-stop, not hard-fail.

---

## 3. DATA blocks

A member object's payload layout:

| Field         | Size     | Encoding                                          |
|---------------|----------|---------------------------------------------------|
| path_length   | 2        | big-endian u16, number of path bytes               |
| path          | p        | raw path bytes (canonical package-relative path)   |
| member bytes  | m        | raw member content, byte-exact                     |

`length` (block header) = `2 + p + m`.

- **Path**: identical to a `manifest.json` asset path — the canonical
  rules of `specs/musicpack-v1.md §2` apply (relative, `/` separators, no
  `.`/`..`/empty segments, no backslash/colon/control chars, ≤4096 chars).
  Readers MUST revalidate path rules on scan-recovery. A DATA block whose
  preamble path violates the rules is a hard error.
- **Member bytes**: stored bit-for-bit unchanged. `.mpc` (complete SV8
  stream: `MPCK SH RG EI SO AP* [ST] SE`), `.flac`, artwork, lyrics,
  `analysis/sonic.json`, `analysis/waveform/*.wfm`, any future asset —
  all opaque, none repacketized. No container-level recompression,
  transcoding, normalization, or codec-aware framing.
- **Self-identification**: the path preamble makes every DATA block
  independently attributable during a sequential scan even with INDX and
  MANF both lost.

Authority relationship (normative):

```text
MANF (manifest)   = semantic authority (path → sha256 mapping, roles, order)
DATA preamble     = physical self-identification (enables scan rebuild)
INDX              = derived physical acceleration
```

Any path present in DATA preambles but absent from the manifest is an
**unreferenced object** — a warning, never an error (mirrors the directory
bundle's extra-files semantics, `musicpack-v1.md §2`).

---

## 4. MANF block

`MANF` payload = the **exact bytes of canonical `manifest.json`**.
Writers MUST NOT reformat, regenerate, renormalize, or otherwise rewrite
it. The JSON serializer's canonical rules (fixed key order per
`musicpack-v1.md`, `%.8g` number formatting, omission of absent optionals,
no timestamps) are inherited unchanged, unchanged from the directory
backend.

Consequences (normative):

- Directory `.mpack` → `.mpak` and back is a **pure repack**: manifest
  bytes identical, member hashes identical. A tool repacking MUST produce
  identical member SHA-256 digests and a MANF payload whose bytes equal
  the directory's `manifest.json`.
- The manifest is **not** duplicated in v1: exactly one MANF block
  ("duplicate MANF copies" are prohibited — see open decision D3).
- `length(MANF) ≤ 16 MiB` (physical expression of the manifest budget).

---

## 5. INDX block

Pure acceleration. Entries are sorted **lexicographically by canonical
path bytes** (unsigned byte comparison, case-sensitive, `/` = 0x2F sorts
below alphanumerics).

| Field          | Size        | Encoding                                        |
|----------------|-------------|-------------------------------------------------|
| entry_count    | 4           | big-endian u32                                   |
| per entry:     |             |                                                  |
|   path_length  | 2           | big-endian u16                                   |
|   path         | p           | raw canonical path bytes                         |
|   member_offset| 8           | big-endian u64, absolute file offset             |
|   member_length| 8           | big-endian u64, bytes                            |
|   sha256       | 32          | raw digest bytes (not hex)                        |

Limits: `entry_count ≤ 4096`, `path_length ≤ 4096` (with total path bytes
respecting the logical path limit), `member_offset + member_length` must
not overflow u64 and must not exceed the physical file size when known.
Duplicate paths in INDX = hard error. INDX payload size ≤ 4096 × (2 +
4096 + 8 + 8 + 32) + 4 ≈ 17.0 MiB worst case; typical albums (tens of
entries) are a few KiB.

- **member_offset semantics**: absolute from file byte 0, pointing at the
  **first byte of the raw member data** — i.e. past the DATA block's
  14-byte header, path_length field, and path preamble. It is NOT the
  DATA block start.
- **INDX is recoverable**: sequential scan of DATA blocks rebuilds path,
  offset, length; SHA-256 recomputed over member bytes. A missing or
  corrupt INDX MUST NOT render the package unreadable (§10).
- **INDX↔manifest consistency** (verify rules):
  - Every manifest-referenced path has **exactly one** INDX entry
    whose `path`, `member_length`, and `sha256` agree with the DATA
    block/wording (manifest sha256 is authoritative — INDX hash may be
    checked against it cheaply before hashing the member itself).
  - INDX entry not referenced by the manifest → **warning** (unreferenced
    object), matching the directory backend's extra-files semantics.
  - Missing entry, mismatched length/hash → **error** (consistency).
  - Duplicate paths (in INDX or in DATA stream) → **error**.
- **No content-addressed identity**: paths are the sole logical identity;
  hashes verify content but never identify objects.

---

## 6. TAIL block

Optional in v1 (RECOMMENDED). Payload:

| Field            | Size | Encoding                                          |
|------------------|------|---------------------------------------------------|
| total_file_size  | 8    | big-endian u64, size of the entire `.mpak` file    |
| object_count     | 4    | big-endian u32, number of DATA blocks              |
| indx_offset      | 8    | big-endian u64, absolute offset of the INDX block, or `0` if absent |
| package_digest   | 32   | raw SHA-256 over all bytes preceding this TAIL block |

- The digest **covers everything before TAIL**: header, INDX, MANF, all
  DATA blocks. It cannot be circular because TAIL itself is excluded by
  construction. Writers MUST compute the digest after all preceding bytes
  are finalized (writers already hash members before writing MANF in the
  directory flow; the same order works single-pass here).
- **indx_offset** provides ZIP-EOCD-style backward discovery: a reader
  fetching only the file tail can locate INDX (and hence all members)
  without a forward scan. Writers of files with no INDX MUST write
  `indx_offset = 0`.
- **What TAIL provides**:
  - *completeness detection*: `total_file_size` matching actual size
    proves no truncation;
  - *whole-package fixity*: one SHA-256 covering header + index +
    manifest + members; closes the v1 "manifest is not self-hashed" hole
    at the physical layer;
  - *physical package identity*: the digest is a compact package
    fingerprint for dedup/cache/verification decisions;
  - *backward discovery of INDX* (above).
- Absence of TAIL degrades **completeness assurance only** — every member
  still verifies against MANF/INDX hashes. Verify MUST report "TAIL
  missing → completeness unproven" as a **warning**, not an error.
- TAIL header CRC mismatch → TAIL treated as absent (warning); TAIL
  content mismatch (wrong size/count/digest) → **error** at verify time,
  but readers MUST still attempt scan recovery.

---

## 7. Deterministic builds

Writers MUST emit blocks in this fixed order: `INDX (if present), MANF,
DATA…, TAIL (optional)`. DATA order = **manifest canonical traversal
order** of referenced assets: audio objects first in `media[]`/`tracks[]`
array order (which is playback order), then `representations[]`,
`artwork[]`, `booklet[]`, `lyrics[]`, `extras[]`, `analysis[]` in manifest
array order. INDX entries always lexicographically sorted by path. No
timestamps, hostnames, tool-version variance, or other nondeterministic
metadata are embedded (manifest `provenance` is `{tool, toolVersion}`
only, unchanged). Two identical logical packages MUST produce
byte-identical `.mpak` files; this is testable by hashing the file.

---

## 8. Limits and security

Physical restatement of the v1 policy budgets (unchanged semantics,
`musicpack-v1.md §8`):

| Limit                         | Value      |
|-------------------------------|------------|
| manifest size (MANF payload)  | ≤ 16 MiB   |
| referenced assets / objects   | ≤ 4096     |
| member size                   | ≤ 8 GiB    |
| aggregate package size        | ≤ 64 GiB   |
| path length (chars)           | ≤ 4096     |

Wire-format limits (container-level):

- `entry_count` u32 but ≤ 4096 policy; INDX payload bounded by §5.
- block `length` is u64; readers MUST require `length ≤ 2^63−1` and
  reject absurd lengths **before** trusting them (an untrusted length is
  rejected, never wrapped).
- **All arithmetic mixing untrusted lengths/offsets MUST be overflow
  checked.** `member_offset + member_length`, running totals, size sums
  — every computation uses saturating/checked arithmetic; on overflow the
  reader MUST hard-fail. (Discipline mirrors the modernized decoder's
  `mpc_u64_lshift`/`mpc_bit_position_add` guards.)
- Readers with access to actual file size (e.g. range-capable clients,
  local files) MUST cross-check `offset + length ≤ file_size` before
  dereferencing; stream-sequential readers rely on CRC-16 + available
  bytes.

---

## 9. Corruption and recovery

| Condition                        | Required behavior                                          |
|----------------------------------|------------------------------------------------------------|
| bad magic                        | hard reject (not MPAK)                                     |
| unsupported major                | hard reject                                                 |
| unknown minor (known major)      | proceed; read known blocks, skip unknown; issue warning    |
| malformed block header (CRC-16)  | for sequential scan: resync by scanning forward for a valid framed block whose CRC-16 passes; must not trust framing before CRC |
| truncated block (short payload)  | that member damaged; members after it unaffected; package incomplete (TAIL/total_size will catch it if written)   |
| missing INDX                     | readable; scan rebuilds all facts; warning                 |
| corrupt INDX (CRC/structure)     | discard INDX; scan rebuild; warning                         |
| INDX vs manifest disagreement    | error (consistency)                                         |
| missing TAIL                     | completeness unproven; warning                              |
| corrupt TAIL header CRC          | treat as absent; warning                                    |
| corrupt TAIL digest/size match   | error; scan recovery still attempted                        |
| corrupted DATA member bytes      | SHA-256 mismatch localizes to one member; other members unaffected; physical extraction possible for the rest |
| duplicate DATA or INDX path      | error                                                       |
| corrupt MANF bytes / unparseable JSON | semantic verification impossible; physical DATA objects still extractable via scan, and INDX (if intact) still provides hashes; recovery extracts members but cannot verify as a MusicPack package |

The scan-recovery requirement is normative: a conforming recovery tool
MUST reconstruct object paths, offsets, lengths from DATA blocks alone
and recompute SHA-256 over member bytes, producing a rebuilt INDX and an
extractable member set. Manifest corruption degrades the package to
"extractable but not verifiable" — an accepted consequence (see the
conservative resolution in §15/D3 of a single MANF).

---

## 10. HTTP Range access model

Recommended access pattern (illustrative, not an additional normative
layer). The implemented transport — the `musicpack_range_source` seam in
libmusicpack, the native `mpakhttp` adapter, and the browser/WASM
transport — is specified in `specs/mpak-http-range-design.md`:

```text
1. GET  Range: bytes=0-(prefix-1)            e.g. prefix = 256 KiB
   → validate magic/version, parse header,
     read INDX (block follows header immediately),
     and likely MANF too for real albums.
2. (only if MANF not in prefix)
   GET  Range: bytes=(manf_start)-(manf_end) offsets known after INDX header
3. Parse manifest: logical track → member path.
4. Find entry in INDX (binary search over sorted paths)
   → member_offset, member_length.
5. GET  Range: bytes=member_offset-(member_offset+member_length-1)
   → feed member bytes directly to the codec decoder.
6. Decode/verify: member SHA-256 may be checked against MANF/INDX.
```

- Requests to first audio: 2–3 typical, 4 worst-case (INDX not in prefix).
- **MPAK does not implement an audio seek table.** At container level
  there is no intra-member seeking: codec independence is preserved and
  the embedded codec already solves it. For Musepack, in-track seeking
  remains the embedded SV8 stream's `SO`→`ST` machinery consumed through
  the existing `mpc_reader` abstraction over the member byte range
  (`object.h:38-47`, `musicpack_package_track_open_reader`). A seekable
  HTTP-range `mpc_reader` plug-in reuses existing libmusepack behavior
  unchanged; non-seekable readers simply can't seek (SV8's `canseek=false`
  behavior — works, hard-fails on seek attempts).
- **Sequential streaming**: a non-seekable reader MUST be able to process
  the file from byte 0 without INDX: header → (optionally skip or decode
  INDX) → MANF → DATA blocks in playback order → optionally TAIL. The
  DATA preamble path identifies each member as bytes arrive.

---

## 11. Relationship to Musepack SV8

MPAK borrows from SV8 *conceptually*, not mechanically.

Adopted: self-framing typed blocks; magic+version preamble; unknown-block
length-skip as the entire forward-compat mechanism; index-as-derivable
acceleration (SV8's lazily rebuilt seek table ⇒ MPAK's rebuildable INDX);
the seekable/non-seekable (`canseek`) capability split; the modernization
tree's checked-arithmetic discipline.

Explicitly **not** inherited:

- 2-byte A–Z block keys (MPAK uses 4-byte ASCII with a private namespace);
- self-including base-128 varint sizes (`encodeSize`/`mpc_bits_get_size`);
- backpatched/reserved-bit offset fields (SV8's SO is patched with a
  fixed reserved width; MPAK is single-pass writable, no backpatching);
- Golomb-coded delta² seek tables;
- bit-level addressing and bit-granular offsets;
- codec-specific packet structure or per-packet error-accounting logic.

Complete MPC members inside DATA **remain complete untouched SV8 streams**
— `MPCK` magic, `SH`, `RG`, `EI`, `SO`, `AP*`, optional `ST`, `SE` — and
MPAK never inspects their block structure.

---

## 12. Binary examples (illustrative)

Offsets in hex; all multi-byte integers big-endian. Illustrative digests
are marked `‹示例›`.

### 12.1 Minimal header

```hex
0000:  4d 50 41 4b  01 00  00 01  00 00 00 00 00 00 00 00
       "MPAK"     major  minor  flags   reserved (all zero)
                =1    =0    INDX_PRESENT
```

(`flags` shown as `0x0001`: INDX present — the common case.)

### 12.2 One DATA block (path `hi.txt`, content `hello`)

```hex
0000:  44 41 54 41                        "DATA"
0004:  00 00 00 00  00 00 00 0e           length = 14  (2+5+5)
000c:  67 10                              crc16 over bytes 0..11 (BYPASS)
000e:  00 05                              path_length = 5
0010:  68 69 2e 74 78 74                  "hi.txt"
0015:  68 65 6c 6c 6f                     "hello"
```

Block ends at offset `0x000e + 14 = 0x001C` (exclusive). Skip distance
from block start = `14 + length` = 28 bytes.

### 12.3 DATA block containing an MPC member

Path `a.mpc` (5 bytes), member length `m` = 1234 (0x04D2):

```hex
0000:  44 41 54 41                        "DATA"
0004:  00 00 00 00  00 00 04 d9           length = 2+5+1234 = 1241 (0x04D9)
000c:  ‹crc16›                            over bytes 0..11
000e:  00 05                              path_length = 5
0010:  61 2e 6d 70 63                      "a.mpc"
0015:  4d 50 43 4b ...                     member: SV8 stream ("MPCK"…SE)
```

The member's absolute offset = block start + 14 + 2 + 5 = start + 21
(0x15). This is the value an INDX entry stores.

### 12.4 INDX entry layout

For path `a.mpc`, member_offset `0x15`, member_length `0x04D2`:

```hex
        00 05                              path_length = 5
        61 2e 6d 70 63                      "a.mpc"
        00 00 00 00  00 00 00 15           member_offset = 0x15
        00 00 00 00  00 00 04 d2           member_length = 0x04D2
        ‹32 bytes of raw SHA-256›           digest
```

Preceded in the block by a 4-byte `entry_count` (u32). INDX payload
length `= 4 + Σ entry sizes`.

### 12.5 TAIL layout

```hex
        00 00 00 00  00 00 10 24           total_file_size = 0x1024
        00 00 00 03                        object_count = 3
        00 00 00 00  00 00 01 00           indx_offset = 0x0100
        ‹32 bytes: SHA-256 of bytes 0..(tail_start-1)›
```

Note: the digest is over all bytes preceding TAIL and therefore excludes
itself by construction.

### 12.6 INDX block fully framed

```hex
0000:  49 4e 44 58                        "INDX"
0004:  00 00 00 00  00 00 00 53           length = 4 + (5+2+8+8+32) = 83
                                           (1 entry of 79 bytes + count)
000c:  ‹crc16›
000e:  00 00 00 01                        entry_count = 1
0012:  ‹entry per §12.4›
```

---

## 13. Non-goals (v1)

MPAK v1 explicitly does NOT provide:

- container-level audio seeking (in-track seeking lives in the embedded
  codec, e.g. SV8 SO/ST);
- deduplication of identical-content members;
- compression;
- encryption;
- signatures (manifest singing remains a MusicPack-level concern, not a
  container concern);
- codec repacketization;
- content-addressed storage (identity is path-only);
- any semantic metadata channel outside the manifest.

---

## 14. Verification semantics mapping

|Directory-backend concept (`musicpack-v1.md`)| MPAK physical rule |
|---|---|
| `manifest.json` at package root      | `MANF` block, exact same bytes |
| referenced asset `{path, sha256}`    | `DATA` block + exactly one INDX entry |
| unreferenced file → warning          | unreferenced DATA object → warning |
| duplicate path                       | hard error (DATA or INDX)         |
| package budgets (16 MiB/4096/8 GiB/64 GiB) | same values as wire limits (§8) |
| manifest not self-hashed              | TAIL whole-package digest closes this (optional) |
| `musicpack verify` semantics         | same, plus INDX/TAIL consistency rules (§5, §6, §9) |

---

## 15. Resolved open decisions (conservative)

- **D1 — flags assignments**: `flags` is big-endian u16, bit 0 (LSB)
  = `INDX_PRESENT` (1 when an INDX block is present). All other bits
  reserved, MUST be zero; readers MUST tolerate non-zero reserved bits at
  the warning level (same rule as the header reserved field).
- **D2 — private/experimental block namespace**: any block type
  containing any character outside uppercase `A`–`Z` is private/
  experimental and skid-safe (§2.4). Public registrations require a spec
  revision.
- **D3 — duplicate MANF copies**: prohibited. Exactly one MANF block per
  file; MANF redundancy is not permitted in v1. Manifest corruption is
  accepted as the semantic worst case (§9); TAIL detects it; scan
  extracts members.
- **D4 — TAIL mandatory in a future minor**: permitted. A future minor
  version may upgrade TAIL from RECOMMENDED to REQUIRED for writers;
  readers of v1 must continue to treat TAIL as optional in either case.
  This is additive within major 1 and therefore forward-compatible under
  §2.1's minor-version rule.

---

## 16. Relationship to existing specifications

This document introduces one new physical file, reuses the existing
logical manifest/format unchanged, and does not modify
`specs/musicpack-v1.md`, `specs/musicpack-waveform-v1.md`,
`specs/musicpack-sonic-v1.md`, or `specs/musicpack-api-v1.md`. The
`musicpack_package` abstraction seam (`include/musicpack/package.h:36-44`)
— "a future packed MPAK backend plugs in behind this same handle" — is
the integration point; the logical spec's deliberate silence on
offsets/packing is lifted only here.

Tooling note for implementers: the existing CLI writers
(create/import/build-draft) currently emit directory bundles; MPAK
writers MUST preserve their observable order of operations — hash
assets, finalize MANF, verify the staged file, atomically publish — when
producing `.mpak`.
