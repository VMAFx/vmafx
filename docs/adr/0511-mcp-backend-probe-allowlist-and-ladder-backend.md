<!-- markdownlint-disable MD013 MD060 -->
# ADR-0511: MCP backend probe, default allowlist, and `vmaf-tune ladder --score-backend` (2026-05-18)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: mcp, vmaf-tune, ai, dx, bugfix

## Context

A triage session against the `vmaf-dev-mcp` container surfaced three
small but compounding defects on the user-facing developer surfaces:

1. **MCP `list_backends` mis-reports CUDA.** The historical
   implementation grepped the output of `vmaf --version` for the
   substrings `"cuda"`, `"sycl"`, `"vulkan"`, ... The fork's `vmaf`
   banner does not advertise compiled-in GPU backends there, so on a
   container where `vmaf --backend cuda -r src01_hrc00 -d src01_hrc01`
   produces a valid 76.66783 score (5-place bit-exact vs CPU on the
   Netflix golden 576×324 pair) the MCP server confidently returns
   `{"cuda": false}`. Any downstream MCP client that orchestrates a
   cross-backend run picks the wrong arm.

2. **MCP default allowlist excludes the Netflix golden YUVs.** The
   default allowlist resolved `python/test/resource` relative to the
   *host repo root* (the fork's actual tree on disk). Inside the
   `vmaf-dev-mcp` container the repo lives at `/workspace/`, so the
   canonical fixture path
   `/workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv`
   landed outside every allowlisted root and `vmaf_score` rejected it
   with `"path not under an allowlisted root; set VMAF_MCP_ALLOW to
   extend."` Every container-side MCP demo therefore had to set
   `VMAF_MCP_ALLOW=/workspace/python/test/resource` before the first
   tool call.

3. **`vmaf-tune ladder` is missing `--score-backend`.** The sibling
   subcommands `compare` and `tune-per-shot` both accept
   `--score-backend {cpu,cuda,sycl,vulkan,auto}` and thread it down
   to the underlying `vmaf` invocation as `--backend $name`. `ladder`
   silently runs the corpus sampler with whatever libvmaf chooses
   itself, defeating the purpose of the dev-machine CPU/CUDA/SYCL
   parity matrix when staging a new VMAF model on a multi-GPU host.

All three are tiny edits in isolation; the value is in shipping them
together with the documentation + ADR + tests that make the
guarantees explicit going forward.

## Decision

1. **Replace the `--version`-grep backend probe with a `--help`-based
   `_probe_backends()` helper** that looks for the documented
   `--no_<backend>` disable flags. The vmaf CLI always advertises a
   `--no_<backend>` line for every compiled-in backend, so presence of
   `--no_cuda` in `--help` output is a sufficient and necessary
   condition for CUDA being live. Cache the result per-binary-path
   for the lifetime of the MCP server process.

2. **Extend the MCP default allowlist** to include the absolute
   container-side mount path
   (`/workspace/python/test/resource`). The host-relative
   `_repo_root() / "python/test/resource"` entry stays so a host-side
   `vmaf-mcp` invocation (used by `make test`) still works. The
   `VMAF_MCP_ALLOW` env-var override is unchanged.

3. **Add `--score-backend {cpu,cuda,sycl,vulkan,auto}` to
   `vmaf-tune ladder`** with `auto` as the default (which preserves
   current behaviour). On any non-`auto` value, `_run_ladder` resolves
   the choice via `score_backend.select_backend()` up-front so an
   unavailable backend errors out *before* any encodes start, then
   threads the resolved value through `make_default_sampler` →
   `CorpusOptions.score_backend` → `vmaf --backend $name`. The
   sibling `--vmaf-bin` flag is added to the same subparser to give
   `select_backend()` a probe target (the rest of the ladder code
   path already shells out to `vmaf` by name).

4. **Deliberately do NOT touch `tune-per-shot`.** Its
   `_build_per_shot_bisect_predicate` already converts
   `args.score_backend == "auto"` to `None` so `bisect_target_vmaf`
   receives `None` for auto and lets libvmaf pick the fastest live
   runtime. Resolving the backend up-front there would change the
   "auto → None → libvmaf-picks" contract that downstream tests
   assert. The asymmetry is intentional: the user spec for ADR-0509
   explicitly called it out as a regression to avoid.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `--version`-grep but add CUDA/SYCL substrings to the banner upstream | One-line patch to `vmaf.c` | Won't help users who run the previously-shipped binary; banner-grep is fragile (every backend rename breaks it again); requires upstream coordination | `--help` flag-presence is a stable contract already enforced by the CLI's argparse table |
| Hard-code `/workspace/...` as the *only* container allowlist | Smallest patch | Breaks host-side use of the MCP server (`make test`, dev-host smoke runs); user would have to set `VMAF_MCP_ALLOW` on every invocation | Add the container path *in addition to* the existing host-relative roots |
| Make `ladder` reject `--score-backend` as "unsupported, use `compare`" | Smallest patch | Diverges DX from the sibling subcommands; users have asked for it three times in MCP-probe sessions | Add the flag and thread it; the cost of the plumbing is one extra kwarg on `make_default_sampler` and one extra branch in `_run_ladder` |
| Pre-resolve `tune-per-shot` backend up-front via `select_backend` too (symmetric with `ladder`) | API symmetry between subcommands; "fail loudly" semantics for both | Breaks the `None if score_backend == "auto" else score_backend` contract the predicate already relies on; would require also touching `_build_per_shot_bisect_predicate` and three downstream tests | User spec explicitly forbade regressing `tune-per-shot`; keep the existing behaviour |

## Consequences

- **Positive**:
  - Container MCP demos work out of the box — no `VMAF_MCP_ALLOW`
    boilerplate for the canonical fixture path.
  - `list_backends` correctly reports CUDA / SYCL / Vulkan / HIP /
    Metal on any host where the binary advertises the corresponding
    `--no_<backend>` flag. Cross-backend parity scripts no longer
    skip CUDA silently.
  - `vmaf-tune ladder --score-backend cuda` finally honours the user's
    intent the same way `compare` and `tune-per-shot` do.
- **Negative**:
  - `tune-per-shot` and `ladder` now have slightly asymmetric
    backend-handling semantics (ladder: fail-fast on unavailable;
    per-shot: defer to libvmaf at score time). The asymmetry is
    documented inline in `_run_tune_per_shot` so the next reader does
    not "fix" it.
- **Neutral / follow-ups**:
  - Future work: align `tune-per-shot` to the `select_backend`
    fail-fast path when we also rev the predicate contract; tracked
    as a follow-up in `docs/state.md`.

## References

- Related: [ADR-0495](0495-mcp-probe-bug-fixes.md) — the original
  MCP probe-driven bug-fix cluster; this ADR is a follow-on
  surfaced by the same probe harness.
- Related: [ADR-0127](0127-cli-flag-unification.md) for the unified
  `--backend NAME` selector this work depends on.
- Related: [ADR-0175](0175-vulkan-backend-scaffold.md) for the
  Vulkan backend that participates in the probe matrix.
- Source: `req` — paraphrased from the 2026-05-18 triage brief that
  bundled the three bugs into a single fix PR.
