# ADR-0118: FFmpeg patches ship as ordered series.txt, not a single carry

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, build, ffmpeg, docker, sycl, ai

## Context

The fork ships three FFmpeg patches against `n8.1`:

1. `0001-libvmaf-add-tiny-model-option.patch` — adds `tiny_model`, `tiny_device`,
   `tiny_threads`, `tiny_fp16` options to `vf_libvmaf` (wires the tiny-AI surface
   from libvmaf's DNN runtime into the existing filter).
2. `0002-add-vmaf_pre-filter.patch` — adds a brand-new `vf_vmaf_pre.c` filter
   that runs a residual-CNN ONNX model via `vmaf_dnn_session_*`.
3. `0003-libvmaf-wire-sycl-backend-selector.patch` — adds `sycl_device` /
   `sycl_profile` options that route the filter through libvmaf's SYCL backend.

Patch 0003 references LIBVMAFContext fields added by 0001, so the series has a
**hard ordering**. Until this PR the build chain ignored that:

- `Dockerfile:86-88` `COPY`'d only `0003-libvmaf-wire-sycl-backend-selector.patch`
  and applied it standalone — failed at hunk 2 because `tiny_device` etc. were
  unknown identifiers.
- `.github/workflows/ffmpeg.yml:115-126` referenced a stale path
  `../patches/ffmpeg-libvmaf-sycl.patch` that no longer existed (the patches
  moved to `ffmpeg-patches/` weeks ago).
- All three patch files lacked the `index <sha>..<sha> <mode>` header line that
  `git apply` and `patch -p1` require, because they were hand-stubbed with
  placeholder SHAs (`a1a1a1…`, `b2b2b2…`, `c3c3c3…`) instead of being
  produced by `git format-patch`.

PR #50 surfaced all three problems simultaneously the moment its consolidated
master-targeting trigger matrix sent the docker / FFmpeg-SYCL jobs through the
gate for the first time on this branch.

Two latent build-flag bugs surfaced in the same trigger window once the patch
series itself applied cleanly:

- **Phantom `--enable-libvmaf-sycl` configure flag.** Both Dockerfile and
  `ffmpeg.yml` passed `--enable-libvmaf-sycl` to FFmpeg's `./configure`. Patch
  0003 only adds `check_pkg_config libvmaf_sycl …` (auto-detection that
  matches how `libvmaf_cuda` is wired) — it never registers the
  `libvmaf_sycl` switch in `EXTERNAL_LIBRARY_LIST`, so configure rejects the
  flag with `Unknown option "--enable-libvmaf-sycl"`. The flag was a
  copy-paste artefact from before patch 0003 was switched to auto-detection.
- **Multi-arch nvcc flags reused for FFmpeg's check_nvcc.** Commit
  `8a995cb0` (ADR-D27) broadened `NVCC_FLAGS` to four `-gencode` lines
  (Turing/Ampere/Hopper/Blackwell) plus experimental host+device flags
  (`--expt-relaxed-constexpr --extended-lambda --expt-extended-lambda`) for
  libvmaf's Thrust/CUB code, then the Dockerfile threaded the same
  `${NVCC_FLAGS}` into FFmpeg's `--nvccflags=`. FFmpeg's `check_nvcc`
  invokes nvcc in `-ptx` device-only mode, which rejects this with the
  fatal error `Option '--ptx (-ptx)' is not allowed when compiling for
  multiple GPU architectures` (verified locally with nvcc 13.2). The
  experimental flags compound the problem (device-only-incompatible) but
  the multi-arch issue is what surfaces first. Result: `failed checking
  for nvcc.` even though the same nvcc compiled libvmaf successfully one
  step earlier.
- **FFmpeg n8.1's libnpp probe explicitly rejects CUDA 13.x.** Once the
  nvcc fix above lets FFmpeg's configure get past the nvcc check, the
  next probe in source order is libnpp (configure line 7330). FFmpeg
  n8.1 carries an explicit `die "ERROR: libnpp support is deprecated,
  version 13.0 and up are not supported"` (configure:7335-7336) that
  fires on the base image's CUDA 13.2 libnpp. The `npp_*_filter` set
  (scale_npp, transpose_npp, etc.) is unrelated to VMAF workflows; we
  use cuvid + nvdec + nvenc + libvmaf-cuda instead.
- **Patch 0002 missing `libavutil/imgutils.h` include.** `vf_vmaf_pre.c`
  calls `av_image_copy_plane()` (declared in `libavutil/imgutils.h`)
  but the original patch only included `pixdesc.h` / `mem.h` / `opt.h`
  / `avstring.h`. FFmpeg's libavfilter Makefile builds with
  `-Werror=implicit-function-declaration`, so the symbol-resolves-at-link
  hack doesn't paper over it. Caught by the local docker build (which
  the user prefers as the validation step over GitHub-Actions waits);
  the patch was regenerated via `git format-patch` after committing the
  include into a fresh n8.1 worktree so it carries a real SHA.

