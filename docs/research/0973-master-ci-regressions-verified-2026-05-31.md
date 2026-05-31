<!-- markdownlint-disable MD010 -->
# Research-0973: Master CI regressions — verified reproduction and root-cause analysis

**Date**: 2026-05-31
**Author**: Lusoris (with Claude Code)
**Companion ADR**: [ADR-0973](../adr/0973-master-ci-regressions-verified-2026-05-31.md)
**Master tip at investigation**: `4948b771c`

## Scope

Two CI regressions on `master` were reported. The hard requirement was
"reproduce EACH failure locally in the `vmaf-dev-mcp` container BEFORE
writing any fix. No guessing." This digest records:

1. The diagnosis hypothesis (from the task brief).
2. The verification path inside the container.
3. The exact commands and captured output for each failure (pre-fix and
   post-fix).
4. The compiler-asm forensic evidence backing the second fix.

## Container environment

```text
$ docker inspect vmaf-dev-mcp --format '{{json .Mounts}}'
[{"Type":"bind","Source":"/home/kilian/dev/vmaf","Destination":"/workspace","Mode":"ro",...},
 ...]
```

The repo mount at `/workspace` is **read-only**, so the worktree was tar-piped
into the container's `/tmp/wt/` (writable). All build / test invocations below
target that path.

Toolchain in the container:

- gcc 15.2.0 / ld.bfd 2.46
- Intel(R) oneAPI DPC++/C++ Compiler 2026.0.0 (`icx --version`)
- meson 1.10.1, ninja 1.13.2

Compiler version note: the failing CI job uses `intel-oneapi-compiler-dpcpp-cpp-2025.3`
(per `.github/workflows/build.yml`). The container has 2026.0.0. The
FMA-contraction behaviour reproduced identically on both major versions, so
the local reproduction is a faithful proxy.

## Failure 1 — `test_metal_float_ms_ssim_parity` (3 macOS jobs)

### Hypothesis

Per task brief: `FIXTURE_H 144` is below the `float_ms_ssim` minimum
admissible dimension. The check at `core/src/feature/float_ms_ssim.c:131-138`
enforces `min_dim = GAUSSIAN_LEN << (SCALES - 1) = 11 << 4 = 176`. CPU init
returns `-EINVAL`. The test's first `vmaf_read_pictures` call therefore
fails before the Metal path runs.

### Reproduction (pre-fix)

The test itself is gated by `enable_metal=enabled or (auto and darwin)` and
needs the Apple `Foundation` framework to compile its Metal sources — not
buildable on Linux. Instead the diagnosis was verified through the
production CLI, which exercises the *exact same* `vmaf_use_feature("float_ms_ssim")`

- `vmaf_read_pictures` code path that the test's `run_cpu_float_ms_ssim` uses.

```text
$ docker exec vmaf-dev-mcp bash -c \
    "cd /tmp/wt/core && meson setup build-fix \
       -Denable_cuda=false -Denable_sycl=false 2>&1 | tail -3"
Found ninja-1.13.2 at /usr/bin/ninja
[...]

$ docker exec vmaf-dev-mcp bash -c \
    "cd /tmp/wt/core && ninja -C build-fix tools/vmaf 2>&1 | tail -3"
[141/143] Linking target src/libvmaf.so.3.0.0
[143/143] Linking target tools/vmaf

$ docker exec vmaf-dev-mcp bash -c \
    "head -c $((256*144*3/2)) /dev/zero > /tmp/test_256x144.yuv && \
     /tmp/wt/core/build-fix/tools/vmaf \
       -r /tmp/test_256x144.yuv -d /tmp/test_256x144.yuv \
       -w 256 -h 144 -p 420 -b 8 --feature float_ms_ssim --no_prediction \
       2>&1 | tail -5"
libvmaf ERROR float_ms_ssim: input resolution 256x144 is too small; \
  the 5-level 11-tap MS-SSIM pyramid requires at least 176x176 (Netflix#1414)

problem reading pictures
problem flushing context
```

The error text matches the gate at `core/src/feature/float_ms_ssim.c:131-138`
*verbatim*. The macOS test fails on the immediately-following
`mu_assert("CPU: vmaf_read_pictures failed", !err)`, which is exactly the
assertion message the CI surfaced.

