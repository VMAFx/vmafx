# Research-2078: MSVC-compatible strict-FP flags — 2026-09-23

## Finding

The strict floating-point carve-outs used Unix compiler options even when
Meson selected an MSVC-compatible driver. Current hosted evidence on master
made both failure modes observable:

- `Windows MSVC+CUDA` and `Windows MSVC+CUDA (full)` passed
  `-ffp-contract=off` through NVCC to `cl.exe`; MSVC emitted D9002 and ignored
  it for every affected translation unit.
- `Windows MSVC+SYCL` used `icx-cl`, which emitted `unknown argument ignored`
  for both `-fp-model=precise` and `-ffp-contract=off`.

The builds remained green because both drivers treat unknown options as
warnings. That made this diagnostic debt rather than evidence that the
bit-exactness policy was active. The same raw literals occurred in the x86
SIMD libraries, three scalar-reference libraries, the SIMD test executables,
and the host-side options forwarded by nvcc. The AArch64 carve-outs had a
second compiler-ID mapping that covered MSVC but would still send the Unix
spelling to clang-cl.

Evidence: GitHub Actions run
[35898349556, MSVC+CUDA job](https://github.com/VMAFx/vmafx/actions/runs/35898349556/job/107307844177),
[35898349556, MSVC+SYCL job](https://github.com/VMAFx/vmafx/actions/runs/35898349556/job/107307844120),
and [35898349439, full CUDA job](https://github.com/VMAFx/vmafx/actions/runs/35898349439/job/107307841817).

## Compiler contract

One policy in `core/src/meson.build` now feeds every production carve-out and
the test build. It separates the general Intel value-safety model from the
strict no-contraction list. A host-system branch separately forwards the
native host spelling through nvcc: `/fp:precise` to `cl.exe` on Windows and
`-ffp-contract=off` elsewhere.

| Meson compiler ID | General model arguments | Strict-FP arguments |
| --- | --- | --- |
| `gcc`, `clang`, other Unix drivers | none | `-ffp-contract=off` |
| `intel-llvm` | `-fp-model=precise` | `-fp-model=precise -ffp-contract=off` |
| `msvc` | none | `/fp:precise` |
| `intel-llvm-cl` | `/fp:precise` | `/fp:precise /Qfma-` |
| `clang-cl` | none | `/clang:-ffp-contract=off` |

The Intel Linux ordering remains load-bearing: `-fp-model=precise` comes
before `-ffp-contract=off`. Intel documents `icx-cl` as its MSVC-style driver,
documents `/fp` as the Windows spelling of `-fp-model`, and documents `/Qfma-`
as requiring separate multiply and add instructions with intermediate
rounding. Microsoft documents that Visual Studio 2022 and newer leave
contraction off under `/fp:precise`. clang-cl accepts the Clang spelling only
when it is forwarded through `/clang:`. The same mapping now covers AArch64,
so a future clang-cl ARM64 build cannot regress to the ignored Unix spelling.

Primary references:

- [Intel oneAPI compiler drivers](https://www.intel.com/content/www/us/en/docs/dpcpp-cpp-compiler/developer-guide-reference/2026-0/invoke-the-compiler.html)
- [Intel `fma`, `/Qfma` option](https://www.intel.com/content/www/us/en/docs/dpcpp-cpp-compiler/developer-guide-reference/2026-0/fma-qfma.html)
- [Intel floating-point optimization model](https://www.intel.com/content/www/us/en/docs/dpcpp-cpp-compiler/developer-guide-reference/2026-0/floating-point-optimizations.html)
- [Microsoft `/fp` behavior](https://learn.microsoft.com/en-us/cpp/build/reference/fp-specify-floating-point-behavior?view=msvc-170)

## Alternatives considered

Using `cc.get_supported_arguments()` was rejected because MSVC returns success
while warning D9002 for some unknown options; a configure probe would bless the
same ignored flag. `/fp:strict` was rejected because it also enables floating-
point environment and exception semantics, a broader numerical change than
the existing precise-plus-no-contraction contract. Duplicating the mapping in
`core/test/meson.build` or retaining a separate AArch64 mapping was rejected
because production and scalar reference objects must never acquire different
contraction policies. Deriving nvcc's host flag from the main C compiler ID
was rejected because the Windows CUDA build explicitly selects `cl.exe` as
nvcc's host compiler; the host operating system is the controlling fact.

No new ADR is needed: this is the compiler-spelling completion of the policy
already recorded by ADR-1260 and the open bug row, not a new architecture or
scope decision.

## Verification

`core/test/test_strict_fp_compiler_args.py` executes the shipped Meson policy
for six compiler IDs and both host-system mappings, and asserts that all
eighteen strict consumers, the two CUDA kernels, and the SIMD test arguments
use the shared lists. The test failed against the old raw literals before the
implementation was added. After rebasing onto master through PR #1528, the GCC
release build passed 145 of 145 combined `fast` and `simd` tests and the
complete configured suite passed 159 of 159. Intel LLVM 2026.0 passed the six
strict-FP-sensitive tests. A fresh AArch64 GCC cross-build compiled all 1,495
steps and passed 19 of 19 SIMD tests under QEMU. Both affected CUDA fatbins
compiled with nvcc 13.4 and retained the Unix host spelling on Linux. Hosted
Windows acceptance remains the PR check: the three jobs above must contain no
`-ffp-contract=off` D9002/unknown-argument diagnostic after this change.
