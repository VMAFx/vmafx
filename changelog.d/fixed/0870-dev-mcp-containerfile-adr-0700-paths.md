- **`dev/Containerfile` ADR-0700 path drift — `libvmaf/` → `core/`
  ([ADR-0870](../docs/adr/0870-helm-values-schema-and-container-rebuild-audit.md),
  [ADR-0700](../docs/adr/0700-vmafx-repo-layout.md)).**
  ADR-0700 renamed `libvmaf/` → `core/` (and `python/vmaf/` →
  `compat/python-vmaf/` with a shim), but the dev-MCP container
  image was still doing `COPY --chown=vmaf:vmaf libvmaf/
  /build/vmaf/libvmaf/` in stage 3 and `cd libvmaf && meson setup
  build` / `cd libvmaf && ninja -C build install` in stages 3 and
  4. Against current master this fails with `"libvmaf": not found`
  on the first COPY and breaks every fresh `docker compose build
  dev-mcp` invocation. Fixes: (1) `COPY libvmaf/` → `COPY core/`,
  add new `COPY compat/` so the editable Python install through the
  `python/` shim can resolve `compat/python-vmaf/`; (2) both
  `cd libvmaf` invocations → `cd core`; (3) `.dockerignore` gains
  `core/build*/`, `core/build-*/`, `core/build_*/` siblings to the
  pre-existing `libvmaf/build*/` entries with a comment naming
  ADR-0700. The pre-existing `libvmaf/build*/` lines are retained
  so the file remains useful against pre-rename worktrees during
  rebases. Image now builds clean against `master` tip
  `bbcaa8d127`. Audit cycle 2026-05-30 per CLAUDE.md §15
  (vmaf-dev-mcp rebuild policy).