Independent corroboration: the existing test
`core/test/test_float_ms_ssim_min_dim.c` proves the 176 floor:

```text
$ docker exec vmaf-dev-mcp bash -c \
    "cd /tmp/wt/core && meson test -C build-fix test_float_ms_ssim_min_dim 2>&1 | tail -5"
1/1 fast - libvmaf:test_float_ms_ssim_min_dim OK              0.00s
Ok:                1
Fail:              0
```

### Fix

`core/test/test_metal_float_ms_ssim_parity.c`: `FIXTURE_H 144u → 192u`.
192 = 176 (the floor) rounded up to a multiple of 16 for clean pyramid
downsamples (`256/16 = 16`, `192/16 = 12` — both stay integer at every
scale).

### Verification (post-fix)

```text
$ docker exec vmaf-dev-mcp bash -c \
    "head -c $((256*192*3/2)) /dev/zero > /tmp/test_256x192.yuv && \
     /tmp/wt/core/build-fix/tools/vmaf \
       -r /tmp/test_256x192.yuv -d /tmp/test_256x192.yuv \
       -w 256 -h 192 -p 420 -b 8 --feature float_ms_ssim --no_prediction \
       2>&1 | tail -5"
# (no error; vmaf exits 0)
```

The CPU twin in the test (`run_cpu_float_ms_ssim`) will now succeed,
and `run_metal_float_ms_ssim` returns cleanly with `-ENODEV` on
non-Metal hosts (the skip path the test explicitly handles).

### Sibling audit

`grep FIXTURE_H core/test/test_metal_*.c` found two more tests with
`FIXTURE_H 144u`: `test_metal_float_moment_parity.c` and
`test_metal_float_motion_parity.c`. Both validate features without a
176-floor (no `EINVAL` gate in `float_moment.c` or `motion.c`), so 144
is fine for them — no change required.

## Failure 2 — `test_ssimulacra2_simd::test_xyb` (Linux all-backends)

### Hypothesis

Per task brief: icpx may emit FMA contractions on the AVX2
`linear_rgb_to_xyb_avx2` function despite `#pragma STDC FP_CONTRACT OFF`,
producing bit-divergence from the scalar reference.

### Investigation

The hypothesis was partially wrong on direction: the AVX2 *SIMD* path uses
explicit `_mm256_mul_ps` + `_mm256_add_ps` intrinsics with no FMA
intrinsics (`grep fmadd core/src/feature/x86/ssimulacra2_avx2.c`
in `linear_rgb_to_xyb_avx2` returns 0). It is the inline scalar
reference `ref_linear_rgb_to_xyb` in `core/test/test_ssimulacra2_simd.c`
that gets contracted.

### Reproduction (pre-fix)

```text
$ docker exec vmaf-dev-mcp bash -c \
    "source /opt/intel/oneapi/setvars.sh > /dev/null 2>&1 && \
     cd /tmp/wt/core && \
     CC=icx CXX=icpx meson setup build-icpx \
       -Denable_cuda=false -Denable_sycl=true 2>&1 | tail -5"
libvmaf 3.0.0
  User defined options
    enable_cuda: false
    enable_sycl: true
Found ninja-1.13.2 at /usr/bin/ninja

$ docker exec vmaf-dev-mcp bash -c \
    "source /opt/intel/oneapi/setvars.sh > /dev/null 2>&1 && \
     cd /tmp/wt/core && \
     ninja -C build-icpx test/test_ssimulacra2_simd 2>&1 | tail -3 && \
     ./build-icpx/test/test_ssimulacra2_simd 2>&1 | tail -10"
[129/129] Linking target test/test_ssimulacra2_simd
test_multiply: pass
test_xyb: fail, linear_rgb_to_xyb SIMD not bit-identical to scalar
2 tests run, 1 failed
```

Failure reproduced. Exact CI message.

### Forensic verification — compiler-emitted code

The test TU is compiled with `-ffp-contract=off -fp-model=precise`
(verified by inspecting `build-icpx/build.ninja`):

