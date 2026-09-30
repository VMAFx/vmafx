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

`hw_encoder_corpus.py` is a long-running, append-only corpus producer. It may
retain rows from successful quality points for diagnosis, but any encode,
decode, score, or canonical-row failure must make the process return non-zero.
Never turn a failed quality point into a successful partial corpus. Preserve
the positive, failed-encode, empty-metrics, and missing-input controls in
`tests/test_hw_encoder_corpus.py`.

`resolve-state-md-conflict.py` (ADR-1383) resolves a conflicted
`docs/state.md` from the index stages `:1:` / `:2:` / `:3:`, never from
conflict markers. Preserve:

- rows and move tombstones keyed by bug id; state = text plus `##` section
- disposition rows keyed by bold label; id list merged as a set, ours' order
  first, theirs' additions after; same-label repeats folded before the merge
- one-side change wins; both sides changed differently -> exit 1, nothing
  written
- `--take NAME=ours|theirs` as the only override
- LF bytes via `write_bytes`; row gate run on the result, exit 3 on reject

Id and tombstone patterns mirror `scripts/ci/check-state-md-rows.sh`; change
both together. Never restore "ours wins": mid-rebase ours already holds the
replayed branch commits. `test-resolve-state-md-conflict.py` drives real
`git rebase` conflicts and runs in the Rules workflow and under
`scripts/ci/test_git_fixture_isolation.py`.

`rc3-home-gpu-retest.sh` (ADR-1386) runs the `ryzen-4090-arc` verify and
time commands of `docs/state.md` rows. Preserve:

- one explicit entry per row and backend; never parse `docs/state.md` at run
  time
- row gains or changes a command for this box -> same PR edits its entry;
  closed row may drop its entry
- every device run under its `flock` (`cuda-4090`, `hip-gfx1036`,
  `sycl-a380` `.lock`); timing block takes lock before clock, holds it through
  its reps; never two device locks at once; CPU runs unlocked
- `lock_take` local stays `_take_fd`: bash scope is dynamic, a local named
  like the caller's variable leaves the lock held, next `lock_take` on that
  file waits on itself; `FakeDeviceRunTests` catches it
- JSON compare and summaries only in `rc3_retest_helpers.py`

`tests/test_rc3_home_gpu_retest.py` (pre-commit `rc3-home-gpu-retest-contract`)
checks every entry names an existing row and runs the real path on a fake
`vmaf`.

`hip_dispatch_drop_probe.hip` (T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01)
is a standalone HIP program, not built by Meson. Keep it vmafx-free: one
stream, memset + kernels + readback per frame, a never-reset dispatch
counter, exit 1 on any wrong slot or lost dispatch. Its value is that it
reproduces the gfx1036 command loss without libvmaf; do not route it through
vmafx code or relax its checks. The state row names the command that closes
the row.
