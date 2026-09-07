<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1221: `clip_db` is a ceiling on the MS-SSIM dB output, not a clamp on the linear score

- **Status**: Proposed
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: `cuda`, `sycl`, `hip`, `correctness`, `feature-extractor`, `testing`

## Context

`float_ms_ssim` exposes `enable_db` (return the dB-domain score) and `clip_db`.
The CPU derives a ceiling from the frame geometry at `init()`:

```c
const unsigned peak = (1 << bpc) - 1;
if (s->clip_db) {
    const double mse = 0.5 / (w * h);
    s->max_db = ceil(10. * log10(peak * peak / mse));
} else {
    s->max_db = INFINITY;
}
```

and applies it in `convert_to_db()`:

```c
if (score >= 1.0)
    return max_db;                                  /* log10(0) would be -Inf */
return MIN(-10. * log10(1.0 - score), max_db);
```

The CUDA, SYCL and HIP twins declared both options and read `clip_db` as a
clamp on the **linear** score instead:

```c
if (s->enable_db) {
    if (s->clip_db)
        score = score < 0.0 ? 0.0 : (score > 1.0 ? 1.0 : score);
    score = -10.0 * log10(1.0 - score);
}
```

None of the three carried a `max_db` field at all. Two consequences:

1. On an **identical reference/distorted pair** — an entirely ordinary thing to
   score — MS-SSIM is `1.0`, the clamp leaves it at `1.0`, and
   `-10 * log10(0)` is `+Inf`. The CPU returns the finite `max_db`.
2. For any high-similarity pair the twins return an **unbounded** dB value
   where the CPU caps it, so `clip_db` did not clip.

The pre-existing parity tests could not see either: they ran with `NULL`
options, and with `enable_db` off neither path converts to dB at all.

## Decision

We will give each of the three twins a `max_db` field derived in `init()` with
the CPU's exact expression, and a `ms_ssim_convert_to_db()` helper that mirrors
`float_ms_ssim.c::convert_to_db()` — including the `score >= 1.0` short-circuit.
Each backend's MS-SSIM parity test gains a variant that sets
`enable_db=true, clip_db=true`, feeds an **identical** picture as reference and
distorted so the ceiling actually binds, and asserts both that the GPU score is
finite and that it matches the CPU.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Derive `max_db` in `init()` and mirror `convert_to_db()` (chosen) | Byte-for-byte the CPU's rule, including the `score >= 1.0` short-circuit; costs one `double` per state | None material | — |
| Keep the linear clamp and just guard `score == 1.0` | Smaller diff; removes the `+Inf` | Still returns unbounded dB for every other high-similarity score, so `clip_db` still does not clip | Fixes the symptom, not the option |
| Reject `clip_db` in `init()` with `-EINVAL` | Honest about not implementing it | Turns a documented option into a hard failure on three backends | Removes a working surface |
| Compute `max_db` per frame instead of at `init()` | No state field | `w`, `h` and `bpc` are fixed for the extractor's lifetime, so it would recompute a constant on every frame | Pointless work |

## Consequences

- **Positive**: `--feature float_ms_ssim_<backend>:enable_db=true:clip_db=true`
  returns the same finite, capped dB value as the CPU, and scoring a file
  against itself no longer yields `+Inf` on GPU.
- **Negative**: any GPU MS-SSIM dB score recorded with `clip_db` set is wrong
  and must be re-measured. No shipped model uses the dB domain — `enable_db`
  defaults to `false` — so nothing in the repo changes.
- **Neutral / follow-ups**: with `clip_db=false` the CPU sets
  `max_db = INFINITY`, so `convert_to_db()` still short-circuits `score >= 1.0`
  to `INFINITY`. The twins now do the same, where they previously produced
  `NaN` for a score slightly above `1.0`. The Metal MS-SSIM twin exposes only
  `enable_lcs` and rejects `enable_db` / `clip_db` outright; giving it the full
  option set is a separate gap, tracked in `docs/state.md`.

## References

- Findings 42 / 43 / 44 from the twin-drift sweep: `clip_db` means something
  different on the twin — the CPU clamps the dB output at a frame-geometry-
  derived `max_db` (and short-circuits to `max_db` when the linear score reaches
  1.0), while the twins clamp the linear score into `[0, 1]` and then convert
  with no ceiling; none of the three state structs has a `max_db` field.
- Measured on an RTX 4090, 256x192 8-bpc fixture with an identical
  reference/distorted pair and `enable_db=true, clip_db=true`: the unfixed twin
  returns a **non-finite** score where the CPU returns the finite `max_db`.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — the GPU parity CI gate.
- ADR-1216 (PR #1375), ADR-1217 (PR #1376) and ADR-1220 (PR #1379) — the same
  default-options blind spot on other feature families. Referenced by number
  because this branch does not carry those files.
