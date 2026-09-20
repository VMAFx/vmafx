<!-- markdownlint-disable MD013 -->
# Pelorus v0.2.2 Interop Safety Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-pin VMAFx's read-only Pelorus interop mirror to released Pelorus v0.2.2, prove and fix the misaligned caller-buffer undefined behavior without changing ABI 1.3, and make exact-source drift a required CI check.

**Architecture:** Pelorus commit `93bef1206d68d9e09024c08a12732fb8e77b9b16` remains the sole source of truth. VMAFx vendors the exact source and shared fixture body with only ADR-1113's banner and include-path rewrites. The parser copies wire headers and directory entries into aligned locals before reading them. VMAFx's tooling exempts the complete exact-source mirror from local formatting/tidy mutation, while an existing required workflow checks the mirror against the exact Git object.

**Tech Stack:** C23, Meson/Ninja, ASan/UBSan, Bash, Python `unittest`, GitHub Actions, clang-format/clang-tidy ratchet, Towncrier-style changelog fragments.

---

## Task 1: Capture the baseline and add the failing alignment regression

**Files:**

- Modify: `core/test/test_pelorus_interop.c`
- Reference: `/home/kilian/dev/vmafx/pelorus/libpelorus/test/interop_test.c`

- [ ] Confirm the worktree is clean, branch is `fix/pelorus-interop-sync-v022`, and `HEAD == origin/master == 371ff5891ad43b6d8072d9fac132349ee3ddaaa9`.
- [ ] Record the exact upstream delta with:

  ```bash
  git -C /home/kilian/dev/vmafx/pelorus diff \
    818d844066e73326c6300c9827ce7324d04cd884..93bef1206d68d9e09024c08a12732fb8e77b9b16 \
    -- libpelorus/include/pelorus/pelorus.h \
       libpelorus/src/interop.c libpelorus/test/interop_test.c
  ```

  Expected: library version `0.1.0 -> 0.2.2`, a cast-free `memcpy` parser/packer, and two fixture cases; no `PELORUS_ABI_MAJOR` or `PELORUS_ABI_MINOR` change.
- [ ] Apply only v0.2.2's `test_misaligned_blob_base()` and `test_unaligned_header_size()` fixture additions, preserving the existing VMAFx-authored header/include rewrite.
- [ ] Configure and build an isolated UBSan target against the still-old parser:

  ```bash
  CC=clang CXX=clang++ meson setup core/build-pelorus-v022-ubsan core \
    -Db_sanitize=undefined -Db_lto=false -Db_lundef=false \
    -Denable_cuda=false -Denable_sycl=false \
    -Dc_args=-fno-sanitize=function -Dcpp_args=-fno-sanitize=function
  meson compile -C core/build-pelorus-v022-ubsan test_pelorus_interop
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    meson test -C core/build-pelorus-v022-ubsan --print-errorlogs test_pelorus_interop
  ```

  Expected RED: failure from an alignment sanitizer diagnostic in `pel_blob_is_present()` or `pel_blob_find_section()` when the fixture re-homes a valid blob at skews 1 through 7.
- [ ] Do not commit the failing intermediate state.

## Task 2: Port the released parser and restore exact fixture provenance

**Files:**

- Modify: `core/include/libvmaf/pelorus/pelorus.h`
- Modify: `core/src/interop/pelorus_interop.c`
- Modify: `core/test/test_pelorus_interop.c`
- Modify: all ADR-1113 `VENDORED FROM` banners selected by `scripts/sync-pelorus-interop.sh`
- Reference: Pelorus commit `93bef1206d68d9e09024c08a12732fb8e77b9b16`

- [ ] Port `libpelorus/src/interop.c` exactly: build pack headers/directories in aligned locals, `memcpy` them to/from the byte image, and reject a non-8-byte-aligned `header_size` with `PEL_ERR_ABI`.
- [ ] Update the vendored library version constants to `0.2.2`; leave `PELORUS_ABI_MAJOR=1` and `PELORUS_ABI_MINOR=3` byte-for-byte unchanged.
- [ ] Restore the complete fixture body to the v0.2.2 source, apart from the existing include rewrite. Remove PR #1351's VMAFx-only `NOLINTBEGIN`/`NOLINTEND` band and `(void)` return-value casts.
- [ ] Update every vendored banner to the full pin `93bef1206d68d9e09024c08a12732fb8e77b9b16` so provenance is unambiguous.
- [ ] Rebuild and rerun the UBSan regression command from Task 1.

  Expected GREEN: `test_pelorus_interop` passes with no alignment diagnostic.
