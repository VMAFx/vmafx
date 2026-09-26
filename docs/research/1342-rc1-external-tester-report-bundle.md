<!-- markdownlint-disable MD013 MD024 MD060 -->
# Research-1342: RC1 external tester readiness and report evidence

- **Status**: Implemented
- **Workstream**: RC1 build/correctness readiness
- **Last updated**: 2026-09-26
- **Governing ADR**: [ADR-1342](../adr/1342-rc1-external-tester-report-bundle.md)

## Question

Can a tester with CPU, CUDA, SYCL, HIP, or Metal hardware run bounded RC1
compiled tests plus a bounded four-frame correctness check, then send one report with
enough evidence to distinguish build absence, device/runtime absence, process
failure, and an unexpected reported backend?

## Existing-tool audit

| Surface | Current capability | RC1 finding |
| :--- | :--- | :--- |
| `core/tools/vmaf` | Accepts `--backend cpu\|cuda\|sycl\|hip\|metal`; successful JSON is amended with `backend_used`; explicit backend initialization failure has status 100. | Suitable execution primitive when the collector checks output rather than trusting process status. |
| `core/tools/vmaf_bench` | C benchmark supports CPU/CUDA/SYCL; `--list-devices` is implemented for SYCL enumeration. | Performance tool, incomplete as an all-backend device probe; deferred to RC2. |
| `testdata/bench_backends.py` | Benchmark orchestrator for CPU/CUDA/SYCL/HIP. | Reads Linux `/proc/loadavg` and has no default Metal lane; cross-platform cleanup belongs to RC2. |
| `scripts/ci/cross_backend_parity_gate.py` | CI comparison for a subset of CPU/CUDA/SYCL paths. | Not an all-hardware external tester collector; its CUDA selector/default needs separate correction before reuse. |
| `dev/scripts/smoke-probe-loop.sh` | Long-running dev-container health loop. | Existing flag/server/provenance drift makes it the wrong public RC1 entrypoint; separate dev-container repair work owns that surface. |
| `tools/vmaf-tune` and benchmark scripts | Encoder search and performance data. | RC2 by release decision; never invoked by the RC1 collector. |
| `ai/`, `tools/ensemble-training-kit/` | Corpus/model training. | RC3 by release decision; never invoked by the RC1 collector. |

The native release payload currently stages the VMAFx library, `vmaf`, and
models. The collector and its checked-in short YUV pair therefore run from the
RC1 source checkout; the guide states this instead of implying the native
archive contains every tester tool.

The oneAPI 2026.1.1 toolchain in `vmaf-dev-mcp:local` currently exposes a bfd
LTO-plugin mismatch on the default SYCL release link. A targeted
`-Db_lto=false` Meson build links and passes all six Arc A380 ADM tests, so the
RC1 guide uses that correctness-build workaround and leaves LTO/performance
characterization to RC2.

## False-success risks found in the first draft

The initial generated draft was not safe to ship:

1. It searched `python/test/resource/yuv/src01_*`, which is not present in this
   checkout, then treated a successful `vmaf --version` fallback as backend
   PASS.
2. It accepted exit zero without requiring JSON or checking `backend_used`.
3. `--help` selector vocabulary was labelled "compiled backends" even though
   the names are accepted syntactically regardless of compiled/runtime state.
4. CLI `validate` returned zero for unavailable/skipped work, while `bundle`
   returned zero for every verdict.
5. Relative defaults depended on the caller's current directory, and the
   version parser ignored the real CLI's stderr output.
6. Archives omitted exact argv, binary/fixture hashes, and requested-versus-
   observed backend-state evidence; home paths and NVIDIA UUID fallback could leak into
   a report.
7. The no-subcommand path crashed by reading arguments that argparse had never
   created.
8. Any finite score, including values far outside the pinned model's clip
   range, passed; one-frame pooled values were not checked against that frame.
9. Multi-device HIP output collapsed into one row, and validation could not
   select a runtime-visible accelerator ordinal.
10. A one-frame finite score was still too weak: it never reached the first
    non-zero temporal metric and could publish a materially wrong in-range
    value as PASS.
11. The `rocminfo` fallback treated the host CPU agent as HIP device 0 and
    shifted real GPU runtime ordinals.

Each item now has a regression test.

## Implemented contract

### Probe

The no-runtime-third-party-dependency probe uses a packaged bounded argv-only
process helper, which applies a process-group timeout and a 1 MiB combined
stdout/stderr ceiling. Keeping that helper inside the distribution makes the
installed console command work outside a source checkout. It collects
platform/CPU features, compiler and build-tool versions, and accelerator
visibility through vendor utilities. NVIDIA collection uses only the
name/driver query and never falls back to `nvidia-smi -L`, which includes UUIDs.
Windows default binary discovery includes `vmaf.exe` and the guide uses Python
to invoke the extensionless launcher there. A path-discovery regression runs on
Linux; no hosted Windows or macOS execution result is claimed for the launcher.

### Validation

The validator uses the checked-in `testdata/ref_576x324_48f.yuv` and
`testdata/dis_576x324_48f.yuv` pair with `--frame_cnt 4` and passes the pinned
`model/vmaf_v0.6.1.json` explicitly. Frame 3 contains non-zero
`integer_motion2`, so this window reaches temporal behavior. The caller must
name each accelerator explicitly; the collector always runs CPU first and
labels it as an automatic reference when the tester did not request it. PASS is
the conjunction:

