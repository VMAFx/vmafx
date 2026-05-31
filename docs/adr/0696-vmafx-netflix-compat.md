<!-- markdownlint-disable MD060 -->
# ADR-0696: `--netflix-compat` flag for restoring legacy defaults

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `vmafx`, `rebrand`, `cli`, `netflix-compat`, `fork-local`

## Context

Phase 1A (ADR-0690, PR #1565) added a `vmafx` symlink and `detect_vmafx_mode()`
so that when the binary is invoked as `vmafx` it applies modernized defaults:
`--precision=max` (`%.17g` IEEE-754 lossless output) and a distinct startup
banner. The `vmaf` binary continues to behave exactly as before — CPU backend,
`%.6f` precision — satisfying the Netflix golden-data gate.

This creates a gap: a user running `vmafx` in a CI pipeline or script that
expects Netflix-compatible output has no single flag to opt back into the full
legacy behavior. They would have to know to pass `--precision=legacy`
individually. Additionally, the `vmafx` modernization currently only changes
precision; if future phases add more modernized defaults (concurrency options,
output schema changes, etc.), there must be a single escape hatch to restore
all of them at once.

The umbrella VMAFX rebrand plan (ADR-0686, PR #1546) explicitly called for a
`--netflix-compat` flag as the opt-out mechanism.

## Decision

Add a `--netflix-compat` boolean flag to `cli_parse.c` and `CLISettings`. When
set, apply a **final post-parse pass** that overrides any vmafx-mode
modernizations back to the Netflix-upstream legacy defaults:

- Backend: CPU only (all GPU backends disabled — equivalent to `--backend=cpu`).
- Precision: `%.6f` (equivalent to `--precision=legacy`).

The flag is applied **after** the `vmafx_mode` block, so it wins cleanly
regardless of binary name. It is idempotent on the `vmaf` binary (already the
defaults). Both `--netflix-compat` and `--netflix_compat` spellings are
accepted for consistency with the rest of the flag set.

The `CLISettings.netflix_compat` field is set during the parse loop and read
only in the post-parse block; no caller other than `cli_parse.c` needs to
inspect it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `--precision=legacy` alias (Phase 1A already has this) | Zero new code | Only reverts precision, not backend; does not scale to future vmafx modernizations | Insufficient — does not address backend and is not a complete escape hatch |
| Separate `vmaf-legacy` binary / script wrapper | Obvious name | Installs a third binary, adds maintenance surface, does not compose with existing flags | Overhead disproportionate to the use case |
| Environment variable `VMAF_NETFLIX_COMPAT=1` | Scriptable without flag passing | Invisible from `--help`; env vars are harder to audit in CI logs | Environment variables for behavior are harder to discover and review than flags |
| No escape hatch — require `vmaf` for legacy | Simplest | Breaks existing scripts that switch to `vmafx` expecting to pass individual precision flags | Violates the principle of least surprise for early adopters |

## Consequences

- **Positive**: A single flag gives users a complete, auditable escape hatch
  that scales to any future `vmafx` modernizations.
- **Positive**: The Netflix golden-data gate is trivially satisfied by any
  script that adds `--netflix-compat` to a `vmafx` invocation.
- **Positive**: The flag self-documents its intent from `--help` output.
- **Negative**: One more flag for users to know about; mitigated by the
  `--help` description and `docs/usage/vmafx-cli.md` section.
- **Neutral**: `CLISettings` grows one `bool` field. No ABI impact (struct
  is internal to the CLI tools TU).
- **Follow-up**: If Phase 2B adds more `vmafx` modernized defaults (e.g.
  concurrency, output format changes), those should also be reverted in the
  `settings->netflix_compat` block in `cli_parse.c`.

## References

- ADR-0690 — vmafx binary and AI tool aliases (Phase 1A, PR #1565).
- ADR-0686 — VMAFX rebrand umbrella (PR #1546).
- ADR-0119 — CLI precision default revert (`%.6f` as default).
- CLAUDE.md §8 — Netflix golden-data gate requirement.
- req: "add a `--netflix-compat` flag to the SAME CLI that RESTORES legacy
  defaults regardless of binary name" (task specification 2026-05-28).
