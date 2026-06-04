<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1022: Cast `dst_buf_read_sz` operands to `size_t` in y4m_input to prevent signed-integer overflow

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `core`, `security`, `correctness`, `tools`, `c`

## Context

`core/tools/y4m_input.c::y4m_input_open_impl()` computes `dst_buf_read_sz` for five
chroma-type branches (420/420jpeg/420mpeg2, 420p10, 420p12, 422p10, 422p12) using bare
`int * int` arithmetic. `pic_w` and `pic_h` are `int`-typed fields, so intermediate
products such as `pic_w * pic_h` are signed `int` multiplications. For a crafted Y4M
header with large `pic_w`/`pic_h` values (e.g. 65536×65536) the product overflows the
signed 32-bit range, wrapping `dst_buf_read_sz` to a value far smaller — or even
negative, which then coerces to a large unsigned `size_t` — than the real frame size.

The companion field `dst_buf_sz` was correctly computed using `(size_t)` casts
(introduced in an earlier fix together with `memset` zero-initialisation of the y4m
struct and malloc-NULL checks). However, that earlier change missed the five
`dst_buf_read_sz` assignments above, leaving a mismatch between `dst_buf_sz` (correct,
used for `malloc`) and `dst_buf_read_sz` (overflowed, used for `fread` at line 931).
A subsequent `fread(_y4m->dst_buf, 1, _y4m->dst_buf_read_sz, _fin)` would request a
read of `dst_buf_read_sz` bytes into a buffer allocated for `dst_buf_sz` bytes. For an
overflowed `dst_buf_read_sz` that is smaller than `dst_buf_sz` no data would be lost;
for a negative-wrapping `dst_buf_read_sz` that coerces to a huge `size_t` the fread
would write past the heap allocation (heap buffer overflow).

The same `(size_t)` promotion pattern is already used in the non-affected branches
(444p10, 444p12, 420paldv, 422, 411, 444, 444alpha, mono) and in the `dst_buf_sz`
computation. This fix applies the pattern uniformly to the five remaining branches.

SEI CERT C rule INT30-C (unsigned integer overflow) and INT32-C (signed integer
overflow) apply; this is a security-relevant fix per those rules.

## Decision

Add leading `(size_t)` casts to the `pic_w` or the leading constant in every
`dst_buf_read_sz` arithmetic expression that previously used bare `int` arithmetic.
This widens the computation to 64-bit `size_t` arithmetic on all supported platforms
(LP64, LLP64, ILP32) and eliminates the signed overflow.

The five affected chroma branches, after the fix:

- **420/420jpeg/420mpeg2**: `(size_t)pic_w * pic_h + (size_t)2 * ((pic_w+1)/2) * ((pic_h+1)/2)`
- **420p10**: `((size_t)pic_w * pic_h + (size_t)2 * ((pic_w+1)/2) * ((pic_h+1)/2)) * 2`
- **420p12**: same formula as 420p10
- **422p10**: `(size_t)2 * ((size_t)pic_w * pic_h + (size_t)2 * ((pic_w+1)/2) * pic_h)`
- **422p12**: same formula as 422p10

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Cast `pic_w` / `pic_h` fields from `int` to `int` in the struct and add an explicit `INT_MAX` check at parse time | Surfaces invalid headers early | Requires struct change and parser change; wider diff | No-one-way fix vs cast; struct change expands the PR scope unnecessarily. The size fields (`dst_buf_sz`, `dst_buf_read_sz`, `aux_buf_sz`) are already `size_t`; only the input dimensions are `int`. Adding early checks would be a complementary hardening, but the cast fix is the minimum correct resolution. |
| No action (document as won't-fix) | Zero code change | Heap buffer overflow with crafted Y4M input; CVE-grade security flaw in the CLI | Not acceptable. |

The cast approach is the only-one-way fix: it mirrors the pattern already applied to
`dst_buf_sz` in the same function and matches all other `dst_buf_read_sz` branches.

## Consequences

- **Positive**: `dst_buf_read_sz` is now consistent with `dst_buf_sz`; `fread` cannot
  request more bytes than were allocated. Crafted Y4M headers with large
  `pic_w`/`pic_h` values no longer produce heap buffer overflows.
- **Negative**: None. The arithmetic result is identical for valid (small) dimensions;
  only crafted large dimensions were broken before.
- **Neutral / follow-ups**: An integration fuzz test that feeds crafted Y4M headers
  with `pic_w = pic_h = 65536` (or `INT_MAX/2 + 1`) and checks for clean rejection or
  correct partial decode would be a worthwhile follow-up. No ADR scope change needed.

## References

- r5-fuzz-surface audit findings HIGH×3 (July 2026 internal security review).
- SEI CERT C: INT30-C, INT32-C.
- `dst_buf_sz` cast precedent: existing code in `y4m_input_open_impl()`, same function.
- PR: fix/y4m-dst-buf-read-sz-overflow
