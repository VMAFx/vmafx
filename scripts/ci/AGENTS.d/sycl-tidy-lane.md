---
paths:
  - scripts/ci/gen-sycl-compile-commands.py
  - scripts/ci/clang-tidy-sycl.sh
  - scripts/ci/test_sycl_tidy_workflow_contract.py
  - scripts/ci/tests/test_*sycl*.py
invariant: SYCL TUs reach clang-tidy only through generated compile database; `Tidy SYCL` is strict-required, always reports.
area: tidy
---
<!-- markdownlint-disable MD013 MD060 -->
# SYCL custom-command lint database

Meson emits per-feature SYCL translation units as `CUSTOM_COMMAND` rules
(`CUSTOM_COMMAND_DEP` + two `DEPFILE` lines when target has depfile:
feature and runtime TUs since PR #1764, test probes not), so
`gen-sycl-compile-commands.py` must augment native compilation database
before clang-tidy can see them. Generator counts build statements compiling
`.cpp` with icpx; fewer parsed -> exit 1, database untouched. Rule name or
layout changes again -> extend `SYCL_COMMAND_PATTERN`, never loosen count
(lane measuring no SYCL TU still reports clean:
`T-SYCL-TIDY-COMPDB-DEPFILE-RULE-2026-10-02`). Analyzer command drops
`-MD -MF <file>`: build's depfile stays build's. Keep both legacy `-Xs` removal and current
target-scoped pair (`-Xsycl-target-backend=spir64_gen` plus its following
backend argument) in translator; passing either device-only option to
stock clang++ breaks analyzer lane. `test_sycl_aot_command.py` is
required pre-commit contract for Meson AOT spelling, built-in language
standard policy, and translator output. Do not exempt SYCL TU because it was
ported from upstream or predates gate.

Running generator is not left to caller. `make tidy-ratchet` and
`make tidy-ratchet-write` expand `TIDY_RATCHET_COMPDB_$(LANE)` between
native `write-compile-commands.py` export and measurement; for `sycl` that
variable runs `gen-sycl-compile-commands.py`, and for `cpu` / `cuda` / `hip` /
`arm64` it is empty ([ADR-1290](../../../docs/adr/1290-sycl-tidy-lane-compile-database.md)).
Keep hook in both targets and keep it ordered between two: while it was
missing lane measured zero SYCL feature TUs, and `tidy-baseline-sycl.json`
recorded empty backend while still reporting lane as clean.
`test_tidy_ratchet_sycl_compdb.py` pins that wiring. GPU lanes also need
their build dir configured `-Db_lto=false` and placed outside repository;
see variable block in `Makefile`.
`make tidy-ratchet LANE=sycl` sets `TIDY_RATCHET_EXTRA_sycl := --clang-tidy $(CURDIR)/scripts/ci/clang-tidy-sycl.sh`,
anchoring wrapper to worktree root. Under ADR-1270, `safe_subprocess.py`
requires allowlisted executables to be bare binary names or absolute paths;
relative paths with slashes fail validation. In addition, `tidy-ratchet.py`'s
`resolve_clang_tidy()` resolves multi-component relative binary paths to absolute
paths before measurement and execution. Preserve both `$(CURDIR)` anchoring
in `Makefile` and `resolve_clang_tidy()` in `tidy-ratchet.py`.

In CI, `clang-tidy-sycl` job (`Tidy SYCL`) in `lint-and-format.yml` has been
required, non-advisory merge gate since `6475fa9ea` (ADR-1297). It belongs to
both `required` and `strictMustReport`: detect step can skip work, but
job has no path filter and must always report. Each pull-request, push-fallback,
normal-push, and dispatch command covers all changed SYCL sources, headers
(`.cpp`, `.hpp`, `.h`), and tests independently. coupling between
`lint-and-format.yml`, `required-aggregator.yml`, and `rule-enforcement.yml` is
pinned fail-closed by `test_sycl_tidy_workflow_contract.py` using shared
`required_aggregator_harness.py` driver. exact strict-required set is also
pinned by `tests/test_hiss_replay_contract.py`; update that replay contract when
reporting-always context legitimately joins or leaves `strictMustReport`.
Every workflow that reports strict context lists `ready_for_review` in its
`pull_request` types. aggregator ignores check runs older than its own run,
so without that type PR opened as draft (every Renovate PR) keeps only
draft-era runs and fails with "never reported". same test pins this for
each strict context.

## Synthetic SYCL compile database

`gen-sycl-compile-commands.py` converts Meson's `icpx` custom commands into
stock-Clang commands for SYCL tidy lane. Remove only device-compilation
arguments that stock Clang cannot parse, and translate compatible spellings
such as `-fp-model=` to `-ffp-model=`. Preserve every diagnostic option,
including `-pedantic`, `-Wall`, `-Wextra`, and `-Werror`; analyzer noise is
defect to fix, not reason to weaken generated command. Keep
`tests/test_gen_sycl_compile_commands.py` paired with translator changes and
wired through `test-sycl-compile-command-generator` pre-commit/pre-push
hook; unwired regression test protects no CI lane.

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `test_sycl_tidy_workflow_contract.py` | `rule-enforcement.yml` — `Verify SYCL required clang-tidy contract`; `.pre-commit-config.yaml` — `test-sycl-tidy-workflow-contract` | Enforces that `Tidy SYCL` in `lint-and-format.yml` is strict-must-report, non-advisory required gate (`# required-aggregator`, no `continue-on-error`, full `.h`/`.cpp`/`.hpp` SYCL source and test coverage in every event branch). Uses shared aggregator harness to prove failure or absence blocks merge while success passes. |

## The wrapper silences one driver note

`clang-tidy-sycl.sh` passes `-Wno-overriding-option`. icx build records
`-fp-model=precise -ffp-contract=off` twice on every target that names
`vmaf_strict_fp_args` next to project-wide arguments (ADR-1461); after
wrapper's `-fp-model=` -> `-ffp-model=` rewrite, stock clang's driver prints
`warning: overriding '-ffp-model=precise' option with '-ffp-contract=off'`
without source location. ratchet fails closed on warning it cannot
place, so unit becomes compile failure and lane exits 4
(`T-SYCL-TIDY-OVERRIDING-OPTION-2026-10-02`). Do not drop flag, and do not
"fix" it by reordering build's arguments: contraction-off last is
order ADR-1461 requires. `SyclWrapperDriverWarnings` in
`tests/test_tidy_ratchet.py` holds it.
