<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3396: zimg is on by default

- **Status**: Accepted
- **Date**: 2026-10-31
- **Deciders**: lusoris (maintainer decision vmafx-zimg-default-20261031)
- **Tags**: build, zimg, meson, hdr, dependencies

## Context

The VMAF CLI automatically selects the HDR model `hdr_vmaf_v1.1.8_3d0h` when both reference and distorted inputs have all colour properties set (`color_all_set`), both inputs have transfer characteristics set to SMPTE 2084 / PQ (`settings->color_ref.trc == VMAF_COLOR_TRC_SMPTE2084 && settings->color_dist.trc == VMAF_COLOR_TRC_SMPTE2084`), and no explicit model is specified (`core/tools/cli_parse.cpp:1367-1375`). In a default build prior to this change, `enable_zimg` was `false` ([ADR-1822](1822-additive-picture-convert.md)), which caused picture colour conversion to fail closed with `-95` (`-ENOTSUP`), requiring the user to explicitly reconfigure with `-Denable_zimg=true`.

Default builds must work out-of-the-box for discoverable CLI behaviour. When HDR inputs are presented to a default binary, video quality scoring should proceed directly without forcing a manual recompile.

## Decision

The Meson option `enable_zimg` is now `true` by default:

- `core/meson_options.txt` sets `value: true` for `enable_zimg`.
- In `core/src/meson.build`, `dependency('zimg', version: '>=2.7', required: false)` is probed. When enabled and not found, Meson halts immediately via `error()` stating that `zimg >= 2.7` was not found via pkg-config and recommending either installing system packages (`libzimg-dev` on Linux, `brew install zimg` on macOS, `vcpkg install zimg:x64-windows-static` on Windows) or passing `-Denable_zimg=false`.
- An explicit `-Denable_zimg=false` continues to build cleanly without zimg; in that configuration, HDR colourspace conversion fails closed with `-ENOTSUP` and logs a descriptive error message directing the user to rebuild with `-Denable_zimg=true`.
- Every published container image and distribution bundle carries `libzimg` (`libzimg2` runtime package or static linkage).
- Licensing companion structures record zimg 3.0.6 under the WTFPL licence, and the licensing gate accepts WTFPL by an explicit classifier entry in `tools/rc1-tester/image/licensing.py`.
- This decision supersedes only the fourth bullet of [ADR-1822](1822-additive-picture-convert.md) ("zimg is an opt-in Meson option, `enable_zimg` (boolean, default `false`, as upstream)"). The remainder of ADR-1822 stands unchanged, including the public API shape (`vmaf_picture_convert_context_init_with_color()` and unmodified `VmafPicture`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `auto` feature for `enable_zimg` | Does not fail configure if zimg is missing on the host. | Silent degradation: a build without zimg appears to succeed but later fails at runtime when evaluating HDR video, violating fail-closed predictability. | Refused. The build must either satisfy the dependency or fail fast at configure time with a clear actionable remedy. |
| Vendored subproject via Meson WrapDB | Self-contained build without host package dependencies. | WrapDB does not host a zimg wrap; upstream zimg uses autotools rather than Meson; maintaining an out-of-tree Meson port inside VMAFx imposes unacceptable maintenance overhead. | Refused. Upstream distro packages (`libzimg-dev`, Homebrew `zimg`, vcpkg `zimg`) are widely available and well-maintained. |
| Opt-in default (`false`, ADR-1822) | Zero external dependencies for minimal builds. | Breaks the default CLI HDR auto-selection workflow; fails with runtime error `-95`. | Superseded by this ADR. |

## Consequences

- **Positive**: Standard out-of-the-box builds can score HDR PQ video without manual options.
- **Negative**: Building from source now requires `zimg >= 2.7` headers and pkg-config unless `-Denable_zimg=false` is explicitly provided.
- **Neutral / follow-ups**: CI matrix legs and container builder stages install `libzimg-dev` / `zimg`; licence gate includes WTFPL in approved lists.
- Windows jobs install zimg through `scripts/ci/setup-zimg-windows.ps1`, which also installs `pkgconf` and names it in `PKG_CONFIG`, because Meson refuses the Strawberry Perl pkg-config on hosted runners. The vcpkg triplet follows the C runtime of the build: `-static-md` for `/MD`, `-static` for `-Db_vscrt=mt`.
- Tidy lanes install `libzimg-dev` when their pinned image lacks it (`scripts/dev/tidy-lane.sh`), and the arm64 lane opts out because its cross sysroot has no arm64 zimg.

## References

- Maintainer decision: `vmafx-zimg-default-20261031` (paraphrased: maintainer decided `enable_zimg` on by default, missing zimg halts `meson setup`, all images carry libzimg, WTFPL accepted in licensing gate, superseding ADR-1822's opt-in bullet).
- Prior ADR: [ADR-1822](1822-additive-picture-convert.md) (`vmaf_picture_convert` ships additively, with the source colour as an argument).
- CLI HDR auto-selection: `core/tools/cli_parse.cpp:1367-1375`.
