<!-- markdownlint-disable MD013 -->
# Research-3402: Semgrep parse limits in the GPU test sources

- **Status**: Active
- **Workstream**: [issue 3252](https://github.com/VMAFx/vmafx/issues/3252); the CPU half is the companion fix for the same issue
- **Last updated**: 2026-11-08

## Question

The float-identity Semgrep rule in `.semgrep.yml` reported a parse
diagnostic on 16 CUDA, HIP, SYCL and Vulkan test sources, so the rule could
not read the unparsed regions. Each file carried a row in
`.config/lint-exceptions.d/vmaf-test-float-identity-parse.toml`. Which
constructs does the Semgrep C and C++ parser refuse, and which spelling of
the same code does it read without changing what the compiler builds?

## Method

- Semgrep 1.183.0 with the repository's `.semgrep.yml`, run per file; a file
  passes when the JSON output has an empty `errors` array.
- Each rewrite is checked with `clang-format` (the repository's
  `.clang-format`), because a formatted spelling the parser refuses would
  come back on the next `make format`.
- The rewritten tests are built and run on the GPU backends they cover.
  With the 16 rows removed, `scripts/ci/tests/test_semgrep_float_identity.py`
  accepts an exception file with no rows left, so the file can be deleted
  once the CPU half lands.

## Findings

| Construct the parser refuses | Spelling it reads | Files |
| --- | --- | --- |
| A string literal spliced with an object-like macro, `"no " LZ_BACKEND " device"` | Pass the macro as a `%s` argument, or wrap the splice in a function-like macro such as `LZ_DIFFERS_MSG(b)` | `test_gpu_speed_lanczos4_parity.c`, `test_sycl_fp_arith_contract.c`, `test_sycl_adm_parity.c` |
| A qualifier macro before the return type, `FADM_HD void f(` or `extern "C" VMAF_WRAP_EXPORT void *f(` | A function-like macro that spells the whole head, `FADM_HD_FN(f)(`, or the attribute written out, `__attribute__((visibility("default")))` | `hip_float_adm_math_sample.h`, `test_sycl_init_unwind.cpp` |
| A macro that expands to a whole attribute, `[=](sycl::id<1> i) VMAF_SYCL_REQD_SG_SIZE(16) {` | Literal `[[ ]]` around a macro for the attribute's contents, `[[VMAF_SYCL_REQD_SG_ATTR(16)]]`, or `[[VMAF_SYCL_FUNCTOR_SG_ATTR(16)]]` on a functor's `operator()` | `test_sycl_float_adm_math_probe.cpp`, `test_sycl_float_vif_math_probe.cpp`, `test_sycl_integer_vif_math_probe.cpp`, `test_sycl_ordered_sum_probe.cpp` |
| A C variable named after a C++ keyword, `const uint64_t virtual = ...` | A different name, `start_virtual` | `test_vmafx_import_fence.c` |
| A calling-convention macro between type and name, `static CUresult CUDAAPI f(` | The declarator in parentheses, `static CUresult(CUDAAPI f)(`, which is also what clang-format writes | `test_cuda_runtime_unwind.c`, `test_cuda_float_ms_ssim_host_traffic.c` |
| The same for a `void` host function, `static void(CUDAAPI f)(` | A declaration through the driver's typedef, `static CUhostFn f;`, then a plain `static void f(void *arg)` definition | `test_vmafx_import_cuda_fence.c` |
| A comma expression inside a `&&` chain, `(s.clip = clip8, true) &&` | The assignment as its own statement | `test_vmafx_import_cuda.c`, `test_vmafx_import_hip.c` |
| `#if` / `#else` splitting one initializer list | One complete declaration per branch | `vmafx_vulkan_test_util.h` |

### Calling convention

`CUDAAPI` is `__stdcall` on `_WIN32` and empty elsewhere
(`ffnvcodec/dynlink_cuda.h`). Dropping it would change the ABI of every
fake driver entry point on 32-bit Windows, so each rewrite keeps it. For
host functions, the `CUhostFn` typedef
(`typedef void CUDAAPI CUhostFn(void *userData);`) carries the convention,
and a later definition without it takes the declared one. A probe built for
`i686-pc-windows-msvc` and `i686-w64-windows-gnu` compiles only with the
declaration present; x86-64 and Linux targets accept either form.

### Empty attribute under AdaptiveCpp

`VMAF_SYCL_REQD_SG_ATTR(N)` expands to nothing under AdaptiveCpp, which
leaves `[[ ]]`. An attribute-specifier with an empty attribute list is valid
C++11 and later. Probes of both positions, on a functor's `operator()` and
after a lambda's parameter list, compile with clang 23.1.4, GCC 16.2.1 and
`icpx` at `-std=c++17 -pedantic -Wall -Wextra -Werror`.

### Contract test

`core/test/test_sycl_sub_group_size_contract.py` reads the sub-group
attribute of each kernel functor. With the attribute now spelled
`[[VMAF_SYCL_FUNCTOR_SG_ATTR(N)]]`, the test was extended so that a spelling
it cannot read is reported, not skipped:

- a functor whose shape size is a `constexpr` function call, such as
  `vif_hori_sg_size(SCALE)`, still needs an attribute on its call operator;
- every `operator()` overload of a functor needs its own attribute;
- a size that is no literal, constant or `constexpr` function of the file is
  a failure;
- a `VmafSyclKernelShape` use that is not the base of a class or struct the
  test reads is a failure.

Planted tests cover each case, and each fails when its check is removed.

## Reproducer

```bash
semgrep --config .semgrep.yml --json core/test/test_vmafx_import_cuda_fence.c \
  | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["errors"]))'
python3 scripts/ci/tests/test_semgrep_float_identity.py
python3 core/test/test_sycl_sub_group_size_contract.py
```
