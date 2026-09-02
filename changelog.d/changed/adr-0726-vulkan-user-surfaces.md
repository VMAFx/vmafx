### ADR-0726: drop Vulkan from user-facing surfaces

- `README.md` no longer advertises Vulkan as a supported backend, drops the
  Vulkan backend bullet, the Vulkan row from the backend table, the
  `-Denable_vulkan=true` configure-flag hint, the `--enable-libvmaf-vulkan`
  FFmpeg configure flag, the `--vulkan_device` CLI option, and the
  `Vulkan` entry from the `docs/backends/` cross-reference list.
- `mkdocs.yml` removes the three Vulkan nav entries (`api/vulkan-image-import`,
  `backends/vulkan/overview`, `backends/vulkan/moltenvk`). Deletion of the
  underlying `.md` files is tracked by PR #299 (orphan-tree cleanup).
- `pkg/gpu/detect.go` drops `vulkan` from the NVIDIA / AMD / Intel
  capability `Backends` slices (now `cuda+cpu` / `hip+cpu` / `sycl+cpu`).
  `pkg/gpu/detect_test.go` updated accordingly.
- `cmd/vmafx-mcp/{impl,tools}.go`: the `backendDisable` map, the
  `probeBackends` advertised list, the `list_backends` / `vmaf_version`
  response dicts, the `backendKeywords` symbol-suffix table, the
  `inferBackendFromPayload` 30-key heuristic, and every tool-schema
  `enum` no longer mention `vulkan`.
- `cmd/vmafx-controller/proto/controller.proto`: drop `"vulkan"` from
  the `ScoringParams.backend` hint enumeration.
- `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`: `_BACKEND_DISABLE`,
  `_probe_backends`, `_list_backends`, `_vmaf_version` build flags,
  `_BACKEND_KEYWORDS`, `_infer_backend_from_payload`, and every `Tool`
  schema enum drop `vulkan`. The module docstring is updated.
- `mcp-server/vmaf-mcp/README.md`: drop `vulkan` from the `list_backends`
  tool-description backend list.
- `tools/vmaf-tune/src/vmaftune/score_backend.py`: `ALL_BACKENDS` and
  `DEFAULT_FALLBACKS` drop `vulkan`; `_probe_vulkan()` is removed;
  `detect_available_backends` no longer probes for it; the strict-mode
  selector now rejects `prefer="vulkan"` with `ValueError`.
  `score.py`, `fast.py`, `corpus.py`, and `cli.py` drop docstring and
  `--help` text mentions of Vulkan; the `--score-backend` priority
  string changes from `cuda > vulkan > sycl > cpu` to
  `cuda > sycl > hip > cpu`.
- `tools/vmaf-tune/README.md` and `tools/vmaf-tune/AGENTS.md` are
  updated to cite ADR-0726 alongside ADR-0314 / ADR-0667.
- Test suites under `tools/vmaf-tune/tests/` and
  `mcp-server/vmaf-mcp/tests/` invert their Vulkan-acceptance pins to
  ADR-0726 rejection pins: the V4-A and V5-1 strict-refusal integration
  tests now assert that `vmaf --backend vulkan` exits non-zero (CLI
  rejection); the backend-dispatch parametrize tables drop the `vulkan`
  row; the source-grep gates in `test_bbb_e2e_v2_bug_cluster.py` and
  `test_adr_0543_backend_enforcement.py` assert `vulkan` is no longer
  one of the per-backend `strcmp` targets in `core/tools/vmaf.c`.

Backend-drop reference: ADR-0726 (2026-05-28).