```text
process_status == 0
AND output is a readable JSON object
AND output has exactly four consecutively numbered frames (0..3)
AND every required model metric is finite
AND every VMAF score is inside [0, 100]
AND output has min/max/mean/harmonic_mean pooled VMAF metrics inside [0, 100]
AND pooled VMAF values agree with those four frames within 1e-6
AND output.backend_used == requested_backend
AND CPU model metrics agree with the pinned CPU snapshot within 5e-5 per frame
AND accelerator model metrics agree with this run's CPU result within 5e-5 per frame
```

Missing binary/fixture evidence is `SKIPPED` with CLI status 2. Ordinary
failures and false/missing backend evidence return 1. Explicit initialization
failure remains `BACKEND_UNAVAILABLE`/100.

ADR-0214 already owns the `5e-5` threshold for the feature metrics listed in
its matrix. ADR-1342 deliberately adopts that conservative threshold for this
reporter's model-input metrics and overall per-frame VMAF; ADR-0214 did not
previously define an overall-model VMAF gate.

This is bounded emitted-metric correctness plus backend-state initialization
evidence. `backend_used` is a root execution-state field; because individual
feature extractors may fall back, it does not prove every model feature ran on
the accelerator. The report therefore claims no per-feature dispatch proof and
does not replace the full compiled backend suites.

`--device-index` selects a runtime-visible accelerator ordinal. The native
SYCL/HIP/Metal device flags carry it in argv. CUDA lacks a device-index API, so
the collector records and applies `CUDA_VISIBLE_DEVICES=N`; the child then uses
CUDA ordinal 0 inside that filtered process. HIP probing preserves up to eight
`rocm-smi` device rows instead of merging them.

### Bundle and privacy

One invocation can repeat `--backend` to place multiple attempts in one
archive. `diagnostics.json` records exact argv, requested/observed backends,
device ordinal and environment overrides, binary/model/fixture SHA-256 values,
toolchain/device evidence, and raw process
output. Per-backend log/parsed-JSON files make triage convenient; non-finite
failed-run values become RFC 8259 `null` rather than bare `NaN`. `SHA256SUMS` hashes
payload files other than itself and `manifest.json`; the manifest repeats those
hashes and adds the checksum-file hash; the CLI separately prints the complete
archive hash. This avoids self-hashing cycles and does not claim authenticity.

The serializer redacts repository and home-directory prefixes from shared text,
and temporary validation paths become `$REPORT_TMP`. Device serials and UUIDs
are not queried. Tar/ZIP ownership, permissions, and timestamps are normalized.
CUDA captures the bounded `nvidia-smi` driver field; other accelerator records
say `unknown/not reported` when the visibility probe has no driver field. Since
arbitrary driver/tool output can still evolve, the guide requires the tester to
inspect the report before upload.

The binary version/hash and collector checkout revision are intentionally
separate. They identify what ran and what collected the report but do not prove
the binary came from that checkout. Full Meson options and `testlog.txt` are not
automatically bundled because they can be large and contain local paths; the
guide tells a tester to attach reviewed build/test logs when the failure is in
those phases. This is an explicit provenance boundary, not a reproducible-build
claim.

The source module now has package metadata and is wired into the `rc1_tester`
nox session, canonical Makefile Python lint paths, and the hosted required
`RC1 Tester Report` job. Runtime stays dependency-free; the `dev` extra owns
pytest/Ruff/Black only.

## Verification evidence

No GPU workload was run while developing this collector.

- Exact-master CPU binary `v3.1.0-2763-g4e6916d16` -> `cpu: PASS`, four
  frames, maximum pinned model-metric delta `0`.
- `python3 -m pytest -q tools/rc1-tester/tests` -> 71 passed.
- `python3 -m ruff check tools/rc1-tester` -> clean.
- `python3 -m black --check tools/rc1-tester` -> clean.
- Tests cover missing fixture/binary, missing/malformed output, backend
  mismatch, exit 100, generic failure, privacy redaction, checksums, exact
  provenance, model and score-schema enforcement, output ceilings, normalized
  archive metadata, strict failed-run JSON, Windows `.exe` discovery, CLI status
  propagation, repeated backends, automatic CPU-reference ordering,
  out-of-range and materially wrong in-range scores, the accepted `5e-5`
  boundary, multi-device HIP enumeration without CPU-agent ordinal drift,
  explicit accelerator ordinals, real timeout/output-limit enforcement, and
  both archive formats.
- A non-editable wheel install found the checkout from the documented working
  directory, passed the real four-frame CPU check, and its
  `vmaf-rc1-report list-tools` entry point ran from `/tmp` without repo-only
  imports.
- No `mypy` result is claimed; `mypy` was not installed in the active host
  environment when the first draft's claim was checked.

## Remaining phase-specific work

RC1 report collection is bounded and backend-complete at the selector and
four-frame emitted-metric layer. Actual CUDA/SYCL/HIP/Metal success still
requires testers or CI with that hardware, and the compiled suites remain the
full correctness authority. RC2 owns
benchmark-harness portability, per-feature/exclusive dispatch evidence, Metal
coverage, and performance methodology. RC3 owns real training and corpus runs.
