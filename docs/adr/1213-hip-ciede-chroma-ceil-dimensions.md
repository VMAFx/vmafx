<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1213: `ciede_hip` sizes its chroma staging with the picture's ceil dimensions

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: hip, correctness, feature-extractor, memory-safety

## Context

`core/src/picture.c` allocates subsampled chroma planes as
`(w + ss_hor) >> ss_hor` — a **ceil** — with a comment naming the exact hazard:
floor under-allocates by one row/column on odd dimensions and causes
one-past-end reads in ciede's chroma upsampling. CPU, CUDA, SYCL and Metal all
consume the picture's real `w[1]` / `h[1]`.

`ciede_hip` was the one implementation re-deriving the geometry itself, with a
floor:

```c
s->chroma_w = ss_hor ? (w >> 1) : w;
s->chroma_h = ss_ver ? (h >> 1) : h;
```

For a 577-wide 4:2:0 picture the real chroma plane is 289 wide; HIP staged 288.
The last chroma column was never uploaded, and the kernel's `cx = x >> 1` for
the last luma column read one element past the staged row — the first sample
of the *next* chroma row, and past the end of the allocation on the last row.
Every ciede parity fixture was even-sized, which is how this survived.

## Decision

We will size the HIP chroma staging buffers with the same ceil formula
`picture.c` uses, so the staged planes match the picture's `w[1]` / `h[1]` and
the upload copies every column. `test_hip_ciede_parity` is additionally
registered at 577x325.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Use `picture.c`'s ceil formula in init (chosen) | Two-line change; matches every other implementation; the upload code needs no change | Init does not have the picture, so the formula is duplicated rather than read from `pic->w[1]` | — |
| Read `pic->w[1]` / `pic->h[1]` at first extract and allocate lazily | No duplicated formula | Moves allocation out of init into the frame path and complicates error handling for a geometry the formula already defines | Rejected |
| Clamp `cx` / `cy` in the kernel to the staged width | Prevents the out-of-bounds read | Still drops the real last chroma column, so odd widths would score differently from every other backend | Rejected — hides the bug instead of fixing it |

## Consequences

- **Positive**: odd-dimension 4:2:0 / 4:2:2 input no longer reads past the
  staged chroma row on HIP. `test_hip_ciede_parity_oddw` (577x325) passes on a
  gfx1030 and the even-sized test is unchanged.
- **Negative**: none measured; the extra column/row of staging is one sample
  wide.
- **Neutral / follow-ups**: none.

## References

- `core/src/picture.c` (chroma allocation and its hazard comment).
- [ADR-1154](1154-hip-backend-gaps.md) — the HIP backend gap inventory.
- Source: `req` — user direction to fix bugs found by the twin-drift sweep.
