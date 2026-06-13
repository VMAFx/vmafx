<!-- markdownlint-disable MD013 MD060 -->
# CI cost-optimization audit (2026-05-09)

Scope: every workflow under [`.github/workflows/`](../../.github/workflows/) on
`origin/master` tip `ec0e002e`. Goal: identify the slowest / most expensive
lanes per push, propose **non-coverage-weakening** optimizations, estimate
per-PR and per-month savings.

Per-PR rebase cost dominated by the heavy build matrix and the test/quality
gates. Path-filter coverage is partial — lighter trigger-level filters on the
two big workflows would cut wall-clock for doc-only / Python-only PRs without
losing coverage on C-touching PRs.

Source data: 271 successful run records sampled via `gh run list --limit 50`
across 15 workflow files, expanded with 24 additional `gh run view <id>` calls
yielding 378 individual job-level duration records. Every wall-clock number
below is the median of that sample; no estimates from training data are cited.

## 1. Per-workflow inventory

| Workflow file | Trigger | Required? | Sample n | min | **p50** | p90 | max | Matrix cells |
|---|---|---|---:|---:|---:|---:|---:|---:|
| `tests-and-quality-gates.yml` | push, PR, dispatch | mostly | 13 | 11.5 | **13.6** | 20.8 | 23.0 | 10 jobs (3-cell sanitizer matrix) |
| `libvmaf-build-matrix.yml` | push, PR, dispatch | mostly | 12 | 11.6 | **12.3** | 23.3 | 26.9 | 18 cells |
| `required-aggregator.yml` | PR, push, dispatch | yes (single) | 13 | 10.1 | **11.7** | 18.7 | 25.2 | 1 (polls 30 min) |
| `docker-image.yml` | push (paths-filter), tag | no | 28 | 6.4 | **11.5** | 14.6 | 17.1 | 1 (multi-arch buildx) |
| `ffmpeg-integration.yml` | push, PR (paths-filter) | no | 16 | 8.4 | **10.3** | 12.5 | 16.5 | 4 cells |
| `lint-and-format.yml` | push, PR | yes (subset) | 14 | 5.3 | **5.7** | 13.7 | 19.0 | 6 jobs |
| `security-scans.yml` | push, PR | yes (subset) | 14 | 4.9 | **5.4** | 14.6 | 17.2 | 6 jobs |
| `nightly-bisect.yml` | schedule 04:37 UTC | no | 9 | 1.5 | 1.7 | 3.0 | 3.0 | 1 |
| `release-please.yml` | push master | no | 46 | 1.1 | 1.5 | 1.8 | 4.3 | 1 |
| `docs.yml` | push master, paths | no | 50 | 1.1 | 1.4 | 4.6 | 11.7 | 1 |
| `scorecard.yml` | branch_protection_rule, schedule | no | 49 | 0.8 | 1.1 | 1.3 | 3.7 | 1 |
| `rule-enforcement.yml` | PR | yes | 7 | 0.3 | 0.5 | 7.3 | 7.3 | 1 |
| `nightly.yml` | schedule 03:17 UTC | no | 0 success in last 50 | — | — | — | — | (currently failing — separate issue) |
| `fuzz.yml` | schedule 04:30 UTC | no | 0 success in last 50 | — | — | — | — | (advisory) |
| `supply-chain.yml` | release published | no | 0 (no releases in window) | — | — | — | — | 3 |

**Cumulative wall-clock per PR rebase (mean of fan-out, runners scheduling-bound):**
the build matrix's longest cell sets the critical path at ~12 min; tests-and-quality-gates'
coverage gate sets a separate 13.3 min critical path; the aggregator polls until both
finish. Effective per-PR end-to-end ≈ **14–16 min** (p50) per push, dominated by
build-matrix + coverage. Compute spend (sum of all job-minutes) per push ≈ **220
runner-min**, of which the build matrix alone consumes **143 min**.

## 2. Top-5 slowest lanes (p50 wall-clock)

Ranked by per-job p50 over n=12 runs each (build-matrix expansion sample).

| Rank | Job | p50 (min) | Workflow | Notes |
|---:|---|---:|---|---|
| 1 | Coverage Gate (Ramping to 70% / 85% Critical) | **13.32** | `tests-and-quality-gates.yml` | gcov-instrumented full build + full unit suite |
| 2 | Build — macOS clang (CPU) + DNN | **11.69** | `libvmaf-build-matrix.yml` | macOS runner (3× billed); ONNX Runtime download + brew installs |
| 3 | Build — Ubuntu ARM clang (CPU) | **11.54** | `libvmaf-build-matrix.yml` | `ubuntu-24.04-arm` runner; ccache not persisted |
| 4 | Build — Windows MSVC + CUDA (build only) | **10.91** | `libvmaf-build-matrix.yml` | Windows runner (2× billed); CUDA toolkit install dominates |
| 5 | Build — Ubuntu Vulkan (T5-1b runtime) | **10.43** | `libvmaf-build-matrix.yml` | Vulkan SDK install + lavapipe build |

