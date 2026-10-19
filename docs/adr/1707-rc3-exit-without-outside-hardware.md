<!-- markdownlint-disable MD013 MD060 -->
# ADR-1707: Cut v1.0.0-rc.3 without waiting for outside-hardware reports

- **Status**: Accepted
- **Date**: 2026-10-05
- **Tags**: release, rc3, rc, process, fork-local, testing

## Context

ADR-1421 (superseding ADR-1352) orders first-release candidates so that twin
exactness is settled in RC3 before Rust work in RC4, deduplication in RC5,
the GPU capability table in RC6, benchmarks and tuning in RC7, and one-shot
retrain in RC8. It lists as exit evidence that no correctness row for a twin is
open in `docs/state.md`, and that "Metal rows are classified with a stated reason
while no device is available". [ADR-1496](1496-metal-gate-in-tester-bundle.md)
went further: it stated that rc.3 is cut only when every open RC3 row is closed,
the Metal rows included, by a report from an outside tester with an Apple
device.

Originally, twenty-five open RC3 rows were identified that could only be closed
by a device the project does not own: twenty Metal twin rows (an Apple device)
and five verification on other hardware: the Xe-LP half of
`T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02`,
`T-CUDA-TWINS-OTHER-ARCHITECTURES-2026-10-03`,
`T-HIP-TWINS-OTHER-TARGETS-2026-10-03`,
`T-CUDA-WINDOWS-BUILD-NEVER-RUN-ON-A-GPU-2026-10-04` and
`T-SYCL-WINDOWS-BUILD-NEVER-RUN-ON-A-GPU-2026-10-04`. Each has a tester kit
that produces the report ([ADR-1493](1493-macos-tester-bundle.md),
[ADR-1496](1496-metal-gate-in-tester-bundle.md),
[ADR-1505](1505-intel-gpu-tester-image.md),
[ADR-1509](1509-nvidia-gpu-tester-image.md),
[ADR-1511](1511-amd-gpu-tester-image.md),
[ADR-1516](1516-windows-cuda-tester-zip.md),
[ADR-1566](1566-windows-sycl-tester-zip.md)).

On 2026-10-05, outside-tester reports arrived and were recorded in the ledger
by PR #2135 ([`docs/hardware-reports/2026-10-05-*`](../hardware-reports/)):

1. **Apple M4 Pro macOS tester bundle report** (issue #2118,
   [`docs/hardware-reports/2026-10-05-apple-m4-pro.json`](../hardware-reports/2026-10-05-apple-m4-pro.json)):
   closed three Metal rows (`T-GPU-FLOAT-ADM-FRAME-SUM-FLOOR-2026-10-01`,
   `T-GPU-FLOAT-ADM-TINY-FRAME-FLOOR-2026-10-01`,
   `T-GPU-TWIN-PARITY-GAPS-OUTSIDE-CUDA-2026-09-30`), declared 18 Metal twins
   exact (`scripts/ci/exact_twins.d/*.metal`), and measured the remaining Metal
   rows on hardware. The open Metal rows stay in RC3 awaiting re-verification
   in the next bundle report; none need carrying past rc.3.
2. **NVIDIA RTX 3050 CUDA tester image report** (issue #2119,
   [`docs/hardware-reports/2026-10-05-13th-gen-intel-r-core-tm-i5-13500-cuda.json`](../hardware-reports/2026-10-05-13th-gen-intel-r-core-tm-i5-13500-cuda.json)):
   a GeForce RTX 3050 (Ampere `sm_86`) ran the CUDA tester image with 66 of 66
   device tests passing and all 19 parity-gate features identical to the CPU,
   closing the Ampere family of `T-CUDA-TWINS-OTHER-ARCHITECTURES-2026-10-03`.
3. **Intel UHD 770 SYCL tester image reports** (issue #2116, #2122,
   [`docs/hardware-reports/2026-10-05-13th-gen-intel-r-core-tm-i5-13500-sycl.json`](../hardware-reports/2026-10-05-13th-gen-intel-r-core-tm-i5-13500-sycl.json)):
   found every SYCL twin identical to the CPU, but 12 kernels used scratch
   memory. Four were fixed on `fix/sycl-xelp-scratch` and SIMD-32 vif was
   dropped ([ADR-1830](1830-sycl-vif-simd16-only.md)), with an Xe-LP re-run
   pending.

