### Research-0734 — CUDA 13.3 fix-list deep audit (per-issue exposure mapping)

Audited all 40 "Fixed/Resolved an issue where…" entries in CUDA 13.3 release notes
(plus 13.2 / 13.1 / 13.0) against `core/src/feature/cuda/` and `core/src/cuda/`.

Findings: 37 NOT AFFECTED (cuBLAS/cuSOLVER/cuSPARSE/nvJPEG/NPP — none used by the
fork), 1 LOW scope-guarded (NPP `nppiCFAToRGB` SSIM path — no call sites), 1 MEDIUM
scope-guarded (cuFFT multi-GPU FP exception — no cuFFT), and **1 CRITICAL** (NVCC
thread-reconvergence compiler bug [6156910], present since 12.8, fixed in 13.3 only).

The CRITICAL finding confirms that `dev/Containerfile` and `Dockerfile` must be bumped
from `cuda-toolkit-13-2` to `cuda-toolkit-13-3` to eliminate the risk of stale register
values in divergent-branch kernels. The 25 `.cu` files with nested `if`-divergence —
`float_adm_score.cu`, `adm_decouple.cu`, `filter1d.cu`, `adm_dwt2.cu`, and others — are
all candidates for silent score corruption under the 13.2 toolchain.