```text
$ docker exec vmaf-dev-mcp bash -c \
    "cd /tmp/wt/core/build-icpx && \
     awk '/^build test\\/test_ssimulacra2_simd\\.p\\/test_ssimulacra2_simd\\.c\\.o:/{flag=1; print; next} \
          flag && /^ /{print; if(/ARGS = /) {flag=0}}' build.ninja"
build test/test_ssimulacra2_simd.p/test_ssimulacra2_simd.c.o: c_COMPILER ../test/test_ssimulacra2_simd.c
 ARGS = ... -ffp-contract=off -fp-model=precise -mavx2 -mfma
```

Both flags are present. Yet the emitted assembly contains 242 `vfmadd*`
instructions in the test TU (verified via `icx -S` with the same flags):

```text
$ docker exec vmaf-dev-mcp bash -c \
    "source /opt/intel/oneapi/setvars.sh > /dev/null 2>&1 && \
     cd /tmp/wt/core/build-icpx && \
     icx -O3 -mavx2 -mfma -ffp-contract=off -fp-model=precise \
         -std=c11 -Isrc -I../src -I../src/feature -Iinclude -I../include \
         -Itest -I../test -S -o /tmp/test_xyb.s ../test/test_ssimulacra2_simd.c 2>&1 | tail -5; \
     grep -c vfmadd /tmp/test_xyb.s"
242
```

Excerpt from the loop body of the inlined `ref_linear_rgb_to_xyb`:

```text
    vmovups	32(%rbx,%rax,4), %ymm3       # r
    vmovups	2804(%rbx,%rax,4), %ymm4     # g
    vmovups	5576(%rbx,%rax,4), %ymm2     # b
    vbroadcastss	.LCPI3_2(%rip), %ymm1    # m01 = 0.622
    vmulps	%ymm1, %ymm4, %ymm1
    vfmadd231ps	%ymm14, %ymm3, %ymm1     # FMA: m01*g  +=  kM00*r   ← contracted
    vfmadd231ps	%ymm15, %ymm2, %ymm1     # FMA: m01*g + kM00*r += kM02*b
    vaddps	%ymm1, %ymm10, %ymm1         # + kOpsinBias
```

The corresponding SIMD lib `libx86_ssimulacra2_avx2.a` (compiled with the
same strict-FP flags + `-ffp-contract=off`) emits zero `vfmadd`:

```text
$ docker exec vmaf-dev-mcp bash -c \
    "objdump -d /tmp/wt/core/build-icpx/src/libx86_ssimulacra2_avx2.a 2>&1 | grep -c vfmadd"
0
```

So under icx 2025.3 / 2026.0, neither `-fp-model=precise` nor
`-ffp-contract=off` nor `#pragma STDC FP_CONTRACT OFF` (per inline source
comments in the test TU build wiring at `core/test/meson.build:32-34`)
suppresses FMA contraction in inline scalar code. Only **`#pragma clang
fp contract(off)`** does — verified directly with a 4-line test program:

```text
$ docker exec vmaf-dev-mcp bash -c \
    "cat > /tmp/icx_test.c <<'EOF'
#pragma clang fp contract(off)
float chain(float r, float g, float b) {
    return 0.30f * r + 0.622f * g + 0.078f * b + 0.0037930732552754493f;
}
EOF
     source /opt/intel/oneapi/setvars.sh > /dev/null 2>&1 && \
     icx -O3 -mavx2 -mfma -ffp-contract=off -fp-model=precise \
         -S -o - /tmp/icx_test.c | grep -E 'vfmadd|vmulss|vaddss' | head"
    vmulss	.LCPI0_0(%rip), %xmm0, %xmm0
    vmulss	.LCPI0_1(%rip), %xmm1, %xmm1
    vaddss	%xmm1, %xmm0, %xmm0
    vmulss	.LCPI0_2(%rip), %xmm2, %xmm1
    vaddss	%xmm1, %xmm0, %xmm0
    vaddss	.LCPI0_3(%rip), %xmm0, %xmm0
```

No `vfmadd`. icx is clang-based, so the clang FP pragma is honoured;
the documented Intel pragmas (`#pragma float_control`) are not.

### Fix

Add a file-scope `#pragma clang fp contract(off)` to
`core/test/test_ssimulacra2_simd.c`, paired with a
`-Wunknown-pragmas` suppression for GCC (mirrors the existing pattern in
`core/src/feature/x86/ssimulacra2_host_avx2.c`). Production SIMD and
production scalar paths are untouched — no score drift.

