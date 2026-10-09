---
paths:
  - scripts/ci/tidy-ratchet.py
  - scripts/ci/tidy-baseline-*.json
  - scripts/ci/tests/test_tidy_ratchet.py
  - scripts/ci/tests/test_tidy_scoped_write.py
  - scripts/ci/tests/test_tidy_lane_container.py
  - scripts/ci/tests/test_gen_gpu_compile_commands.py
  - scripts/ci/gen-gpu-compile-commands.py
  - scripts/ci/clang-tidy-hip.sh
  - scripts/dev/tidy-lane.sh
  - .clang-tidy
  - scripts/ci/praetor_tidy_coverage.py
  - scripts/ci/tests/test_praetor_tidy_coverage.py
  - .config/lint-exceptions.d/HISS-11.toml
invariant: Baselines only via `make tidy-lane-write` (dev container); counts only decrease; `HeaderFilterRegex` starts `(^|/)`.
area: tidy
---
<!-- markdownlint-disable MD013 MD060 -->
# tidy-ratchet.py invariants (ADR-1142)

- `scripts/ci/tidy-baseline-<lane>.json` generated only by `tidy-ratchet.py --write`
  (or `make tidy-ratchet-write`); never hand-edit count. Baseline may only
  decrease; raising number to make CI green = policy violation, not fix.
- Dedup key `(path, line, column, check)` and NOLINT rule ("cited" =
  `ADR-NNNN` on previous, same or next line, or anywhere in
  `/* ... */` block comment holding marker; `NOLINTEND` never counts) =
  load-bearing: baselines measured with exactly these rules, so
  changing either requires re-measuring every lane in same PR
  (`make tidy-lane-write LANE=all`, ADR-1471; ADR-1243 permits only guarded
  scoped tightening afterward).
- `Tidy Ratchet` job starts unconditionally, gates its
  work on ADR-1140 planner's `c_core` selector; `.clang-tidy`, this
  directory (ratchet + baselines) and workflow = CI-authority inputs, so
  editing any of them forces `mode=full`, lane runs. Never add
  `paths:` filter or custom early-skip probe to job.
- `clang-diagnostic-error` in any TU = measurement failure (exit 4), never
  zero. Build (generated headers) before measuring.
- Baseline `measured_sources` TU missing from measurement = "not measured"
  (exit 4, file named), never 0 and never "tighten"
  (`T-TIDY-RATCHET-UNMEASURED-AS-CLEAN-2026-10-06`: hosted job lacked MEX
  compile commands + libvpl, measured 424 of 438, asked to tighten 5 MEX
  files). New TU outside baseline = fine.
- **cpu / cuda / hip / sycl / arm64 measured in dev container only (ADR-1471).**
  `make tidy-lane LANE=<lane|all>` checks, `make tidy-lane-write` rewrites
  baseline (`scripts/dev/tidy-lane.sh`: checkout tar-streamed into throwaway
  container of `vmaf-dev-mcp:local`, nothing mounted, baseline copied back).
  Host run of `make tidy-ratchet` = look, never measurement: glibc 2.44
  reports `misc-static-assert` for every C++ `assert()`, host without hipcc
  lints `-ENOSYS` stubs, host ORT adds DNN TUs. Hosted run 37011276599
  (master `513d2a6fc`) failed on host-written cpu baseline: 24 files below,
  322 measured vs 376. Container report == hosted artifact byte for byte
  (SHA-256 `ffb5ca1819a3`). `cc_version` mismatch warning means "stop".
- `tidy-lane.sh` refuses check + scoped write (exit 5) when installed
  clang-tidy != baseline `clang_tidy_version`; only full `--write` moves
  baselines to new version. Host clang-tidy (23.1.1 on workstation since
  2026-10-02) never used.
- Scoped tightening also in container:
  `scripts/dev/tidy-lane.sh --write --only <tu> <lane>`. Scoped write cannot
  lower header counts and never extends `measured_sources`; header cleanup
  or new TU -> full `make tidy-lane-write`.
