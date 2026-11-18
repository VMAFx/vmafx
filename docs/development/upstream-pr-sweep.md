<!-- markdownlint-disable MD013 -->
# Upstream PR sweep tool

`scripts/dev/upstream_pr_sweep.py` replaces the volatile ad-hoc job scripts
(`uprebase.sh` and `upbuild.sh`) with a single tracked, tested, and bounded tool.
It automates rebasing, signing verification, optional building and testing, and
safe force-pushing of our open [Netflix/vmaf](https://github.com/Netflix/vmaf)
pull request branches against `upstream/master`.

## Purpose and design

When maintaining multiple pull requests against upstream `Netflix/vmaf`, branch
heads must periodically be rebased on `upstream/master` to ensure clean merges,
verify commit signatures, and ensure test suites continue to pass.

The tool provides:

- **Isolated worktree execution**: Each PR is evaluated in a dedicated detached
  worktree registered to `--repo` and serialized by a per-worktree lock file
  (`<worktree>.lock`), preventing workspace pollution and concurrent races.
- **Fail-safe refusals**: Invariant checks run before touching any ref or worktree.
- **Lease-protected updates**: Pushes strictly target the `origin` remote using
  `--force-with-lease` bound to the observed pre-rebase commit SHA.
- **Deterministic logs**: Build and test logs are written outside the worktree,
  ensuring worktrees remain clean.

## Newcomer workflow

The tool defaults to dry-run mode. Mutations require explicit flags.

### 1. Dry run (default)

Inspect open PRs and view the planned rebase actions without modifying any ref,
worktree, or file:

```bash
# Using an explicit PRs list file ("<number> <branch> <head_owner>";
# e.g. "1707 fix/cambi-heatmaps VMAFx", or "<number> <branch>" where
# head_owner defaults to the owner in --fork)
python3 scripts/dev/upstream_pr_sweep.py --fork VMAFx/vmafx --prs prs.txt

# Or discover open PRs automatically via the GitHub CLI
python3 scripts/dev/upstream_pr_sweep.py --fork VMAFx/vmafx --discover
```

### 2. Apply rebase and verify signatures

Fetch the current `upstream/master` and `origin/<branch>` tips, execute the
rebase onto `upstream/master`, and verify commit signatures:

```bash
python3 scripts/dev/upstream_pr_sweep.py --fork VMAFx/vmafx --prs prs.txt --apply
```

To also execute the Meson setup, Ninja build, and test suite in `libvmaf`:

```bash
python3 scripts/dev/upstream_pr_sweep.py --fork VMAFx/vmafx --prs prs.txt --apply --build --jobs 4
```

Meson setup, Ninja compilation, and the test suite run with `build_environment()`,
which strips credential-bearing variables (`sanitize_process_environment` from
`scripts/ci/run_meson_test.py`, ADR-1333) and restricts the child environment to
an allow-list of build and compiler variables.

Branches matching `--cuda-prefix` (default `fix/cuda-`) automatically enable
CUDA options (`-Denable_cuda=true -Denable_nvcc=true`).

### 3. Push rebased branches

Once validated, push the rebased branch to `origin` with `--force-with-lease`:

```bash
python3 scripts/dev/upstream_pr_sweep.py --fork VMAFx/vmafx --prs prs.txt --apply --push
```

Note: `--push` strictly requires `--apply`.

## Refusals and safety invariants

Any invariant failure marks the affected PR row as `REFUSED` and prevents
subsequent mutations:

1. **Not ours**:
   - `head_owner` (and `headRepository` when discovered via `gh`) must match
     `--fork`.
   - Both the fetch URL and all push URLs (`git remote get-url --push --all origin`)
     of `--repo` and the PR worktree are parsed and compared case-insensitively
     against `--fork`; embedded URL credentials and query strings are redacted
     before any refusal detail is formatted.
   - Every commit in `base..old_sha` must have an author and committer email in
     the operator's configured emails (`git config --get-all user.email`,
     `GIT_AUTHOR_EMAIL`, `GIT_COMMITTER_EMAIL`) unless `--allow-foreign-authors`
     is passed, preventing foreign commits from being signed with the operator's
     key during `git rebase --gpg-sign`.
2. **Registered clean worktree and lock**:
   - Each PR worktree acquires a non-blocking exclusive lock (`<worktree>.lock`);
     contention refuses the row.
   - If the PR worktree path already exists, it must appear as a linked worktree
     in `git -C <repo> worktree list --porcelain` and `git status --porcelain`
     must be empty.
   - Worktrees are prepared in detached HEAD mode (`git checkout --detach <old_sha>`)
     so a branch checked out in another worktree does not block rebasing.
