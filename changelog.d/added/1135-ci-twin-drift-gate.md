- **CI gate: `.c`/`.cpp` twin drift + stale source references (ADR-1135).**
  New required check `Twin Drift + Stale Source Refs (ADR-1135)` in
  `lint-and-format.yml`, backed by `scripts/ci/twin-drift-check.sh`. It fails
  when (a) either side of a same-directory `.c`/`.cpp` twin pair is compiled
  by no `meson.build` / `setup.py` / `*.pyx` at all, or (b) any of those build
  files names a source path that does not exist — the two classes behind the
  three-month `output.c`/`output.cpp` split and the two-month Cython + fuzz
  breakage after the `mem.c` / `dict.c` renames. The three known dead twins
  (`core/src/model.cpp`, `core/test/test_dict.c`, `core/test/test_feature.c`)
  are listed with a reason each in `scripts/ci/twin-drift-allowlist.txt`;
  rows without a reason or that go stale fail the gate too. Runs locally in
  ~2 s (`bash scripts/ci/twin-drift-check.sh`) and as a `pre-push` hook; 24
  fixture cases in `scripts/ci/tests/test-twin-drift-check.sh`. Its first run
  on master flagged the fuzz harness's `dict.c` reference, so the PR carries
  the same one-line `dict.cpp` fix as #1186.