Five rows remain that only outside hardware the project does not own can close:
the Xe-LP half of `T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02`,
the unverified CUDA architectures of `T-CUDA-TWINS-OTHER-ARCHITECTURES-2026-10-03`
(Hopper, Blackwell, and sm_80 Ampere),
the unverified HIP targets of `T-HIP-TWINS-OTHER-TARGETS-2026-10-03`,
and the Windows GPU builds (`T-CUDA-WINDOWS-BUILD-NEVER-RUN-ON-A-GPU-2026-10-04`
and `T-SYCL-WINDOWS-BUILD-NEVER-RUN-ON-A-GPU-2026-10-04`). Holding the candidate
for them blocks RC4 to RC9, whose work
([ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md),
[ADR-1490](1490-rc3-rc9-candidate-map-cpu-capability.md)) does not depend on
those devices.

The tree also carries recorded standards debt ([ADR-1142](1142-whole-codebase-standards.md)):
the epic's whole-tree clause (every tracked file read by its gates, with no
baselined finding or uncited suppression, or listed with a reason) is not yet
met, and five lanes are driving the clang-tidy part to zero.

## Decision

1. **rc.3 does not wait for outside-hardware reports.** The five rows that only a
   device the project lacks can close are carried past rc.3 with a stated
   reason: the Xe-LP half of
   `T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02`,
   `T-CUDA-TWINS-OTHER-ARCHITECTURES-2026-10-03`,
   `T-HIP-TWINS-OTHER-TARGETS-2026-10-03`,
   `T-CUDA-WINDOWS-BUILD-NEVER-RUN-ON-A-GPU-2026-10-04` and
   `T-SYCL-WINDOWS-BUILD-NEVER-RUN-ON-A-GPU-2026-10-04`.
   `docs/state.md` files them under the disposition "RC3 carried past rc.3: needs
   outside hardware", each with its device and its report path. A row that a
   device the project owns can close (RTX 4090, Arc A380, gfx1036, Arc B580,
   Arc Pro B60, the CPU hosts) stays in RC3 and blocks the cut. Metal rows are
   retained in RC3 (or closed by the 2026-10-05 report) and are not carried.
2. **The rc.3 release notes say what was verified where.** They carry a
   "verified on / not yet verified on" table:
   - NVIDIA RTX 4090 (Ada, sm_89) and RTX 3050 (Ampere, sm_86), CUDA: verified
   - Intel Arc A380 (Xe-HPG), Arc B580 and Arc Pro B60 (Xe2), SYCL: verified ([ADR-1501](1501-sycl-float-adm-terms-large-grf-xe2.md))
   - AMD gfx1036 (RDNA2 graphics), HIP: verified
   - Apple M4 Pro (Metal): verified (18 twins exact)
   - CPU x86, AVX2 and AVX-512 (Zen 5): verified
   - CPU arm64 under qemu: verified
   - NVIDIA Hopper, Blackwell, sm_80 Ampere: not yet verified
   - AMD CDNA, RDNA1, RDNA3 to RDNA4, discrete RDNA2: not yet verified
   - Intel Xe-LP and Xe-LPG: not yet verified (UHD 770 measured; re-run pending after scratch fixes)
   - Windows builds with an NVIDIA or an Intel GPU: not yet verified
   Reports that arrive later are processed as fix rows in the window of the
   next candidate that is open when they arrive. A twin that a report shows to
   differ from the CPU is fixed, never given a tolerance (ADR-1421).
