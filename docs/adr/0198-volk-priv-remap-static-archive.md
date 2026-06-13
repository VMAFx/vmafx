# ADR-0198: Rename volk's `vk*` symbols to `vmaf_priv_vk*` for static-archive builds

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, build, fork-local, abi
- **Supersedes**: nothing — extends [ADR-0185](0185-vulkan-hide-volk-symbols.md)
  to the case `--exclude-libs,ALL` cannot reach.

## Context

[ADR-0185](0185-vulkan-hide-volk-symbols.md) hid volk's `vk*` /
`volk*` symbols from `libvmaf.so`'s public ABI by passing
`-Wl,--exclude-libs,ALL` to the shared-library link step. That
flag works because GNU ld applies it during the `gcc -shared`
link that produces `libvmaf.so.3.0.0`.

Static archives (`.a`) are not produced by a link step — meson
runs `ar rcs libvmaf.a *.o` and concatenates the per-target
object files. `--exclude-libs` has nothing to bind to. Result:
`libvmaf.a` keeps every `vk*` PFN dispatcher volk defines as a
**STB_GLOBAL** symbol (700+ entries from the bundled volk-1.4.341
loader).

The collision shows up in **fully-static link environments**
that consume both libvmaf and the official Vulkan loader:

```text
$ gcc -static main.c libvmaf.a libvulkan.a -ldl
ld: volk.c.o (symbol from plugin):
    multiple definition of `vkGetInstanceProcAddr';
    libvulkan.a(loader.c.o): first defined here
[...700+ more, one per Vulkan API entry point...]
```

This is exactly the failure lawrence reported on 2026-04-27 on
his BtbN-style cross-toolchain `glibc-2.28` static FFmpeg build
when adding `--enable-libvmaf` with libvmaf compiled as
`default_library=static`.

## Decision

Rename volk's `vk*` symbols to `vmaf_priv_vk*` at the C
preprocessor level, via a force-included header:

1. The volk packagefile generates `volk_priv_remap.h` from the
   bundled `volk.h` (parses every
   `extern PFN_vkXxx vkXxx;` declaration and emits
   `#define vkXxx vmaf_priv_vkXxx`). Generation is keyed on
   `volk.h` so the remap tracks the wrap version automatically.
2. The packagefile compiles `volk.c` with
   `-include <build>/volk_priv_remap.h` so volk's PFN definitions
   come out as `vmaf_priv_vkXxx`.
3. `volk_dep` exports the same `-include` flag in its
   `compile_args`, propagating it to every libvmaf TU that calls
   `volk.h` (via the meson dependency graph).

