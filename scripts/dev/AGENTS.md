# Developer control scripts

`merge_train_guard.py` (ADR-1244) gates every merge-train mutation. Preserve:

- master-only selection
- release/hold/owner exclusions on every action
- exact-head leases
- rebase-before-ready ordering
- error propagation

Never reuse or force-remove existing owner's checkout. Full local gate
receipt = both Make commands run on clean source; zero required checks and
handwritten pass flags do not count as validation. Gateway changes require
its disposable-repository tests plus operator-guide update at
`docs/development/merge-train.md`. Runtime scripts stay local state, never
shipped; reviewing and migrating running processes differs from shipping
source.

`check_repository_security.py` checks repository security policy (ADR-1248,
amended by ADR-1252) against named active master ruleset and its effective
rules. Preserve:

- fixed repository/HTTPS-host targeting
- exact lists and scalar types
- GitHub Actions origin on aggregate check
- bypass count verified against **declared** actor list, even when REST
  hides actors

Policy declares exactly one `User` bypass actor (ADR-1252). Do not relax
this to role tier. Do not restore unconditional zero-bypass assertion
without first removing that actor from live ruleset. Missing, truncated, or
error API data must fail. Checker only reads; CI must never gain
administration credentials or automatic apply path. Run its offline
adversarial controls after every change.

Policy bypass mode = `pull_request`, review count 0, no last-push approval
(ADR-3313, supersedes ADR-3273); no exception entry. `bypass_mode_findings()`
fails on another mode without unexpired entry of rule
`solo-repository-bypass-mode` for policy file in `.config/lint-exceptions.d/`,
and on stale entry while every mode is `pull_request`. Never drop the call
from `main()`, never widen the rule to a second file.

`hw_encoder_corpus.py` is long-running, append-only corpus producer. It may
retain rows from successful quality points for diagnosis, but any encode,
decode, score, or canonical-row failure must make process return non-zero.
Never turn failed quality point into successful partial corpus. Preserve
positive, failed-encode, empty-metrics, one-failed-point, missing-input and
non-executable-binary controls in `tests/test_hw_encoder_corpus.py`.
`main(argv)` keeps its argument: the tests drive it, and a stale-base merge
(#1509 over #1518) once dropped it together with the non-zero exit.

`resolve-state-md-conflict.py` (ADR-1383) resolves conflicted
`docs/state.md` from index stages `:1:` / `:2:` / `:3:`, never from
conflict markers. Preserve:

- rows and move tombstones keyed by bug id; state = text plus `##` section
- disposition rows keyed by bold label; id list merged as a set, ours' order
  first, theirs' additions after; same-label repeats folded before merge
- one-side change wins; both sides changed differently -> exit 1, nothing
  written
- `--take NAME=ours|theirs` as only override
- LF bytes via `write_bytes`; row gate run on result, exit 3 on reject

Id and tombstone patterns mirror `scripts/ci/check-state-md-rows.sh`; change
both together. Never restore "ours wins": mid-rebase ours already holds
replayed branch commits. `test-resolve-state-md-conflict.py` drives real
`git rebase` conflicts and runs in Rules workflow and under
`scripts/ci/test_git_fixture_isolation.py`.

`rc3-home-gpu-retest.sh` (ADR-1386) runs a reference workstation verify and
time commands of `docs/state.md` rows. Preserve:

- one explicit entry per row and backend; never parse `docs/state.md` at run
  time
- row gains or changes command for this box -> same PR edits its entry;
  closed row may drop its entry
- every device run under its `flock` (`cuda-4090`, `hip-gfx1036`,
  `sycl-a380` `.lock`); timing block takes lock before clock, holds it through
  its reps; never two device locks at once; CPU runs unlocked
- `lock_take` local stays `_take_fd`: bash scope is dynamic, local named
  like caller's variable leaves lock held, next `lock_take` on that
  file waits on itself; `FakeDeviceRunTests` catches it
- JSON compare and summaries only in `rc3_retest_helpers.py`

`tests/test_rc3_home_gpu_retest.py` (pre-commit `rc3-home-gpu-retest-contract`)
checks every entry names existing row and runs real path on fake
`vmaf`.

`hip_dispatch_drop_probe.hip` (T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01)
is standalone HIP program, not built by Meson. Keep it vmafx-free: one
stream, memset + kernels + readback per frame, never-reset dispatch
counter, exit 1 on any wrong slot or lost dispatch. Its value is that it
reproduces gfx1036 command loss without libvmaf; do not route it through
vmafx code or relax its checks. State row names command that closes
row.

`preflight.sh --stage msvcism` runs `find-posix-only-headers.py` (ADR-2646):
POSIX-only header outside platform conditional, in source Windows build
compiles, fails stage. Scanner reads `meson.build` gates
(`host_machine.system() != 'windows'`, `elif` / `else`, `foreach` lists,
`subdir()`), counts header through its includers. Scan that cannot run fails
stage; never swallow its exit status. Exceptions only in
`.config/lint-exceptions.d/msvcism-posix-headers.toml` (file, reason, expiry).
Keep mcp / fuzz / `vmaf_vpl` Meson gates; Windows request for mcp or fuzz
errors at configure, never silent skip. Planted cases:
`scripts/ci/tests/test-preflight-msvcism.sh`.

`preflight.sh --stage msvcism` runs `find-windows-minmax.py`: `::min(` /
`::max(` call behind `<windows.h>` without `NOMINMAX`, in C++ translation unit
Windows build compiles, fails stage (MSVC C2589). Scanner walks includes in
Windows preprocessor order with explicit frame stack, bounds `MAX_UNITS` /
`MAX_STEPS` / `MAX_DEPTH`; hitting bound = error, never pass. Reuses
`git_lines()` + `windows_builds_source()` of `find-posix-only-headers.py`;
no second copy of Meson gate reader. Fix spelling: `(std::min)(a, b)`.
`core/src/compat/win32/pthread.h` includes `<windows.h>` without `NOMINMAX`;
every C++ unit taking `<pthread.h>` on Windows: both macros defined. Not
followed: system / SDK headers, CUDA device code, C sources, macro bodies,
command-line `NOMINMAX`. Planted cases + failed-scan case: same test file.
