<!-- markdownlint-disable MD013 MD060 -->
# ADR-1897: VMAFx ABI numbering before 1.0: additions take a patch bump and join the current minor's version node

- **Status**: Accepted
- **Date**: 2026-10-05
- **Deciders**: maintainer
- **Tags**: api, abi, rc4, codegen, release

## Context

[ADR-1852](1852-vmafx-api-redesign.md) gives every `vmafx_` function a linker
version node named after the ABI minor that introduced it (`since` in
`core/api/vmafx.toml`, node `VMAFX_<major>.<minor>`) and freezes the ABI at
`1.0.0` with the `v1.0.0` tag (decision D2); before that the ABI is a 0.x
preview that may still change. ADR-1852 does not say how the 0.x number moves
when the definition only grows. The RC4 work packages add functions to the
definition in parallel (the core API, devices and frames, windows,
provenance, bindings), and the append-only check
(`scripts/codegen/vmafx-api.py --abi-check`, Meson test
`test_vmafx_api_abi_append_only`) compares each branch with its merge base, so
the rule decides how often parallel branches collide on `[api] abi_version`
and on version nodes.

## Decision

Within 0.x an addition (a function, struct, field, constant, flag bit,
callback, handle or option) raises the patch number of `abi_version` and may
join the current minor's version node: its `since` is the current minor or a
newer one, never an older one. Only a break (a removed, renamed, reordered,
retyped or renumbered entry, a changed signature or `since`) raises the minor,
and `--abi-check` lists every accepted break so the pull request names it.
From `1.0.0` a version node that shipped is frozen: an addition's `since` is a
newer minor than the ABI it is compared with, and a break needs a higher
major. `scripts/codegen/vmafx_api/abi_check.py` enforces this; the generator
tests in `scripts/codegen/tests/test_vmafx_api_generator.py` plant each case.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Patch bump, current minor's node (chosen) | Parallel work packages add functions without each claiming a minor; a collision on `abi_version` is a one-line patch-number conflict; version nodes stay few until the freeze; matches D2 (the preview ABI may change until `1.0.0`) | A program built against a newer 0.x patch that uses a new function finds the same node in an older 0.x library: it fails with a symbol-lookup error (at load with immediate binding, at the first call with lazy binding) instead of the loader's check naming a missing version node | Chosen: the preview ABI is not a compatibility promise, and the strict rule applies from `1.0.0` |
| Every addition takes a new minor and its own node | Each 0.x node is frozen once it exists, so an older library is refused at load by its missing node; strict semver for additions | Every work package that adds a function claims a minor; parallel branches collide on the minor and on the node, and each rebase onto a branch that landed first renumbers `abi_version` and every new entry's `since`; one short-lived node per landed addition before the freeze | Not chosen: churn on every landing for a guarantee the 0.x preview does not need |

## Consequences

- **Positive**: RC4 work packages add API entries in parallel with a patch
  bump; `--abi-check` against the merge base stays the only gate.
- **Negative**: between two 0.x patch releases a missing function is reported
  as a symbol-lookup error, not by the dynamic loader's version-node check.
- **Neutral / follow-ups**: from `1.0.0` the frozen-node rule applies; whether
  the 0.x nodes are kept or renamed at the freeze (a break the major bump would
  allow) is decided with the freeze. [API generation](../development/api-generation.md)
  documents the rule.

## References

- [ADR-1852](1852-vmafx-api-redesign.md) decision D2 (ABI 1.0.0 frozen at the `v1.0.0` tag).
- PR #2187 (RC4 work package 1) implements the rule in `scripts/codegen/vmafx_api/abi_check.py`.
- `Q` (popup 2026-10-05): "Patch bump, current node (Recommended)"
