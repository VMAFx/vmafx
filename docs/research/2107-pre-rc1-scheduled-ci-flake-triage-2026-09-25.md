<!-- markdownlint-disable MD013 MD060 -->
# Research-2107: Pre-RC1 scheduled-CI flake triage — 2026-09-25

**Status:** Complete

**Authorities inspected:**

- Exact starting commit: `df0b3561c017273bfce215ba8ea2a81dc3a9658e`.
- Current `.github/workflows/nightly.yml`, `.github/workflows/fuzz.yml`,
  `.github/workflows/sanitizers.yml`, and the required CPU tidy-ratchet lane in
  `.github/workflows/lint-and-format.yml`.
- GitHub Actions job metadata for nightly runs `35975614507` through
  `34453737732`, fuzz runs `35981061040` and `35842763631`, and sanitizer run
  `35981157602`.
- The `clang-tidy-full-report` artifact from run `35975614507`.
- ADR-1093, ADR-1099, current `core/test/meson.build`, and merged PR #840 at
  `cf3ff8a8c136394345f304e7f8beeeb9c1423b1b`.

**Scope:** Targeted diagnosis of repeatedly failing scheduled jobs and stale
test-quarantine state. This is not a claim that every repository workflow was
live-run or green. Netflix golden assertions, score snapshots, model weights,
runner configuration, benchmarks, tuning, and retraining are unchanged.

## 1. Nightly whole-tree clang-tidy failure

The 15 newest `master` runs queried on 2026-09-25 each reported `failure` for
the job named `Full clang-tidy scan`. In the newest run (`35975614507`), the
other two jobs (`ThreadSanitizer` and `Netflix benchmark suite`) succeeded, so
the workflow failure is isolated to the ratchet job.

The uploaded report for that run records:

- `clang_tidy_version`: `21.1.8`;
- `cc_version`: GCC 15.2.0;
- 309 measured translation units;
- 295 compile failures, making the measurement unusable.

The old nightly configuration relied on the runner's ambient clang-tidy and
configured the project without `-Db_lto=false`. `core/meson.build` enables LTO
with four threads, producing GCC's `-flto=4`; clang rejects that spelling when
clang-tidy parses the GCC compilation database. The required CPU ratchet lane
already carries the repaired contract: GCC 15, LLVM 22 clang-tidy, and LTO
disabled for analyzer-only compilation metadata.

The remediation copies that contract into the nightly job, including the GCC
15 PPA, apt.llvm.org LLVM 22 installer, hash-locked build requirements,
`CC=gcc-15 CXX=g++-15`, `-Db_lto=false`, and the explicit
`--clang-tidy /usr/bin/clang-tidy-22` argument.

## 2. Fuzz job timeout headroom

Scheduled fuzz run `35981061040` recorded the `fuzz_cli_parse` matrix job as
cancelled after 16m36s under a 15-minute job ceiling. The adjacent scheduled
run `35842763631` completed the same target successfully in 11m35s. The target
installs Clang 22, configures AddressSanitizer, builds full libvmaf, and then
runs a bounded 60-second fuzz interval; the variable install/build portion has
too little margin under a 15-minute ceiling.

The sanitizer workflow contains a second scheduled `fuzz_cli_parse` job with
the same Clang 22 plus ASan build shape. Its latest inspected run
(`35981157602`) succeeded in 10m03s, but retaining a different 15-minute ceiling
would preserve the same known headroom defect. Both job timeouts therefore move
to 30 minutes. The fuzzer's own per-input and per-target bounds remain
unchanged, so this does not make individual fuzz execution unbounded.

## 3. ADR-1093 quarantine state

ADR-1093 described temporary `should_fail: true` registrations for two tests.
Both underlying defects were later repaired:

- ADR-1099 restored the SYCL motion add-UV test.
- PR #840 (`cf3ff8a8c`) repaired the picture-pool CUDA layout mismatch.

Current `core/test/meson.build` registers `test_pic_preallocation` normally and
explicitly records that `should_fail` was removed from
`test_sycl_motion_add_uv_parity`. ADR-1093 is therefore superseded rather than
an active waiver.

## 4. Regression contract

`scripts/ci/test_fail_closed_ci.py` binds the repair to the actual job blocks:

- the nightly job must retain the GCC 15 repository, GCC 15 compiler selection,
  disabled LTO, and explicit clang-tidy 22 binary;
- the standalone and sanitizer fuzz jobs must each retain the 30-minute budget.

The timeout increase is intentionally exact rather than a weak `>= 30` prose
claim: changing it requires updating the executable contract and the decision
record together.

## 5. Reproduction and verification

Live evidence was queried with:

```bash
gh run list --workflow nightly.yml --branch master --limit 15 --json databaseId
gh run view 35975614507 --json jobs
artifact_dir=$(mktemp -d)
gh run download 35975614507 --name clang-tidy-full-report --dir "$artifact_dir"
gh run view 35981061040 --json jobs
gh run view 35842763631 --json jobs
gh run view 35981157602 --json jobs
```

Local regression commands:

```bash
python3 -B scripts/ci/test_fail_closed_ci.py
python3 -B scripts/ci/check-source-adr-citations.py
bash scripts/ci/check-state-md-rows.sh
python3 -B scripts/docs/check-adr-index.py
python3 -B scripts/ci/check-adr-links.py
```

## 6. Rebase and maintenance notes

The nightly CPU ratchet is not an independent toolchain policy. When the
required CPU ratchet changes compiler family/version, build directory, LTO
handling, or clang-tidy binary, update the scheduled lane and its contract in
the same change. The two fuzz workflows intentionally retain identical timeout
headroom for their shared full-libvmaf target.
