# Research-2091: restoring silent-reverted test seams

## Finding

The BUG-048 silent-revert ledger identified three historical test slices:

- `ba5cdec32` added a direct `VMAF_PIX_FMT_YUV400P` allocation test;
- `1bdbd29f2` added explicit `--precision` and `--sycl_device` parser cases;
- `8f884b1f6` added Metal registration smoke cases and Metal/HIP registration
  invariant notes.

At exact `origin/master` `4e6916d16ac57647105d14a47a6680117d6b5738`,
the YUV400P and explicit CLI cases were genuinely absent. The implementation
contracts still worked, but no focused test would fail if a later change
reintroduced chroma allocation for monochrome pictures, ignored an explicit
SYCL device, or drifted one of the three accepted precision modes.

The Metal part had a different current-tree answer. The historical commit
added six lookups, but `float_ansnr_metal` was later removed with the ANSNR
backend. All five surviving names are already covered by
`core/test/test_metal_kernel_registration.c`, and the newer
`core/test/test_metal_kernel_coverage_audit.c` checks the complete 17-extractor
inventory against the feature registry. Its temporal table also pins the two
motion descriptors from the historical slice. Duplicating those five lookups
in `test_metal_smoke.c` would add no observable contract and would split the
registration authority again. The invariant prose itself was still missing,
so it is restored against the current canonical tests; the matching HIP note
points at the existing comprehensive `test_hip_smoke.c` registration table.

## Coverage audit

| Contract | Current result | Action |
| --- | --- | --- |
| YUV400P public allocation geometry | Consumer tests allocate YUV400P, but none pins both zero chroma geometries and pointers on standard and odd dimensions through `vmaf_picture_alloc()` | Restore a focused public-seam test in `test_picture.c` |
| `--precision=max`, `legacy`, and numeric `6` | Default/compatibility tests cover indirect overrides, not the three explicit parser forms | Restore exact parser cases and resolved formats |
| `--sycl_device 3` | Backend selection covers default device 0 only | Restore explicit-device parsing and ensure SYCL remains enabled |
| Five live Metal registrations | Present in the dedicated eight-extractor registration test and the 17-kernel audit | Superseded; do not duplicate in runtime smoke |
| Registration invariant prose | Missing from Metal and HIP backend notes | Restore with current paths and authoritative test roles |

The YUV400P check records its geometry before unref and releases the picture
before asserting it, so a failed geometry assertion does not leak test-owned
memory. The standard and odd-size cases share one public-seam helper without
changing production code.

## Alternatives considered

| Alternative | Result |
| --- | --- |
| Cherry-pick the three historical commits | Rejected. Paths moved from `libvmaf/` to `core/`, Vulkan and Metal ANSNR were removed, and later test-structure rules changed. |
| Re-add all five Metal lookup functions to `test_metal_smoke.c` | Rejected. Current dedicated registration and full-kernel audit tests already assert the same names and temporal flags. |
| Restore only the two genuinely missing executable seams and update invariant authority | Chosen. It closes the live gaps while preserving one canonical registration inventory. |

No ADR is needed: the change restores tests and documentation for existing
behavior and makes no architectural, policy, public-surface, or runtime
decision. No Netflix golden assertion is touched.

## Red-cap and verification

Before the restoration, an exact-source sentinel failed with the missing
YUV400P function, four CLI functions, five historical Metal smoke functions,
and both backend invariant headings. That sentinel intentionally modeled the
historical file shape; the source audit above then established that the five
Metal functions were already semantically superseded elsewhere.

After restoring the live gaps, a clean CPU Meson build passed
`test_picture` and `test_cli_parse` (2/2). A repository-aware static audit
checks the five live historical Metal names in both the feature registry and
the current registration/audit sources, including temporal membership for
`float_motion_metal` and `integer_motion_metal`.

## Reproducer

Configure a clean CPU-only build and run the restored executable contracts:

```sh
meson setup /tmp/vmafx-bug048-tests core \
  -Denable_cuda=false -Denable_sycl=false -Denable_hip=false \
  -Denable_metal=disabled -Denable_dnn=disabled -Denable_mcp=false
meson compile -C /tmp/vmafx-bug048-tests
meson test -C /tmp/vmafx-bug048-tests test_picture test_cli_parse \
  --print-errorlogs
```

The full CPU fast-suite regression gate is:

```sh
meson test -C /tmp/vmafx-bug048-tests --suite fast --print-errorlogs
```
