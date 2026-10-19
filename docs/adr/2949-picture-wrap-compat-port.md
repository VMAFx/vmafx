<!-- markdownlint-disable MD013 MD060 -->
# ADR-2949: `vmaf_picture_wrap` ships with upstream's signature, on `vmafx_frame_wrap_host`, with the fork's plane rules

- **Status**: Accepted
- **Date**: 2026-10-09
- **Deciders**: lusoris (upstream sync brief, 2026-10-09)
- **Tags**: api, upstream-port, compat, fork-local

## Context

Netflix/vmaf `700124a4c` adds `vmaf_picture_wrap(VmafPicture *, VmafPictureWrapped)`:
a picture on planes the caller owns, released through a caller callback when
the last reference goes. In the fork, `libvmaf.so.3` is a compat library on the
VMAFx API ([ADR-2094](2094-libvmaf-compat-library-split.md)); every libvmaf
function has an engine body (the old libvmaf) and a compat body on `vmafx_`
calls, and `test_compat_conformance` requires the two to behave alike. The VMAFx
API already has the same operation, `vmafx_frame_wrap_host()`, which checks the
borrowed planes against the frame's geometry. The fork's chroma planes of an odd
size are rounded up ([ADR-1483](1483-odd-size-chroma-planes-round-up.md));
upstream's wrap rounds them down and checks only the format and the bit depth.
Upstream's callback receives its internal picture; VMAFx release callbacks get
only a user pointer.

## Decision

We ship upstream's struct and signature unchanged, additively in
`libvmaf.so.3`, deprecated towards `vmafx_frame_wrap_host()` like the rest of
`picture.h`. The compat body wraps the planes with `vmafx_frame_wrap_host()` and
runs the caller's `release_picture` from the frame's release, with a picture
that carries the wrapped format, size, data and strides and NULL internal
fields. The engine body is upstream's function, adapted: it sizes the planes
with `vmaf_picture_plane_extents()` (chroma rounded up), refuses what the VMAFx
API refuses (a size of 0, a NULL plane, a stride that is negative or shorter
than a row) with `-EINVAL`, leaves `*pic` untouched on failure, and has no
`goto`. Its construction step, `vmaf_picture_wrap_bind()`, is also what
`vmafx_frame_bind()` uses, so the engine has one way to put a picture on
borrowed planes. No SONAME change: the addition is backward compatible, as for
[ADR-1822](1822-additive-picture-convert.md).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Port upstream's semantics exactly (chroma rounded down, no plane checks) | Identical to upstream | A 4:2:0 frame of odd width loses its last chroma column against every other fork path, and `vmafx_frame_wrap_host()` would refuse what the engine accepts, so the conformance test could not compare them | Contradicts ADR-1483 and the conformance rule |
| Skip the port; point callers at `vmafx_frame_wrap_host()` | No new libvmaf surface | Code written for upstream's libvmaf does not build against the fork; a coverage gap stays open | The fork keeps libvmaf source compatibility for released and upcoming upstream calls |
| Compat body without a thunk, passing upstream's internal picture to the callback | Same callback argument as upstream | The compat library cannot reach the engine's picture (it links exported `vmafx_` symbols only, ADR-2094) | Not possible without exporting engine internals |
| A second construction routine in the engine next to `vmafx_frame_bind()` | Smaller diff in `frame_host.c` | Two implementations of one behaviour (HISS-19) | One routine serves both |
| Bump the libvmaf SONAME minor | Signals the addition | ADR-1151 keeps the SONAME for interface breaks; ADR-1822 added functions without a bump | Consistent with the precedent |

## Consequences

- **Positive**: callers ported from upstream's libvmaf can score decoder frames
  without a copy; wrapped pictures score bit for bit what allocated copies
  score (`test_picture_wrap_api`); the engine and the compat body are compared
  by `test_compat_conformance` (refusals, every format, 8 to 16 bits, a scored
  pair and its releases).
- **Negative**: a caller relying on upstream accepting a short stride, a NULL
  plane or a rounded-down chroma row gets `-EINVAL`; the callback's picture has
  NULL `ref` and `priv`. Both are documented in `docs/api/pictures.md`.
- **Neutral / follow-ups**: the FFmpeg patch series needs no edit: FFmpeg
  n9.0.2's `vf_libvmaf.c` calls `vmaf_picture_alloc()` / `vmaf_picture_unref()`
  only, and no patch in `ffmpeg-patches/` names a wrap call. A zero-copy filter
  path would be a separate change. Wide-stride pictures whose last row ends
  short of a full stride rely on Netflix/vmaf `9f4bd165f` (integer VIF copies
  each row's samples), on master since #2668.

## References

- req (upstream sync brief, 2026-10-09; paraphrased): port the needed upstream
  commits; the new public `vmaf_picture_wrap` needs documentation, including an
  FFmpeg patch impact check and `docs/api`, and an ADR if its API shape needs a
  decision.
- Netflix/vmaf `700124a4c` "libvmaf: add vmaf_picture_wrap api".
- [ADR-1483](1483-odd-size-chroma-planes-round-up.md),
  [ADR-1822](1822-additive-picture-convert.md),
  [ADR-1852](1852-vmafx-api-redesign.md),
  [ADR-2094](2094-libvmaf-compat-library-split.md).
