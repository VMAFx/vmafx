<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1290: The tidy ratchet owns its per-lane compilation database

- **Status**: Accepted
- **Date**: 2026-09-22
- **Deciders**: Lusoris
- **Tags**: `ci`, `sycl`, `cuda`, `hip`, `lint`, `agents`

## Context

[ADR-1142](1142-whole-codebase-standards.md) puts every file under the same
standards and measures the debt per lane in
`scripts/ci/tidy-baseline-<lane>.json`. The measurement is only as wide as the
compilation database it is handed, and for the `sycl` lane that database was
silently short: meson emits the SYCL feature translation units as
`CUSTOM_COMMAND` rules (`icpx -fsycl`), and `scripts/ci/write-compile-commands.py`
deliberately exports only the native `c_COMPILER` / `cpp_COMPILER` rules.
`scripts/ci/gen-sycl-compile-commands.py` exists to synthesise the missing
entries and `core/src/feature/sycl/AGENTS.md` already instructs contributors to
run it, but `make tidy-ratchet` never did — so the lane measured **0** SYCL
feature TUs against the 18 the generator produces, and the committed
`tidy-baseline-sycl.json` recorded an empty backend.

The consequence was not confined to the baseline. Because a lane only counts
`NOLINT` markers in files it measures, 15 uncited markers — 14 under
`core/src/feature/sycl/` and `core/src/sycl/`, 1 under `core/src/feature/metal/`
— were invisible to every lane, and the GPU NOLINT backlog was reported as 6
when the tree-wide figure was 21. A gate that cannot see a file reports it as
clean, which is the failure mode [ADR-1142](1142-whole-codebase-standards.md)
§1 exists to prevent.

Two further preconditions were undocumented and cost sibling agents whole
sessions: the GPU build dir must be configured `-Db_lto=false`, because
`b_lto_threads=4` ([ADR-1172](1172-bound-lto-link-parallelism.md)) renders as
GCC's `-flto=4` and clang-tidy rejects it on every TU
(T-TIDY-CHANGED-LTO-FLAG-2026-09-05 already fixed this for the `cpu` lane); and
the build dir must live outside the repository, or meson's generated
`<build>/src/*.json.c` enter the measurement under build-dir-specific keys.

## Decision

The ratchet targets own the database they measure. `make tidy-ratchet` and
`make tidy-ratchet-write` expand a per-lane hook,
`TIDY_RATCHET_COMPDB_<lane>`, between the native export and the measurement;
for `sycl` that hook runs `gen-sycl-compile-commands.py`, and for the lanes
whose database meson writes natively it is empty. The two build-dir
preconditions are documented at the variable block in the `Makefile` and in
[`docs/development/ci.md`](../development/ci.md#whole-tree-lint-ratchet-adr-1142),
and `scripts/ci/tests/test_tidy_ratchet_sycl_compdb.py` pins the wiring.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-lane `TIDY_RATCHET_COMPDB_<lane>` hook in the Makefile (**chosen**) | Mirrors the existing `TIDY_RATCHET_EXTRA_<lane>` shape, so a future lane adds one line; keeps `tidy-ratchet.py` free of backend knowledge; testable without a toolchain | A caller who invokes `tidy-ratchet.py` directly still has to run the generator | — |
| Call the generator from inside `tidy-ratchet.py` when `--lane sycl` | Covers direct script callers too | Puts meson/icpx build-graph knowledge inside the measurement tool and makes it mutate its own input; the script deliberately takes a database it does not own | Rejected: wrong layer |
| Teach `write-compile-commands.py` to emit `CUSTOM_COMMAND` rules | One database, one tool | That script's contract is "native compiler rules only", and widening it would pull icpx flag translation (`-fsycl`, `-fp-model=`) into a generic exporter | Rejected: conflates two contracts |
| Document the extra step and leave the Makefile alone | No code change | This was already the state — `AGENTS.md` documented it and the baseline was still recorded without it. A rule only a human can apply is the thing that failed | Rejected: it is the status quo that broke |

## Consequences

- **Positive**: the `sycl` lane measures the SYCL backend it is named for; the
  GPU baselines are reproducible from a documented recipe; the tree-wide
  uncited-NOLINT count and the sum of the lane baselines can no longer diverge
  silently.
- **Negative**: the re-measured `sycl` baseline is substantially larger than
  the one it replaces, because 18 previously unmeasured TUs enter it. That is
  newly-visible pre-existing debt, not a regression, and it is now bounded by
  the ratchet.
- **Neutral / follow-ups**: none of the GPU lanes runs in CI yet — they remain
  local-only, so the baselines are recorded and unenforced until a hosted
  toolchain exists. Metal has no Linux toolchain and stays covered by the
  tree-wide NOLINT scan rather than by a lane measurement.

## References

- [ADR-1142](1142-whole-codebase-standards.md) — whole-tree standards and the ratchet.
- [ADR-0141](0141-touched-file-cleanup-rule.md) §2 — the NOLINT carve-out and its citation requirement.
- [ADR-0278](0278-t7-5-nolint-sweep.md) — the CPU-lane cite-only sweep this completes for the GPU lanes.
- [ADR-1172](1172-bound-lto-link-parallelism.md) — the `b_lto_threads=4` default behind the `-flto=` obstacle.
- [ADR-1243](1243-tidy-scoped-baseline-tightening.md) — scoped baseline tightening.
- `docs/state.md` rows `T-GPU-TIDY-LANE-BLIND-SPOT-2026-09-22`, `T-SYCL-LINT-SWEEP-2026-09-16`, `T-TIDY-CHANGED-LTO-FLAG-2026-09-05`.
- Ledger: `.workingdir/BUGS.md` `BUG-029`, `BUG-041`.
