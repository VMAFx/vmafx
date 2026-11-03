# Research-3266: Upstream PR sweep tool and hook repository-location isolation

<!-- markdownlint-disable MD013 -->

- **Status**: Active
- **Workstream**: PR #3266 (fork-local developer tooling and hook isolation)
- **Last updated**: 2026-11-03

## Question

1. How should the fork rebase, verify commit signatures, build, test, and force-push open pull requests authored from `VMAFx/netflix-vmaf-contributions` against `Netflix/vmaf` without risking pushes to the wrong remote, signing foreign commits, leaking caller credentials into PR build scripts, or corrupting worktrees?
2. Why did running repository unit tests inside a `pre-commit` / `pre-push` hook mutate the caller checkout's `.git/config` (`core.bare = true`), and which `GIT_*` environment variables must be stripped at both the hook bridge and child-process boundaries?

## Sources

- `git(1)` and `git-worktree(1)` manual pages: `GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE`, `GIT_COMMON_DIR`, `GIT_OBJECT_DIRECTORY`, `GIT_ALTERNATE_OBJECT_DIRECTORIES`, and `GIT_CONFIG_PARAMETERS`.
- [ADR-1333](../adr/1333-meson-test-secret-env-sanitization.md): credential variable sanitization (`scripts/ci/run_meson_test.py`).
- `scripts/git-hooks/framework-hooks.sh` and `scripts/githooks/tests/test_install_hooks_env.py`: `pre-commit install-hooks` environment isolation (pre-commit/pre-commit#3609, #3774).

## Findings

1. **Hook `GIT_DIR` leak into child `git init` / `git config` calls**:
   Git exports `GIT_DIR` (and in worktrees `GIT_WORK_TREE`, `GIT_COMMON_DIR`, `GIT_INDEX_FILE`) to hooks. When a test fixture executed under a hook runs `git init --bare` or `git config core.bare true` in a temporary directory without clearing `GIT_DIR`, Git targets the caller repository's `.git` directory instead of the temporary directory. Similarly, `GIT_CONFIG_PARAMETERS` carries `git -c` overrides (`core.bare=true`) to child processes.
   - Defense 1 (`scripts/git-hooks/framework-hooks.sh`): `unset GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES` before running `pre-commit`, while keeping `GIT_INDEX_FILE` for `pre-commit run` (partial commits) and keeping `env -u GIT_INDEX_FILE -u GIT_DIR -u GIT_WORK_TREE -u GIT_OBJECT_DIRECTORY` on `install-hooks`.
   - Defense 2 (`scripts/dev/upstream_pr_sweep.py` `git_environment()`): strip all `GIT_*` variables except `GIT_CONFIG_*` (excluding `GIT_CONFIG_PARAMETERS`), `GIT_AUTHOR_*`, `GIT_COMMITTER_*`, and `GIT_SSH*`.
2. **Remote identity, ref qualification, and push safety**:
   - Substring checks on `origin` (`fork in url`) accept lookalike repositories (`VMAFx/vmafx-other`, path prefixes, or query parameters) and miss divergent `remote.origin.pushurl` settings. Parsing canonical `owner/repo` across HTTPS, SSH (`ssh://` and `user@host:owner/repo.git`), and local paths—and checking both `git remote get-url origin` and `git remote get-url --push --all origin` on `--repo` and the target worktree—prevents cross-repository pushes while redacting URL userinfo/query secrets from refusal logs.
   - Resolving `refs/remotes/origin/<branch>` explicitly prevents a local `refs/heads/origin/<branch>` ref from shadowing the remote-tracking tip. Fetching `upstream` and `origin/<branch>` under `--apply` ensures rebases and `--force-with-lease` use current remote tips.
3. **Commit authorship and build environment isolation**:
   - Checking that every commit in `base..old_sha` has an author and committer email in the operator's identity set before `git rebase --gpg-sign` prevents signing untrusted third-party commits unless `--allow-foreign-authors` is explicitly set.
   - Passing `build_environment()` (which applies `sanitize_process_environment` and an allow-list of compiler/build variables) to `meson setup`, `ninja`, and `meson test` prevents PR build files from reading `GITHUB_TOKEN`, `GH_TOKEN`, `SSH_AUTH_SOCK`, or cloud credentials.
   - Author verification and unsigned commit counting fail closed by refusing any PR range exceeding `MAX_PR_COMMITS` commits instead of silently truncating the inspection.
4. **Worktree registration, detached checkout, and locking**:
   - Verifying existing worktrees against `git worktree list --porcelain` prevents modifying unrelated checkouts at candidate paths.
   - Preparing worktrees with `git checkout --detach <old_sha>` avoids `git checkout -B <branch>` failures when `<branch>` is checked out in another worktree.
   - Acquiring a non-blocking `fcntl.flock` lock (`<worktree>.lock`, with `O_CREAT | O_EXCL` fallback when `fcntl` is unavailable) serializes concurrent sweeps targeting the same PR.
   - On platforms with `fcntl` support, the lock file persists on disk after lock release to prevent inode-recreation race conditions between concurrent processes, while the non-flock fallback unlinks the lock file on release.

## Alternatives explored

| Alternative | Verdict | Reason |
| :--- | :--- | :--- |
| Keep ad-hoc shell scripts (`uprebase.sh`, `upbuild.sh`) outside the repository | Rejected | Untested shell scripts lacked `GIT_DIR` isolation, URL/lease validation, credential stripping during `meson setup`/`ninja`, and structured TSV error reporting. |
| Check out `refs/heads/<branch>` in each PR worktree via `git checkout -B` | Rejected | Fails whenever `<branch>` is checked out in the main repository or another worktree; detached HEAD + `HEAD:refs/heads/<branch>` push refspec needs no local branch. |
| Substring check `fork in origin_url` on fetch URL only | Rejected | Vulnerable to substring lookalikes, unvalidated `pushurl` overrides, and credential leakage in refusal messages. |

## Open questions

- None.

## Related

- ADRs: [ADR-1333](../adr/1333-meson-test-secret-env-sanitization.md)
- PRs: #3266
- Files: `scripts/dev/upstream_pr_sweep.py`, `scripts/dev/tests/test_upstream_pr_sweep.py`, `docs/development/upstream-pr-sweep.md`, `scripts/git-hooks/framework-hooks.sh`, `scripts/githooks/tests/test_install.py`
