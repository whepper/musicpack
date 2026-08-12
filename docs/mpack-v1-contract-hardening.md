# MusicPack v1 contract hardening

The v1 contract-hardening series is complete at `138afaa`:

- `cc78e12` `mpack: harden v1 contract validation`
- `311b935` `ci: restore cross-platform package validation`
- `f865089` `ci: cover mpack contract across platforms`
- `f867877` `tests: make mpack conformance output-agnostic`
- `138afaa` `tests: keep conformance paths portable`

## Hosted evidence

[CI run 31626271783](https://github.com/whepper/musicpack/actions/runs/31626271783)
passed on 2026-08-12:

- Linux GCC and Clang: native SIMD gates, package/server core, Unix package,
  scanner, fuzz, Author, and live-reference `compat` plus `enc_compat`.
- macOS ARM64: the same native package and live-reference compatibility gates.
- Windows MSVC: native SIMD, `mpack`, `mpack_conformance`, server core, and
  live-reference `compat` plus `enc_compat`.
- Linux SIMD-off: package, conformance, Author, fuzz, server core, and forced
  SIMD rejection.
- Wasm/Node: smoke and scalar-versus-explicit-SIMD decoder tests.

`mpack_conformance` is now a required cross-platform package gate. Its
generated corpus contains 3 valid manifests, 42 invalid manifests, and 8
invalid asset cases. The runner requires every valid case to pass `info` and
`verify`, every invalid manifest to fail both, and every invalid asset case to
fail `verify`.

## Enforced v1 rules

The parser and package tests exercise nested-object rejection, strict one-value
JSON parsing, recursive duplicate-key rejection, finite and range-safe numeric
parsing, mandatory SHA-256 values for every referenced asset, duplicate path
rejection, and documented package-root-only extension preservation.

Package, integration, and server tests exercise traversal rejection,
existing-ancestor symlink containment on POSIX, transactional `create` and
`import`, checksum-failed package invisibility, explicit verification bypassing
the unchanged-manifest fast path, scanner coverage of `analysis[]`, and
unbounded identity-key construction. Windows CI compiles and runs the same
package/server core checks; Windows reparse-point containment is implemented
through final-path canonicalization but has no dedicated hosted junction test.

Reference packages and normal Author-generated packages are covered by
`mpack`, `mpack_integration`, `author_backend`, and `author_encode` on hosted
Unix jobs. `enc_compat` uses a same-toolchain, pristine live reference encoder
on Linux, macOS, and Windows. The committed manifest-only `compat` fallback is
intentionally not used as evidence because it is optimization/toolchain
specific.

## Remaining limitations

- Hosted CI has no ASan/UBSan job. Local ASan/UBSan coverage passed for the
  package, conformance, Sonic, server, integration, and fuzz suites (8/8).
- The conformance corpus retains Unicode metadata coverage, but uses ASCII
  filesystem paths because the current Windows directory-storage layer uses
  narrow path APIs. Unicode path support is not claimed by this evidence.