The `PFN_vkXxx` typedef name is a different identifier from
`vkXxx`, so the C preprocessor leaves typedefs alone. Only the
**variable / call-site** names change. volk's own `dlsym(...,
"vkGetInstanceProcAddr")` lookups use string literals, not
identifiers, so the runtime loader path is unaffected.

Net effect:

| symbol class                        | shared `libvmaf.so.3.0.0` | static `libvmaf.a` |
|-------------------------------------|--------------------------:|-------------------:|
| `vk*` exported (before this ADR)    | 0 (ADR-0185)              | 700+               |
| `vk*` exported (after this ADR)     | 0                         | **0**              |
| `vmaf_priv_vk*` internal-bound      | 627                       | 719                |
| `volk*` (kept, unrenamed)           | 0 / hidden                | ~30                |

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Force-include `volk_priv_remap.h` (chosen)** | Fixes both shared and static cases identically; pure CPP — no objcopy / partial-link / linker-script gymnastics; tracks volk version automatically | Force-include is non-obvious to a casual reader; missing a `vk*` declaration in volk.h would silently leave it un-renamed | Cleanest solution; the volk header pattern is uniform enough that the regex catches every entry point (verified: 784 declarations → 784 remaps for volk-1.4.341) |
| **Partial-link + `objcopy --localize-symbol` for static builds only** | Surgical to the static case | Requires bypassing meson's `library()` for static builds with a `custom_target` chain (`ld -r` → `objcopy` → `ar`); duplicates the link step; brittle in cross-compile lanes | Significant build-system restructuring; outcome equivalent to the chosen approach, with more failure modes |
| **Pass `--allow-multiple-definition` to consumer** | Zero libvmaf changes | Punts the problem to every downstream user; "disabling the symptom is not a bugfix" — explicit user direction | Rejected on user direction 2026-04-27 |
| **Drop volk; use `dlsym` per-symbol in libvmaf** | Removes the bundled loader entirely | Major refactor of [`core/src/vulkan/common.c`](../../core/src/vulkan/common.c)'s init path; loses volk's per-instance / per-device PFN tables | Out of scope for a build-system fix; revisit only if volk causes other issues |
| **Compile volk with `-fvisibility=hidden`** | Standard ELF approach for shared libs | Visibility attributes have no effect on STB_GLOBAL symbols inside `.a` archives — they only matter at the shared-library link step that ADR-0185 already covers | Doesn't address the static case |

## Consequences

- **Positive**: BtbN-style fully-static FFmpeg builds that link
  `libvmaf.a` next to the Khronos `libvulkan.a` succeed at link
  time. lawrence's repro (cross-toolchain `glibc-2.28` build)
  goes from ~700 multi-def errors to a clean link.
- **Positive**: identical fix for shared and static — no
  conditional code paths in the volk packagefile, no
  per-build-mode meson branches.
- **Positive**: tracks the upstream wrap version. Bumping volk
  to a future SDK release re-runs the generator at configure
  time and picks up new entry points automatically.
- **Negative**: a build-system reader who greps for
  `vkCreateInstance` in libvmaf's compiled object files won't
  find it under that name; they have to know the remap is
  active. A short note in
  [`docs/backends/vulkan.md`](../backends/vulkan.md) and the
  packagefile comment block point future maintainers at this
  ADR.
- **Negative**: if a future volk drop introduces a declaration
  pattern not matching the regex
  (`^extern\s+PFN_vk\w+\s+(vk\w+)\s*;`), those symbols stay
  un-renamed and would leak. Mitigation: the generator emits
  the rename count to stdout, and a CI check on the
  Linux Vulkan static lane greps libvmaf.a for any leftover
  `T vk[A-Z]` symbols (follow-up).
- **Neutral / follow-ups**:
  1. Add a static-archive lane assertion: `nm libvmaf.a | grep
     -E '^[0-9a-f]* (T|D|B) vk[A-Z]'` must return zero matches
     when Vulkan is enabled. Lands in the existing Vulkan
     build job in
     [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml).
  2. Re-run the BtbN-style link simulation in `make test-fast`
     so a regression in the rename mechanism fails locally.

## References

- Source: lawrence's chat report 2026-04-27 — full
  cross-toolchain build log captured at
  `.workingdir2/stderr.log` lines 1282-2000+ shows the
  multi-def cascade.
- Pre-fix verification:
  `nm libvmaf.a | grep -cE '^[0-9a-f]* (T|D|B|R) vk[A-Z]'`
  on `default_library=static -Denable_vulkan=enabled` returned
  ~700 before this commit; after, returns `0`.
- Post-fix verification:

  ```text
  $ gcc -static main_stub.c libvmaf.a libvulkan-stub.a \
        -lstdc++ -lm -lpthread -ldl -o btbn_sim
  $ echo $?
  0
  ```

  (See PR description for the full repro script.)
- Vulkan smoke test (`test_vulkan_smoke`) still passes 10 / 10
  on the renamed build — runtime-loaded entry points dispatch
  through `vmaf_priv_vk*` PFNs identically.
- Parent: [ADR-0185](0185-vulkan-hide-volk-symbols.md) —
  shared-library symbol hiding via `-Wl,--exclude-libs,ALL`.
- User direction: 2026-04-27 — "fix it. disabling is not a
  bugfix" (paraphrased from chat).
