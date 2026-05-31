# ADR-0185: Hide volk / Vulkan-loader symbols from libvmaf's public ABI

- **Status**: Accepted
- **Date**: 2026-04-26
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, build, fork-local, abi

## Context

When libvmaf is built with `-Denable_vulkan=enabled`, the bundled
volk Vulkan-loader (a static-archive function-pointer dispatcher,
[volk on GitHub](https://github.com/zeux/volk)) gets folded into
`libvmaf.so` along with its `vk*` and `volk*` entry points. Those
symbols have default visibility, so they end up in the shared
library's public ABI:

```text
$ nm -D libvmaf.so.3.0.0 | grep volk
T volkFinalize
T volkGetInstanceVersion
T volkGetLoadedDevice
T volkInitialize
[…etc, ~30 entry points]
```

Plus volk's `vk*` thunks. This is invisible to most consumers
(libvmaf's `pkg-config --libs libvmaf` adds libvmaf.so and a
small downstream binary doesn't care about a few extra exported
symbols). It bites in **one specific environment** that matters
to the fork's user base:

**Static FFmpeg builds** (BtbN-style cross-toolchain releases,
Docker static images, and similar all-static link environments)
that link **both** libvmaf-with-Vulkan **and** libvulkan.a/.so
into the same final binary. Both contributors define every
Vulkan entry point (`vkGetInstanceProcAddr`,
`vkCopyMemoryToImage`, …); the GNU linker rejects with:

```text
/opt/ffbuild/lib/libvulkan.a(loader.c.o):
  multiple definition of `vkGetInstanceProcAddr';
volk.c.o (symbol from plugin): first defined here
```

— exactly the failure lawrence reported in chat on 2026-04-26
on his glibc-2.28 / BtbN-style static FFmpeg build.

The collision is fundamental to the volk-vs-libvulkan-loader
choice: both are full Vulkan loaders. They can coexist only
when one is internal (private to a single .so) and the other is
the publicly-resolved one. libvmaf's volk usage is purely
internal — the kernels in `core/src/feature/vulkan/` call
`vkXxx` only inside libvmaf — so volk's symbols never need to
be exported.

## Decision

Pass `-Wl,--exclude-libs,ALL` to libvmaf.so's link step.
GNU-ld-specific flag that hides every symbol coming from a
static archive on the link line; symbols compiled directly from
libvmaf source TUs (`vmaf_*` and the kernel-internal `vk*`
thunks via volk) keep their original visibility.

Net effect on libvmaf's public ABI:

- **Before**: 80+ exported `volk*` and `vk*` symbols leaking
  from the bundled volk static archive.
- **After**: 0 exported `volk*` / `vk*` symbols. Only
  libvmaf's own `vmaf_*` API surface remains public.

Linker compatibility:

- **GNU ld** (Linux, mingw-w64): `-Wl,--exclude-libs,ALL`
  honoured.
- **Apple ld** (macOS): no equivalent flag with the same
  semantics. The Vulkan backend isn't shipped on macOS today
  (no `-Denable_vulkan=enabled` CI lane), so we gate the flag
  off on Darwin to avoid a "unknown linker option" warning.
- **MSVC link.exe** (Windows): symbols from static archives
  aren't auto-exported by default; the equivalent issue
  doesn't arise. Gate off the flag on Windows for the same
  reason.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `-Wl,--exclude-libs,ALL` (chosen) | Two lines in meson.build; covers volk, VMA, and any future static-archive Vulkan dep without listing each one; widely-supported GNU-ld feature | Linux/mingw only | Best signal-to-cost ratio. macOS and Windows don't have the conflict in the first place. |
| Per-archive `-Wl,--exclude-libs,libvolk.a:libvk-mem-alloc.a` | More explicit | List grows when we add static-archive deps; archive names depend on meson's wrap layout | Brittle — easier to use `ALL` and inherit broad protection |
| GNU ld version script (`--version-script vmaf.map`) | Most precise; lets us declare the exact public API once | Requires maintaining an explicit list of every `vmaf_*` export — drift between header and .map; touches every PR that adds a public function | Too much maintenance for a single-bug fix |
| Drop volk; link `libvulkan.so` dynamically with `dlopen` | Matches FFmpeg's model; eliminates the conflict at the source | Significant refactor of [`core/src/vulkan/common.c`](../../core/src/vulkan/common.c)'s init path; volk's whole-vulkan-API loader is replaced with hand-written `dlsym` for every entry we use | Too much code churn for a build-system fix |
| Mark volk symbols `__attribute__((visibility("hidden")))` at the C level | Per-symbol granularity | Requires editing volk's source (a vendored subproject); rebases when we bump volk; misses `vk*` symbols volk emits with its own attributes | Brittle — meson `link_args` is the right layer |

## Consequences

- **Positive**: BtbN-style static FFmpeg builds that link both
  libvmaf and libvulkan in one binary now succeed. Closes the
  symbol-collision gap lawrence hit on glibc-2.28 / gcc.
  Reduces libvmaf.so's public ABI surface — anyone relying on
  the leaked `volk*` / `vk*` symbols from libvmaf would need
  to switch to a real Vulkan loader, but no fork user ever
  meant for those to be public.
- **Negative**: Linux/mingw-only fix; macOS users theoretically
  exposed to the same collision pattern would need an
  Apple-ld-specific equivalent (none ships today). Windows
  isn't affected by this class of issue.
- **Neutral / follow-ups**:
  1. **Validation in CI**: add an `nm -D` post-build check on
     the Linux Vulkan lane that asserts no `vk*` / `volk*`
     symbols leak. Cheap regression gate.
  2. **macOS coverage**: if a future `enable_vulkan` macOS
     CI lane lands, port the equivalent
     (`-Wl,-hidden-l<name>` per archive, or `MachO export
     list`) to keep the symbol-hide invariant on Darwin.

## References

- Source: lawrence's chat report 2026-04-26 (BtbN-style static
  FFmpeg build with `--enable-libvulkan` failing at link time).
- Pre-fix verification: `nm -D libvmaf.so | grep volk` on
  build-vulkan-test before this commit shows ~30 leaked
  `volk*` symbols + the full Vulkan API.
- Post-fix verification: same command shows zero leaked
  symbols; `vmaf_*` API surface unchanged; smoke test +
  end-to-end psnr_vulkan on Arc A380 still match CPU scalar
  byte-for-byte (`psnr_y = 34.760779` on Netflix normal pair
  frame 0, identical to PR #125's reference value).
- Relevant: [GNU ld `--exclude-libs` docs](https://sourceware.org/binutils/docs/ld/Options.html).
