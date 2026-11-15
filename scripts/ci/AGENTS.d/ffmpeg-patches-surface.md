---
paths:
  - scripts/ci/ffmpeg-patches-surface-check.sh
  - scripts/ci/tests/test_ffmpeg_patches_surface_check.py
invariant: Tree checks run in hook (--surface-only) + CI; scan failure or zero docs fails closed; bash 3.2 safe.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# FFmpeg patches surface check (ADR-0409, ADR-2166)

Script: `scripts/ci/ffmpeg-patches-surface-check.sh`. Full mode = CI gate (tree checks + PR-body/diff check). `--surface-only` = tree checks alone, local hook.

## Tree checks

Retired-name patterns: script only, one per function. Never copy here.

| Function | Reads |
| --- | --- |
| `check_surface_series` | `ffmpeg-patches/*.patch`: retired filters/options, `vf_libvmaf.c` touches, added `--enable-libvmaf*` flags |
| `check_surface_fork_build_configs` | allow-listed `configs=(...)` array: `--enable-libvmaf` without `x`, retired filters |
| `check_surface_docs_examples` + `scan_single_doc` | `docs/**/*.md`, `README.md`, `ffmpeg-patches/README.md`, minus `is_excluded_doc` (dated records) + `## Migrating from the libvmaf filters` section of `docs/usage/ffmpeg.md`; plain `libvmaf` filter/flag line passes only with stock/upstream qualifier |
| `check_surface_patch_tests` | `ffmpeg-patches/test/*.sh`, minus `is_allowlisted_surface_file` + lines naming LD_PRELOAD shim `fault_inject_libvmaf.c` |

## Fail-closed contract

- Doc list captured first (`git ls-files`; `find` outside work tree); listing failure -> `[FAIL] cannot list docs`, exit 1.
- Zero scanned docs -> `[FAIL] docs examples: zero doc files scanned`, exit 1. Never PASS over 0 files.
- Bash 3.2 (macOS system bash): no `mapfile`, no `readarray`; loop over maybe-empty array uses `${a[@]+"${a[@]}"}` (bare `"${a[@]}"` aborts under `set -u`). Contract tests `test_bash_32_contract_*`.
- Dispatch: `check_... || exit 1` -> errexit off inside each check; every failure path needs explicit `return 1`.

## Hook wiring

- `.pre-commit-config.yaml` hook `ffmpeg-patches-surface-docs`, stages `pre-commit` + `pre-push`; test file pins `files` regex to script config list.
- Framework `pre-commit run` stashes unstaged edits -> hook reads staged content.
- Native mode (`VMAFX_NATIVE_HOOKS=1`): no run at commit; pre-push runs hook ([pre-commit hooks](../../../docs/development/pre-commit-hooks.md)).
- PR-body opt-out + merge-base diff check: CI only (needs PR body and `origin/master`).

## References

- [ADR-0409](../../../docs/adr/0409-ffmpeg-patches-surface-gate.md): public-surface gate.
- [ADR-2166](../../../docs/adr/2166-ffmpeg-series-drops-vulkan-shims.md): series drop, tree checks.
- Tests: `scripts/ci/tests/test_ffmpeg_patches_surface_check.py` (clean tree, planted defect per surface in both modes, zero docs, failed doc listing, bash 3.2 contract).
