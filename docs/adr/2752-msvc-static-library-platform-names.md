<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2752: A static MSVC build installs vmaf.lib and vmafx.lib

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: lusoris
- **Tags**: build, windows, packaging, upstream-port

## Context

Meson names a static library `libNAME.a` on every platform unless told
otherwise. An MSVC, clang-cl or icx-cl build of this tree therefore installed
`libvmaf.a` and `libvmafx.a`. The MSVC linker, and FFmpeg's MSVC toolchain
that turns the pkg-config `-lvmaf` into a file name, open `vmaf.lib`, so a
consumer had to rename the files first. Upstream Netflix/vmaf `3b4dd350e`
installs `vmaf.lib` for a static MSVC build (`vmaf-static.lib` beside the
import library when both kinds are built) and drops the rename from its FFmpeg
job. Meson 1.10 added a `namingscheme` option whose `platform` value does the
same, but this project requires only Meson 1.4.0.

## Decision

We will name the installed static libraries of an MSVC-like build
(`cc.get_argument_syntax() == 'msvc'`) `vmaf.lib` and `vmafx.lib` when the
build is static only, through explicit `name_prefix` / `name_suffix` keywords
(`vmaf_static_name_kwargs` in `core/src/meson.build`). Every other build keeps
Meson's names. `scripts/ci/check_msvc_library_names.py` checks the installed
files and pkg-config files on the MSVC CUDA, SYCL and ARM64 legs.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Explicit names for static-only MSVC builds (chosen) | Works with the current Meson floor; names what consumers look for | One conditional in the build | — |
| `namingscheme=platform` | Meson's own mechanism | Needs Meson 1.10; raising the floor from 1.4.0 affects every build host | Floor stays (maintainer: explicit names when the floor is below 1.10) |
| Upstream's split into `shared_library()` + `static_library()` with `vmaf-static.lib` | Also names `both` builds | Rewrites the fork's `library()` target, whose split halves the tests and the compat library already use | MSVC builds are static only (ADR-0121); `both` keeps the classic static names, which cannot collide with the import library |
| Keep the classic names | No change | Every MSVC consumer renames the files | The reason for the change |

## Consequences

- **Positive**: `-lvmaf` and `-lvmafx` resolve on MSVC without renaming;
  the planned MSVC FFmpeg job links the installed files directly.
- **Negative**: Windows static file names differ between MSVC (`vmaf.lib`)
  and MinGW (`libvmaf.a`) builds.
- **Neutral / follow-ups**: if the Meson floor reaches 1.10, the option can
  replace the explicit keywords.

## References

- Q-299 (maintainer, 2026-10-08): "D2: platform names on MSVC — vmaf.lib /
  vmafx.lib (verify the Meson floor for namingscheme=platform first; explicit
  names otherwise). Update docs (Windows build guide) and the consumer checks
  (upstream-consumers / pkg-config)."
- Upstream Netflix/vmaf `3b4dd350e` ("MSVC: install static library as
  vmaf.lib").
- Meson 1.10.0 release notes, "Added new `namingscheme` option".
- [ADR-0121](0121-windows-gpu-build-only-legs.md) — static MSVC builds.
