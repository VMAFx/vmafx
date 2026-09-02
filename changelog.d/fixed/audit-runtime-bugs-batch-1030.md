- Full-fork correctness-audit batch: 18 independent, disjoint-file runtime
  bug fixes (PR #1030).
  - **SYCL init error-path leaks** (`integer_motion_sycl.cpp`): five early
    returns — including the `motion_add_uv` + unsupported-`pix_fmt` path at
    line 516, which is reachable without an out-of-memory condition — skipped
    `close_fex_sycl`, leaking the per-extractor USM device buffers.
  - **CUDA init error-path leaks**: `integer_ssim_cuda.c` `free_ref` now
    `free()`s the five `calloc`'d `VmafCudaBuffer` host wrappers
    (`vmaf_cuda_buffer_free` only `cuMemFree`s the device allocation, never
    the host struct); `integer_vif_cuda.c` `free_ref` post-module OOM path
    now unloads the PTX module, destroys the stream, and destroys both events
    that were previously leaked.
  - **CPU `ssimulacra2.c` init**: partial-OOM cleanup routes through a
    goto-cleanup label that frees all twelve `aligned_malloc` buffers
    (previously buffers 1–11 leaked when a later allocation failed).
  - **AI extraction crash-hardening**: `bvi_dvc_to_full_features.py` (zip +
    directory modes), `extract_full_features.py`, and
    `konvid_to_full_features.py` wrap each clip in per-clip `try`/`except`
    (mirroring `extract_k150k`), so one corrupt clip no longer aborts a
    multi-day extraction run.
  - **AI NaN guards**: `datamodule.py` drops non-finite feature/MOS rows and
    `train_fr_regressor_v2.py` drops non-finite rows instead of coercing
    `NaN`→`0`, so a single bad row no longer collapses training.
  - **MCP Go/Python parity** (`cmd/vmafx-mcp/impl.go`): removed the dead
    `vulkan` backend from the ≥30-metric branch and the backend keyword set,
    matching the Python MCP server after the Vulkan backend removal
    (ADR-0726).
