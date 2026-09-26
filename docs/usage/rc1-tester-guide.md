<!-- markdownlint-disable MD013 MD024 MD046 -->
# RC1 external tester guide

RC1 testing answers a narrow question: can another person build VMAFx, run its
compiled tests, initialize an explicitly requested CPU or accelerator backend,
and return enough evidence for a maintainer to reproduce a failure? Performance
benchmarking and tuning start in RC2. Real model training starts in RC3.

| Release phase | Scope |
| :--- | :--- |
| RC1 | Build/correctness readiness, device discovery, bounded four-frame backend checks, and report collection. |
| RC2 | Performance benchmarking and tuning (`vmaf_bench`, `vmaf-tune`, backend harness cleanup). |
| RC3 | Real corpus materialization, LOSO runs, and model training (`ai/`, `ensemble-training-kit`). |

The RC1 collector never starts a benchmark sweep, encoder search, corpus job, or
training run.

## 1. Obtain the RC1 source

Download the source archive for the RC1 release from the
[VMAFx releases page](https://github.com/VMAFx/vmafx/releases), or check out the
exact RC1 tag in Git. Keep the source checkout: the current native release
payload contains `vmaf` and the library, while the report launcher and its small
YUV fixtures live in the source tree.

```bash
git clone https://github.com/VMAFx/vmafx.git
cd vmafx
git switch --detach <RC1-TAG>
```

Replace `<RC1-TAG>` with the tag shown on the RC1 release. Do not report an
uncommitted branch as RC1; the bundle labels the collector checkout revision
when Git metadata is available. The tested binary has its own version and
SHA-256 identity, so the checkout is never presented as proof of binary origin.

## 2. Build the backend to test

Run Meson from the repository root. These examples isolate one accelerator so
the resulting binary is easy to reason about:

=== "CPU"

    ```bash
    meson setup build core \
      -Denable_cuda=false -Denable_sycl=false -Denable_hip=false \
      -Denable_metal=disabled
    meson compile -C build
    ```

=== "CUDA"

    ```bash
    meson setup build core \
      -Denable_cuda=true -Denable_sycl=false -Denable_hip=false \
      -Denable_metal=disabled
    meson compile -C build
    ```

=== "SYCL"

    ```bash
    meson setup build core \
      -Denable_cuda=false -Denable_sycl=true -Denable_hip=false \
      -Denable_metal=disabled -Db_lto=false
    meson compile -C build
    ```

    The current `vmaf-dev-mcp:local` image uses oneAPI 2026.1.1, whose full
    release build hits a binutils/LTO-plugin link mismatch. `-Db_lto=false` is
    the validated RC1 correctness-build workaround; six Arc A380 ADM tests pass
    with it. Performance/LTO characterization remains RC2 work.

=== "HIP"

    ```bash
    meson setup build core \
      -Denable_cuda=false -Denable_sycl=false \
      -Denable_hip=true -Denable_hipcc=true \
      -Denable_metal=disabled
    meson compile -C build
    ```

=== "Metal"

    ```bash
    meson setup build core \
      -Denable_cuda=false -Denable_sycl=false -Denable_hip=false \
      -Denable_metal=enabled
    meson compile -C build
    ```

Use the current backend guides for prerequisites and supported toolchains:
[CUDA](../backends/cuda/overview.md), [SYCL](../backends/sycl/overview.md),
[HIP](../backends/hip/overview.md), and [Metal](../backends/metal/index.md).
Windows-specific build setup is documented in
[Building on Windows](../getting-started/building-on-windows.md). The compiled
CLI is normally `build/tools/vmaf` (`build\tools\vmaf.exe` on Windows).

Run the build's bounded fast suite before collecting a report:

```bash
meson test -C build --suite=fast --print-errorlogs
```

The collector does not run or embed that suite automatically. If it fails,
include the command output and review `build/meson-logs/testlog.txt` before
attaching it; Meson logs can contain local paths. The bundle's binary hash and
collector checkout revision are deliberately separate identities and do not
prove that the binary was built from that checkout.

## 3. Inspect the available tools

On Linux and macOS:

```bash
./tools/rc1-tester/vmaf-rc1-report list-tools
```

On Windows:

```powershell
py tools\rc1-tester\vmaf-rc1-report list-tools
```

The inventory distinguishes tools ready for RC1 from deferred RC2/RC3 tools and
states known gaps. In particular, the current C `vmaf_bench` covers
CPU/CUDA/SYCL rather than HIP/Metal, and its `--list-devices` mode is SYCL-only.
Those benchmark-coverage gaps belong to the RC2 work, not to RC1 correctness
reporting.

## 4. Probe the host

```bash
./tools/rc1-tester/vmaf-rc1-report probe --vmaf-bin build/tools/vmaf
```

The bounded probe records OS/architecture, CPU SIMD flags, build tools, vendor
compiler versions, and accelerator visibility through `nvidia-smi`, `sycl-ls`,
`rocm-smi`/`rocminfo`, or macOS `system_profiler`. It also records the VMAFx
binary version and SHA-256. CUDA includes the driver string returned by
`nvidia-smi`; accelerator probes without a bounded driver field explicitly say
`unknown/not reported` rather than inventing provenance. Detected accelerator
rows include their runtime-visible ordinal when the vendor probe provides one.

The selector names printed from `vmaf --help` are syntax, not proof that a
backend was compiled or used. The explicit smoke below adds bounded
backend-state and emitted-metric correctness evidence.

## 5. Check each backend on the host

There is deliberately no `--backend auto`. Ask for every backend you want the
report to cover; repeat the option to keep all results in one bundle:

```bash
./tools/rc1-tester/vmaf-rc1-report validate \
  --vmaf-bin build/tools/vmaf \
  --backend cpu --backend cuda \
  --device-index 0
```

`--device-index N` is a runtime-visible accelerator ordinal and applies to all
accelerator backends in that invocation; CPU ignores it. SYCL, HIP, and Metal
receive `--sycl_device`, `--hip_device`, or `--metal_device`. CUDA has no native
device-index field, so the collector records and applies
`CUDA_VISIBLE_DEVICES=N`, then VMAFx uses CUDA device 0 inside the filtered
process. Run a separate bundle for each ordinal, or when different backends
need different ordinals.

The collector always runs CPU first. If the command requests only an
accelerator, the CPU row is labelled `automatic reference` and is included in
the report. A failing or incomplete CPU reference prevents an accelerator
PASS.

Each attempt processes frames 0 through 3 of the checked-in 576x324 `testdata`
pair with `model/vmaf_v0.6.1.json` passed explicitly. Frame 3 has non-zero
`integer_motion2`, so the bounded window reaches temporal behavior. A PASS
requires all of the following:

1. the process exits zero;
2. VMAFx creates readable JSON output;
3. JSON contains exactly four consecutively numbered frames and all required
   model metrics are finite;
4. every `vmaf` value is inside the pinned model's `[0, 100]` clip range;
5. JSON contains `min`, `max`, `mean`, and `harmonic_mean` pooled VMAF metrics
   in that range and consistent with the four frames;
6. JSON contains `backend_used` equal to the explicitly requested selector;
7. CPU model metrics match the first four rows of the pinned
   `testdata/scores_cpu_576.json` snapshot within `5e-5` per frame; and
8. accelerator model metrics match the CPU result from the same invocation
   within `5e-5` per frame.

It never converts a missing fixture, missing output, or silent CPU fallback into
a PASS.

ADR-0214 already uses `5e-5` for its listed feature metrics. ADR-1342
conservatively applies the same threshold to this report's model inputs and
overall VMAF; ADR-0214 did not define an overall-model VMAF gate.

PASS means the requested backend state initialized and its emitted metrics met
this bounded CPU comparison. The root `backend_used` field does not prove which
individual feature implementation ran on the accelerator. Use the compiled
backend suites for per-feature dispatch and full correctness coverage.

| Collector exit | Meaning |
| :--- | :--- |
| `0` | The CPU reference and every requested backend produced the required bounded correctness evidence. |
| `1` | Command/output/backend-state evidence failed. |
| `2` | Evidence is incomplete because a required binary or fixture is missing. |
| `100` | An explicit backend could not initialize under the VMAFx backend-unavailable contract. |

## 6. Create one report archive

```bash
./tools/rc1-tester/vmaf-rc1-report bundle \
  --vmaf-bin build/tools/vmaf \
  --backend cpu --backend cuda \
  --device-index 0 \
  --out-dir reports
```

Windows uses the same options:

```powershell
py tools\rc1-tester\vmaf-rc1-report bundle `
  --vmaf-bin build\tools\vmaf.exe `
  --backend cpu --backend cuda `
  --device-index 0 `
  --out-dir reports --format zip
```

The command writes an archive even when validation fails or returns 100, because
that evidence is useful for diagnosis. Its directory contains:

```text
vmafx-rc1-report-<platform>-<timestamp>/
├── manifest.json
├── report.md
├── diagnostics.json
├── smoke_cpu.log
├── smoke_cpu.json            # present when VMAFx emitted JSON
├── smoke_<gpu>.log
├── smoke_<gpu>.json          # present when VMAFx emitted JSON
└── SHA256SUMS
```

The diagnostics record exact argv, bounded environment overrides, whether each
row was requested or automatic, requested and observed backend, device ordinal,
VMAFx binary hash, pinned-model/CPU-snapshot/fixture hashes, parity delta, raw
logs, available driver/toolchain strings, and the separately
labelled collector checkout revision. `--build-dir` helps discover
`tools/vmaf`; it does not make the checkout-to-binary relationship a provenance
claim or capture the full Meson configuration.

The checksum model is non-circular. `SHA256SUMS` covers each payload file except
itself and `manifest.json`; the manifest repeats those payload hashes and adds
the hash of `SHA256SUMS`; the manifest does not hash itself. The CLI separately
prints the complete archive SHA-256. These are integrity aids, not a signature
or proof of who created the archive.

Home and repository path prefixes are redacted, and the probes do not request
device serial numbers or GPU UUIDs. Archive user/group, permission, and
timestamp metadata are normalized. Still inspect `report.md` and
`diagnostics.json` before sharing. Then attach the archive to a
[VMAFx issue](https://github.com/VMAFx/vmafx/issues) with a short description of
the expected and observed behavior.
