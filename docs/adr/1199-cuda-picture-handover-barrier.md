<!-- markdownlint-disable MD013 MD060 -->

# ADR-1199: Order caller-written CUDA pictures once per frame, at the dispatch point

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: cuda, correctness, api, testing

## Context

FFmpeg's `libvmaf_cuda` filter intermittently returned a wrong pooled VMAF —
`T-CUDA-FFMPEG-FILTER-NONDETERMINISM-2026-09-06`, first seen as an unexplained 10-in-40.
Exactly one frame per bad run was corrupted, and only the ADM family
(`integer_adm2`, `integer_adm3`, `integer_adm_scale0..3`, `integer_aim`); `vif`, `motion`
and `psnr` were bit-identical.

The cause is a hole in the picture hand-over contract.
`VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_DEVICE` documents that the caller fetches a
libvmaf-owned device picture and *copies into it itself* — "caller copies directly" — which
is what the FFmpeg filter does via `hwupload`, on a stream libvmaf never sees. libvmaf
records a picture's `ready` event only inside `vmaf_cuda_picture_upload_async()`, i.e. only
when **libvmaf** performed the upload. In the caller-copies path that event is never
recorded, so every extractor's `cuStreamWaitEvent(stream, ready)` waits on an event with no
recorded work and is satisfied immediately. Nothing ordered the kernels against the
producer's write.

It presented as ADM-only because `integer_adm_cuda` reads the raw planes first, in its
scale-0 DWT2. The other extractors were later in the queue and therefore lucky, not
synchronised.

Three narrower fixes were built and measured before this one, and all three were rejected
on evidence: waiting on the pictures' `ready` events before the scale-0 DWT2 (vacuous, as
above), fencing ADM's shared `s->buf` against the previous frame's `s->str` work, and
removing the ADR-0242 `drained` shortcut so `collect_fex_cuda()` always synchronises. Each
scored 14/60 against 14/60 for control in an interleaved A/B — no effect.

Measurement conditions matter here and cost real time to establish. The rate tracks
**concurrent CUDA work**, not host CPU load: pure CPU spin at load average 22 gave 1/80,
while three concurrent `vmaf --backend cuda` processes gave 56-57/60. On an idle host it
does not reproduce at all. Two builds must therefore be compared by interleaving runs; a
sequential comparison of the same two builds once suggested a 4× improvement that an
interleaved run showed to be nothing.

## Decision

`read_pictures_extractor_loop()` will push the CUDA context, call `cuCtxSynchronize()` and
pop it, once per frame pair, immediately before the CUDA extractor dispatch. A full context
barrier is the only construct that orders against a producer whose stream is not exposed to
libvmaf, and the dispatch point is the one place where a single host-side wait covers every
CUDA extractor rather than one of them.

Measured through the FFmpeg CUDA filter, interleaved against control under concurrent CUDA
load: **56 of 60 runs corrupted without it, 0 of 60 with it.** Cost on an idle host is
within noise — 177 ms against 179 ms, median of 7 — because the work being waited on is
precisely the data those kernels were about to read.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One context barrier at the CUDA dispatch point (**chosen**) | 56/60 → 0/60 measured; covers every CUDA extractor with one wait; no measurable cost | A full barrier is blunt, and serialises the host against the device once per frame | — |
| Wait on the picture `ready` event in `integer_adm_cuda` | Targeted; matches what other extractors already do | **Measured no effect (14/60 vs 14/60).** The event is never recorded in the caller-copies path, so the wait is vacuous | Rejected on measurement |
| Fence ADM's shared `s->buf` against the previous frame's `s->str` | Closes a genuine write-after-read hazard on shared scratch | **Measured no effect.** Real gap, wrong one | Rejected on measurement |
| Always synchronise in `collect_fex_cuda()` (drop the ADR-0242 `drained` shortcut) | Simple; removes a subtle flag contract | **Measured no effect**, and it costs the ADR-0242 batching win. The data was already wrong before readback | Rejected on measurement |
| Barrier inside `integer_adm_cuda` only | Fixes the observed corruption; narrowest diff | Leaves every other CUDA extractor relying on being later in the queue. `float_moment` was in fact observed failing its parity test under the same GPU contention | Fixes the symptom we saw, not the class |
| Document the contract and require callers to synchronise | Zero runtime cost; arguably the correct division of responsibility | Cannot be enforced, does not fix the FFmpeg filter we ship against, and the failure is silent wrong numbers | A contract nobody checks is what produced this |
| Extend the public API so callers hand over a completion event | Precise, no barrier | A public ABI addition that upstream FFmpeg would have to adopt before it helps; worth proposing, but it does not fix today's filter | Right long-term shape, wrong tool for a live correctness bug |

## Consequences

- **Positive**: the FFmpeg CUDA filter returns a deterministic score. Every CUDA extractor is
  covered, not just ADM. No measurable throughput cost.
- **Negative**: a full context barrier per frame pair is coarse. If the CUDA path is ever
  restructured to keep several frames in flight on the device, this becomes the
  serialisation point and should be revisited together with a real hand-over event.
- **Neutral / follow-ups**: `scripts/test/repro-cuda-ffmpeg-nondeterminism.sh` ships with
  this change and documents that GPU contention, not CPU load, is the reproduction
  condition. Proposing a hand-over-event API to upstream remains open.

## References

- req: found while draining the 1.0.0 PR queue; the symptom was reported by the epic #1245
  benchmark pass in [PR #1334](https://github.com/VMAFx/vmafx/pull/1334).
- `T-CUDA-FFMPEG-FILTER-NONDETERMINISM-2026-09-06` in [state.md](../state.md).
- [ADR-0271](0271-cuda-drain-batch-ms-ssim.md) — the CUDA drain-batch fence batching whose
  `drained` shortcut was one of the rejected suspects. Note the in-tree code comments cite
  it as "ADR-0242"; that number belongs to the tiny-AI training corpus, so the citation in
  `drain_batch.c` and `integer_adm_cuda.c` is stale.
