# Vendored Pelorus interop ABI + sync guard + conformance gate (ADR-1113)

Vendored the Pelorus <-> vmafx data-plane interop ABI into vmafx as a pinned,
read-only, append-only mirror of `VMAFx/pelorus@835e097`. The ABI is a small,
CPU-only, dependency-free C surface (a self-describing per-frame side-data blob
plus its pack/parse pair); it stays single-sourced in Pelorus (Pelorus
ADR-0103) and vmafx mirrors it byte-for-byte so the two repos build and test
independently with no submodule and no Vulkan coupling.

- **Vendored files** (byte-identical to Pelorus except a `VENDORED FROM … DO NOT
  EDIT` banner + a `pelorus/<x>.h` → `libvmaf/pelorus/<x>.h` include rewrite):
  - `core/include/libvmaf/pelorus/{pelorus,interop,deband}.h`
  - `core/src/interop/pelorus_{interop,deband_params,version}.c` (compiled into
    `libvmaf`)
- **Sync guard** `scripts/sync-pelorus-interop.sh` — pins
  `PELORUS_VENDOR_SHA=835e097`, reads the pinned commit's git tree object,
  diffs the mirror, and exits non-zero on any drift (`--update` re-vendors after
  a deliberate ABI-minor bump). Records the synced `PELORUS_ABI_MINOR` (0).
- **Shared conformance fixture** `core/test/test_pelorus_interop.c` — the same
  seven vectors Pelorus runs (roundtrip, forward-compat, abi-major-mismatch,
  foreign-buffer, header-only, truncation, deband-params), wired into the `fast`
  suite. A green run proves vmafx's vendored parser is byte-compatible with
  Pelorus's writer. The `_Static_assert` size locks in `interop.h` are enforced
  at compile time in every including translation unit.

Docs: `docs/api/pelorus-interop.md`. The reader side (perceptual weighting) and
the autotune control plane are separate, later workstreams.
