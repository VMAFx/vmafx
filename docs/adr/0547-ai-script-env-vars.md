<!-- markdownlint-disable MD013 MD033 MD060 -->
# ADR-0547: VMAF_<NAME>_DIR env-var overrides for ai/scripts corpus paths + drop cli.py.bak

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: ai, scripts, container, hygiene, fork-local

## Context

The multi-dimensional audit (surfaced during PR #1309's verification pass) flagged two
persistent paper-cuts that this PR splits cleanly from the HIP fat-binary work:

1. **Hard-coded `.workingdir2/` corpus defaults in `ai/scripts/*.py`.** Roughly 15
   scripts under `ai/scripts/` default `--corpus-root`, `--data-root`, `--clips-dir`,
   etc. to the maintainer's gitignored `.workingdir2/<corpus>/` planning directory.
   On the `vmaf-dev-mcp` container and on every non-maintainer machine the path is
   absent, so the first invocation of each script fails with `FileNotFoundError` before
   any real work starts. The scripts are otherwise correct; the defaults are just wrong
   for any layout other than the maintainer's.

2. **Untracked 142 KB `tools/vmaf-tune/src/vmaftune/cli.py.bak` backup.** An editor
   backup had been sitting in the main worktree for weeks — untracked in git, but
   visible to `rg --type py` audits and polluting `grep` sweeps. No corresponding
   `.gitignore` pattern existed to prevent a recurrence.

These two items were originally bundled with the HIP `gfx_targets` fallback widening
in PR #1318 (ADR-0546). That bundle is being split because the HIP parity claim in the
PR (CPU-HIP delta = 0.031, rationalised as "within places=3 per ADR-0537") fails the
project's non-negotiable places=4 cross-backend parity bar (ADR-0214). The clean items
here are separated now so they can land independently while the HIP issue is
investigated on `investigate/hip-gfx1036-precision`.

## Decision

1. **Layer `os.environ.get("VMAF_<NAME>_DIR", "<old default>")` on every
   `.workingdir2/`-rooted constant** in the 15 affected scripts. The maintainer's
   defaults are byte-identical; operators (and the container) override via env var
   rather than patching the script. The env vars are: `VMAF_CHUG_DIR`,
   `VMAF_NETFLIX_CORPUS_DIR`, `VMAF_KONVID_1K_DIR`, `VMAF_KONVID_150K_DIR`,
   `VMAF_LSVQ_DIR`, `VMAF_LIVE_VQC_DIR`, `VMAF_YOUTUBE_UGC_DIR`,
   `VMAF_WATERLOO_IVC_DIR`, `VMAF_BVI_DVC_RAW_DIR`. `bvi_dvc_to_full_features.py`
   already used `VMAF_BVI_DVC_ZIP` and is unchanged.

2. **Delete `tools/vmaf-tune/src/vmaftune/cli.py.bak`** (untracked; 142 KB) and add
   `*.bak` plus `*.orig` to `.gitignore` as a permanent prophylactic. These are
   universal editor-backup patterns; upstream is unlikely to ever commit either pattern.

3. **Ship `docs/ai/scripts-env-vars.md`** documenting all env vars with a usage table
   and container workflow examples, satisfying the per-PR docs requirement (CLAUDE.md
   §12 r10, ADR-0100).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Env-var override (this decision)** | Strictly additive; maintainer workflow unchanged; single `export` suffices on container. | Env vars are informally documented (relies on `--help` or this ADR). | Chosen — lowest friction, zero behaviour change for maintainer. |
| **Relocate defaults to `.workingdir/` bind-mount path** | Aligns with container mount convention (`/workspace/`). | Breaks maintainer's current scripts overnight; doesn't help anyone whose layout differs. | Env-var layer is strictly additive; relocation is a breaking change. |
| **Wholesale rewrite to `pydantic-settings` / `python-decouple`** | Cleaner long-term. | Blast radius across 15–20 scripts; touches functionality beyond constant-wrapping; out of scope for audit cleanup. | Out of scope; audit fixes touch only constants. |
| **Leave `.bak` untracked; add no `.gitignore` row** | Zero effort. | Continues polluting `rg`/`grep` audits; next editor session may recreate without the pattern. | Five-second fix; permanent benefit. |

## Consequences

- **Positive**: Every `ai/scripts/*.py` works on a clean `vmaf-dev-mcp` container
  invocation by setting one env var per corpus (or none, if `.workingdir2/` is
  present). The `*.bak` / `*.orig` patterns are gitignored globally, preventing future
  accumulation. Human-readable docs at `docs/ai/scripts-env-vars.md` make discovery
  from `git log` or `mkdocs` straightforward.
- **Negative**: None material. Env vars are a very shallow API surface; the risk of
  collision with other tools is low (the `VMAF_` prefix is distinctive).
- **Neutral / follow-ups**: The `VMAF_*_DIR` env vars are not yet injected into the
  `dev/docker-compose.yml` environment block. A follow-up PR can add default values
  pointing at `/workspace/<corpus>/` so operators need not set them at all when using
  the container. Tracked as a separate chore.

## References

- Source: `req` — PR #1309 verification report ("Dim H deep audit: heavy
  `.workingdir2/` defaults across 15+ scripts"). PR split from ADR-0546 because the
  HIP parity claim in the original bundle does not meet the places=4 bar (ADR-0214).
- Related: ADR-0546 (original bundle), ADR-0214 (cross-backend parity gate),
  ADR-0100 (project-wide docs rule), ADR-0042 (tiny-AI docs bar), ADR-0108
  (deep-dive deliverables rule).
- `investigate/hip-gfx1036-precision` branch holds the HIP `gfx_targets` work
  pending root-cause investigation of the 0.031-delta anomaly.
