<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1083: y4m_input_fetch_frame signed-integer overflow + fread(NULL) UB fixes

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `core`, `security`, `correctness`, `tools`, `c`, `fork-local`, `bugfix`

## Context

`core/tools/y4m_input.c::y4m_input_fetch_frame()` computes two size values
as `int`:

```c
int pic_sz = _y4m->pic_w * _y4m->pic_h * xstride;
int c_sz   = c_w * c_h * xstride;
```

`pic_w`, `pic_h`, `c_w`, `c_h`, and `xstride` are all 32-bit quantities.
For an 8K frame (7680×4320, depth 10 → xstride=2):
`pic_sz = 7680 × 4320 × 2 = 66,355,200` — safely inside INT_MAX.
However, a Y4M header with adversarial dimensions such as `W 65536 H 65536`
produces `pic_sz = 65536 × 65536 × 2 = 8,589,934,592`, which wraps on
signed 32-bit to a small negative value. `pic_sz` is then used as a pointer
offset (`_y4m->dst_buf + pic_sz`) — adding a negative `ptrdiff_t` is
undefined behavior and in practice produces an address far below the
allocation, causing a heap-buffer-overflow on the next dereference.

The same arithmetic bug appears in three conversion functions:

- `y4m_convert_42xmpeg2_42xjpeg`: `_dst += _y4m->pic_w * _y4m->pic_h`
- `y4m_convert_42xpaldv_42xjpeg`: `_dst += _y4m->pic_w * _y4m->pic_h`;
  `c_sz = c_w * c_h` (where c_w, c_h are each ~ pic_w/2, so product can
  reach ~ 10^9 for large dimensions, overflowing int)
- `y4m_convert_mono_420jpeg`: `_dst += _y4m->pic_w * _y4m->pic_h`;
  `c_sz = ...` chroma-plane product also computed in int

Additionally, `y4m_input_fetch_frame()` unconditionally calls:

```c
fread(_y4m->aux_buf, 1, _y4m->aux_buf_read_sz, _fin);
```

When `aux_buf_read_sz == 0` (no-conversion format paths: 420, 420p10,
420p12, 422p10, 422p12, 444, 444p12, mono, 444alpha), `aux_buf` is NULL.
C11 §7.21.1 does not grant a NULL-pointer exception when `nmemb == 0`;
passing a NULL buffer pointer to `fread` is technically undefined behavior.
While glibc and musl tolerate it at the cost of a zero-length no-op,
CERT MEM10-C and sanitizers flag it, and the guarantee does not hold across
all conforming implementations.

Prior fixes (ADR-0977, ADR-1022) addressed `dst_buf_sz` and
`dst_buf_read_sz` computations in `y4m_input_open_impl()` but left the
`y4m_input_fetch_frame()` locals and the conversion-function pointer
advances unaddressed.

## Decision

Promote `pic_sz` and `c_sz` in `y4m_input_fetch_frame()` from `int` to
`size_t`, casting operands before the first multiplication so all
intermediate products proceed in 64-bit precision. Apply the same cast to
the three pointer advances in the conversion functions (`_dst += ...`)
and to the `c_sz` variables in `y4m_convert_42xpaldv_42xjpeg` and
`y4m_convert_mono_420jpeg`. Guard the `fread(aux_buf, ...)` call with
`if (_y4m->aux_buf_read_sz > 0)` to avoid the NULL-pointer UB on
no-conversion paths.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add an explicit dimension cap (e.g. reject `pic_w > 32768`) at open time | Simple; prevents the class at source | Would break legitimate 8K/16K Y4M inputs; not aligned with the fork's goal of handling large-format content | Rejected |
| Cast only inside `fetch_frame`, leave conversion functions as-is | Minimal diff | Conversion functions called by `fetch_frame` still advance `_dst` by an overflowed int, producing the same UB | Rejected |
| Use `int64_t` instead of `size_t` | Explicit sign; portable | Unnecessarily signed for a buffer size/offset; `size_t` is the canonical type for object sizes | Rejected |

## Consequences

- **Positive**: Eliminates signed-integer-overflow UB in `y4m_input_fetch_frame`
  and the three conversion-function pointer advances; removes the
  `fread(NULL)` UB on no-conversion format paths. The fix makes the code
  consistent with the `size_t`-typed `dst_buf_sz` / `dst_buf_read_sz`
  fields already set by `y4m_input_open_impl` (ADR-0977, ADR-1022).
- **Negative**: No functional change for any frame size that fits in
  2 GiB; no performance impact (the casts compile to zero instructions
  on 64-bit targets).
- **Neutral / follow-ups**: The vendored Daala filter-body subscripts
  (`tmp[y * c_w]`, etc.) in `y4m_convert_42xpaldv_42xjpeg` remain `int`
  loop variables; the products are bounded by `c_h × c_w` (each half of
  `pic_h/pic_w`), so they stay within int range for the largest practical
  Y4M dimensions. A future hardening pass could promote those too.

## References

- req: "core/tools/yuv_input.c + y4m_input.c — header parsing safety, dimension overflow, malformed stream handling"
- ADR-0977 — prior `dst_buf_sz` size_t cast + malloc-NULL guards
- ADR-1022 — prior `dst_buf_read_sz` size_t cast
- CERT C MEM10-C — define and use a pointer validation function
- CERT C INT32-C — ensure that integer operations do not result in overflow
