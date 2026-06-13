# ADR-1099: Propagate `-fsycl` via `sycl_dependency` to fix test-binary SIGSEGV

- **Status**: Accepted
- **Date**: 2026-06-07
- **Supersedes**: —
- **Superseded by**: —

## Context

`test_sycl_motion_add_uv_parity` crashed with SIGSEGV on every run after two
previous fix attempts (PRs #768, #796). The test was marked `should_fail: true`
in `core/test/meson.build` (ADR-1093) to keep CI green while the root cause
remained under investigation.

**Root cause 1 — missing `-fsycl` at link time for test executables**

SYCL source files are compiled with `icpx -fsycl`, which embeds SPIR-V device
images in the resulting `.o` files. At link time, `icpx` must also see `-fsycl`
to invoke `clang-offload-wrapper`, which registers those device images with the
SYCL runtime's `ProgramManager` during shared-library or executable
initialization. Without `-fsycl` at link, the registration step is silently
skipped.

When a test executable links `libvmaf.a` (the static archive containing all
SYCL-compiled `.o` files) without `-fsycl`, `ProgramManager::getDeviceKernelInfo`
receives a `CompileTimeKernelInfoTy` whose backing program was never registered.
This produces a null-dereference SIGSEGV at the first `queue.submit()` call —
specifically inside `ProgramManager::getDeviceKernelInfo` in `libsycl.so`.

The `libvmaf.so` shared library was unaffected because `vmaf_link_args`
(a library-only variable in `core/src/meson.build`) already carried `-fsycl`.
Test executables declared `sycl_dependency` but received no `-fsycl` from it,
so only the `.so` link was correct.

**Root cause 2 — wrong feature name in `vmaf_feature_score_at_index` calls**

After fixing root cause 1, a secondary failure appeared: `vmaf_feature_score_at_index`
returned `-EINVAL` (-22) for both the SYCL and CPU score queries.

The VMAF feature-name system (`core/src/feature/feature_name.c:vmaf_feature_name_from_opts_dict`)
transforms feature names when any non-default `VMAF_OPT_FLAG_FEATURE_PARAM` option
is active:

1. It replaces the raw name with its canonical alias
   (`VMAF_integer_feature_motion2_score` → `integer_motion2`,
    `VMAF_feature_motion2_score` → `float_motion2`).
2. It appends each active option's alias as a suffix
   (`motion_add_uv=true`, alias `mau`, type `BOOL` → appends `_mau`).

So with `motion_add_uv=true`, the feature-collector stores scores under
`integer_motion2_mau` (SYCL) and `float_motion2_mau` (CPU), not under the raw
`VMAF_*_score` names. The original test queried the raw names, obtaining "not
found" (-EINVAL) for every lookup.

Note: when no FEATURE_PARAM option is non-default, `opts_dict` is empty, the
`!sorted_dict` branch fires, and the raw name is stored unchanged — which is
why other parity tests that use default options query successfully with the raw
name.

## Decision

**Fix 1**: Move `-fsycl` from `vmaf_link_args` (library-only) into
`sycl_dependency.link_args` in `core/src/meson.build`. Every Meson target that
declares `dependencies: [sycl_dependency]` — the shared library, the static
library, and all SYCL test executables — now automatically receives `-fsycl` at
link time. The `vmaf_link_args` variable is retained as an empty list for future
library-only link flags.

**Fix 2**: Update `core/test/test_sycl_motion_add_uv_parity.c` to query the
correct aliased names:

- SYCL path (`motion_sycl`, `motion_add_uv=true`): query `"integer_motion2_mau"`
- CPU path (`float_motion`, `motion_add_uv=true`): query `"float_motion2_mau"`
- SYCL Y-only baseline (default options): query
  `"VMAF_integer_feature_motion2_score"` (raw name; no aliasing when all
  FEATURE\_PARAM options are at default)

**Fix 3**: Remove `should_fail: true` and the ADR-1093 bypass comment from
`core/test/meson.build`. The test must pass unconditionally on SYCL-capable
hosts and skip cleanly on hosts without a SYCL device.

**Fix 4**: Clean up lint warnings in the test file introduced by touching it:
remove unused `#include <errno.h>`, `<stdlib.h>`, `<string.h>`; split
`VmafPicture ref, dist;` multi-declaration statements; add NOLINT comments with
ADR-0141 citations for the two functions that exceed the size threshold due to
their unavoidable test-harness structure.

## Alternatives considered

**1. Per-target `-fsycl` in `core/test/meson.build`**: Add `link_args: ['-fsycl']`
to each SYCL test executable individually instead of centralizing in
`sycl_dependency`. Rejected: error-prone — every future SYCL test must
remember the flag, and omitting it produces a silent runtime crash rather than a
build error. Centralization in the dependency is the correct Meson idiom.

**2. Keep `vmaf_link_args` with `-fsycl` AND add it to `sycl_dependency`**:
This passes `-fsycl` twice to the library link. `icpx` accepts duplicate flags
silently, so it is technically safe, but it is redundant and confusing. Removing
it from `vmaf_link_args` is cleaner.

**3. Query raw feature names everywhere**: Instrument `vmaf_feature_collector_get_score`
to resolve aliases before lookup. Rejected: the aliased-name storage is load-bearing
behaviour that other parts of the system depend on (model scoring, score pooling);
changing it would have wide blast radius and is out of scope for a bug fix.

**4. Add an alias-resolution helper for tests**: Expose a helper that maps a raw
feature name + options to the aliased name at test time. Rejected: over-engineered
for a two-string constant fix; test code should document the aliasing contract
explicitly so future readers understand what name is stored.

## References

- req: user direction — reproduce locally, real fix, remove `should_fail`,
  draft PR (2026-06-07)
- PRs #768, #796 (prior incomplete attempts)
- ADR-1093 (test-disable policy that this ADR supersedes for this test)
- ADR-0989 (SYCL motion_add_uv implementation)
- ADR-0141 (touched-file lint-clean rule)
- ADR-0214 (cross-backend tolerance places=4)
- `T-SYCL-MOTION-ADD-UV-SIGSEGV-2026-06-07` in `docs/state.md`