3. **Caller repository untouched**:
   - Every `git` and `gh` call runs with `git_environment()`, which drops each
     `GIT_*` variable that locates a repository (`GIT_DIR`, `GIT_WORK_TREE`,
     `GIT_INDEX_FILE`, ...) and keeps the settings (`GIT_CONFIG_*`,
     `GIT_AUTHOR_*`, `GIT_COMMITTER_*`, `GIT_SSH*`), except
     `GIT_CONFIG_PARAMETERS`: that one carries the caller's `git -c` overrides
     (`core.bare`, `core.worktree`), so it is dropped as well. Started from a
     git hook, the tool therefore acts on the repository in its working
     directory, never on the hook's.
4. **Push destination safety**:
   - Push targets are strictly restricted to `origin` and refspec
     `HEAD:refs/heads/<branch>`. Pushing to `upstream` is strictly forbidden.
   - `<old_sha>` is resolved strictly from the fully qualified remote-tracking
     ref `refs/remotes/origin/<branch>` (after fetching under `--apply`); a
     local branch or `refs/heads/origin/<branch>` is refused.
   - Pushes use `--force-with-lease=refs/heads/<branch>:<old_sha>`. If the remote
     ref was modified since it was read, the push is rejected and marked `REFUSED`.

## Output format

The tool emits one TSV (tab-separated) row per PR to standard output (with
`\t`, `\n`, `\r`, and `\\` escaped within fields):

| num | branch | status | old | new | unsigned | detail |
| --- | --- | --- | --- | --- | --- | --- |

- **num**: Pull request number.
- **branch**: Pull request head branch name.
- **status**:
  - `PLANNED`: Eligible; dry-run mode active (`dry-run (local refs)`).
  - `OK`: Rebase (and optional build/push) succeeded.
  - `REFUSED`: Invariant pre-check failed, lock was busy, or lease was rejected.
  - `CONFLICT`: Rebase hit conflicts; rebase was aborted.
  - `REBASE-FAIL`: Rebase stopped without a conflict (signing failure, refused
    hook, unusable base) or post-rebase inspection failed; rebase was aborted.
  - `REBASE-ABORT-FAIL`: Rebase failed and `git rebase --abort` also failed.
  - `UNSIGNED`: Commits in `base..HEAD` lacked valid signatures (`%G? != G`).
  - `BUILD-FAIL`: Meson setup or Ninja compilation failed.
  - `TEST-FAIL`: Meson test suite exited with a non-zero status.
  - `ERROR`: Unexpected per-PR exception caught so sibling PR rows are preserved.
- **old**: Abbreviated 9-character SHA of the PR branch before rebase.
- **new**: Abbreviated 9-character SHA of the rebased HEAD (or `-` if unapplied).
- **unsigned**: Count of unsigned commits in `upstream/master..HEAD`.
- **detail**: Informative message, conflict file list, or failure summary.

## Exit codes

- `0`: Every processed PR is `OK` or `PLANNED`, or the PR target list is empty.
- `1`: Any PR resulted in `REFUSED`, `CONFLICT`, `REBASE-FAIL`,
  `REBASE-ABORT-FAIL`, `UNSIGNED`, `BUILD-FAIL`, `TEST-FAIL`, or `ERROR`, or a
  command-line / PR-list argument error occurred.

## Regressions and testing

