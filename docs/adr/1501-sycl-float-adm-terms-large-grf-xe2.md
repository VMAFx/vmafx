<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1501: The `float_adm_sycl` term kernel takes the large register file and leaves the sub-group size to the compiler, so it uses no scratch memory on Xe2

- **Status**: Accepted
- **Date**: 2026-10-03
- **Deciders**: lusoris
- **Tags**: `sycl`, `testing`, `rc3`, `fork-local`

## Context

`T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02` asked for the SYCL device
suite on an Xe2 GPU. It ran on an Intel Arc B580 (`bmg-g21`, PCI `8086:e20b`,
xe driver, Linux 7.0.0-27, compute runtime 26.35.39758.10, IGC 2.41.5,
Level Zero loader 1.34.0, icpx 2026.1.1) from a default-list build of master
`7ac07442c`. Two tests failed; everything else passed, including the parity
gate on the three Netflix fixtures:

- `test_sycl_kernel_scratch`: the term kernel of `float_adm_sycl`
  (`launch_terms`, a plain `parallel_for` lambda) uses 128 bytes of spill
  memory. The scratch probes report correct values on the B580, and the
  `float_adm` parity tests and gate cells were exact, but the fork's rule is
  that no SYCL kernel uses scratch memory
  ([ADR-1395](1395-sycl-kernels-no-scratch.md)): the same kernel returns
  wrong values on an Arc A-series GPU under xe.
- `test_sycl_float_adm_math`: `test_scale_reductions_device` returned 1.5 or
  1.66129827 for one scale sum where the reference gives 1.8766818, changing
  from run to run. The probe submits the decouple, term and row kernels to a
  default (out-of-order) queue without dependencies. On the B580 the row
  kernel ran before the terms were written. With the v1 Level Zero adapter and
  `UR_L0_SERIALIZE=2` or `ZE_SERIALIZE=2` the test passed three times out of
  three; without serialisation it failed ten times out of ten. libvmaf's own
  queues are in order (`core/src/sycl/common.cpp`); only the probe was wrong.

The ahead-of-time build log shows the spill per target without a device
(`[bmg-g21] warning: in kernel '...launch_terms...': compiled SIMD32
allocated 128 regs and spilled around 2`). Left to its own choice, icpx
compiles the term kernel at SIMD-32 for `lnl-m`, `bmg-g21` and `bmg-g31` and
spills two registers there; it spills on no other target. Measured
alternatives, one `float_adm_sycl.o` for all 19 default targets each:

| Shape | Targets that spill (registers) |
|---|---|
| compiler's choice, 128 registers (master) | `lnl-m`, `bmg-g21`, `bmg-g31` (2) |
| sub-group 16, 128 registers | the 16 others: `dg2-*`, `acm-*` (84), `mtl-*`, `arl-*` (89 to 91), `tgllp`, `adl-*`, `rpl-*` (58 to 60) |
| compiler's choice, automatic register file size | `lnl-m`, `bmg-g21`, `bmg-g31` (2) |
| compiler's choice, 256 registers | none |

A required size of 16 with 256 registers is not an option either: Xe-LP
(`tgllp`, `adl-*`, `rpl-*`) has no large register file, ignores the property
and spills at 16 (the build log shows the same for `ssimulacra2_sycl`'s slot
kernel, which has that shape). The term kernel fits Xe-LP only at the
SIMD-8 the compiler picks there, and Xe2 accepts no size below 16
([ADR-1468](1468-sycl-sub-group-sizes-every-aot-target.md)).

## Decision