- [ ] Configure and run a normal focused build:

  ```bash
  meson setup core/build-pelorus-v022 core \
    -Db_lto=false -Denable_cuda=false -Denable_sycl=false
  meson compile -C core/build-pelorus-v022 test_pelorus_interop
  meson test -C core/build-pelorus-v022 --print-errorlogs test_pelorus_interop
  ```

  Expected: all sixteen shared vectors pass.
- [ ] Commit the parser and exact fixture together:

  ```bash
  git add core/include/libvmaf/pelorus core/src/interop core/test/test_pelorus_interop.c
  git commit -m "fix(interop): sync Pelorus v0.2.2 parser safety"
  ```

## Task 3: Make the pin, drift check, and lint boundary enforceable

**Files:**

- Modify: `scripts/sync-pelorus-interop.sh`
- Modify: `scripts/ci/tidy-ratchet.py`
- Modify: `scripts/ci/tests/test_tidy_ratchet.py`
- Add: `scripts/ci/tests/test-sync-pelorus-interop.sh`
- Modify: `.github/workflows/lint-and-format.yml`
- Modify: `.pre-commit-config.yaml`
- Modify: `Makefile`
- Modify: `scripts/ci/AGENTS.md`

- [ ] Add a tidy-ratchet unit test that puts `core/src/interop/pelorus_interop.c`, `core/test/test_pelorus_interop.c`, and an ordinary C file into a compile database. Assert only the ordinary file is selected and legacy baseline entries for the exact mirror are ignored consistently.
- [ ] Run:

  ```bash
  python3 scripts/ci/tests/test_tidy_ratchet.py
  ```

  Expected RED: the current ratchet selects the two vendored translation units and retains their historical allowances.
- [ ] Add one explicit exact-vendor predicate covering `core/src/interop/pelorus_*.c`, `core/include/libvmaf/pelorus/**`, and `core/test/test_pelorus_interop.c`. Use it both when selecting translation units and when normalizing pre-existing baseline measurements, so all lane baselines remain generated artifacts and need no hand edits.
- [ ] Extend `.pre-commit-config.yaml` and both `Makefile` format selectors to exclude the exact shared fixture as well as the already-excluded vendored sources/headers.
- [ ] Add a hermetic shell fixture for `scripts/sync-pelorus-interop.sh` that proves a non-Git checkout and a Git checkout missing the pinned object fail closed rather than silently checking working-tree files.
- [ ] Change `PELORUS_VENDOR_SHA` to the full v0.2.2 commit and remove the working-tree fallback. A valid checkout may be on any branch/HEAD only if `git cat-file -e "$PELORUS_VENDOR_SHA^{commit}"` succeeds; all source reads come from `git show` of that object.
- [ ] Update the script's maintenance policy: re-pin for reviewed ABI additions and released parser correctness/security fixes, even when ABI major/minor do not change.
- [ ] Add the shell fixture to the required Pre-Commit hook set, and add a step to the existing `Pre-Commit` job in `.github/workflows/lint-and-format.yml` that checks out `VMAFx/pelorus` at the script's exact pin and runs the default drift/conformance check.
- [ ] Run the tooling tests and exact drift check:

  ```bash
  python3 scripts/ci/tests/test_tidy_ratchet.py
  bash scripts/ci/tests/test-sync-pelorus-interop.sh
  scripts/sync-pelorus-interop.sh /home/kilian/dev/vmafx/pelorus
  ```

  Expected: all pass; the drift check reports ABI 1.3 at the full v0.2.2 pin.
- [ ] Commit the policy/tooling/CI unit:

  ```bash
  git add scripts/sync-pelorus-interop.sh scripts/ci .github/workflows/lint-and-format.yml \
    .pre-commit-config.yaml Makefile
  git commit -m "ci(interop): enforce exact Pelorus mirror drift"
  ```

