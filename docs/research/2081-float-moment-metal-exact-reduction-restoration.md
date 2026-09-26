# Research-2081: Float moment Metal exact reduction restoration

## Finding

`core/src/feature/metal/float_moment.metal` on exact base
`4e6916d16ac57647105d14a47a6680117d6b5738` reduces all four moments through
binary32 `simd_sum` partials. That is exact at 8 bpc because every input and
square is an integer and a 256-lane workgroup stays below `2^24`. Above 8 bpc,
the CPU's power-of-two normalisation introduces fractional values and the
second-moment workgroup sum needs more than 24 significant bits. A model of
the 10-bit parity fixture gives a second-moment error of `-0.001220703125` for
reference and `-0.001708984375` for distorted, both beyond the `1e-4` GPU
parity gate.

This is a silent-revert regression, not a new design. Commit
`0fce64b47f82ae49a1a1146c998143882c4535a7` (PR #1029) replaced the float
partials with integer lo/hi planes. Commit `c2a3c7e0f` (PR #1067) then restored
the exact pre-fix host and kernel blobs while combining unrelated work. The
current paths moved from `libvmaf/src/feature/metal/` to
`core/src/feature/metal/`, but the lost reduction remained visible there.

The original #1029 implementation cannot be replayed literally. It split each
per-lane `ulong` into low and high halves and called `simd_sum(uint)` on each
half independently. For a 16-bit maximum square, one 32-lane SIMD group sums
`32 * 65535^2`, so its low half overflows and the carry never reaches the high
half. The regression contract demonstrates both that loss and the current
float32 rounding before accepting the replacement.

## Selected restoration

Each of the 256 lanes writes its four raw integer moments to four
`threadgroup ulong[256]` arrays. Lane 0 serially adds each array, then exports
the four exact uint64 results as eight uint32 lo/hi planes. The Objective-C++
host reconstructs each uint64, accumulates workgroup results in double, and
divides first moments by `pixels * scaler` and second moments by
`pixels * scaler^2`. This matches ADR-1212's CUDA/SYCL/HIP normalisation and
uses the same 64-bit reduction shape already compiled in
`integer_vif.metal`, where MSL's `simd_sum` is likewise not relied on for
64-bit values.

The existing 8-bit Metal parity test is retained and the same translation unit
is now built at 10 bpc. Its low codeword bits are deliberately non-zero, so the
old float path exceeds the tolerance instead of passing vacuously. Linux also
runs `test_metal_float_moment_contract.py`, which pins the kernel bindings,
the exact scratch reduction, host reconstruction, and scaling without
pretending that a source contract is an Apple device run.

## Alternatives considered

| Alternative | Result | Reason |
| --- | --- | --- |
| Replay PR #1029 verbatim | Rejected | Separate uint32 SIMD sums discard low-half carries at 16 bpc. |
| Keep float partials and widen only on the host | Rejected | Precision is already lost inside each workgroup. |
| Use a double reduction in MSL | Rejected | Metal GPU double support is not portable across the supported Apple devices. |
| Use 64-bit atomics | Rejected | MSL does not expose a usable `atomic_ulong`; the repository records device failures for that shape. |
| Threadgroup uint64 scratch plus lane-0 sum | Selected | Integer-exact, carry-safe, bounded to 256 iterations, and already proven by the Metal VIF kernel. |

No new ADR is needed. This restores the correctness contract already governed
by ADR-0421 (Metal extractor), ADR-1212 (high-bit-depth moment scaling), and
ADR-0214 (cross-backend parity).

## Reproducer and verification

The red-cap source contract was added before the production change and failed
11 assertions on the exact base: the kernel still contained `threadgroup
float` and `simd_sum`, and the host had neither eight integer buffers nor
lo/hi reconstruction. After the restoration:

```text
python3 core/test/test_metal_float_moment_contract.py
Ran 4 tests in 0.001s
OK
```

The permanent Apple execution gate is:

```text
meson test -C build test_metal_float_moment_parity \
  test_metal_float_moment_parity_10bit
```

This Linux host has no Metal compiler or Apple GPU, so MSL compilation and
device parity remain hosted-macOS acceptance. Netflix golden assertions are
untouched, and the change exposes no public C/CLI surface or FFmpeg patch
surface.
