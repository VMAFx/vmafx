<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3360: The `libvmaf_sycl` filter routes `feature=` names to their SYCL twins

- **Status**: Accepted
- **Date**: 2026-10-05
- **Deciders**: lusoris
- **Tags**: `sycl`, `ffmpeg`, `zero-copy`, `correctness`, `testing`, `fork-local`

## Context

FFmpeg's `libvmaf_sycl` filter (patch `0005`) registers every `feature=` name
with `vmaf_use_feature()` as written. `feature=name=psnr` therefore registers the
CPU `psnr` extractor, never `psnr_sycl`. The `vmaf` CLI resolves a CPU name to
its SYCL twin with `vmaf_feature_backend_twin()`
([ADR-1359](1359-cli-feature-backend-twin.md)); the filter never called it.

On QSV zero-copy input a CPU extractor cannot run: the frames have no host
pictures. Since [ADR-1688](1688-sycl-zero-copy-luma-only-admission.md),
`vmaf_read_pictures_sycl()` refuses such an extractor by name with `-ENOTSUP`
before it counts the frame, so the run fails loudly, but it fails for every
feature the user asks for by its CPU name, including the ones whose SYCL twin
runs zero-copy (`psnr` with `enable_chroma=false`, `cambi`, `float_moment`, ...).
On host-upload input the filter computes such a feature on the CPU, unlike the
CLI, which picks the twin.

The QSV path also accepted any QSV surface format, although the VA import
handles NV12 and P010 only ([ADR-1121](1121-sycl-qsv-zerocopy-p010-normalization.md)).

This was drafted on the Tualua fork as ADR-1595 ("SYCL zero-copy fails loudly
and routes `feature=` to SYCL twins"), together with a pre-pass guard against
CPU extractors, per-extractor `-ENOTSUP` returns for missing host pictures and a
fatal VA import error. ADR-1688 decided the first two on `VMAFx/vmafx` master in
another form (one admission check with an option-aware hook), and
`VMAFx/vmafx#2110` decides the import failure (retry, then stop naming the
frame). This record keeps only what neither covers.

## Decision

- Patch `0005` resolves each `feature=` name through
  `vmaf_feature_backend_twin()` before it registers it (`use_feature_sycl()`,
  on the `libvmaf_sycl` filter only). A twin found is registered instead of the
  CPU name and logged at info level (`feature 'psnr' -> psnr_sycl`).
- With no usable twin (none exists, the twin cannot honour an option, the
  picture size or bit depth, or there is no SYCL device):
  - on QSV zero-copy input the filter fails at configuration with
    `feature '<name>' cannot run on zero-copy input: <reason>`;
  - on software (host-upload) input it warns and computes the feature on the
    CPU, as the CLI does.
- A twin that needs chroma or host pictures is still refused on zero-copy input
  by ADR-1688's admission check, now under the twin's name.
- QSV zero-copy accepts NV12 and P010 surfaces only and names any other
  `sw_format`.
- `ffmpeg-patches/test/check-sycl-feature-routing.sh` pins these in the patch
  text; `make sycl-zerocopy-contract` runs it on every pull request.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Route through `vmaf_feature_backend_twin()` in the filter (chosen) | Same resolution as the CLI; zero-copy runs every twin ADR-1688 admits; host upload uses the device | Host-upload scores of a routed feature are the twin's (bit-exact with the CPU for every case the e2e harness covers) | Chosen |
| Keep CPU names and let ADR-1688 refuse them | No patch change | Every feature named by its CPU name fails on zero-copy, even when its twin would run | Rejected |
| Require users to write twin names (`psnr_sycl`) | No patch change | Differs from the CLI; documentation burden; host upload still runs the CPU for CPU names | Rejected |
| Fail on host upload too when no twin exists | One rule for both paths | Host upload can run the CPU extractor; the CLI falls back too | Rejected |
| Keep the fork's per-extractor `-ENOTSUP` guards and CPU-extractor pre-pass next to ADR-1688 | Defence in depth | Duplicates ADR-1688's single check, which its own alternatives rejected | Dropped |

## Consequences

- **Positive**: `feature=name=<cpu name>` runs the SYCL twin on both input
  paths, as `vmaf --feature` does; a feature that cannot run zero-copy fails at
  configuration or at the first frame with its name.
- **Negative**: host-upload runs of a routed feature now use the device.
- **Neutral / follow-ups**: chroma-reading twins become admissible with the
  post-1.0 zero-copy import ([ADR-1685](1685-post-1-0-embedding-zero-copy-milestone.md));
  the routing needs no change then.

## References

- Draft PR #2217 (Tualua) carried this record as ADR-1764, and ADR-2091 and
  Research-2160 cite it by that number; the rework renumbered it with
  `scripts/adr/next-free.sh --claim` because the open draft still holds ADR-1764.
- `req` (user, 2026-10-02), D-04 on the Tualua fork: "`libvmaf_sycl` routes `feature=` names to SYCL twins via `vmaf_feature_backend_twin` on BOTH paths: zero-copy (no twin / -ENOTSUP = fatal error) and host-upload (fall back to the CPU extractor with a warning, like the CLI). The host-upload switch to twins must be shown bit-exact vs CPU by test."
- `req` (user, 2026-10-05): split the fork's zero-copy work; port the parts that do not import chroma, keeping VMAFx's behaviour where ADR-1688 already covers them.
- [ADR-1359](1359-cli-feature-backend-twin.md), [ADR-1688](1688-sycl-zero-copy-luma-only-admission.md), [ADR-1121](1121-sycl-qsv-zerocopy-p010-normalization.md), [ADR-3359](3359-sycl-primary-queue-immediate-cmdlist.md).
- Testing: [SYCL zero-copy testing](../development/sycl-zerocopy-testing.md).
