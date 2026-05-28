# Research Digest 0742 — CompileIQ Pilot v2: filter1d.cu Auto-Tuning (Abandoned)

**Date**: 2026-05-28
**Branch**: `research/cuda-compileiq-filter1d-pilot-v2-20260528`
**Outcome**: Abandoned — `compileiq` PyPI package is an empty placeholder (v0.0.0a0)
**Supersedes**: PR #66 (CompileIQ pilot v1, abandoned for Python 3.14 incompatibility)

---

## Summary

A second attempt was made to run CompileIQ automated kernel-tuning against
`core/src/feature/cuda/integer_vif/filter1d.cu` using the newly-built
`vmaf-dev-mcp:cuda13.3` container image (CUDA 13.3, Ubuntu 26.04 LTS).

The attempt was abandoned immediately after confirming that the `compileiq`
package available on PyPI (version `0.0.0a0`) is an empty placeholder with no
functional CLI. There is no `run` subcommand, no `--apply-controls` flag, and
no module contents beyond `__version__ = "0.0.0a0"`.

---

## Environment Probed

| Component | Result |
|---|---|
| Container image | `vmaf-dev-mcp:cuda13.3` (50.9 GB, Ubuntu 26.04 LTS) |
| CUDA toolkit | 13.3, release 13.3 V13.3.33 (confirmed via `nvcc --version`) |
| Python in container | 3.14.4 (system default; Ubuntu 26.04 ships Python 3.14 only) |
| Python 3.13 availability | Available via `ppa:deadsnakes/ppa` for Ubuntu 26.04 |
| Python 3.12 availability | Not available (no apt package, not in deadsnakes for 26.04) |
| `compileiq` on PyPI | v0.0.0a0 — empty package (`Empty package compileiq.`) |

---

## Attempted Steps

**Step 1: Branch setup** — succeeded. Branch
`research/cuda-compileiq-filter1d-pilot-v2-20260528` created from
`origin/master`.

**Step 2: Smoke test (entrypoint bypass)** — `nvcc --version` confirmed CUDA
13.3. Container entrypoint has SYCL/HIP GPU probes that each wait up to 300 s
before proceeding; bypassed with `--entrypoint bash`.

**Step 3: Python 3.13 install** — `python3.13` is not in Ubuntu 26.04's default
apt repos. The `python3.13-venv` package is also absent. The deadsnakes PPA
(`ppa:deadsnakes/ppa`) does provide `python3.13` for Ubuntu 26.04.

**Step 4: compileiq on Python 3.14** — Attempted direct install without
Python 3.13 sub-venv. `pip install compileiq` succeeded (exit 0) but installed
version `0.0.0a0`, which contains only an empty `__init__.py` with
`__version__ = "0.0.0a0"`. No `compileiq` console-script entry point is
registered. The `ls /tmp/civ/bin/` output showed a `𝜋thon` symlink but no
`compileiq` binary.

**Conclusion**: The `compileiq` package name on PyPI is a placeholder — either
a name reservation, an abandoned project, or the tool was never published under
this name. The tool referenced in the pilot brief does not exist at
`pypi.org/project/compileiq`.

---

## Root Cause

The pilot v1 (PR #66) was abandoned because the container had Python 3.14 and
the deep-research assumed `compileiq` required `<3.14`. Pilot v2 was initiated
with the hypothesis that Python 3.13 in a sub-venv would unblock it. This v2
investigation revealed the actual blocker: the `compileiq` tool itself does not
exist on PyPI as a functional package. The Python version issue in v1 was a
false lead.

---

## Alternatives for CUDA Auto-Tuning

The following real tools exist for GPU kernel parameter search:

| Tool | Status | Notes |
|---|---|---|
| [KTT (Kernel Tuning Toolkit)](https://github.com/HiPerComp/KTT) | Active open source | C++ API, search block sizes / registers / unroll factors |
| [CLTune](https://github.com/CNugteren/CLTune) | Archived | CUDA + OpenCL, Python bindings, last commit 2019 |
| [NVIDIA `ncu --set full`](https://docs.nvidia.com/nsight-compute/) | Available in CUDA 13.3 | Profiler, not auto-tuner; guides manual tuning |
| [OpenTuner](https://opentuner.org/) | Mature | Framework for arbitrary parameter search; can wrap nvcc |
| Custom grid search via `subprocess.run(nvcc ...)` | Always available | Low-overhead; used by the `/profile-hotpath` skill |
| cuTuner / AutoTVM CUDA backend | Research prototypes | Not production-ready for our kernel surface |

The `/profile-hotpath` skill (ADR-0199) with `ncu` is the recommended next step
for `filter1d.cu` if auto-tuning is still desired. A grid search over
`BLOCKX`, `BLOCKY`, and `val_per_thread` can be implemented as a thin
Python wrapper using `subprocess.run` + meson reconfigure; estimated
implementation effort: 1–2 days.

---

## Disposition

- ADR-0742: Decision recorded as "abandon CompileIQ; tool not available on
  PyPI."
- No code changes committed.
- No ACF file produced.
- PR #66 closure is requested in the accompanying DRAFT PR.
- Future CUDA kernel auto-tuning should use `/profile-hotpath` + `ncu` or
  a grid-search wrapper over nvcc block-size parameters.

---

## References

- PR #66 (pilot v1, abandoned 2026-xx-xx)
- `pypi.org/project/compileiq` — v0.0.0a0, installed size ~3 KB, no entry
  points
- ADR-0742 (this decision)
- CUDA 13.3 toolkit: `/opt/cuda/bin/nvcc --version` → `release 13.3, V13.3.33`