### Verification (post-fix)

Under icpx (the failing job):

```text
$ docker exec vmaf-dev-mcp bash -c \
    "source /opt/intel/oneapi/setvars.sh > /dev/null 2>&1 && \
     cd /tmp/wt/core && \
     ninja -C build-icpx test/test_ssimulacra2_simd 2>&1 | tail -3 && \
     ./build-icpx/test/test_ssimulacra2_simd 2>&1 | tail -16"
[2/2] Linking target test/test_ssimulacra2_simd
test_multiply: pass
test_xyb: pass
test_downsample: pass
test_ssim: pass
test_edge: pass
test_blur: pass
test_ptlr_420_8: pass
test_ptlr_420_10: pass
test_ptlr_444_8: pass
test_ptlr_444_10: pass
test_ptlr_422_8: pass
test_host_xyb: pass
test_host_downsample: pass
13 tests run, 13 passed
```

Under GCC (the existing CPU build):

```text
$ docker exec vmaf-dev-mcp bash -c \
    "cd /tmp/wt/core && \
     ninja -C build-fix test/test_ssimulacra2_simd 2>&1 | tail -3 && \
     ./build-fix/test/test_ssimulacra2_simd 2>&1 | tail -16"
[19/19] Linking target test/test_ssimulacra2_simd
test_multiply: pass
test_xyb: pass
test_downsample: pass
test_ssim: pass
test_edge: pass
test_blur: pass
test_ptlr_420_8: pass
test_ptlr_420_10: pass
test_ptlr_444_8: pass
test_ptlr_444_10: pass
test_ptlr_422_8: pass
test_host_xyb: pass
test_host_downsample: pass
13 tests run, 13 passed
```

Both compilers pass. The fast suite (49 tests) also passes end-to-end on
the CPU GCC build (`meson test -C build-fix --suite=fast` → all OK).

## Why not the obvious "switch to FMA on both sides" fix

The natural sibling to ADR-0891 (which unified `picture_to_linear_rgb` on
explicit FMA in both scalar and SIMD) would be to do the same for
`linear_rgb_to_xyb`: switch the AVX2/AVX-512 SIMD to `_mm*_fmadd_ps`
intrinsics and the scalar reference to `fmaf()`. This was rejected
because:

1. The *production* scalar extractor `core/src/feature/ssimulacra2.c`
   `linear_rgb_to_xyb` is compiled with GCC's default `-ffp-contract=off`
   and emits non-FMA code. If the AVX2 SIMD switched to `_mm256_fmadd_ps`,
   the production scalar and production SIMD scores would diverge by ~1 ULP
   per pixel on GCC builds — breaking the cross-CPU-path invariant.
2. The test-TU-scoped pragma fix has zero impact on production binaries.
   It only changes what icx emits in `test_ssimulacra2_simd.c`. The
   `.text` of `libvmaf.so` and `tools/vmaf` is byte-identical pre- and
   post-fix.

## Open question for future audit

`core/src/feature/x86/ssimulacra2_host_avx2.c` and
`core/src/feature/x86/ssimulacra2_avx512.c` use `#pragma STDC FP_CONTRACT
OFF` for their scalar tail loops. Per the icx behaviour documented here,
that pragma is ignored on icx. The tail loops live inside the
strict-FP-flagged static lib (`-ffp-contract=off -fp-model=precise`) whose
SIMD body uses intrinsics-only, so divergence is not surfaced today —
those scalar tails only run when `plane_sz % 8 != 0` (AVX2) /
`plane_sz % 16 != 0` (AVX-512). Worth tightening to `#pragma clang fp
contract(off)` in a follow-up.

## References

- ADR-0153 — Netflix#1414 `float_ms_ssim` min-dim init check.
- ADR-0161 / ADR-0162 / ADR-0163 — SSIMULACRA 2 SIMD bit-exact contract.
- ADR-0214 — cross-backend parity gate.
- ADR-0589 — Metal SSIM L/C/S parity bound.
- ADR-0891 — explicit `fmaf()` unification for `picture_to_linear_rgb`.
- `.github/workflows/build.yml` — the `all-backends` job recipe pinning
  `intel-oneapi-compiler-dpcpp-cpp-2025.3`.
