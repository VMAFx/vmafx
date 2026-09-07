<!-- markdownlint-disable MD013 -->

# 2038 — When a twin implements the right-sounding thing: `clip_db`

**Date**: 2026-09-07
**Scope**: `clip_db` on the CUDA, SYCL and HIP `float_ms_ssim` twins.
**Outcome**: all three fixed to the CPU's ceiling semantics
([ADR-1221](../adr/1221-gpu-ms-ssim-db-ceiling.md)); a dB-option variant added
to each backend's parity test.

## Two readings of one option name

`clip_db` reads naturally as "clip the score before the dB conversion". That is
what all three GPU twins implemented:

```c
if (s->enable_db) {
    if (s->clip_db)
        score = score < 0.0 ? 0.0 : (score > 1.0 ? 1.0 : score);   /* clamp the LINEAR score */
    score = -10.0 * log10(1.0 - score);                            /* convert, unbounded */
}
```

The CPU means something else: `clip_db` selects a **ceiling on the dB output**,
derived once from the frame geometry.

```c
const unsigned peak = (1 << bpc) - 1;
s->max_db = s->clip_db ? ceil(10. * log10(peak * peak / (0.5 / (w * h)))) : INFINITY;

static double convert_to_db(double score, double max_db)
{
    if (score >= 1.0)
        return max_db;                              /* log10(0) would be -Inf */
    return MIN(-10. * log10(1.0 - score), max_db);
}
```

The twin's version compiles, reads correctly, and is wrong in two ways:

1. **`+Inf` on an identical pair.** The clamp leaves a perfect `1.0` at `1.0`,
   and `-10 * log10(0)` is `+Inf`. Scoring a file against itself is an ordinary
   thing to do; the CPU returns the finite `max_db`.
2. **`clip_db` does not clip.** For every high-similarity pair the twin returns
   an unbounded dB value where the CPU caps it. The option's entire purpose was
   unimplemented.

None of the three state structs even had a `max_db` field — the strongest tell
that the semantics were reconstructed from the option name rather than ported
from the reference.

## The fixture trap

The obvious parity variant — set `enable_db=true, clip_db=true` on the existing
fixture — **passes against the unfixed code**. On a merely high-similarity pair
`-10*log10(1 - score)` sits well below `max_db`, so the ceiling never binds and
both paths agree.

Verified: the first version of this test was green against the unfixed twin.
Only after changing the fixture to feed the **same picture** as reference and
distorted — driving MS-SSIM to exactly `1.0` — did the divergence appear, as a
non-finite GPU score against a finite CPU one. Measured on an RTX 4090 and on
gfx1030, 256x192 / 256x256 8-bpc fixtures.

The general rule: **a test for a saturating rule has to saturate.** A ceiling,
a clamp, a floor or a min-val option is only exercised by an input that reaches
it; a "typical" fixture proves nothing about it.

## Why the default-options tests were blind

Same shape as the three preceding digests ([2033](2033-identity-default-option-blind-spot.md),
[2034](2034-kernel-local-constants-shadowing-options.md),
[2037](2037-advertised-but-unimplemented-gpu-options.md)): every MS-SSIM parity
test instantiated its extractors with `NULL` options, and with `enable_db` off
neither path converts to dB at all. The divergent code was simply never
executed.

What makes this instance different from the earlier three is that the twin was
not *ignoring* the option — it was honouring a plausible misreading of it. A
grep for "is this option read anywhere?" finds `clip_db` and moves on. Only a
line-by-line comparison against the reference's `convert_to_db()` shows the
divergence.

## Remaining gap

`float_ms_ssim_metal.mm` exposes only `enable_lcs`. It rejects `enable_db`,
`clip_db` and `enable_chroma` outright, so it can never emit a dB-domain or
chroma score at all — a loud failure rather than a wrong answer, and therefore
a feature gap rather than a defect. Tracked in `docs/state.md`.
