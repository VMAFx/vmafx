<!-- markdownlint-disable MD060 -->
# ADR-0457: model/tiny/*.onnx blobs ≥1MB live in GitHub Releases, not git

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, model-storage, repo-size, fork-local

## Context

`model/tiny/` ships 28 ONNX artefacts. Three of them dominate the
size budget:

| File                  | Size    |
|-----------------------|---------|
| `transnet_v2.onnx`    | 30.8 MB |
| `fastdvdnet_pre.onnx` | 10.0 MB |
| `lpips_sq.onnx`       | 3.3 MB  |
| **Subtotal**          | 44.1 MB |
| Other 25 files (each ≤500 KB) | 12 MB |

Inlined in git history those three blobs cost every clone — including
every CI runner and every agent worktree spawn — the full 44 MB
even though most workflows never touch them. They were briefly tracked
through Git LFS, but the LFS objects were never actually pushed to
GitHub LFS storage, so smudge in fresh worktrees produced 130-byte
pointer text instead of the real binaries (closed by PR #846, which
dropped the LFS attribute and reverted to inline binary storage).

## Decision

Host the three large blobs as **release attachments on a non-version
GitHub Release** (`tiny-blobs-vN`) and download them on demand via
`scripts/ai/fetch-tiny-blobs.sh`.

Boundary: the cutoff is **1 MB**. Files above it (currently 3) move
to releases; files below it (currently 25) stay inline. Below the
cutoff the per-file fetch overhead dominates the per-byte storage
cost; above it the inverse holds. The 1 MB cutoff is recorded in
the fetcher comment so future additions know the rule.

The fetcher reads `model/tiny/registry.json`. Each migrated entry
gains a `release_url` field; the fetcher downloads any blob whose
`release_url` is non-empty AND whose local file is missing,
verifies the recorded `sha256`, and writes atomically to
`model/tiny/`. Idempotent — re-running with everything present is a
no-op.

Release tagging: `tiny-blobs-vN` is intentionally separate from the
`vN.M.K-lusoris.X` semver release tags driven by release-please. The
two timelines never collide; tooling that filters by tag prefix
(documentation generators, release notes assemblers) treats
`tiny-blobs-*` as out-of-band.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **GitHub Releases attachments + fetcher (chosen)** | Free hosting on the same origin as the repo; no extra credentials; sha256 already in registry; immutable per-tag URLs; no DNS/billing setup | One extra step on first checkout (`scripts/ai/fetch-tiny-blobs.sh`); CI needs a cache step | Chosen — minimum new surface, maximum reuse of existing infra |
| **Git LFS (re-attempt)** | Transparent on `git checkout` | LFS storage costs money on private mirrors; agents that don't `git lfs install` still get pointer text; same failure mode that closed PR #846 just months ago | Rejected — same issue as before; would need org-wide LFS storage allocation |
| **Hugging Face Hub** | Discoverable to ML community; free hosting; native versioning | Adds an external runtime dependency (`huggingface_hub`); needs an HF org + token in CI; the `model/tiny/` set is more "build artefact" than "shareable model" — not discoverability-critical | Rejected for now — possibly later for the `vmaf_tiny_v*` family if upstream adoption grows |
| **Self-hosted S3-compatible (Backblaze B2 / Cloudflare R2)** | Maximum control + no LFS quota | New DNS + bucket + credentials in every CI workflow + monthly billing; bus-factor risk on credential rotation | Rejected — cost outweighs benefit at the 44 MB scale |
| **Just keep the blobs inline (status quo)** | No new code | Every clone pays 44 MB always; agent worktree spawns are slower; CI cache is bigger | Rejected — the user explicitly flagged the storage as friction |

## Consequences

### Positive

- Working-tree `model/tiny/` drops from 56 MB to 12 MB.
- New clones run a 3.5-second one-time fetch (measured against
  `releases/download/tiny-blobs-v1`) instead of inlining 44 MB into
  every checkout.
- Future large ONNX additions follow the same pattern; the cutoff
  rule (≥1 MB → release attachment) is documented inline in the
  fetcher.
- The `release_url` field plus the existing `sha256` field gives the
  fetcher a deterministic, audit-friendly download recipe — no
  out-of-band manifest maintenance.

### Negative

- Git history is not rewritten; clones still get the full historical
  44 MB across all parents. The fix is forward-only: HEAD slims down
  immediately; full history-rewrite would require a `git filter-repo`
  pass + master force-push, which is blocked by branch protection
  (ADR-0037). A future controlled-window rewrite is a separate
  decision.
- Code paths that load `transnet_v2.onnx` / `fastdvdnet_pre.onnx` /
  `lpips_sq.onnx` must call the fetcher (or assume an out-of-band
  pre-fetch step) before opening the file. The fetcher is a 70-line
  bash script with no third-party dependencies, callable from
  Python via `subprocess.run`.
- `tiny-blobs-vN` adds a non-version tag to the release list. Tools
  that surface "latest release" must filter by `^v[0-9]` if they
  care about semver tags only.

### Neutral

- The 25 small ONNX files (≤500 KB each) stay inline. Per-file
  fetch overhead would dominate the per-byte storage savings below
  1 MB; the cutoff is intentional, not arbitrary.
- The fetcher is idempotent and safe to call from CI cache-warmup
  steps without coordination.

## References

- [PR #846](https://github.com/VMAFx/vmafx/pull/846) — preceding
  LFS revert that motivated the blob-storage retry.
- [`scripts/ai/fetch-tiny-blobs.sh`](../../scripts/ai/fetch-tiny-blobs.sh)
  — the fetcher implementation.
- [`docs/ai/tiny-blob-storage.md`](../ai/tiny-blob-storage.md) —
  user-facing doc page on the storage model.
- Source: `req` — direct user direction (Slack 2026-05-15:
  "we could fix the blob storage github bullshit in the foreground").
