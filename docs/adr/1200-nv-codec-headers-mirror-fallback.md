<!-- markdownlint-disable MD013 MD060 -->

# ADR-1200: The dev container falls back to the GitHub mirror for nv-codec-headers

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: build, supply-chain, ci

## Context

`dev/Containerfile` installs nv-codec-headers — which supplies
`ffnvcodec/dynlink_cuda.h` for the CUDA backend — from a single host,
`code.ffmpeg.org`. Its comment explained the choice: *"Pinned to commit that has
cuStreamCreateWithPriority; GitHub mirror lags so use code.ffmpeg.org."*

On 2026-09-06 that host was unreachable for over six hours. `curl` returned HTTP 000 after a
30-second timeout on every attempt, and the layer simply stalled. The dev container is the
canonical build and measurement environment under CLAUDE.md rule 15 and, since
[ADR-1102](1102-phase4b9-container-only-publishing.md), the environment published artifacts
are meant to come from. For that whole window it could not be built at all, and the only
symptom was a build that appeared to hang.

The stated reason for the single source does not hold for the pin actually in use. "The
GitHub mirror lags" is true of an unreleased commit, but `NV_CODEC_HEADERS_REF` is a **tag**,
`n13.1.15.0`, and that tag is published on both hosts. The GitHub tarball was checked and
carries the `cuStreamCreateWithPriority` declaration in
`include/ffnvcodec/dynlink_loader.h` — the exact symbol the pin exists for.

## Decision

The layer will try `code.ffmpeg.org` first and fall back to
`https://github.com/FFmpeg/nv-codec-headers` when it is unreachable. Upstream's own host
stays authoritative; the mirror only runs when the primary fails outright.

Whichever archive arrives is then **asserted** rather than trusted: the build requires
`include/ffnvcodec/dynlink_cuda.h` to exist and
`grep -q cuStreamCreateWithPriority include/ffnvcodec/dynlink_loader.h` to succeed before
`make install` runs. A fallback that silently installs the wrong headers would be worse than
the outage it replaces.

The primary is given a short leash — `--retry 1 --connect-timeout 10 --max-time 60`. With a
working fallback behind it, minutes spent on a dead host are pure latency: measured, the
first draft's `--retry 3 --max-time 120` took **487 s** to fall through and succeed, and the
tightened values take **123 s**. The healthy path is a single 0.5 s fetch either way.

The two archives unpack to different top-level directory names — `nv-codec-headers` from
`code.ffmpeg.org`, `nv-codec-headers-<tag>` from GitHub — so the build `cd`s into whatever
was extracted instead of a hard-coded name.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Try upstream, fall back to the GitHub mirror, assert the contents (**chosen**) | Survives either host being down; upstream stays authoritative; content assertion means a fallback cannot silently install the wrong headers | Two hosts can now supply a build input, so the trust surface is two names instead of one | — |
| Switch to GitHub only | One host, and it was the reachable one today | Hands the canonical source to a mirror, and the original comment's concern — that the mirror lags for unreleased commits — becomes real the moment someone moves the pin off a tag | Trades one single point of failure for another, and a worse-provenanced one |
| Vendor the headers into the repository | No network dependency at all; fully reproducible | ~86 KB of third-party headers to carry and re-sync by hand on every bump, and it hides the upstream pin | Disproportionate for a tagged 86 KB archive |
| Add a build-time cache or internal proxy | Fixes this for every fetch in the file at once | Real infrastructure to run and keep alive; nothing exists to hang it on today | Right answer at a larger scale, unavailable now |
| Leave it and wait out the outage | Zero work | It cost more than six hours of a build environment CLAUDE.md rule 15 makes mandatory, with no diagnostic beyond an apparent hang | The status quo is the defect |

## Consequences

- **Positive**: a single unreachable host can no longer make the dev container unbuildable.
  Verified end to end while `code.ffmpeg.org` was still down: the layer fell through to
  GitHub, installed the headers, and the next meson step reported
  `Has header "ffnvcodec/dynlink_cuda.h" : YES`.
- **Negative**: build inputs may now come from either of two hosts. The content assertion
  bounds that — an archive that does not carry the expected header and symbol fails the
  build rather than installing.
- **Neutral / follow-ups**: other fetches in `dev/Containerfile` (oneAPI, ROCm, ONNX Runtime,
  SVT-AV1, vvenc, AMF, FFmpeg) remain single-sourced. This ADR does not claim to have
  audited them; it fixes the one that actually failed. A cache or proxy would address the
  class.

## Supply-chain impact

- **New dependencies**: none. Same package, same tag, second source.
- **Build-time fetches**: adds one conditional `curl` to
  `github.com/FFmpeg/nv-codec-headers`, reached only when the primary fails.
- **Sigstore-signable**: unchanged. Neither host serves a signed archive; the new
  content assertion (header present, `cuStreamCreateWithPriority` present) is the
  check that the bytes are the ones expected, and it did not exist before.
- **CVE surface delta**: neutral. No new component enters the image.

## References

- req: found while the container rebuild for the epic #1246 retrain gates stalled for over
  six hours on this exact layer.
- [ADR-1102](1102-phase4b9-container-only-publishing.md) — container-canonical publishing,
  which is what makes this outage a release-path problem and not just an inconvenience.