## Task 4: Reconcile documentation, state, and release artifacts

**Files:**

- Modify: `docs/api/pelorus-interop.md`
- Modify: `docs/adr/1113-vendor-pelorus-interop-abi.md`
- Modify: `docs/adr/_index_fragments/1113-vendor-pelorus-interop-abi.md`
- Modify (generated): `docs/adr/README.md`
- Add: `docs/research/pelorus-interop-v022-sync-2026-09-20.md`
- Modify: `docs/state.md`
- Modify: `docs/rebase-notes.md`
- Modify: `core/test/AGENTS.md`
- Add: `changelog.d/fixed/pelorus-interop-v022-parser-alignment.md`
- Modify (generated): `CHANGELOG.md`

- [ ] Document v0.2.2's alignment contract: the parser accepts any caller-buffer base alignment, but a consumer must `memcpy` a returned section before typed access when its base is not suitably aligned.
- [ ] Expand the conformance-vector list from fourteen to sixteen and record the non-aligned `header_size` rejection.
- [ ] Add a dated ADR-1113 amendment clarifying that reviewed correctness/security releases trigger a re-pin even without an ABI-minor change. Keep the original vendoring alternatives and append-only ABI decision intact.
- [ ] Regenerate the ADR index with the repository generator; do not hand-edit generated ordering/content.
- [ ] Add a research digest containing the exact source commits, the RED sanitizer reproduction, the no-ABI-change evidence, and why exact upstream fixture identity supersedes PR #1351's local lint edits.
- [ ] Reconcile `docs/state.md`: close the #1351 fixture-drift item, but retain the authoritative fixture's `fopen(..., "w")` world-writable-file finding as a separate open upstream-owned follow-up rather than claiming it fixed.
- [ ] Add the cross-repo rebase note and an invariant to `core/test/AGENTS.md`: the fixture body is exact Pelorus source; lint/format scope changes belong in VMAFx tooling, never in the fixture.
- [ ] Add and render the changelog fragment using the repository's changelog renderer.
- [ ] Run Markdown/repository doc checks on every changed document.
- [ ] Commit the documentation and release artifacts:

  ```bash
  git add docs core/test/AGENTS.md changelog.d CHANGELOG.md
  git commit -m "docs(interop): record the Pelorus v0.2.2 safety pin"
  ```

## Task 5: Final verification and self-review

**Files:**

- Verify all files in `origin/master..HEAD`

- [ ] Run the focused normal test and the default mirror check again.
- [ ] Configure a fresh ASan build and run the focused test:

  ```bash
  CC=clang CXX=clang++ meson setup core/build-pelorus-v022-asan core \
    -Db_sanitize=address -Db_lto=false -Db_lundef=false \
    -Denable_cuda=false -Denable_sycl=false
  meson compile -C core/build-pelorus-v022-asan test_pelorus_interop
  ASAN_OPTIONS=allocator_may_return_null=1:detect_leaks=1 \
    meson test -C core/build-pelorus-v022-asan --print-errorlogs test_pelorus_interop
  ```

- [ ] Run the fresh UBSan build from Task 1 with `UBSAN_OPTIONS=halt_on_error=1`.
- [ ] Run the relevant script/tidy tests, ShellCheck/shfmt, YAML/pre-commit hooks, and a CPU tidy-ratchet measurement.
- [ ] Run the repository-required aggregate command:

  ```bash
  make verify-all
  ```

- [ ] Inspect `git diff --check origin/master..HEAD`, `git status --short`, the commit sequence, and `git diff origin/master..HEAD` for accidental fixture or ABI drift.
- [ ] Verify directly that de-bannered vendored files match Pelorus v0.2.2 and that `PELORUS_ABI_MAJOR/MINOR` remain `1/3`.
- [ ] Record any environment-only unavailable gate as a concern with the exact command/error; do not claim it passed.
- [ ] Do not push or open a PR. Report `DONE`, `DONE_WITH_CONCERNS`, or `BLOCKED` with commit SHAs, commands/results, and remaining risks.