The term kernel of `float_adm_sycl` is a functor with the shape
`VmafSyclKernelShape<vmaf_sycl_fadm::kTermsSubGroup, vmaf_sycl_fadm::kTermsGrf>`,
where `sycl_float_adm_math.h` sets the sub-group size to 0 and the register
file to 256. `VmafSyclKernelShape` accepts 0 as "no required sub-group
size", only with 256 registers (a `static_assert`); its properties are then
`grf_size<256>` alone. The probe launches its term kernel in the same shape
and creates its queue in order, as libvmaf does.
`test_sycl_sub_group_size_contract.py` resolves the namespace-qualified
constants and rejects size 0 without the large register file;
`test_sycl_float_adm_exact_contract.py` holds the shape in the twin and the
probe and the probe's in-order queue.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **No required size, 256 registers (chosen)** | No target spills; Xe-LP keeps its SIMD-8 kernel; one kernel | A second meaning for the first argument of `VmafSyclKernelShape` | — |
| Sub-group 16 (as ADR-1468 did for the row kernels) | Same form as the other shaped kernels | Spills 58 to 91 registers on 16 of 19 targets, the A380 among them | Moves the defect to every other device |
| Sub-group 16 with 256 registers | Fits Xe2 and Xe-HPG | Xe-LP has no large register file and spills at 16 | Moves the defect to the UHD 770 class |
| Automatic register file size | One property, no new shape | Still SIMD-32 with 128 registers and the same spill on Xe2 | Does not fix it |
| Split the term kernel or reorder its loads to save two registers | No shape change | Two registers short of the limit; the next IGC can undo it | Not robust |
| A kernel per device family, chosen at run time | Each device its best shape | Two kernels and a dispatch for one spill | More code than the fault |
| List the kernel in `scratch_ratchet.txt` | No code change | The list is empty and stays empty (ADR-1395) | Against the rule |

## Consequences

- **Positive**: on the Arc B580 `test_sycl_kernel_scratch` audits 127
  kernels and finds none in scratch memory, the whole `gpu` suite passes
  (67 passed, 1 CUDA test skipped), `test_sycl_float_adm_math` passes 20
  times out of 20 (v2 and v1 adapters), and the parity gate reports every
  cell at its declared bound on the 576x324 pair and both 1080p
  checkerboards. On the A380 the same build passes the `gpu` suite, the
  scratch audit and the gate.
- **Positive**: the build log of the default-list build names no spill on
  Xe2 any more; the one left is `ssimulacra2_sycl`'s slot kernel on Xe-LP,
  for the UHD 770 half of the row.
- **Negative**: none measured. 4K BBB end to end, medians of 7 interleaved
  runs of (t(35 frames) - t(5 frames)) / 30: `float_adm_sycl` 12.22 ms per
  frame before and 12.11 after on the B580; on the A380, 34.95 and 35.32 ms
  over (t(60) - t(10)) / 50, a figure the 29 ms picture upload dominates.
  Both differences are inside the spread of the runs. The kernel alone was
  not timed.
- **Positive**: the Arc Pro B60 (`8086:e211`) of the same cluster gives the
  same picture once its VM was rebooted (the xe driver had wedged the device
  on 2026-09-21): master fails the same two tests, and with the fix the
  `gpu` suite, every `test_sycl_*` test, the scratch audit (127 kernels,
  none in scratch memory), the probe (16 of 16 runs) and the gate pass.
- **Neutral / follow-ups**: the Xe-LP / Xe-LPG half of the row (UHD 770)
  stays open, with the slot kernel spill as its first item.

## References

- `req` (maintainer, 2026-10-03, paraphrased): use the k8s repository to learn how to use the cluster's Arc B580 and B60, keep it simple: run the tests, fix what fails, then bring the node back up.
- [ADR-1395](1395-sycl-kernels-no-scratch.md) (no scratch memory),
  [ADR-1468](1468-sycl-sub-group-sizes-every-aot-target.md) (sub-group
  sizes on every AOT target), [ADR-1434](1434-sycl-float-adm-cpu-arithmetic.md)
  (`float_adm_sycl`'s arithmetic and its probe).
- `docs/state.md`: `T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02`,
  `T-SYCL-FLOAT-ADM-TERMS-XE2-SPILL-2026-10-03`,
  `T-SYCL-FLOAT-ADM-PROBE-OUT-OF-ORDER-QUEUE-2026-10-03`.