## Decision

**We will treat `ffmpeg-patches/` as a managed quilt-style series, applied in
the order recorded in `ffmpeg-patches/series.txt`.** The Dockerfile and the
`ffmpeg.yml` workflow now both walk `series.txt` in order and apply each patch
via `git apply` with a `patch -p1` fallback. The patches themselves are
regenerated via real `git format-patch -3` runs against an FFmpeg `n8.1`
worktree, so they carry valid `index` lines and committable SHAs.

**The two latent build-flag bugs are fixed in the same PR** (in scope per the
no-skip-shortcuts memory rule — they're regressions newly exposed by the
trigger consolidation, not orthogonal cleanup):

- Drop the `--enable-libvmaf-sycl` configure flag from the Dockerfile and
  `ffmpeg.yml`. SYCL support is enabled by setting `-Denable_sycl=true` at
  libvmaf build time; FFmpeg auto-detects libvmaf-sycl via the
  `check_pkg_config` line patch 0003 adds.
- Split nvcc flags into two ARGs in the Dockerfile. `NVCC_FLAGS` keeps the
  four `-gencode` lines plus the experimental `--extended-lambda` /
  `--expt-*` flags for libvmaf's CUDA build; new `FFMPEG_NVCC_FLAGS`
  carries a single `-gencode arch=compute_75,code=sm_75 -O2` (matches
  FFmpeg's own modern-nvcc default — PTX is forward-compatible with all
  newer GPUs via driver JIT, and `-ptx` mode requires single-arch). No
  experimental host+device flags either.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Apply only 0003 (status quo before this PR) | Smallest CI surface. | tiny-AI and `vf_vmaf_pre` never reach a docker FFmpeg build; 0003 fails to apply standalone because it depends on 0001's struct fields. | Broken — chosen approach is the only one that compiles. |
| Squash all three patches into one mega-patch | Single `git apply`, no ordering risk. | Loses the per-feature commit message + signed-off-by trail; harder to upstream piecemeal; harder to diff what's a Lusoris addition vs. an FFmpeg change. | Future upstreaming pressure (each of the three is a sensible standalone PR target). |
| Maintain a permanent `lusoris-fork` branch on a vendored FFmpeg checkout | Always green; no patch fragility. | Doubles repo size; rebases on every FFmpeg release become a manual chore; breaks the "patches against tagged release" provenance story. | Operational cost too high for a 3-patch series. |
| Switch to Quilt or StGit for in-tree patch management | Industry-standard ordering tooling. | New dev dependency; extra learning curve for one-off contributors; CI runners would need the tool installed. | series.txt + plain `git apply` already gives us 90% of Quilt with zero new tools. |
| Convert each patch to an upstream FFmpeg PR and pin the merge SHAs | No carry burden long-term. | tiny-AI / vmaf_pre depend on libvmaf 3.0+ APIs that aren't yet released; FFmpeg upstream won't take patches against unreleased deps. | Premature — revisit once libvmaf 3.x ships. |

## Consequences

- **Positive**: docker image and CI FFmpeg-SYCL build now exercise the full
  fork-added FFmpeg surface (tiny-AI + `vmaf_pre` + SYCL selector), not just
  SYCL. Patch ordering is documented in one file (`series.txt`) and enforced
  by both Dockerfile and CI. Patches carry real SHAs so they round-trip
  through `git format-patch` / `git am` for anyone wanting to upstream them.
- **Negative**: every new patch must be appended to `series.txt` and respect
  the existing ordering; CI failures from a missing entry will surface late
  (only at the docker / FFmpeg-SYCL job, ~10 min in). The hard ordering
  between 0001 and 0003 is implicit — no static check enforces "0003 must
  come after 0001".
- **Neutral / follow-ups**: when libvmaf 3.0 ships, revisit option E (upstream
  the patches). `ffmpeg-patches/test/build-and-run.sh` already loops over
  series.txt the same way — Dockerfile and ffmpeg.yml now match its
  contract. No rebase impact (the patches target FFmpeg n8.1, not the fork).

## References

- PR #50 (consolidated CI trigger matrix; surfaced the patch-application
  bugs as side effects of routing docker/FFmpeg-SYCL jobs through the gate
  for the first time on a master-targeting PR).
- Per-feature patch authoring rationale: tiny-AI surface (ADR-0102), C3
  pre-processing (ADR-0107), SYCL backend (ADR-0101).
- Doc-substance rule (ADR-0100) — drove the rebase-notes + CHANGELOG bundle.
- Source: PR #50 CI failure on docker job (`series.txt:0001 missing — patch
  malformed at line 38`); per user direction, expand PR #50 scope to
  regenerate all three patches and apply the full series rather than ship a
  partial fix.
