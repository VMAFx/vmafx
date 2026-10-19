<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2817: Re-pin the Pelorus mirror to v0.3.0, its EUPL-1.2 release, and check the licence on every sync

- **Status**: Accepted
- **Date**: 2026-10-09
- **Deciders**: lusoris
- **Tags**: interop, abi, vendoring, pelorus, license, fork-local

## Context

The Pelorus interop mirror ([ADR-1113](1113-vendor-pelorus-interop-abi.md),
[ADR-1276](1276-pelorus-v022-parser-safety-repin.md)) was pinned at
`11e183ec0aed`. Its ten files carried the BSD+Patent prose header of that
commit. Because the mirror is byte-locked, VMAFx described their licence
outside the files: a `REUSE.toml` annotation labelled `core/src/interop/pelorus*`
`BSD-2-Clause-Patent`, `docs/credits.yaml` listed the same identifier, and
`.config/lint-exceptions.d/spdx.toml` held one exception per mirrored file
(expiry 2026-12-31) for the missing `SPDX-License-Identifier` line.

Pelorus then moved to the EUPL-1.2 with REUSE metadata (Pelorus ADR-0171,
VMAFx/pelorus #101, `10e032a22ab6`) and released v0.3.0 at
`e2e4040311a443210927549c3a336f909c6473f3` (2026-10-08), which is also the head
of its `master`. Between the two pins the mirrored files change as follows:

| File | Change | Pelorus source |
| --- | --- | --- |
| all ten | the 17-line prose header becomes a 6-line header whose `SPDX-License-Identifier` line names `EUPL-1.2` | #101 |
| `interop.h` | comments only: units of `PelorusMotionSection`, coded-value ranges of `uv_mult`, `uv_mult_luma`, `uv_offset` | `5812929`, `e9ae50c` |
| `pelorus.h` | `PELORUS_VERSION_*` 0.2.2 -> 0.3.0 | #243 |
| `test/interop_test.c` | the fixture checks owner-only privacy on the open descriptor or handle and writes in a bounded loop | #65 |

`PELORUS_ABI_MAJOR` and `PELORUS_ABI_MINOR` stay 1 and 3, and `interop.c` is
unchanged apart from its header. The sync script could not render the new
files: its renderer cut a fixed 17-line header.

The maintainer decided (Q-311) that the Pelorus re-pin follows the praetor pin
move of [ADR-2784](2784-praetor-pin-3a766f2d.md), so the re-pin must pass that
engine's REUSE gates.

## Decision

We pin the mirror to the v0.3.0 tag's commit `e2e4040311a4`. The sync script
finds the licence header as the leading comment block and requires it to carry
exactly one `SPDX-License-Identifier` line naming `PELORUS_MIRROR_LICENSE`
(`EUPL-1.2`); the same holds for the Pelorus fixture, whose VMAFx prefix now
carries that line too. Any other identifier, or none, stops both modes of the
script. With the line in every mirrored file, the `REUSE.toml` annotation and
the ten SPDX exceptions are removed, and the credits entry names `EUPL-1.2` and
all three mirror locations.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Pin the v0.3.0 tag's commit and check the licence on every sync | Release pin as ADR-1276 requires; every licence record agrees with the source; the exceptions go before they expire | A Pelorus licence change needs a reviewed VMAFx change before any re-pin | Chosen |
| Pin the relicence commit `10e032a22ab6` alone | Smallest content change | Not a release; leaves out the fixture fix of Pelorus #65 that v0.3.0 carries | ADR-1276 pins released commits |
| Keep `11e183ec0aed` and relabel `REUSE.toml` and the credits | No mirror change | The mirrored headers keep a licence statement Pelorus no longer makes; the ten exceptions stay and expire on 2026-12-31 | Records would disagree with the files |
| Accept any SPDX identifier in the sync script | Fewer moving parts | A later Pelorus licence change would reach VMAFx through `--update` while `REUSE.toml` and the credits still describe the old one | Rejected: the check is what keeps the records in step |
| Relabel the `REUSE.toml` annotation to `EUPL-1.2` instead of removing it | Keeps an explicit record | Repeats the root annotation and the per-file line; one more record to keep in sync | Rejected as redundant |

## Consequences

- **Positive**: `reuse lint` resolves every mirrored file to EUPL-1.2 from its
  own header; `check-copyright` reads the mirror like any other file; the
  credits page states the actual licence.
- **Negative**: none in code. The fixture's new descriptor checks run on every
  host where the fast suite runs.
- **Neutral / follow-ups**: the wire ABI stays 1.3, so no VMAFx consumer
  changes. Pelorus #222 still asks for `SPDX-FileCopyrightText` lines; when they
  land the renderer needs no change, since it keeps the whole header. ABI 1.4
  (Pelorus #218, #86) will be its own re-pin.

## References

- Q-311: Pelorus follows the praetor pin move, which landed as #2644 (orchestrator ledger, 2026-10-08).
- req (paraphrased): re-vendor Pelorus at the newest release that contains the relicence; pass the new engine's gates; retire the ten per-file SPDX exceptions once the SPDX lines are in; never edit the mirror (coordinator brief, 2026-10-08).
- VMAFx/pelorus #101 (relicence), #222 (SPDX lines for the mirror), #223 (mirror contract with release-tag pins), #65 (fixture privacy check).
- [ADR-1113](1113-vendor-pelorus-interop-abi.md), [ADR-1276](1276-pelorus-v022-parser-safety-repin.md), [ADR-1250](1250-eupl-fork-relicense.md), [ADR-2784](2784-praetor-pin-3a766f2d.md).
