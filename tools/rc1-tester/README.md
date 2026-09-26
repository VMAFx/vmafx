<!-- markdownlint-disable MD013 MD024 -->
# RC1 external tester report kit

`tools/rc1-tester/vmaf-rc1-report` collects a bounded system probe, checks that
one or more explicitly requested VMAFx backend states initialize and produce a
four-frame CPU-checked result, and writes one archive that a tester can send to a
maintainer.

It requires no third-party Python packages at runtime. Run it from an RC1 source
checkout; there is no install step.

## Quick start

Build `vmaf`, then run from the repository root:

```bash
./tools/rc1-tester/vmaf-rc1-report probe --vmaf-bin build/tools/vmaf
./tools/rc1-tester/vmaf-rc1-report list-tools

# Repeat --backend to check every backend available on this host.
./tools/rc1-tester/vmaf-rc1-report bundle \
  --vmaf-bin build/tools/vmaf \
  --backend cpu --backend cuda \
  --out-dir reports
```

On Windows, invoke the same launcher through Python and point it at the `.exe`:

```powershell
py tools\rc1-tester\vmaf-rc1-report bundle `
  --vmaf-bin build\tools\vmaf.exe `
  --backend cpu --backend cuda `
  --out-dir reports
```

Valid selectors are `cpu`, `cuda`, `sycl`, `hip`, and `metal`. There is no
`auto` mode. The collector always runs a CPU reference, then requires four
frames of finite model metrics, bounded and consistent VMAF output,
`backend_used` matching the requested state, and a maximum per-frame metric
delta of `5e-5`. CPU is checked against the pinned score snapshot;
accelerators are checked against CPU from the same invocation. The pinned
model, snapshot, and fixture pair are hashed.

Frame 3 exercises temporal motion. The bounded comparison establishes emitted
metric correctness and backend-state initialization, but `backend_used` does
not prove every individual feature executed on that accelerator. Full
per-feature dispatch and correctness testing remain in the compiled backend
suites.

Use `--device-index N` to select a runtime-visible accelerator ordinal. SYCL,
HIP, and Metal receive their native device flags. CUDA receives a bounded
`CUDA_VISIBLE_DEVICES=N` environment override and uses device 0 inside that
filtered process. Run a separate bundle for each ordinal you want to cover.

## Release boundaries

| Phase | Tester activity |
| :--- | :--- |
| RC1 | Build and correctness readiness, device/toolchain discovery, bounded four-frame backend checks, shareable reports. |
| RC2 | Performance benchmarking and tuning with tools such as `vmaf_bench` and `vmaf-tune`. |
| RC3 | Real corpus generation and model training under `ai/` and `tools/ensemble-training-kit/`. |

`list-tools` also records current benchmark coverage gaps so an RC1 report does
not imply that the deferred RC2 harnesses already cover every backend or host
operating system.

## Exit status

| Status | Meaning |
| :--- | :--- |
| `0` | The automatic CPU reference and every requested backend produced bounded correctness evidence. |
| `1` | A command failed, output JSON was absent/unreadable/implausible, or the observed backend did not match. |
| `2` | Evidence was incomplete, for example because the binary or checked-in fixture was missing. |
| `100` | An explicitly requested backend could not initialize, matching the VMAFx backend-unavailable contract. |

`bundle` still writes an archive for statuses `1`, `2`, and `100`; the failure
evidence is usually what the maintainer needs.

## Archive contents

The `.tar.gz` or `.zip` contains:

- `report.md`, a human-readable summary;
- `diagnostics.json`, including exact argv/environment overrides,
  requested/observed backends and device ordinal,
  binary/model/fixture SHA-256 values, and available tool/driver probes;
- `smoke_<backend>.log` and, when emitted, `smoke_<backend>.json`;
- `manifest.json` and `SHA256SUMS` for integrity checking.

The checksum model is intentionally non-circular: `SHA256SUMS` covers every
payload file except itself and `manifest.json`; the manifest repeats those
payload hashes and records the hash of `SHA256SUMS`; the manifest does not hash
itself. The CLI separately prints the SHA-256 of the complete archive. These
are integrity aids, not an authenticity signature.

Home-directory and repository-root prefixes are redacted from bundle text.
Device serial numbers and GPU UUIDs are not requested, and archive ownership,
permission, and timestamp metadata are normalized. CUDA reports the driver
string returned by `nvidia-smi`; other accelerator rows say
`unknown/not reported` when the bounded visibility probe has no driver field.
Testers should still inspect `report.md` and `diagnostics.json` before sharing
an archive.

## Development checks

```bash
python3 -m pytest -q tools/rc1-tester/tests
python3 -m ruff check tools/rc1-tester
python3 -m black --check tools/rc1-tester
nox -s rc1_tester
```
