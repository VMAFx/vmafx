<!-- markdownlint-disable MD013 -->
# Pelorus interop ABI (vendored) — `libvmaf/pelorus/`

The [Pelorus](https://github.com/VMAFx/pelorus) GPU pre-encode pipeline and
vmafx share a small, versioned **data-plane interop ABI**: a self-describing
per-frame metadata blob that Pelorus filters (`vf_pelorus_*`) write and vmafx
reads for perceptually-weighted scoring. The blob rides each `AVFrame` as an
`AV_FRAME_DATA_SEI_UNREGISTERED` payload, prefixed by a fixed project UUID so it
round-trips a filtergraph and never collides with codec side data.

This ABI is **single-sourced in Pelorus** (Pelorus ADR-0103). vmafx carries a
**verbatim, pinned, read-only mirror** of it so the two repos can build, test,
and evolve independently without a submodule or a shared package. The vendoring
decision and its guard rails are recorded in
[ADR-1113](../adr/1113-vendor-pelorus-interop-abi.md).

> **The mirror is read-only.** Do not edit the vendored files. They are
> byte-identical to their Pelorus origin (pinned at `VMAFx/pelorus@835e097`)
> except for a `VENDORED FROM … DO NOT EDIT` banner and the include-path
> rewrite described below. Fix any defect upstream in Pelorus, then re-sync.

## What is vendored

| Vendored path | Pelorus origin (`libpelorus/`) | Role |
| --- | --- | --- |
| [`core/include/libvmaf/pelorus/pelorus.h`](../../core/include/libvmaf/pelorus/pelorus.h) | `include/pelorus/pelorus.h` | Umbrella header: version + `pel_result` codes. |
| [`core/include/libvmaf/pelorus/interop.h`](../../core/include/libvmaf/pelorus/interop.h) | `include/pelorus/interop.h` | The blob ABI: header, section catalogue, pack/parse API, `_Static_assert` size locks. |
| [`core/include/libvmaf/pelorus/deband.h`](../../core/include/libvmaf/pelorus/deband.h) | `include/pelorus/deband.h` | The deband filter parameter contract (control plane). |
| [`core/src/interop/pelorus_interop.c`](../../core/src/interop/pelorus_interop.c) | `src/interop.c` | `pel_blob_pack` / `pel_blob_find_section` / `pel_blob_is_present` / `pel_blob_free`. |
| [`core/src/interop/pelorus_deband_params.c`](../../core/src/interop/pelorus_deband_params.c) | `src/deband_params.c` | `pel_deband_params_default` / `pel_deband_params_validate`. |
| [`core/src/interop/pelorus_version.c`](../../core/src/interop/pelorus_version.c) | `src/version.c` | `pelorus_version*` / `pel_result_str`. |
| [`core/test/test_pelorus_interop.c`](../../core/test/test_pelorus_interop.c) | `test/interop_test.c` | The **shared conformance fixture** (see below). |

The three `.c` files compile straight into `libvmaf` (CPU-only, dependency-free,
no Vulkan; registered in `core/src/meson.build`). `deband_params.c` and
`version.c` are vendored alongside `interop.c` because the shared conformance
fixture links the `deband` parameter contract and the version/result-string
accessors — vendoring them keeps the fixture byte-identical to Pelorus's.

### The only local edits

1. A `VENDORED FROM VMAFx/pelorus@835e097 — DO NOT EDIT` banner inserted after
   the (unchanged) Pelorus license header.
2. Intra-Pelorus `#include "pelorus/<x>.h"` rewritten to
   `#include "libvmaf/pelorus/<x>.h"` so the headers resolve under
   `core/include/`. Nothing else changes.

The conformance test additionally carries a vmafx-authored
(`Copyright 2026 Lusoris`) header instead of cloning Pelorus's, and is run
through the fork's `clang-format`; its body is otherwise the same seven checks.

## The blob, in brief

A blob is a flat, pointer-free, little-endian byte image:

```text
[16-byte UUID][PelorusSideData header (48 B)][PelorusSectionDir dir[count] (16 B each)][section payloads, each 8-byte aligned]
```

- **Magic / identity**: 8-byte `"PELOR1\0\0"` magic, a fixed 16-byte routing
  UUID (`e1d7c4a2-6b93-4f08-9a55-0f3c2db17e64`), and an ABI major/minor.
- **Sections** (append-only bits, never reused): `BANDING`, `VARIANCE`,
  `DENOISE`, `FILMGRAIN`, `MOTION`. Each is independently optional and gated by
  `section_mask`.
- **Directory**: one `PelorusSectionDir{section_id, offset, size, struct_minor}`
  per present section, so an older consumer can locate and tail-skip a newer
  producer's larger section.

The full struct layout lives in `interop.h`; the `_Static_assert` size locks
(`sizeof(PelorusSideData)==48`, banding `==24`, variance `==28`, denoise `==28`,
filmgrain `==216`, motion `==32`, dir `==16`) are compiled in every translation
unit that includes the header — including the conformance test — so any
accidental layout change is a **build failure**, not a silent corruption.

### Pack / parse API (`interop.h`)

| Function | Purpose |
| --- | --- |
| `pel_blob_pack(meta, sections, nb, &blob, &len)` | Allocate a UUID-prefixed blob from header fields + a set of sections. |
| `pel_blob_free(blob)` | Free a blob returned by `pel_blob_pack`. |
| `pel_blob_is_present(blob, len)` | Cheap pre-check: is this a valid Pelorus blob (UUID + magic + ABI major)? |
| `pel_blob_find_section(blob, len, sec, known_size, &ptr, &size)` | Locate a section; returns a pointer into the blob plus `min(producer, consumer)` readable bytes. |

vmafx is a **reader only** — it never mutates a blob (single-writer invariant:
Pelorus is the sole writer). The reader side (perceptual weighting) is a
separate workstream and is not part of this vendor PR.

## Forward / backward compatibility (R1–R6)

The ABI is normative; both repos depend on these rules (verbatim from
`interop.h`):

- **R1 — Append-only.** New fields are added at the END of a section struct, or
  as a NEW section with a new bit. Never reorder, resize, or repurpose a field.
- **R2 — Never remove** a section bit or a field. Deprecate by documentation;
  the slot stays reserved forever.
- **R3 — Every section is independently optional**, gated by `section_mask`. A
  consumer MUST ignore bits it does not understand (forward-compat) and MUST
  tolerate absence of bits it does understand (back-compat).
- **R4 — Explicit offsets/sizes** (`PelorusSectionDir`) let an older consumer
  parse a newer producer's larger section: read `min(known_size, dir.size)`
  bytes and ignore the tail.
- **R5 — Flat, pointer-free, little-endian** byte image. A byte-swap path is a
  future, additive change gated on `PELORUS_ABI_MINOR`.
- **R6 — `PELORUS_ABI_MAJOR` bumps only on a breaking change** (which R1/R2
  forbid for additive evolution) — in practice it never bumps.
  `PELORUS_ABI_MINOR` bumps when a new section bit or appended field lands.

The current ABI is **1.0** (`PELORUS_ABI_MAJOR=1`, `PELORUS_ABI_MINOR=0`).

## Conformance fixture — the byte-compat proof

`core/test/test_pelorus_interop.c` is the **same fixture both repos run**. It
exercises seven vectors against the vendored parser:

1. `roundtrip` — pack three sections, parse them back, verify scalars + the
   8-byte-aligned 64-bit `seed` survives.
2. `forward_compat` — an older consumer that knows a *smaller* struct gets
   `min(producer, consumer)` readable bytes (R4).
3. `abi_major_mismatch` — a bumped major is detected and rejected, not misread
   (R6).
4. `foreign_buffer` — a non-Pelorus SEI (e.g. an x264 user-data blob) is
   cleanly ignored.
5. `header_only` — a zero-section blob is valid and parses.
6. `truncation` — a short buffer is detected, never read out of bounds.
7. `deband_params` — the deband contract: defaults validate, out-of-range is
   rejected with the offending field name.

A green run here proves vmafx's vendored parser is byte-identical to Pelorus's
writer: a blob packed by Pelorus parses in vmafx and vice versa. The fixture is
wired into the `fast` suite:

```bash
meson setup core/build-cpu core -Denable_cuda=false -Denable_sycl=false
ninja -C core/build-cpu
meson test -C core/build-cpu test_pelorus_interop   # 7 vectors, must be 7/7
```

## Re-syncing the mirror

The pin and the drift guard live in
[`scripts/sync-pelorus-interop.sh`](../../scripts/sync-pelorus-interop.sh). It
reads the vendored sources from the **pinned commit's git tree object**
(`git show 835e097:libpelorus/…`), so it stays accurate even when the local
Pelorus checkout's `HEAD` has moved past the pin.

```bash
# Drift check (CI gate). Path defaults to $PELORUS_DIR or ../pelorus.
scripts/sync-pelorus-interop.sh /path/to/pelorus
#   OK   — mirror matches pelorus@835e097
#   FAIL — mirror has drifted (prints a diff), exit 1

# Re-vendor after a DELIBERATE pelorus ABI-minor bump:
#   1. bump PELORUS_VENDOR_SHA in the script + every banner + this doc + ADR
#   2. re-vendor and confirm clean:
scripts/sync-pelorus-interop.sh --update /path/to/pelorus
scripts/sync-pelorus-interop.sh /path/to/pelorus
#   3. rebuild + run the conformance fixture (must stay 7/7).
```

A re-sync that changes the ABI is an ADR-worthy event (a new section bit or an
appended field bumps `PELORUS_ABI_MINOR`); record it as a follow-up to
[ADR-1113](../adr/1113-vendor-pelorus-interop-abi.md).
