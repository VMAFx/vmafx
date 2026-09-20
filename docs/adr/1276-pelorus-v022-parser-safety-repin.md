<!-- markdownlint-disable MD013 -->

# ADR-1276: Re-pin the Pelorus mirror for released parser safety fixes

- **Status**: Proposed
- **Date**: 2026-09-20
- **Deciders**: Lusoris
- **Supersedes**: the Pelorus mirror pin and update workflow in
  [ADR-1120](1120-pelorus-abi-minor3-resync-complexity.md); its
  complexity-scoring decision
  remains in force
- **Tags**: interop, abi, vendoring, pelorus, security, ci, fork-local

## Context

[ADR-1113](1113-vendor-pelorus-interop-abi.md) made the Pelorus interop
surface a pinned, read-only mirror. ADR-1120 advanced that mirror to
`818d844066e73326c6300c9827ce7324d04cd884` and ABI 1.3 while adding
`PEL_SEC_COMPLEXITY` scoring. Pelorus v0.2.2, at
`93bef1206d68d9e09024c08a12732fb8e77b9b16`, keeps the wire ABI at 1.3 but
contains a parser safety release: wire headers and directory entries move
through aligned local objects with `memcpy`, and a `header_size` that would
misalign the directory is rejected.

The old VMAFx copy performs typed access through caller-owned byte buffers.
Clang UBSan reproduces undefined behaviour when an otherwise valid blob starts
at a one-byte-skewed address. The shared fixture also gained two cases that pin
the released behaviour. Waiting for a future ABI-minor change would therefore
leave a known parser defect in a supposedly faithful mirror.

The existing drift guard was not strong enough to preserve that fidelity. It
accepted a fixture with VMAFx-only `NOLINT` bands and ignored-return casts, and
local lint exclusions covered path patterns rather than the exact mirror
manifest. The pin policy and the verification boundary must move together.

## Decision

VMAFx will re-pin the complete Pelorus interop mirror to the released v0.2.2
commit `93bef1206d68d9e09024c08a12732fb8e77b9b16` even though the ABI version
does not change.

The maintenance contract is:

1. `PELORUS_VENDOR_SHA` names a reviewed released commit. A released parser
   correctness or security fix triggers a coordinated re-pin, exact fixture
   sync, sanitizer run, and drift check even when the ABI major/minor are
   unchanged.
2. The required VMAFx Pre-Commit workflow checks out that exact Git object and
   runs the default drift guard. A non-Git source directory or a checkout
   missing the pinned object fails closed.
3. The shared fixture body remains exact Pelorus source apart from the
   documented include rewrite. Its VMAFx prefix is rendered canonically from
   the pin and ABI version, and the complete file is compared through EOF.
4. VMAFx lint and format policy lives outside the mirror. Exclusions are
   limited to manifest-owned paths, and the guard rejects any extra or missing
   tracked file in those exempt namespaces.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Re-pin the complete released mirror and tighten its guard | Removes the reproduced UB; preserves one parser and fixture; makes future fixes deterministic | Requires coordinated source, fixture, CI, and lint-policy updates | Chosen |
| Wait for the next ABI-minor bump | Fewer mirror updates | Leaves a released parser safety defect in VMAFx even though the wire version is unchanged | Rejected: release safety is not an ABI-version concern |
| Copy only `interop.c` and the two new fixture cases | Smallest source delta | Creates a partial release mirror and weakens the exact-source proof | Rejected: contradicts ADR-1113's complete read-only mirror |
| Keep VMAFx-only lint edits in the fixture | Satisfies local tools in place | Makes the shared conformance fixture cease to be shared source | Rejected: policy belongs in manifest-scoped tooling |

## Consequences

- **Positive**: valid misaligned-base blobs no longer invoke undefined
  behaviour; all sixteen released vectors run against exact Pelorus source;
  drift and lint exemptions fail closed on unowned paths.
- **Negative**: a parser correctness/security release now causes a coordinated
  mirror update even when the ABI number is unchanged.
- **Neutral / follow-ups**: the wire ABI remains 1.3. ADR-1120's
  `PEL_SEC_COMPLEXITY` scoring behaviour is unchanged. Future wire-format or
  vendoring-architecture changes still require their own ADR.

## References

- [ADR-1113](1113-vendor-pelorus-interop-abi.md) — original pinned,
  read-only mirror decision.
- [ADR-1120](1120-pelorus-abi-minor3-resync-complexity.md) — previous pin and unchanged
  complexity-scoring decision.
- [Research-2072](../research/2072-pelorus-interop-v022-sync-2026-09-20.md) —
  UBSan reproduction, exact-source proof, and verification evidence.
- Pelorus v0.2.2 source:
  `VMAFx/pelorus@93bef1206d68d9e09024c08a12732fb8e77b9b16`.
- Source: `req` — update VMAFx for Pelorus releases and keep the interop mirror
  exact rather than carrying fork-local parser changes.