All sweep operations are covered by unit tests in
[`scripts/dev/tests/test_upstream_pr_sweep.py`](https://github.com/VMAFx/vmafx/blob/master/scripts/dev/tests/test_upstream_pr_sweep.py),
[`scripts/dev/tests/test_upstream_pr_sweep_hardening.py`](https://github.com/VMAFx/vmafx/blob/master/scripts/dev/tests/test_upstream_pr_sweep_hardening.py),
and support fixtures in
[`scripts/dev/tests/upstream_pr_sweep_fixture.py`](https://github.com/VMAFx/vmafx/blob/master/scripts/dev/tests/upstream_pr_sweep_fixture.py),
using isolated bare upstream and fork repositories with local SSH-based Git commit
signing.

## Pre-push gate for upstream branches

Run `scripts/dev/upstream_push_gate.py` before publishing a branch from a contribution worktree. The gate is offline: it reads the supplied local upstream ref and exact proposed head, and never fetches or contacts a remote service. Refresh the local upstream ref through your usual workflow before relying on the merge result.

Invoke the script from this checkout and point `--repo` at the contribution worktree:

```bash
python3 /path/to/fork/scripts/dev/upstream_push_gate.py \
  --repo /path/to/contribution-worktree \
  --base upstream/master --head HEAD \
  --draft /path/to/pr-draft.md --self-review /path/to/self-review.txt
```

Repeat `--draft FILE` for additional drafts. Both refs resolve to immutable commit IDs before checking. The output has one `PASS`, `FAIL`, or `SKIP` line per class, with its class ID, name, and reason. Exit 0 means no check failed; exit 1 reports check failures; exit 2 reports invalid arguments, unavailable inputs, or setup errors. A skip is never evidence that its class passed.

The canonical class list, hit counts, and full review questions live in `scripts/dev/upstream_review_classes.json`. The gate refuses an implemented class missing from that file or a listed mechanical/self-review class without an implementation.

| Class ID | Check | Mode |
| --- | --- | --- |
| C01 | Require green fork CI on the exact proposed head before publishing or updating the upstream pull request. | online |
| C02 | Catalogue reviewer requests only: outside authors cannot request reviewers and the base has no CODEOWNERS. | non-actionable |
| C03 | Require upstream subsystem subjects, a non-empty body, and 72-column message wrapping. | mechanical |
| C04 | Require a test change for source changes or an explicit C04 self-review reason. | mechanical |
| C05 | Reject model or tool attribution and fork artifacts in commit messages, drafts, and added diff lines. | mechanical |
| C06 | Require the pushed head to merge cleanly onto the supplied local upstream ref. | mechanical |
| C07 | Require one contribution commit unless C07 explains the reviewable split. | mechanical |
| C08 | Require recorded performance measurements of candidate forms under two compilers. | self-review |
| C09 | Require every resolvable commit ID in drafts to be reachable from the pushed head or upstream ref. | mechanical |
| C10 | Check for open upstream pull requests touching the same functions before publishing. | online |
| C11 | Require a release-build reproducer demonstrating an output, exit-code, or memory effect. | self-review |
| C12 | Require checks of fallible calls and resource release on failure in touched functions. | self-review |
| C13 | Require a search of sibling backends and call sites, with fixes or an explicit scope reason. | self-review |
| C14 | Require signature status G for every contribution commit in the exact pushed range. | mechanical |
| C15 | Require prior upstream issue agreement for score, public API, or exit-behaviour changes. | self-review |
| C16 | Require sanitizer evidence for extremal inputs at 8, 10, 12, and 16 bits. | self-review |
| C17 | Reject fork-only paths and contribution ranges exceeding three commits. | mechanical |

C01 and C10 are deferred to a later round; this command does not establish exact-head CI status or detect overlapping open pull requests. C02 is catalogued only because outside authors cannot request reviewers and the upstream base has no CODEOWNERS. These classes print SKIP with their reason.

### Self-review evidence

Supply a UTF-8 file with one `Cxx: <answer>` line per self-review class. Answer yes/no and include the command, result, input, or review evidence; each answer must contain at least 12 characters. `n/a: <reason>` is accepted when a class does not apply. Example:

```text
C08: yes: two compiler commands and timings are in perf-results.txt
C11: yes: release-case.yuv changes peak memory; before/after in results.txt
C12: yes: checked allocation and cleanup paths in the touched function
C13: yes: searched sibling backends and callers; paths in parity-results.txt
C15: n/a: no score, public API, or exit-code behaviour changes
C16: yes: sanitizer commands and 8/10/12/16-bit extrema in results.txt
```

C08 and C16 cannot start with `n/a` when the diff touches the x86, arm64, or CUDA kernel directories, or filenames containing `avx` or `neon`. The gate checks evidence presence and applicability; a reviewer must assess its substance.

A source change under `libvmaf/src/`, `libvmaf/tools/`, or `python/vmaf/` without a test change requires an additional `C04: <reason>` line. Multiple contribution commits require `C07: <reason>` explaining the split, and more than three commits always fails C17. These optional reasons also need at least 12 characters.

### Sweep integration and limits

`push_branch` calls the same gate directly on the resolved `base_sha..new_sha` immediately before pushing. A FAIL produces a REFUSED row whose detail is the first FAIL line, and no push runs. The gate verifies the proposed head even if a build moved symbolic HEAD.

The sweep supplies `--mechanical-only "rebase-only push"`, which skips the six self-review classes and prints that reason. A person can use this mode for a pure rebase after reviewing the existing patch. It does not waive C04, C07, signatures, message shape, or any other mechanical check.

For those cases, pass `--self-review /path/to/self-review.txt` to the sweep.
Use the same UTF-8 `Cxx: <answer>` lines described above, including the C04
or C07 reason as applicable. The file must be readable before the sweep starts.

Commit subjects use `<subsystem>: <summary>` and at most 72 characters; conventional-commit prefixes are refused. A blank second line and a non-empty body are required. Body lines wrap at 72 columns, except bare URLs, four-space/tab-indented lines, and trailers in the final paragraph. Attribution checks share the DCO checker's attribution functions without requiring DCO sign-off. Leak diagnostics name a commit or file and line number without echoing the offending line.

Inputs have byte, line, draft, token, and commit-count bounds; Git calls use the sweep's allow-listed executable and timeout. The gate needs an installed Git supporting `merge-tree --write-tree --no-messages --name-only` and signature verification configured for the contribution commits. Regression fixtures cover planted failures in disposable signed repositories.