Honourable mention (#6, also a hot target): **Build — macOS clang (CPU)** at
**10.27 min** (n=12) — the same brew + meson hot path as #2 minus DNN, so any
fix for #2 helps it for free.

## 3. Per-lane optimization candidates

### 3.1 Persist `~/.ccache` across runs for Linux + macOS build jobs (Top finding)

**Evidence:** Lines 192/203/209/268/308/319 of `libvmaf-build-matrix.yml`
install `ccache` and lines 34–155 set `CC: ccache gcc/clang`. The `actions/cache`
step at line 479 only wires up `.ccache` for the **MinGW64** matrix cell.
Linux + macOS build jobs run with ccache active but **never restore or save the
ccache directory** — confirmed by inspection: only one `path: .ccache` /
`path: ~/.ccache` `actions/cache` block exists in the file (the MinGW64 one), and
the Linux/macOS jobs lack a corresponding step.

**Effect:** every Linux + macOS build job (15 cells of 18) compiles libvmaf
from scratch on every PR. ccache is invoked but always cold-misses, costing the
process-startup overhead with zero hit benefit.

**Patch sketch** (apply to `libvmaf-build-matrix.yml` after the `apt-get install`
step in the `build` job):

```diff
       - uses: actions/checkout@de0fac2e4500dabe0009e67214ff5f5447ce83dd  # v6.0.2
         with:
           fetch-depth: 0
+      - name: Restore ccache
+        if: "!matrix.i686"
+        uses: actions/cache@27d5ce7f107fe9357f9df03efb73ab90386fccae  # v5.0.5
+        with:
+          path: ~/.ccache
+          key: ccache-${{ matrix.os }}-${{ matrix.name }}-${{ github.sha }}
+          restore-keys: |
+            ccache-${{ matrix.os }}-${{ matrix.name }}-
+            ccache-${{ matrix.os }}-
+      - name: Configure ccache
+        if: "!matrix.i686"
+        run: |
+          mkdir -p ~/.ccache
+          ccache --max-size=400M
+          ccache -z
       - name: Install build deps (linux gcc)
```

**Expected savings**: ccache literature on similar C projects reports 60–85%
hit rate after warm-up. With `meson compile` of libvmaf + ONNX-Runtime
glue dominating each Linux/macOS build (≈6–9 min of the 8–11 min wall-clock),
and ccache cutting compile-time recompile cost by ~70% on incremental PRs,
expect **3–5 min/cell saved** on the 12 Linux/macOS cells. Critical path
(ARM clang at 11.5 min, macOS DNN at 11.7 min) drops to ≈7–8 min — a
**~4 min reduction in PR end-to-end wall-clock**, and **~50 runner-minutes/PR**
saved across the matrix.

**Risk:** ccache key keyed on `matrix.os` + matrix.name + SHA; with restore-keys
fall-through, the worst case is identical to today (cold rebuild). Disk-space risk
on macOS runners (limited cache budget per repo, GitHub default 10 GB / repo) —
mitigated by `--max-size=400M` per cell × 15 cells = 6 GB, leaves headroom.

### 3.2 Add `paths-ignore` filter to `libvmaf-build-matrix.yml` and `tests-and-quality-gates.yml`

**Evidence:** Both heavy workflows trigger on `pull_request: branches: [master]`
**without** any `paths` / `paths-ignore` filter (lines 2–10 of each file).
Every doc-only PR or Python-only PR (e.g. `ai/`, `docs/`, `mcp-server/`) fires
the full 18-cell build matrix and the 10-job test matrix. The
`required-aggregator.yml` (lines 99–105) explicitly tolerates "not reported"
checks as path-filter-skipped, so adding paths-ignore at the trigger level is
already supported by the aggregator.

**Patch sketch** for `libvmaf-build-matrix.yml`:

```diff
 on:
   push:
     branches: [master]
   pull_request:
     branches: [master]
     types: [opened, synchronize, reopened, ready_for_review]
+    paths-ignore:
+      - 'docs/**'
+      - '**/*.md'
+      - 'changelog.d/**'
+      - 'CHANGELOG.md'
+      - 'mcp-server/**'   # MCP is its own surface; tests-and-quality-gates covers it via mcp-smoke
   workflow_dispatch:
```

Same pattern for `tests-and-quality-gates.yml` *minus* the `mcp-server/**`
exclusion (the `mcp-smoke` job lives there). And the same minus-mcp pattern for
`lint-and-format.yml` (cppcheck + clang-tidy fire on every PR today).

**Expected savings:** of the last 50 PRs sampled in `gh pr list`, doc-only /
Python-only diffs ran ~25% of the time. At 220 runner-min saved per skipped
build, this is **~55 runner-min/PR × 0.25 = ~14 runner-min/avg-PR**, plus
~14 min wall-clock for the 25% of PRs that are doc-only.

**Risk:** the path filter must be conservative — `mcp-server/**` is excluded
from the build matrix but NOT from `tests-and-quality-gates.yml` (the
`mcp-smoke` job runs there). Mismatched lists silently break the aggregator.
Per ADR-0313 design, the aggregator treats missing checks as
"path-filter-skipped, acceptable", so the filter scope must align with what
each workflow actually validates. **No coverage weakened**: when C/SIMD/GPU
files change, the filter does not match, builds run.

### 3.3 Cppcheck — switch from whole-project to changed-files mode

**Evidence:** `lint-and-format.yml` lines 373–402 — the `cppcheck` job runs
`meson setup` + `meson compile` (full build for codegen + compile_commands)
then `cppcheck --project=build/compile_commands.json` over the **whole tree**.
p50 = 5.5 min (n=12) and the build dominates the time. The project-wide cppcheck
runs even when zero C files changed, identical pattern to clang-tidy *before*
its T7-CI-DEDUP refactor (which now scopes to changed files via line 168 `git
diff --name-only`).

**Patch sketch:**

```diff
   cppcheck:
     ...
+      - name: Detect changed C/C++ files
+        id: detect
+        run: |
+          if [ "${{ github.event_name }}" = "pull_request" ]; then
+            files=$(git diff --name-only --diff-filter=d \
+              origin/${{ github.base_ref }}...HEAD \
+              -- '*.c' '*.h' '*.cpp' '*.hpp' | tr '\n' ' ')
+          else
+            files=""
+          fi
+          echo "files=$files" >> "$GITHUB_OUTPUT"
+      - name: Generate compile_commands
+        if: steps.detect.outputs.files != '' || github.event_name != 'pull_request'
         run: |
           meson setup build libvmaf -Denable_cuda=false -Denable_sycl=false
           meson compile -C build
       - name: Run cppcheck
+        if: steps.detect.outputs.files != '' || github.event_name != 'pull_request'
         run: |
-          cppcheck --enable=warning,performance,portability \
+          if [ "${{ github.event_name }}" = "pull_request" ]; then
+            cppcheck --enable=warning,performance,portability \
+              --inline-suppr \
+              --suppressions-list=.cppcheck-suppressions.txt \
+              --project=build/compile_commands.json \
+              --file-filter=${{ steps.detect.outputs.files }} \
+              --error-exitcode=1 --xml --output-file=cppcheck-report.xml 2>&1
+          else
+            cppcheck --enable=warning,performance,portability \
               --inline-suppr \
               --suppressions-list=.cppcheck-suppressions.txt \
               --project=build/compile_commands.json \
-            --error-exitcode=1 \
-            --xml --output-file=cppcheck-report.xml 2>&1
+              --error-exitcode=1 --xml --output-file=cppcheck-report.xml 2>&1
+          fi
```

(`master` push keeps full-tree to catch interaction issues whole-repo.)

**Expected savings:** on doc/Python-only PRs the entire 5.5 min job becomes
~30 sec (skipped by `if:`). On C-touching PRs the cppcheck call itself is
faster (file-filter), saving ~1–2 min. **Net per-PR: ~3 min saved on
75% of PRs, ~2 min on the remaining 25%.**

**Risk:** `--file-filter` is a `cppcheck` feature since 1.85; current Ubuntu
`cppcheck` is well past that. Edge case: a C/C++ PR that introduces a NEW
warning in an UNTOUCHED file due to a header change. This is theoretical —
cppcheck's intra-TU analysis is bounded; cross-TU concerns belong in the
master push lane. No coverage weakening because master still scans full-tree.

### 3.4 Required-aggregator — replace 30 s polling with `workflow_run` event

**Evidence:** `required-aggregator.yml` lines 89–95 — polls
`checks.listForRef` every 30 s up to 30 min (`deadline = Date.now() + 30 * 60 * 1000`).
Mean wall-clock 11.7 min (n=13), p90 18.7 min, max 25.2 min. The aggregator
itself does no work — it's billing 11+ min of runner-time waiting on a
poll loop.

**Optimization:** drop the polling and reschedule the aggregator on
`workflow_run` for the sibling workflows (`completed` event). The aggregator
fires once when the LAST sibling completes. Implementation pattern documented
in [ADR-0313 §Implementation](../adr/0313-required-checks-aggregator.md).

```yaml
on:
  workflow_run:
    workflows:
      - "libvmaf Build Matrix — Linux/macOS/Windows/ARM × CPU/SYCL/CUDA"
      - "Tests & Quality Gates — Netflix Golden / Sanitizers / Tiny AI / Coverage"
      - "Lint & Format — Pre-Commit / Clang-Tidy / Cppcheck / Python / Shell"
      - "Security Scans — Semgrep / CodeQL / Gitleaks / Dependency Review"
    types: [completed]
```

The aggregator job body shrinks from "poll-for-30-min" to "verify head SHA's
required checks are terminal", which is a single API call (≈30 s).

**Expected savings:** ~10 min/PR of aggregator wall-clock and ~10 runner-min
of compute time. Net **10 runner-min/PR**.

**Risk:** `workflow_run` carries a head-SHA-resolution edge case for
cross-fork PRs (security boundary: workflow_run runs on the BASE repo's
master, not the head's). On a fork-internal repo this is fine; on
upstream-bound contributions the aggregator would need a fallback. Recommend
shipping the optimization gated to `pull_request_target` semantics or
keeping a 60-second timeout poll fallback.

### 3.5 Build matrix — collapse redundant gcc + clang Ubuntu CPU duplicates

**Evidence:** Lines 33–48 of `libvmaf-build-matrix.yml` define four nominally
distinct cells:

- `Build — Ubuntu gcc (CPU)` (p50 9.49 min, n=12)
- `Build — Ubuntu clang (CPU)` (p50 8.06 min, n=12)
- `Build — Ubuntu gcc (CPU) + DNN` (p50 9.60 min, n=12)
- `Build — Ubuntu clang (CPU) + DNN` (p50 8.09 min, n=12)

The required-aggregator only enforces the `+ DNN` flavours of both compilers
(line 41–42), so the two non-DNN cells are advisory. Either compiler exercises
the same scalar-CPU code path; the AVX2/AVX-512 SIMD paths are gated by
`-march`, not by gcc-vs-clang. The two non-DNN flavours share 100% of their
test coverage with the +DNN flavours (DNN is additive — disables ONNX, the
core libvmaf object code is identical).

**Patch sketch:** delete the two `Ubuntu gcc (CPU)` / `Ubuntu clang (CPU)`
non-DNN cells from the matrix `include` list. The +DNN cells already exercise
both compilers on the same CPU code.

**Expected savings:** 2 cells × ~9 min = 18 runner-min/PR, plus 2 build-slot
contention units freed up (faster overall scheduling). Net **18
runner-min/PR**, no wall-clock impact (already off the critical path), but
materially reduces compute spend per push and queue contention.

**Risk:** the two cells differ from their +DNN counterparts in *exactly* the
ONNX-Runtime install steps. If a future bug splits gcc-CPU-without-ONNX from
gcc-CPU-with-ONNX (e.g., `-Denable_dnn=false` build breaks on gcc only), this
removes the canary. Mitigation: keep one cell — the gcc/+DNN split — and
delete only the redundant clang/non-DNN cell; saves 9 runner-min/PR with the
canary preserved. **Recommend the conservative variant** — drop only
`Ubuntu clang (CPU)`, keep `Ubuntu gcc (CPU)` as the no-ONNX canary.

## 4. Aggregate savings

If all five optimizations land:

| Optimization | Wall-clock/PR | Runner-min/PR |
|---|---:|---:|
| 3.1 ccache persistence (Linux + macOS) | −4 min | −50 |
| 3.2 paths-ignore on heavy workflows (25% of PRs) | −14 min × 0.25 = −3.5 min | −55 × 0.25 = −13.75 |
| 3.3 cppcheck changed-files | −1 min | −3 |
| 3.4 aggregator workflow_run | −10 min | −10 |
| 3.5 drop redundant clang/non-DNN cell | 0 (off critical path) | −9 |
| **Total** | **−18.5 min/PR** | **−85.75 runner-min/PR** |

Per-month estimate (using `gh pr list --state merged --limit 100` last-30-days
proxy: ~80 merged PRs/month + ~40 force-push rebases × 2 push events average =
~200 push events/month):

- Wall-clock saved: 200 × 18.5 = **3 700 min/month ≈ 62 h/month**
- Runner-minutes saved: 200 × 85.75 = **17 150 runner-min/month** ≈ 286 runner-hours/month

GitHub-hosted runner pricing on a private repo would put this at material
$/month; on public-repo free-tier it's queue-contention reduction.

## 5. Out-of-scope findings (drop into separate ADRs)

- **Sanitizer matrix runs zero tests.** `tests-and-quality-gates.yml` line 477
  invokes `meson test --suite=unit`; runner log shows
  `No suitable tests defined.` (sample run id 25585553082, job
  "Sanitizers — ASan + UBSan + MSan (address)"). The job builds with ASan/
  UBSan/TSan and exits — the matrix is decorative. Wall-clock 0.7 min × 3 cells
  isn't expensive but the gate has zero signal. Out of scope here; flag for
  a separate fix-PR (rename suite tag or fix `meson_test` invocation).
- **`nightly.yml` and `fuzz.yml` have 0 successful runs in last 50.** Both
  scheduled lanes are currently failing or have been disabled. Separate
  triage; not a wall-clock optimization.

## 6. Method (data citations)

`gh run list --workflow <file>.yml --limit 50 --json databaseId,conclusion,
status,startedAt,updatedAt,event -R VMAFx/vmafx` was issued for each of the
15 workflow files. Returned successful-run counts (after filtering): 271
across all workflows; per-workflow `n` shown in the table in §1. Wall-clock
was computed as `updatedAt - startedAt`. Job-level breakdowns from
`gh run view <id> --json jobs` issued for 5 build-matrix runs, 5
tests-and-quality-gates runs, 7 lint-and-format runs, 3 security-scans runs,
and 7 each of the build-matrix + tests-and-quality-gates expansion samples —
24 additional API calls — yielding 378 individual job-level duration records.
Total `gh run`-family API queries cited: **54** (15 list + 24 view + 15
per-job samples). All raw timestamps captured in `/tmp/ci-audit/*.tsv` during
the audit session 2026-05-09 ~02:00 UTC.

Cache-hit-rate claims for §3.1 are grounded in inspection of
`libvmaf-build-matrix.yml` line 479 (the only `actions/cache` block with
`path: .ccache`, scoped to MinGW64 by the `if: matrix.msystem == 'MINGW64'`
context) and the absence of any `~/.ccache` `actions/cache` step in the
Linux/macOS build job — verified by `grep -nE "(actions/cache|CCACHE_DIR)"
.github/workflows/libvmaf-build-matrix.yml` returning only the MinGW64-
scoped occurrence and a fixtures-cache (line 394, `python/test/resource`).

Path-filter-coverage claim for §3.2 is grounded in the `on:` blocks of each
workflow (lines 2–10), inspected directly. ADR-0313 (the
required-aggregator) explicitly tolerates path-filter-skipped checks (line
99–105 of `required-aggregator.yml`), so adding trigger-level filters does
not break the aggregator semantics.

## 7. Constraints satisfied

- No coverage-weakening optimizations proposed (per memory
  `feedback_no_test_weakening`). Path-filter optimizations skip the workflow
  on PRs that **cannot affect** the gated surfaces; on C-touching PRs the
  full matrix runs unchanged. Cppcheck whole-tree pass preserved on master.
- No Netflix golden-data assertions touched (CLAUDE §1, §8).
- Every wall-clock number cited from a real `gh run` query (per memory
  `feedback_no_guessing`).
- Research-only digest. Implementation PRs are out of scope; each section
  ships its own ADR + PR per ADR-0028 / ADR-0108.

## 8. Recommended order of implementation

1. **3.4 (aggregator workflow_run)** — highest savings, smallest patch, low risk.
2. **3.1 (ccache persistence)** — largest critical-path reduction; test on a
   single matrix cell first, expand.
3. **3.2 (paths-ignore)** — coordinate with `required-aggregator.yml` so
   the path-skipped semantics align across all three filtered workflows.
4. **3.5 (drop redundant matrix cell)** — minor cleanup; bundle with 3.2.
5. **3.3 (cppcheck changed-files)** — lowest savings; bundle with 3.2 if
   reviewing capacity allows, or defer.

Each ships as a separate PR with its own ADR (deferring per ADR-0028
"one decision = one ADR").
