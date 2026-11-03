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

- **Isolated worktree execution**: Each PR is evaluated in a dedicated worktree
  outside the main clone, preventing workspace pollution.
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
# Using an explicit PRs list file ("number branch [head_owner]")
python3 scripts/dev/upstream_pr_sweep.py --fork VMAFx/vmafx --prs prs.txt

# Or discover open PRs automatically via the GitHub CLI
python3 scripts/dev/upstream_pr_sweep.py --fork VMAFx/vmafx --discover
```

### 2. Apply rebase and verify signatures

Execute the rebase onto `upstream/master` and verify commit signatures:

```bash
python3 scripts/dev/upstream_pr_sweep.py --fork VMAFx/vmafx --prs prs.txt --apply
```

To also execute the Meson setup, Ninja build, and test suite in `libvmaf`:

```bash
python3 scripts/dev/upstream_pr_sweep.py --fork VMAFx/vmafx --prs prs.txt --apply --build --jobs 4
```

The test suite runs through this checkout's `scripts/ci/run_meson_test.py`,
which removes credential variables before Meson starts (ADR-1333); upstream
pull request trees do not carry that runner.

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
   - `head_owner` does not match the fork owner declared in `--fork`.
   - The repository's `origin` remote URL does not name `--fork`.
2. **Dirty worktree**:
   - The PR worktree exists and `git status --porcelain` reports modified or
     untracked files.
3. **Caller repository untouched**:
   - Every `git`, `gh`, `meson` and `ninja` call runs with `git_environment()`,
     which drops each `GIT_*` variable that locates a repository (`GIT_DIR`,
     `GIT_WORK_TREE`, `GIT_INDEX_FILE`, ...) and keeps the settings
     (`GIT_CONFIG_*`, `GIT_AUTHOR_*`, `GIT_COMMITTER_*`, `GIT_SSH*`). Started from
     a git hook, the tool therefore acts on the repository in its working
     directory, never on the hook's.
4. **Push destination safety**:
   - Push targets are strictly restricted to `origin` and refspec
     `HEAD:refs/heads/<branch>`. Pushing to `upstream` is strictly forbidden.
   - Pushes use `--force-with-lease=refs/heads/<branch>:<old_sha>`. If the remote
     ref was modified since it was read, the push is rejected and marked `REFUSED`.

## Output format

The tool emits one TSV (tab-separated) row per PR to standard output:

| num | branch | status | old | new | unsigned | detail |
| --- | --- | --- | --- | --- | --- | --- |

- **num**: Pull request number.
- **branch**: Pull request head branch name.
- **status**:
  - `PLANNED`: Eligible; dry-run mode active.
  - `OK`: Rebase (and optional build/push) succeeded.
  - `REFUSED`: Invariant pre-check failed or lease was rejected.
  - `CONFLICT`: Rebase hit conflicts; rebase was aborted.
  - `UNSIGNED`: Commits in `base..HEAD` lacked valid signatures (`%G? != G`).
  - `BUILD-FAIL`: Meson setup or Ninja compilation failed.
  - `TEST-FAIL`: Meson test suite exited with a non-zero status.
- **old**: Abbreviated 9-character SHA of the PR branch before rebase.
- **new**: Abbreviated 9-character SHA of the rebased HEAD (or `-` if unapplied).
- **unsigned**: Count of unsigned commits in `upstream/master..HEAD`.
- **detail**: Informative message, conflict file list, or failure summary.

## Exit codes

- `0`: Every processed PR is `OK` or `PLANNED`, or the PR target list is empty.
- `1`: Any PR resulted in `REFUSED`, `CONFLICT`, `UNSIGNED`, `BUILD-FAIL`, or
  `TEST-FAIL`, or a command-line argument error occurred.

## Regressions and testing

All sweep operations are covered by unit tests in
[test_upstream_pr_sweep.py](../../scripts/dev/tests/test_upstream_pr_sweep.py),
using isolated bare upstream and fork repositories with local SSH-based Git commit
signing.
