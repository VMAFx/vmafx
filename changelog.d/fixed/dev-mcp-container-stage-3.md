**fix(dev): resolve dev-MCP container stage-3 EACCES + bundle earlier package fixes**

Stage 3 (`libvmaf-build`) of `dev/Containerfile` failed with
`PermissionError: [Errno 13] Permission denied: '/build/vmaf/build'` because
Docker's `WORKDIR` creates directories as root and `COPY --chown` does not
retroactively change the directory entry's owner — only its contents.
Added `RUN chown vmaf:vmaf /build /build/vmaf` (as root) immediately before
`USER vmaf` so meson can create the build subdirectory.

Also bundles three package-name fixes accumulated during PR #845's iteration
that were lost in the squash merge:

- `cuda-compiler` + `libcuda1` removed from the CUDA apt install (`libcuda1`
  must come from `nvidia-container-runtime` at runtime; `cuda-compiler` is a
  legacy alias no longer in the NVIDIA channels).
- `intel-basekit-2025.3` → `intel-basekit` (Intel does not publish
  year-quarter-versioned meta-package names).
- `rocm-hip-sdk` → `rocm-hip-runtime-dev` (`rocm-hip-sdk` pulls in `rccl`
  which depends on packages absent from the ROCm 6.4 noble repo).
