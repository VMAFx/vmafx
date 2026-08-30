- The libvmaf CLI's explicit-backend gate (ADR-0498) now hard-fails
  with a dedicated exit code `100` (`VMAF_EXIT_BACKEND_INIT_FAILED`)
  when `--backend NAME` (anything other than `auto` / `cpu`) requests
  a backend that fails to initialise. Previously the binary returned
  `-1` which POSIX-truncated to exit byte `255`, indistinguishable
  from generic non-zero returns and forcing CI gates to grep stderr
  for the ADR-0498 marker string. Consumers can now match
  `[[ $rc -eq 100 ]]` to distinguish backend failures from other
  errors (ADR-0543).
- When the explicit-backend gate fires and `--output X.json` was
  passed, the libvmaf CLI now overwrites the output path with a
  single-line structured JSON descriptor instead of leaving the file
  empty / 0-byte. The descriptor carries `"error"`,
  `"backend_requested"`, `"errno"`, `"adr"` (always `"ADR-0498"`),
  and `"exit_code"` keys so downstream wrappers
  (`vmaf-tune compare`, MCP probes, CI gates) can decode the
  failure structurally instead of falling back to stderr parsing
  (ADR-0543).
- The libvmaf CLI now hard-fails (same exit `100` + JSON descriptor)
  when a feature name ending in `_cuda` / `_sycl` / `_vulkan` /
  `_hip` / `_metal` is requested but the matching backend isn't
  active in this run. Previously `vmaf_use_feature` silently
  registered the CPU twin, producing scores that looked identical
  to the explicit-backend invocation but were actually computed on
  the wrong silicon — exactly the silent-fallback bug ADR-0498
  banned for `--backend NAME`. Closes the per-feature asymmetry
  (ADR-0543).