- Lane configuration = one definition: `TIDY_RATCHET_COMPILERS_<lane>` +
  `TIDY_RATCHET_SETUP_<lane>` + `TIDY_RATCHET_COMPDB_<lane>` in `Makefile`,
  consumed by `make tidy-ratchet-build` / `make tidy-ratchet`. Hosted cpu
  jobs (`Tidy Ratchet`, nightly `clang-tidy-full`) run those two targets, never
  own `meson setup` or direct `tidy-ratchet.py`; both install `libvpl-dev`
  (dev container: same package). `-Denable_dnn=disabled`: runner lacks ORT,
  container ships ORT. `tests/test_tidy_lane_container.py` pins both jobs and clang-tidy
  major (`CLANG_TIDY_MAJOR` in `tidy-lane.sh` vs `llvm.sh 22`). GPU lanes keep
  device compiler on (`-Denable_nvcc=true`, `-Denable_hipcc=true`,
  `-Denable_sycl=true`) and `-Denable_dnn=enabled`; never back to stubs.
- `gen-gpu-compile-commands.py`: kernel = explicit input of build statement,
  compiler = `COMMAND` argv[0]. Exit 1 when any statement names `.cu` / `.hip`
  it could not parse (rule-name-blind count). Old `<kernel> | <compiler>`
  match found 0 rules once targets listed header deps: cuda + hip baselines
  held no kernel until 2026-10-02. Never loosen that count.
- `.hip` kernels -> ROCm's clang-tidy via `clang-tidy-hip.sh`
  (`HIP_CLANG_TIDY_BIN`, default `/opt/rocm/llvm/bin/clang-tidy`): ROCm 10
  headers call `__builtin_amdgcn_is_invocable`, stock LLVM 22 stops with
  "builtin functions must be directly called". Host TUs stay on
  `CLANG_TIDY_BIN`; `--version` = that one. Missing ROCm tool -> `error:`
  line, exit 127 (TU unusable, not clean). `.hip` gets
  `--extra-arg=--cuda-host-only`: clang-tidy analyses driver's first job;
  ROCm 10.0 (LLVM 23) = host job first, ROCm 10.1 (LLVM 24) = device job
  first -> lane silently measured device AST (+23 in four headers). Keep the
  pin; a ROCm bump re-measures the hip lane against 10.x before landing.
- Self-test prints nothing: `report()` / `main()` print `::error::` under
  Actions; uncaptured, fixture `a.c` became annotation on hosted job. Use
  `_captured()`; `SelfTestOutput` reruns module with `GITHUB_ACTIONS` on and
  requires empty stdout.
- **Build products never measured.** Everything under `--build-dir` = generated
  (xxd `src/*.json.c` + `src/brisque_live.model.c`, HIP `*_hsaco.c`,
  `config.h`); ADR-1142 exempts generated files. `load_compile_commands()`,
  `parse_diagnostics()`, `scan_nolints()` all drop paths under build dir
  (`build_dir_prefix()` / `is_build_product()`) -> in-tree `build/` and
  out-of-tree `$RUNNER_TEMP` measure same set. Baselines = checked-in paths
  only; `build*/` key in any baseline = stale measurement. Before this rule
  nightly in-tree `build/` saw `build/src/*.json.c: warnings 0 -> 2` x18 (run
  36308945712) against out-of-tree cpu baseline.
- **arm64 lane = cross lane (ADR-1283).** Build dir configured with
  `build-aux/aarch64-linux-gnu.ini` + `aarch64-linux-gnu-qemu-user.ini`
  (Ubuntu 26.04: `qemu-aarch64` only, no `qemu-aarch64-static`; meson's
  compiler check needs exe_wrapper; `tidy-lane.sh` installs `CROSS_PACKAGES`); nothing else in tree compiles
  `core/src/feature/arm64/` or `ARCH_AARCH64` bodies of `core/test/`, so
  no other lane's compile database holds them. `TIDY_RATCHET_EXTRA_arm64`
  must keep `--extra-arg=--target=$(AARCH64_TARGET)` and
  `--extra-arg=--sysroot=$(AARCH64_SYSROOT)`: drop target and clang-tidy
  parses `<arm_neon.h>` / `<arm_sve.h>` as x86 and every NEON TU is exit 4;
  drop sysroot and libc resolves against host. `exclude_untidyable()`
  in `lint-and-format.yml` still excludes `^core/src/feature/arm64/` — that
  job's CPU-only `build/` genuinely has no command for those files; this lane
  is where they are measured.
