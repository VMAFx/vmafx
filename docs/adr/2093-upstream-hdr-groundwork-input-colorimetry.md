<!-- markdownlint-disable MD013 MD060 -->
# ADR-2093: HDR-VMAF groundwork from upstream, with the input colorimetry on the context

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: lusoris (maintainer popup answer, 2026-10-06, confirming the per-context function)
- **Tags**: api, abi, upstream-port, cli, models, zimg, fork-local

## Context

Netflix/vmaf merged seven commits on 2026-10-05 that prepare HDR-VMAF: per-input
colorimetry flags for the CLI (`ed61076b2`, #1671), an optional
`conversion_target` in the model file (`1ddf81607`, #1672), conversion of the
pictures to that target inside `vmaf_read_pictures()` (`a6c0ba6d5`, #1673), a
Windows build fix of its test (`130569c45`, #1674), the Python harness
pass-through (`efe90c8b8`, #1675), approximate gamma in the zimg graph
(`5c3f4fb90`, #1677) and a lower CAMBI encode-size minimum (`4f3f71b68`, #1678).

Upstream carries the source colour of each picture in `VmafPicture::color`, the
member [ADR-1822](1822-additive-picture-convert.md) declined to add because it
moves `ref` and `priv` (HISS-14). The CLI attaches the parsed colour to every
fetched picture and `vmaf_read_pictures()` reads it from there, so two of the
seven commits cannot be taken verbatim.

## Decision

We port all seven commits. Six are verbatim (paths moved from `libvmaf/` to
`core/`, `cli_parse.c` / `vmaf.c` to the fork's C++ files, long functions split
for HISS-04). For the seventh's colour carrier we add one function to the public
API:

`int vmaf_set_input_colorimetry(VmafContext *vmaf, const VmafColor *ref, const VmafColor *dist)`

declares the colorimetry of the reference and of the distorted input once, on
the context. `NULL` leaves an input unspecified. It returns `-EBUSY` once a
conversion context exists, because zimg builds its graph from the first picture
and its colour. The CLI calls it after `vmaf_init()` with the flags it parsed.
`conversion_policy.c` is upstream's, with the colour of each picture as an
argument where upstream reads `pic->color`. The glue upstream keeps as
statics in `libvmaf.c` (target registry, zimg contexts, picture replacement) is
`core/src/conversion_context.c`, so `libvmaf.c` gains four call sites and the
conversion state is testable apart from the context.

Fork adaptations of upstream's behaviour, all in `vmaf_read_pictures()`:

- The conversion runs before validation and before any device upload, on the
  host. A picture in device memory is refused with `-ENOTSUP`, because zimg
  reads host planes. Converting on the device is not part of this.
- Upstream returns with the pictures left to the caller when the conversion
  fails; since [ADR-1431](1431-read-pictures-owns-pictures-on-every-return.md) the context owns them whatever the result, so a failed
  conversion releases both.
- The missing-colour message names the real flags (`--color_range_ref/_dist`,
  ...); upstream's names flags that do not exist.
- A repeated `conversion_target` key resets the block before it is parsed again.
- The CAMBI minimum (`enc_width`, `enc_height` from 180 / 150 to 144) is
  applied to the CUDA, HIP, SYCL and Metal twins' option tables as well, so a
  twin does not refuse a size the CPU extractor accepts.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `VmafColor color` in `VmafPicture` (upstream's layout) | Verbatim; no new API | ABI break for every consumer (ADR-1822) | The additive rule stands until a major bump |
| Colour as a parameter of a new `vmaf_read_pictures_with_color()` | Per-frame colour possible | Two read entry points to keep in step with the GPU and zero-copy variants; every caller changes | A stream's colorimetry is constant; one setter is enough |
| Colour in `VmafPicture2::_reserved` | No new function | Not on the `vmaf_read_pictures()` path; spends reserved space (ADR-1822) | Same reasons as in ADR-1822 |
| Keep the glue in `libvmaf.c` as upstream does | Smaller diff to upstream | `libvmaf.c` is 5000 lines and under rewrite by RC4; the state could not be unit-tested | A separate unit is the smaller conflict surface |
| Skip the CAMBI twin tables | Verbatim | Twins refuse sizes the CPU accepts | Twin option parity |

## Consequences

- **Positive**: HDR-VMAF models can be loaded and scored on the CPU path with
  zimg; the public surface only grows (HISS-14); the layout guard of ADR-1822
  still holds.
- **Negative**: a frame-varying colour is not expressible; the colour of a
  `VmafPicture` stays unset, so a caller migrating from upstream's API calls the
  setter instead of assigning `pic.color`. The glue differs from upstream's
  `libvmaf.c`, so a sync of `a6c0ba6d5`'s `libvmaf.c` hunks is not applicable
  (rebase-notes entry).
- **Neutral / follow-ups**: zimg stays an opt-in dependency that no image or CI
  lane installs, so the conversion tests run only where it is installed
  (`docs/state.md`, `T-ZIMG-OPT-IN-UNPINNED-2026-10-06`). When upstream
  releases `VmafPicture::color` with an ABI-breaking major bump, the setter
  becomes the default for pictures whose colour is unset.

## References

- Maintainer popup answer, 2026-10-06, `Q: "Per-context function (Recommended)"` (paraphrased: declare the
  input colorimetry once on the context with a function, not per picture).
- Task brief of 2026-10-06 (paraphrased: port the Netflix commits merged on
  2026-10-05 that the fork lacks, keep the additive rules and the rebase
  invariants, bring the HDR dock fixtures in through the md5-verified fetch).
- Netflix/vmaf `ed61076b2`, `1ddf81607`, `a6c0ba6d5`, `130569c45`, `efe90c8b8`,
  `5c3f4fb90`, `4f3f71b68`.
- [ADR-1822](1822-additive-picture-convert.md), [ADR-1431](1431-read-pictures-owns-pictures-on-every-return.md)
  (the context owns the pictures of `vmaf_read_pictures()` on every return).
