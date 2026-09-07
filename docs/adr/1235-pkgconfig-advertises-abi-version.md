<!-- markdownlint-disable MD013 MD060 -->
# ADR-1235: `libvmaf.pc` advertises the ABI version, not the product version

- **Status**: Accepted
- **Date**: 2026-09-07
- **Deciders**: lusoris
- **Tags**: `release`, `build`, `ffmpeg`, `abi`, `packaging`

## Context

[ADR-1151](1151-vmafx-first-release-1-0-0.md) split the fork's two version
numbers: release-please owns the *product* version (the `vX.Y.Z` tag, the
Python distributions, the Helm `appVersion`), while `vmaf_soname_version =
'3.0.0'` is hand-maintained and ships `libvmaf.so.3`. It assigned
`core/meson.build`'s `project(version:)` — and therefore `libvmaf.pc`'s
`Version:` field — to the product side, and recorded the consequence as
cosmetic:

> The one visible consequence is that `libvmaf.pc` moves from an advertised
> 3.2.1 to 1.0.0; since no release ever shipped that 3.2.1, no consumer can be
> pinned to it.

That reasoning is incorrect, and the first release candidate proved it. The
risk is not a consumer *pinned to 3.2.1*. It is that consumers gate on a
**lower bound** they compiled long before this fork existed:

- Unpatched upstream FFmpeg's `configure` contains
  `require_pkg_config libvmaf "libvmaf >= 2.0.0" libvmaf.h vmaf_init`.
  This is what every distribution's FFmpeg build runs.
- The fork's own `ffmpeg-patches/` additionally require `libvmaf >= 3.0.0`
  for the SYCL, Vulkan and DNN entry points
  (`0004-libvmaf-wire-vulkan-backend-selector.patch`,
  `0005-libvmaf-add-libvmaf-sycl-filter.patch`).