- **Scoped writer (ADR-1243):** `--only` plus `--write` requires exact nonempty
  measured-TU coverage, original tool version/lane, no observed debt
  increase. Preserve every unselected TU/header entry and all full-report
  metadata; append explicit scoped provenance. Validate report/baseline aliases
  before output, replace validated baseline atomically. Full and scoped
  writers share resolved-path advisory lock; preserve baseline-drift checks
  around measurement/replacement, fail on unreadable NOLINT inputs. Diagnostic
  `--only` run is not full comparison. Keep failure/zero-tightening cases in
  `tests/test_tidy_scoped_write.py`; never make CI's full lane use `--only`.
  Scoped write also drops entries (`warnings`, `nolint_uncited`,
  `measured_sources`) of files absent from the tree (Q-309), prints each,
  records `dropped_deleted_files`; an existing-but-unmeasured file keeps its
  entry and fails closed (exit 4). Never make the drop depend on the
  measurement or on existence-by-name guess; rename = old dropped, new
  measured via `--only`. `measured-sources.txt` follows via
  `praetor_tidy_coverage.py --write`.
- Promoted clang-tidy checks (`-warnings-as-errors`) remain counted debt. Only
  recognized promotion exit/summary may bypass nonzero-tool-exit guard;
  parse/compile failures still invalidate that measurement. Reports retain
  actual `measured_sources` and `compile_failures` so partial/error output
  never presented as successful whole-tree scan.
- ADR-1113's Pelorus mirror and ADR-1276's manifest-owned boundary are outside
  native-lint ownership.
  `pelorus-mirror-paths.txt` is single exact-path exemption set consumed by
  sync guard, format hooks, changed-file tidy gate, and `tidy-ratchet.py`;
  do not restore prefix/directory classification. Keep one shared
  `is_exact_pelorus_mirror()` predicate inside ratchet for TU selection,
  header diagnostics, and legacy-baseline normalization. Scoped baseline
  write must preserve historical entries for this excluded scope; only full
  generated write may remove them. Fix mirror diagnostics in Pelorus and
  re-pin; never edit fixture or raise baseline locally. Tests live in
  `test_pelorus_mirror.py`, `test_tidy_ratchet.py`, and
  `test_tidy_scoped_write.py`.

## Tidy ratchet counts headers via an absolute-path-safe filter (ADR-1265)

`.clang-tidy` `HeaderFilterRegex` starts `(^|/)`. clang-tidy matches ABSOLUTE
paths; a `^core/` anchor matches nothing, headers vanish as "non-user code",
ratchet reports 0 header findings forever. That was the state before ADR-1265.

Symptom of regression: `Tidy Ratchet` says every `*.h` went `N -> 0`, asks to
tighten. Do not tighten; restore the `(^|/)`.

CPU baseline = `make tidy-lane-write LANE=cpu` (dev container; equals CI's
`tidy-ratchet-cpu` artifact), never a host run. GPU lanes:
`make tidy-lane-write LANE=<cuda|hip|sycl>`; not required contexts, nightly on
workstation ([measuring lanes](../../../docs/development/tidy-lanes.md)).

## Praetor `exceptions:` block ([ADR-2153](../../../docs/adr/2153-praetor-pin-04cc813.md), [ADR-2321](../../../docs/adr/2321-praetor-pin-afb739ed.md))

- Praetor reads one `exceptions:` key in `.standards.yaml`. Block between
  `BEGIN` / `END generated` markers = output of
  `python3 scripts/ci/praetor_tidy_coverage.py --write`; hook
  `check-praetor-tidy-coverage` fails stale block. Never edit block by hand.
- Sources, one each (HISS-19): tidy coverage entries from baselines'
  `measured_sources` + `.config/lint-exceptions.d/clang-tidy-coverage.toml`;
  HISS-11 entries (declared SLSA gap) from
  `.config/lint-exceptions.d/HISS-11.toml`, HISS-10 entries (workflows
  whose lanes build-warnings gate reads as ungated, ADR-2784) from
  `HISS-10.toml` (`PRAETOR_RULES`). New praetor
  rule = add it to `PRAETOR_RULES` + case in
  `scripts/ci/tests/test_praetor_tidy_coverage.py`.
- Rendered expiry = earlier of entry's date and `PRAETOR_EXPIRY_CAP`
  (praetor refuses more than 90 days out). Renew cap with entries.
- HISS-10 entry path = one workflow; entry excuses every failing lane of it.
  Gate reads werror only as literal on command, never `werror-args.sh`
  output. List empty since ADR-2828: every lane spells switch. New lane
  spells switch too; entry only for lane not yet gateable; stale entry fails
  audit.
- HISS-11 entry path = workflow `praetorctl audit` failure line names. Entry
  for other workflow, or with no gap left, fails audit as stale: remove it
  when release workflows reach declared level.