3. **The cut still needs the gated part of the standards clause.** rc.3 is
   cut only when all of these are at zero: the clang-tidy baselines; the HISS
   baseline (366 rows); SPDX lines with hook coverage for `.hip`, `.metal`,
   `.sh`, `.rs` and `.pyx`; clang-format on `.hip` and `.metal` with hook
   coverage; ruff and black over all in-scope Python with hook coverage; clippy
   on every crate in CI; and the tidy lanes reading the 107 translation units
   no lane reads today (Metal `.mm` through a macOS lane; each file that no tool
   can read, such as `.metal`, is listed with a reason). Two parts are listed
   with a reason and finish before `v1.0.0`, not before rc.3: mypy over the
   whole tree (4,265 errors, two groups that cannot run;
   `T-MYPY-WHOLE-TREE-2026-10-05`) and a citation rule with a gate for the
   about 2,066 non-NOLINT suppressions and 1,569 markdownlint suppressions
   (`T-SUPPRESSION-CITATION-RULE-2026-10-05`). Reason: both need a new gate
   design and a ratchet, and neither changes a score; the gated items are
   existing gates with a wrong scope or a recorded baseline. The numbers come
   from the standards remainder report of 2026-10-05.
4. **RC4 work proceeds in parallel as draft pull requests** labelled `rc4`,
   which land only after the `v1.0.0-rc.3` tag ([ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md)).

This ADR amends ADR-1421's RC3 exit: the allowance for outside-hardware rows with
a stated reason applies to the five rows listed above, and is explicit for the
cut. It replaces the sentence of ADR-1496's context that rc.3 is cut only when
every Metal row is closed; the rest of ADR-1496 (the bundle, the gate's `metal`
backend, the row map) stands, and a Metal report still closes the rows it
measures.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Wait for all outside-hardware reports | Every twin verified on every device before the tag; no carried rows. | Outside reports have no fixed date; RC4 to RC9 stay behind external availability; the work of those candidates does not depend on the answer. | Rejected by the maintainer. |
| Drop unverified targets from the release | The release claims only what a project device verified. | Removes shipped, ported code and its gate; the tester kits already exist; a later report would have nothing to close. | Backends stay in the release, listed as not yet verified on unreached targets. |
| Cut rc.3 now with no standards condition | Fastest tag. | Leaves recorded ADR-1142 debt in a candidate that claims exactness work is settled. | Maintainer chose to clear the debt first. |
| Carry the 5 rows, state the verified matrix, keep the standards condition (**chosen**) | RC4 starts now; the release notes are honest about coverage; late reports become fix rows. | Targets not yet verified reach users of rc.3 before device reports validate them. | Chosen by the maintainer. |

## Consequences

- **Positive**: RC3 closes on evidence the project can produce itself plus the
  received outside reports. RC4 proceeds in parallel. The release notes state
  the coverage instead of implying it.
- **Negative**: rc.3 ships targets no device of the project has run (the
  Windows GPU builds, unreached CUDA / HIP architectures, Xe-LP). A defect a
  later report finds is fixed in a later candidate.
- **Neutral / follow-ups**: `docs/state.md` gains the carried disposition and
  each carried row names its device and report path. The epic checklist marks the
  carried items. The rows keep their detail text and close on an accepted
  report. The table of hardware wanted
  ([hardware we need](../usage/hardware-we-need.md)) is unchanged. This decision
  adds no dependency, build-time fetch, runtime surface or SBOM component.

## References

- `req` (verbatim): "i think holding rc3 for some extern testing is a mistake, we could actually fix what we know and then merge and cut while actually sending agents in parallel to start rc4? gogo"
- `Q: "Clear the debt first"` (verbatim popup answer: rc.3 is cut only when the whole-tree standards clause holds).
- `Q: "Gated debt first (Recommended)"` (verbatim popup answer, 2026-10-05, after the standards remainder report: the gated items of decision 3 are zero before the rc.3 tag; mypy and the suppression citation rule are listed and finish before v1.0.0).
- `Maintainer decision: "Trim it, land it"` (popup 2026-10-05, following outside reports).
- [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md), [ADR-1421](1421-rc3-rc8-candidate-map.md), [ADR-1490](1490-rc3-rc9-candidate-map-cpu-capability.md), [ADR-1142](1142-whole-codebase-standards.md), [ADR-1493](1493-macos-tester-bundle.md), [ADR-1496](1496-metal-gate-in-tester-bundle.md), [ADR-1501](1501-sycl-float-adm-terms-large-grf-xe2.md), [ADR-1830](1830-sycl-vif-simd16-only.md).
- Epic [#1721](https://github.com/VMAFx/vmafx/issues/1721).
