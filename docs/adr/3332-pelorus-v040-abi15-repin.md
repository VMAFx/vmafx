<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3332: Re-pin the Pelorus mirror to v0.4.0 (interop ABI 1.5)

- **Status**: Accepted
- **Date**: 2026-10-15
- **Deciders**: lusoris
- **Tags**: interop, abi, vendoring, pelorus, fork-local

## Context

The Pelorus interop mirror ([ADR-1113](1113-vendor-pelorus-interop-abi.md),
[ADR-1276](1276-pelorus-v022-parser-safety-repin.md)) was pinned at the v0.3.0
tag, `e2e4040311a443210927549c3a336f909c6473f3`, with interop ABI 1.3
([ADR-2817](2817-pelorus-v030-eupl-repin.md)). Pelorus released v0.4.0 at
`1ad9a32c2f0e057c1604a7a0d63c13cb84cc538b`, which carries interop ABI 1.5.
Issue #2667 tracks the re-pin, and Pelorus closes its epic #93 once VMAFx
consumes the release.

Between the two pins the wire ABI grows append-only:

| ABI | Addition |
| --- | --- |
| 1.4 | `PEL_SEC_ENC_TELEMETRY` (104 B), `PEL_SEC_ENCODE_RECORD` (48 B), `PelorusMotionSection.block_size_log2` (32 -> 36 B), `PEL_ERR_MISMATCH`, `pel_blob_pack_into`, `pel_blob_map`, `pel_encode_record_digest_text` |
| 1.5 | zero-free carrier form: `pelorus_carrier_uuid`, `PEL_CARRIER_MAX_LEN`, `pel_blob_carrier_encode`, `pel_blob_unwrap` |

The only VMAFx consumer, `core/src/feature/perceptual_weight.c`, reads the
`BANDING`, `VARIANCE` and `COMPLEXITY` sections; none of them changes. A
consumer locates a section through the directory, and
`pel_blob_find_section()` returns `min(producer, consumer)` readable bytes, so
the larger `MOTION` section does not affect older readers.

## Decision

We pin the mirror to the v0.4.0 tag's commit `1ad9a32c2f0e` with
`scripts/sync-pelorus-interop.sh --update` and change nothing else in the
mirror. The conformance fixture `core/test/test_pelorus_interop.c` comes with
the release and covers the ABI 1.4 and 1.5 additions. The licence check of
ADR-2817 stays as it is: every pinned source still declares `EUPL-1.2`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Pin the v0.4.0 tag's commit | Release pin as ADR-1276 requires; the shared fixture covers ABI 1.5 | 1600 lines of vendored change to review | Chosen |
| Stay on v0.3.0 (ABI 1.3) | No change | Producers on ABI 1.5 can send carrier-form SEI that VMAFx cannot unwrap; Pelorus epic #93 stays open | The mirror would fall behind the producer |
| Pin an ABI 1.4 commit only | Smaller step | Not a release; ADR-1276 pins released commits | Rejected |

## Consequences

- **Positive**: VMAFx can parse every section a Pelorus 0.4.0 producer writes,
  including the carrier form that zero-intolerant SEI sinks need.
- **Negative**: none in code; the perceptual weight reads the same sections.
- **Neutral / follow-ups**: VMAFx does not yet read `ENC_TELEMETRY`,
  `ENCODE_RECORD` or the carrier form; using them is separate work.

## References

- Ledger id `vmafx-2667-repin-20261015` (paraphrased): re-pin the Pelorus mirror to v0.4.0 final with interop tests green.
- Issue #2667; VMAFx/pelorus epic #93.
- [ADR-1113](1113-vendor-pelorus-interop-abi.md), [ADR-1276](1276-pelorus-v022-parser-safety-repin.md), [ADR-2817](2817-pelorus-v030-eupl-repin.md).