A `Version:` of `1.0.0-rc.1` satisfies neither. On the `1.0.0-rc.1` release PR
(#1213) the three FFmpeg lanes and `Docker Image Build` failed with:

```text
Package 'libvmaf' has version '1.0.0-rc.1', required version is '>= 2.0.0'
ERROR: libvmaf >= 2.0.0 not found using pkg-config
```

The library itself built and installed correctly — `libvmaf.so.3.0.0` is in the
install log. Only the advertised number was wrong. The blast radius is not CI:
**any** downstream FFmpeg, unpatched and unmodified, would refuse to link the
fork the moment it advertised a 1.x version.

The two numbers exist precisely because the fork wants to stay a drop-in
libvmaf while renumbering its own product line. Handing the product number to
the field consumers use for interface gating discards that.

### Not the 2026-05-27 symptom

`docs/research/0730-ffmpeg-libvmaf-smoke-20260527.md` records the *same message*
— `libvmaf >= 2.0.0 not found` — from a different cause: a system `libvmaf.so`
built against oneAPI 2025.0 failed configure's **link** test on a host that had
moved to 2026.0, so pkg-config's own check never ran. The two are told apart by
the line above the error. A link failure prints nothing about versions; this one
prints pkg-config's version comparison verbatim:

```text
Package 'libvmaf' has version '1.0.0-rc.1', required version is '>= 2.0.0'
```

Do not treat a future `libvmaf >= 2.0.0 not found` as settled by this ADR without
checking which of the two lines precedes it.

### Sweep for the rest of the class

The same product-version renumber was checked against every other version-coupled
surface in the tree, since a 3.2.1 -> 1.0.0 move breaks anything holding a lower
bound or comparing spellings:

| Surface | Verdict |
| --- | --- |
| `python/test/setup_metadata_test.py` | **Broke** — PEP 440 vs SemVer string compare; fixed separately (`T-RELEASE-RC-PEP440-VERSION-MISMATCH-2026-09-07`) |
| `libvmaf.pc` `Version:` | **Broke** — this ADR |
| `ai/`, `dev-llm/`, `mcp-server/vmaf-mcp/` (hatchling) | Safe — hatchling accepts `1.0.0-rc.1` and normalises to `1.0.0rc1` in the wheel; no test asserts the raw spelling |
| `scripts/release/verify-release-version.sh` | Safe — tag and marker regexes both already accept `-rc.N` |
| `vmaf_version()` / `vcs_version.h` | Safe — descriptive `git describe` output, never compared |
| MCP `_get_version` banner parse | Safe — regex extraction, no comparison |
| Rust crates (`0.1.0`), Helm `version:`, `ARG VMAFX_VERSION` | Safe — deliberately uncoordinated, documented as such |

## Decision

`libvmaf.pc`'s `Version:` is the **library interface version** and is generated
from `vmaf_soname_version`, not from `meson.project_version()`.

`core/meson.build`'s `project(version:)` stays on the release-please product
line and continues to drive the tag, the Python distributions and the Helm
`appVersion`. It no longer reaches `libvmaf.pc`.

`vmaf_soname_version` is bumped when the C API changes, as it already was —
this ADR adds "and this is what pkg-config advertises" to its existing
contract. ADR-1151 is otherwise unchanged; this amends only its pkg-config
clause.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Generate `Version:` from `vmaf_soname_version` (chosen)** | Restores the drop-in property; `>= 2.0.0` and `>= 3.0.0` both pass; nothing downstream has to change; the field means what pkg-config consumers already assume it means | The `.pc` version no longer moves on a product release, so it cannot be used to identify a fork build | **Chosen.** The field's consumers gate on interface capability, and that is exactly what it now reports. Fork build identity belongs in the tag and the image label, which still carry it. |
| Patch the fork's `ffmpeg-patches/` down to `libvmaf >= 1.0.0` | Fixes our own CI lanes | Does nothing for unpatched upstream FFmpeg, which is what distributions ship — the fork would stop being linkable by any FFmpeg it did not patch itself | Rejected: fixes the symptom on the one consumer we control and breaks every consumer we do not |
| Keep the product version on the 3.x line (abandon 1.0.0) | No mechanism change | Reverses the deliberate ADR-1127 / ADR-1151 decision that the fork's first release is a real 1.0.0 on its own number line | Rejected: the product-numbering decision is sound; only its coupling to `libvmaf.pc` was wrong |
| Add an epoch (`1:1.0.0`) | pkg-config has no epoch concept | Not expressible | Not expressible |
| Advertise `max(product, 3.0.0)` | Both gates pass and the number still moves | An expression whose value silently stops tracking the product version at 3.x is harder to reason about than a field that never claimed to | Rejected: complexity with no gain over naming the ABI version directly |

## Consequences

- **Positive**: the fork stays a drop-in `libvmaf` for unpatched upstream
  FFmpeg and for its own patch stack. The release-blocking failure on the three
  FFmpeg lanes and `Docker Image Build` is removed without touching the 1.0.0
  product decision. `Version:` now carries the meaning its consumers already
  ascribe to it.
- **Negative**: `pkg-config --modversion libvmaf` no longer identifies which
  fork release is installed; it identifies which C API is installed.
  `docs/development/rust.md` says the command "should print the library
  version", which is now literally true and no longer the product version.
  Anyone wanting the product version reads the tag, the container label, or
  `vmaf --version`.
- **Neutral / follow-ups**: `vmaf_soname_version` gains a second consumer, so
  the existing "hand-bump only on an ABI break" rule now also governs what
  downstream range checks see. A future fork-only C API addition should bump
  its minor (`3.1.0`) so patches can gate on it, which the SONAME's major
  keeps ABI-safe.

## References

- [ADR-1151](1151-vmafx-first-release-1-0-0.md) — the product/SONAME split this amends.
- [ADR-1127](1127-single-semver-release-stream.md) — the single SemVer release stream.
- `ffmpeg-patches/0004-libvmaf-wire-vulkan-backend-selector.patch`,
  `ffmpeg-patches/0005-libvmaf-add-libvmaf-sycl-filter.patch` — the `>= 3.0.0` gates.
- PR #1213 run 34152028125 — `Package 'libvmaf' has version '1.0.0-rc.1', required version is '>= 2.0.0'`.
