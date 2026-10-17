<!-- markdownlint-disable MD013 -->
# Local Git hooks

Run `make install-hooks` from the checkout or linked worktree you use.
The installer requires Python with `pre-commit` installed in the active
environment and prepares the configured hook environments before replacing
any hooks. `make hooks-install` remains an alias. Install `pre-commit` from
the hash lock, which also carries `reuse` for the `reuse-lint` check:

```bash
python3 -m pip install --require-hashes -r requirements/locks/pre-commit.txt
```

Lefthook owns three hooks next to these dispatchers; install it first, as
described in [Lefthook and the governance hooks](#lefthook-and-the-governance-hooks).

The installer writes regular dispatcher files to Git's effective hooks
directory, including a configured `core.hooksPath`. Each invocation finds
its current worktree with Git. Removing the worktree used for installation
therefore cannot break hooks in the surviving checkout.

## Lefthook and the governance hooks

[`lefthook.yml`](../../lefthook.yml) owns `pre-commit`, `pre-push` and
`post-commit` ([ADR-1249](../adr/1249-praetor-governance-adoption.md)). Its
`pre-commit` and `pre-push` jobs run the pre-commit framework stage through
`scripts/git-hooks/framework-hooks.sh`, next to the praetor governance checks
(`compile-context --verify`, `audit`, `hiss coverage --verify`, `dedupe scan`).
`post-commit` synchronises private state. The dispatchers keep `commit-msg`
(Conventional Commits) and `pre-rebase` (worktree drift guard). Install
lefthook first, then the dispatchers:

```bash
lefthook install
make install-hooks   # reports "left lefthook's .../pre-commit in place"
```

`make install-hooks` leaves the hooks lefthook wrote in place
([ADR-2012](../adr/2012-lefthook-windows-host.md)). In the opposite
order it stops at the first hook it did not write, for example Git LFS's
`pre-push`.

The governance hooks run `praetorctl` at the engine commit pinned as
`PRAETOR_REF` in `.github/workflows/standards-gate.yml`, and they find it
themselves. Each one calls `sh .config/lefthook/engine.sh <arguments>`. The
launcher reads the pin and runs the engine installed under
`${XDG_CACHE_HOME:-$HOME/.cache}/praetor/engine/<pin>`. The first hook after
a pin move installs that engine with `go install`, so it needs the Go
toolchain and network access once per pin (limit
`PRAETOR_ENGINE_INSTALL_TIMEOUT` seconds, default 300). A pin the launcher
cannot read, or an install that fails, ends the hook: no engine from `PATH`
runs in its place. Details:
[Moving the praetor pin](praetor-gate.md#moving-the-praetor-pin).

`make verify-all` and a `praetorctl` command you type still run the engine
on `PATH`. A newer engine can fail an unchanged tree, so for those put the
pinned build first on `PATH`:

```bash
ref=$(sed -n 's/^  PRAETOR_REF: //p' .github/workflows/standards-gate.yml)
GOBIN="$HOME/go/bin-pinned" go install "github.com/cordanaLLM/praetor/cmd/standardsctl@$ref"
cp "$HOME/go/bin-pinned/standardsctl" "$HOME/go/bin-pinned/praetorctl"  # add .exe on Windows
export PATH="$HOME/go/bin-pinned:$PATH"
```

Two lefthook behaviours affect every checkout that shares the Git directory:

- `lefthook run <hook>` installs the hook shims first unless you pass
  `--no-auto-install`. Linked worktrees share one hooks directory, so a trial
  run in a worktree installs lefthook for the main checkout too.
- `lefthook uninstall` rewrites `.claude/settings.json` and `.codex/hooks.json`
  with sorted keys even when it removes nothing. Both files are committed in
  that form, so the rewrite leaves them unchanged; keep them that way
  (`json.dumps(indent=2, sort_keys=True)` plus a newline).
  `scripts/githooks/tests/test_install.py` checks it.

Keep every `run:` value in `lefthook.yml` on one line and free of double quotes.
On Windows lefthook passes it to `sh -c` without escaping, so a double quote
ends the script early; put logic in a script under `scripts/git-hooks/`
instead. The same test rejects both.

## Windows hosts

The hooks run in Git for Windows' Bash. Set up a checkout once:

1. Clone with `core.autocrlf=false` and `core.eol=lf`
   (`git clone -c core.autocrlf=false -c core.eol=lf ...`). Several gates
   compare generated files byte for byte against their LF form. Git for
   Windows' default `core.autocrlf=true` checks them out with CRLF, and so does
   `core.autocrlf=false` alone: `* text=auto` in `.gitattributes` then uses
   `core.eol`, whose default is the platform's CRLF. The files `praetorctl`
   hashes or compares byte for byte (the archetypes `.standards.lock` pins,
   `.standards.*`, `AGENTS.md`, its compiled targets and the agent personas)
   carry `eol=lf` rules, so `hiss-audit` and `context-check` pass whatever the
   clone's settings; `scripts/ci/tests/test_praetor_hashed_files_lf.py` keeps
   the list complete.
2. Create a Python 3.14 virtualenv from the hash lock:

   ```bash
   py -3.14 -m venv .venv
   .venv/Scripts/python.exe -m pip install --require-hashes -r requirements/locks/pre-commit.txt
   ```

   `framework-hooks.sh` prefers `.venv/Scripts/pre-commit.exe` (or
   `.venv/bin/pre-commit` elsewhere) over `pre-commit` on `PATH` and puts the
   virtualenv first on `PATH` for the checks, so `reuse` resolves without
   activating it. The lock installs `reuse` with `charset-normalizer`, because
   reuse cannot use `python-magic` on Windows.
3. Put lefthook v2 on `PATH`
   (`go install github.com/evilmartians/lefthook/v2@v2.1.14`). The governance
   hooks call the launcher with `sh` and install the pinned engine with Go,
   so both must be on `PATH`. For `make verify-all`, put the pinned
   `praetorctl` first on `PATH`, as above.
4. Install from a shell with the virtualenv active
   (`source .venv/Scripts/activate`): `lefthook install`, then
   `bash scripts/githooks/install.sh` (the command behind `make install-hooks`).
   The `commit-msg` dispatcher looks `pre-commit` up on `PATH` when it runs, so
   commit from a shell with the virtualenv active, or install the same lock into
   the Python on `PATH`. Lefthook's jobs find `.venv` themselves.
5. For pushes, install the push-stage tools into the same virtualenv:
   `requirements/locks/mypy.txt` (with `--require-hashes`) and
   `docs/requirements.txt` for the MkDocs strict build. `govulncheck` on
   `PATH` enables the `security` job; without it the job reports a skip.

Check the result with `lefthook run pre-commit --no-auto-install` on a staged
change. A few fixture tests assume POSIX tools and skip on Windows with the
reason printed; the Linux CI jobs still run them. Others still fail on Windows
and run only when `Makefile`, `.pre-commit-config.yaml` or their own files
change; see `T-HOOKS-WINDOWS-POSIX-FIXTURES-2026-09-30` in
[docs/state.md](../state.md) and
[ADR-2012](../adr/2012-lefthook-windows-host.md).

## Installed checks

| Git event | Framework mode, the default | Native mode |
| --- | --- | --- |
| `pre-commit` | Configured formatters and checks | Three native formatters |
| `commit-msg` | Configured Conventional Commits validation | Same framework check |
| `pre-push` | All configured push checks | Same framework checks |
| `pre-rebase` | Agent worktree drift guard | Same guard |

The dispatcher forwards Git's arguments and pushed-ref input to
`pre-commit hook-impl`. The framework selects checks and changed files
from `.pre-commit-config.yaml`. Push checks include assertion density,
twin drift, mypy, bounded-process regressions, FFmpeg patch replay, PR
deliverables, and MkDocs strict validation. The PR-body check may skip a first
push or draft PR. It also skips the PRs whose deliverables CI's checklist
skips: the machine-generated release PR (ADR-1151) and a strictly
dependency-only bot PR (ADR-1152). For those it asks the same scripts CI asks,
with the PR's author, head ref and changed files
([PR body validator](pr-body-validator.md)). That does not skip the other
checks. Existing framework `.legacy` hooks
continue to run in framework stages.

A selected documentation push requires MkDocs. Missing `mkdocs` blocks the
push with an installation hint; install `docs/requirements.txt` in the
active environment. Direct non-doc invocations skip before requiring the
docs toolchain. The required hosted `Docs` job installs these dependencies
and runs strict validation.
Missing `pre-commit` itself blocks framework hook dispatch with a clear
message; activate the environment used for installation.

### Hook environments install outside the commit's git environment

In a linked worktree, Git exports an absolute `GIT_INDEX_FILE` to hooks.
When the framework (re)installs a `language: node` hook environment, it runs
`npm install -g git+file://<hook repository>` with that variable set, and
npm's checkout writes the hook repository's tree into the worktree's index.
The commit then fails or records the wrong tree. The pre-commit project does
not plan a fix ([pre-commit/pre-commit#3609](https://github.com/pre-commit/pre-commit/issues/3609)).

The `framework-hooks` entries of `lefthook.yml` (`pre-commit` and `pre-push`)
therefore run `pre-commit install-hooks` with `GIT_INDEX_FILE`, `GIT_DIR`,
`GIT_WORK_TREE` and `GIT_OBJECT_DIRECTORY` unset before `run` / `hook-impl`,
which keeps the commit's own environment because it needs the index. A failed
install blocks the commit or push. With a warm cache the extra call takes
well under a second. `python3 scripts/githooks/tests/test_install_hooks_env.py`
reproduces the defect in a throwaway linked worktree with a node hook, and
`make` runs it with the other hook tests; it skips, naming the missing tool,
when `npm` or `pre-commit` is absent.

## Post-commit private-state synchronization

The post-commit state hook resolves both the active worktree and Git's common
directory. In a linked worktree it mirrors the canonical ledgers (`OPEN.md`,
`BACKLOG.md`, `BUGS.md`, `QUESTIONS.md`, `STATE.md`) and their metadata
(`bugs.meta.json`, `questions.meta.json`, which Praetor reads for every bug and
question entry) into a regular ignored `.workingdir`, runs Praetor against the
committing worktree,
and atomically publishes the resulting `STATE.md` to the canonical checkout.
The mirror preserves worktree-local caches and evidence; those directories are
not public documentation and are never copied back.

Synchronization is serialized by a lock in the common Git directory. Lock
contention, missing or non-regular canonical ledgers, a symlinked local state
root, and a missing synchronizer all fail the hook. The lock directory holds
an `owner` file with the holder's process id and the host's boot id (Linux
`boot_id`, macOS `kern.bootsessionuuid`). A lock whose owner ran in an earlier
boot or is no longer running, or one with no owner file that is older than two
minutes (left by an earlier version of the hook), is stale: the next sync takes
it over and prints `state-sync: took over the stale lock ...`. A lock with a
running owner still fails the hook, and the message names the owner. A failed hook therefore
cannot silently record the main checkout's branch on behalf of an agent
worktree. See [ADR-1280](../adr/1280-worktree-state-sync.md) and its
[research digest](../research/1280-worktree-state-sync.md).

Run the helper directly from the checkout whose identity should be recorded:

```bash
scripts/githooks/state-sync.sh
```

## Python push scope

The `mypy-local` hook implements the touched-file rule in
[agent hard rule 10](agent-hard-rules.md): every added, copied, modified,
renamed or type-changed `*.py` path under `ai/` and `scripts/` in
`git diff origin/master...HEAD` is checked. Deleted paths are omitted.
The strict settings in `pyproject.toml` still apply. Fetch `origin/master`
before validating a rebased branch; a missing merge base blocks the push.

The hook runs once on every push and derives this complete set itself.
Pre-commit's old-remote-tip/new-tip file list can include unrelated changes
from master and omit an unchanged branch-owned file whose imports changed
after a rebase. Neither omission may narrow the check. This does not expand
the explicit file policy to other Python packages or unchanged master files.
Mypy's normal import checking still applies to the selected sources.

### What makes the hook fail

The hook reports only findings the branch introduces
([ADR-1261](../adr/1261-mypy-pre-push-delta-gate.md)). It checks the selected
files, then checks the same files again as they are at the merge base in a
disposable worktree, and lists what is new. Line numbers are left out of the
comparison, so inserting a line above an existing finding does not make it
look new. The output ends with a count of the findings that were inherited and
therefore not reported.

Two consequences worth knowing:

- A file that already fails still fails for its own reasons, and you may edit
  it. Adding a *different* finding to it is reported; the ones that were
  already there are not.
- A non-zero exit with nothing to attribute to a file is treated as mypy
  breaking, and blocks the push.

The delta keeps inherited debt from blocking unrelated changes. The required
hosted `Python Lint` job and the local hook both run this same gate; neither
uses a separate whole-tree exception list. Pull requests compare with the
fetched `origin/master`, while a master push supplies the event's exact prior
commit through `VMAFX_MYPY_BASE_REF`, so post-merge CI checks the Python paths
that actually landed rather than comparing `HEAD` with itself.

Both blocking callers pass `--no-site-packages` and
`--disable-error-code=import-not-found`. Its result therefore does not depend
on arbitrary PEP 561 packages in the active environment. Selected repository
sources, repository imports that resolve, and standard-library types are still
checked. The hosted job installs the same hash-locked mypy toolchain from
`requirements/locks/mypy.txt`; it does not install or run the training stack.
This isolation prevents a newer ambient stub from changing the gate or crashing
it before any attributable finding is emitted.

Files under `ai/src/` are checked in a separate run with
`--explicit-package-bases`. That directory is a `mypy_path` base, so without
the flag mypy sees each file under two module names and refuses the run
outright.

Run the same check manually from the repository root:

```bash
python3 scripts/git-hooks/pre-push-mypy.py
```

CI may select an explicit comparison authority without changing the local
default:

```bash
VMAFX_MYPY_BASE_REF=<previous-commit> \
  python3 scripts/git-hooks/pre-push-mypy.py
```

The checked-out HEAD must match the outgoing commit supplied by pre-commit;
push a different branch from its own checkout. Missing Git, mypy or selected
files blocks validation. Internal symlinks retain their Git filename for
selection and checking; their target must resolve to an existing regular file
inside the checkout. External, dangling, looping or directory targets fail.
The command always derives its own scope; filename arguments do not narrow it.

## Copyright and SPDX hook, and the declared exception list

`check-copyright` runs `scripts/ci/check-copyright.sh` on every tracked file whose
extension is `c h cpp cxx cc hpp hxx cu cuh hip metal mm go py pyx rs sh`
(pre-commit's `types_or` cannot select `.hip` or `.metal`, so the hook uses one
`files:` regex; keep it equal to the script's `case` lists). It requires:

| Rule | Files | Needs, in the first 40 lines |
| --- | --- | --- |
| ADR-0105 | `c h cpp cxx cc hpp hxx cu cuh hip mm metal` | a `Copyright` line |
| ADR-1250 | every extension above | an SPDX licence identifier line |

The hook excludes one thing, `scripts/ci/exact_twins.d/` (parity-gate data
fragments that only borrow the `.hip` extension). A file that cannot meet a rule
is not skipped by path: it is named in the **declared exception list**,
`.config/lint-exceptions.d/<rule>.toml`, where `<rule>` is `spdx` or
`copyright`. One entry is one tracked file with a reason and an expiry:

```toml
[[exception]]
path = "core/src/interop/pelorus_version.c"   # one tracked file, never a pattern
reason = "Read-only mirror of VMAFx/pelorus (ADR-1113); ..."
expires = 2026-12-31
```

An entry stops holding on its `expires` date: the file is read again and the hook
fails on it, and `check-lint-exceptions` (always run, also in CI's
`pre-commit run --all-files`) names the entry. It also fails on a missing field, a
rule that differs from the file name, a path that is a pattern or not tracked, a
duplicate, and an expiry more than 400 days out. A rule has one entry per file;
`hosted-runner-label` has one per job of a workflow file (key `job`), so there the
same file and job twice is the duplicate. Fix the file when you can; renew an entry
only with its reason still true.

```bash
python3 scripts/ci/lint_exceptions.py check
pre-commit run check-copyright --all-files
```

Two entries of the list are not file standards: `scorecard-code-review` and
`scorecard-branch-protection` ([ADR-2126](../adr/2126-scorecard-single-maintainer-exceptions.md))
declare the single-maintainer gaps that OpenSSF Scorecard reports (alerts 1 and
1054). Their path is the file that owns the policy, and the code-scanning
dismissal cites the entry.

The `dependency-advisory` entries are not file standards either: each names the lock file of an open
Dependabot alert that no reachable version of the dependency fixes (for example `mcp` 1.29.0, pinned exactly by
`semgrep`), with the reason the vulnerable code path is not reached and a short expiry. The alert stays open; the
entry is deleted when the update that closes it lands.

Entries today: ten files of the Pelorus mirror (`spdx`, until the line exists in
Pelorus and the mirror is re-vendored), two praetor-managed files (`spdx`) and
eight third-party MEX sources of the Netflix MATLAB harness (`copyright`).

The `codeql-include-non-header` entries are not file standards: they name the two tests that compile a
device source (the CUDA / HIP decouple header, the Metal kernels) for the host, where CodeQL's
`cpp/include-non-header` finding is the test's purpose. The dismissal of its code-scanning alerts cites the entry.

## GitHub Actions workflow validation

Workflow files under `.github/workflows/` are validated against
`.github/actionlint.yaml` using `actionlint` pinned to `v1.7.12`
(HISS-11 hermetic supply chain pin).

The pre-commit hook runs on staged workflow files:

```bash
pre-commit run actionlint --all-files
```

Both the hook and `make lint-actions` run actionlint through
`scripts/ci/run_actionlint.py` ([ADR-2199](../adr/2199-actionlint-bounded-run.md)),
which gives it 90 seconds (`ACTIONLINT_TIMEOUT_S`; a healthy run takes about
one). actionlint v1.7.12 writes the script of a `run:` block to shellcheck's
stdin pipe before it starts shellcheck, so it hangs when the script is larger
than the pipe, and the kernel shrinks the pipes of a user who holds more than
`fs.pipe-user-pages-soft` pages in total (many builds and agents at once). A
hang now ends with exit 124 and a message that names the pipe capacity and a
file with the goroutine dump (`SIGQUIT` at the deadline, sent to actionlint only, with
the default disposition restored first so a hook started as a background job still gets
its dump; `ACTIONLINT_DUMP_DIR` chooses the directory, the temporary directory by
default); it is a failure, never a pass. Find the process that holds the pipes
(`ls -l /proc/*/fd | grep -c pipe`) and run again. The defect is upstream
(rhysd/actionlint#702).

To validate workflows, composite actions and hosted runner labels across the
repository without pre-commit, run:

```bash
make lint-actions
```

### Composite actions

actionlint reads workflow files only; given an `action.yml` it fails with
`"jobs" section is missing`. The composite actions under `.github/actions/*/`
get their own two checks, run by the same pre-commit job in CI:

| Hook | Reads | Checks |
| --- | --- | --- |
| `check-github-actions` ([check-jsonschema](https://github.com/python-jsonschema/check-jsonschema) `0.38.2`) | `.github/actions/*/action.yml` | The GitHub action manifest schema |
| `check-composite-actions` (`scripts/ci/check_composite_actions.py`) | every manifest under `.github/actions/` | `runs.using: composite`, a `description` on every input, one of `run` or `uses` per step, a `shell` on every `run` step; every `bash` and `sh` `run:` block goes through shellcheck with each `${{ ... }}` replaced by a placeholder and the same ignore list actionlint uses for a workflow block (`SC1091`, `SC2194`, `SC2050`, `SC2153`, `SC2154`, `SC2157`, `SC2043`) |

A step in another shell (`pwsh`, `cmd`, `python`) is printed as skipped with the
reason `no checker for this shell`; the repository has none today.
`test-check-composite-actions` keeps positive, negative and boundary cases for the
script (an unquoted variable, a missing `shell`, a missing input description, a
non-composite action, an unreadable manifest and an empty `.github/actions/`
each fail). Run the script alone with:

```bash
python3 scripts/ci/check_composite_actions.py
```

### Hosted runner labels

actionlint accepts every label GitHub knows, so it cannot say which hosted Linux
image a job should name. `check-hosted-runner-labels`
(`scripts/ci/check_hosted_runner_labels.py`) holds the rule of
[ADR-3251](../adr/3251-hosted-runner-label-explicit.md):

| Label a workflow names | Result |
| --- | --- |
| `ubuntu-26.04`, `ubuntu-26.04-arm` | accepted |
| `ubuntu-latest`, its larger-runner forms (`ubuntu-latest-4-cores`), `ubuntu-slim` | refused everywhere: the label carries no version, so the image under the job changes on a date GitHub picks |
| another versioned image (`ubuntu-24.04`) | refused, unless the job is a listed compatibility target or has a dated fallback |

The check parses every file of `.github/workflows/` and reads every string of
it, not only `runs-on`: a matrix value, the fallback of an expression, the
matrix a job prints as JSON for a later job and an input handed to a reusable
workflow all spell a label. Each label is attributed to the job that holds it;
text outside `jobs` is reported under the job name `(workflow)`. YAML comments
and comment lines inside a `run:` block are not read, and neither are names
that only contain a label, such as the container image `dev-ubuntu-26.04`.

A compatibility target is a job whose subject is the older image. The script
lists them in `COMPATIBILITY_TARGETS` with the record that says why; today that
is one job, `verify-native-artifacts` in `supply-chain.yml`
([ADR-1354](../adr/1354-native-bundle-release-track.md)). The check also fails
when a listed job stops naming its image, so the list cannot go stale.

A dated fallback is for a job that fails on the current image and needs time.
Declare it in `.config/lint-exceptions.d/hosted-runner-label.toml`, the
[declared exception list](#copyright-and-spdx-hook-and-the-declared-exception-list)
with two more keys:

```toml
[[exception]]
path = ".github/workflows/example.yml"
job = "build"
label = "ubuntu-24.04"
reason = "what fails on ubuntu-26.04, and the issue that tracks it"
expires = 2026-12-01
```

The end date is at most 90 days out and still holds on the day itself. An entry
fails the check when it has expired, when its job no longer names the label,
when it names a floating label, or when a key is missing. The list takes one
entry per job, so two jobs of one workflow file can each carry a fallback; the
same file and job twice is refused as a duplicate. The jobs that carry one today
are listed under [Hosted Linux image](ci-runners.md#hosted-linux-image).

`test-check-hosted-runner-labels` keeps positive, negative and boundary cases
for the script: a floating label as a literal, a matrix value, an expression
fallback, a printed matrix and text outside `jobs`; an older image without a
declaration; an expired, stale or malformed fallback; two fallbacks in one
workflow, and the same workflow with one of them removed; an unreadable workflow;
the read bounds (512 workflows, 256 entries, 200 000 nodes per job), each of
which is a finding and never a silent cut. Run the script alone with:

```bash
python3 scripts/ci/check_hosted_runner_labels.py
```

## Existing hooks and migration

The installer recognizes its own dispatchers, unmodified framework
hooks, and the repository's historical source symlinks, including links
into deleted `.claude/worktrees/` directories. It retains replaced
managed hooks in uniquely named `HOOK.vmafx-backup-*` files. Repeating an
unchanged installation makes no new backups.

Unknown regular hooks and symlinks cause installation to stop before
changing any hook. Review the reported paths and relocate or integrate
your custom hook explicitly before retrying. Existing backups are never
overwritten. Merely fetching the fix does not repair an already dangling
symlink: rerun `make install-hooks` from a checkout containing the fix.

Dispatch uses each active worktree's checked-out configuration. Installing
from an updated worktree repairs hook lifetime everywhere, but an older
branch does not acquire newly registered checks until its source config
is updated. In particular, older configs still lack the MkDocs push entry.

For inspection without installing:

```bash
git config --show-origin --get core.hooksPath
git rev-parse --path-format=absolute --git-path hooks
```

## clang-format reads `.hip` and `.metal`

The `clang-format` hook (pin `v23.1.2`, `.clang-format` at the root) reads C, C++
and CUDA by file type and HIP and Metal by extension: pre-commit's `types_or`
has no tag for either, so a second entry, `clang-format-hip-metal`, selects
`\.(hip|metal)$`. Both entries go through `scripts/ci/pelorus_mirror.py`, so the
Pelorus mirror stays exempt. The one exclusion is `scripts/ci/exact_twins.d/`:
those `*.hip` files are parity-gate data fragments ([ADR-1428](../adr/1428-exact-twins-fragments.md))
that only borrow the extension.

| Reader | Files |
| --- | --- |
| hook `clang-format` | C, C++, CUDA by type |
| hook `clang-format-hip-metal` | `*.hip`, `*.metal` |
| `make format`, `make format-check` | `CLANG_FORMAT_FILES` in the `Makefile`: `*.c *.h *.cpp *.hpp *.cu *.cuh *.hip *.metal`, minus the fragments and the mirror |
| native hook (`VMAFX_NATIVE_HOOKS=1`) | the same extensions, staged files |

```bash
pre-commit run clang-format clang-format-hip-metal --all-files
make format-check
```

Formatting a kernel moves line breaks only. For a `.hip` file, prove it with the
device assembly (`hipcc -S --cuda-device-only --offload-arch=gfx1036 ...`, the
random `__hip_cuid_*` symbol masked) before and after; for a `.metal` file, which
only builds on macOS, compare the files with all whitespace removed.
`test_clang_format_scope.py` keeps the selection, the Makefile list and the native
regex in step.

## Native formatting option

```bash
VMAFX_NATIVE_HOOKS=1 make install-hooks  # opt in
make install-hooks                      # restore the default
```

Native mode preserves the formatter-only choice in
[ADR-0924](../adr/0924-native-pre-commit-hooks.md). It runs installed
`ruff check --fix`, `clang-format -i`, and `shfmt -w` on matching staged
paths and restages files changed by formatters. It does not hand a declared
shfmt exception to shfmt (see
[shfmt reads every shell file](#shfmt-reads-every-shell-file)). Missing
formatters print a notice. Its pre-commit stage does not run the framework's security,
metadata, or agent-drift checks; use the default for those local checks.
Commit-message validation and push checks still use the framework in
both modes. CI uses the full framework configuration.

The native formatter reads working-tree files and restages the whole file
when it changes one. Use the framework path for partially staged files to
preserve the unstaged portion through the framework's stash/restore flow.

## black and ruff read every Python file

The `black` (pin `26.10.0`) and `ruff-check` (pin `v0.16.10`) hooks select files by
type, `python` and `pyi`, with no path filter: every tracked `.py`, `.pyi` and
extensionless Python script is read, wherever it lives (`python/`, `compat/`,
`core/test/`, `mcp-server/`, `dev-llm/`, `testdata/`, `.config/`, ...). The only
files left out are the **declared exceptions**,
`.config/lint-exceptions.d/black.toml` and `.config/lint-exceptions.d/ruff.toml`:
one tracked file per entry, with a reason and an expiry (format and rules:
`scripts/ci/lint_exceptions.py`). Today these are the praetor-managed
`tools/figures/mkdocs_hook.py` (ruff only: `praetorctl audit` compares it with
praetor's own bytes, praetor ships it black-formatted since the pin
`3a766f2d56ad`, and its two lazy imports still fail this repository's
`PLC0415`) and five HISS scanner fixtures that are a defect by design.
`.config/agent/hooks/block_evasion.py` is no longer listed: since the pin
`9bf502ec6ce5` it passes both tools as praetor ships it.

The hook `exclude` regexes, the `extend-exclude` of `pyproject.toml` and the list
name the same files; `test-python-format-scope` (always run) fails when they
differ, when a listed file no longer fails its tool (a stale entry), or when an
entry is past its expiry. `make lint-py`, `make format` and `make format-check`
run `ruff check .` and `black .`, which read the same `extend-exclude`; the native
pre-commit hook runs `ruff check --force-exclude` on every staged Python file.

```bash
pre-commit run black ruff-check --all-files
make lint-py
```

## shfmt reads every shell file

The `shfmt-src` hook has no path filter: it reads every tracked shell file
but the **declared exceptions** in `.config/lint-exceptions.d/shfmt.toml`,
one tracked file per entry with a reason and an expiry. Today that is one
file, praetor's hook launcher `.config/lefthook/engine.sh`. `praetorctl adopt`
refreshes it at a pin move only while it is praetor's own text, so a
formatter must not rewrite it
([cordanaLLM/praetor#1282](https://github.com/cordanaLLM/praetor/issues/1282)).

Three places skip the listed files: the `exclude` of the hook, the
`SHFMT_FILES` list that `make format` and `make format-check` share, and the
shell branch of the native hook. `test-shfmt-scope` (always run) fails when
one of them differs from the list, when an entry is incomplete, untracked or
past its expiry, when a listed file is shfmt-clean again (a stale entry), or
when another shell file falls out of scope.

```bash
python3 scripts/ci/tests/test_shfmt_scope.py
bash scripts/ci/tests/test-native-pre-commit.sh
```

## Regression checks

```bash
python3 scripts/githooks/tests/test_install.py
python3 scripts/git-hooks/test-pre-push-mypy.py
python3 -m unittest scripts.lib.test_safe_subprocess \
  scripts.ci.tests.test_agent_eligibility_precheck
```

This runs real Git commits and pushes to disposable local repositories.
It tests installer-worktree deletion, failed commit/message/push gates,
first-push and draft documentation failures, custom-hook refusal,
framework migration, legacy hooks, native mode, and the rebase guard.
The required `Pre-Commit` CI job runs the same fixture before the normal
file checks. `make lint-sh` also runs it.

The mypy fixture exercises a real rebase and the installed pre-commit
framework, including an empty outgoing file list, type changes, safe and
unsafe symlinks, ref mismatches and missing prerequisites. Its registered
pre-commit/pre-push check also runs in required `Pre-Commit` CI.

See [ADR-1241](../adr/1241-worktree-hook-dispatch.md) and the
[research digest](../research/1241-worktree-hook-dispatch.md).

## Disposable Git fixture safety

Git exports repository and index variables to hooks. Changing directory or
using `git -C` does not override them, so a test that creates a temporary
repository must clear inherited `GIT_*` before its first Git command and
disable caller system/global Git configuration. The FFmpeg replay/smoke,
dependency-classifier, Level Zero and agent-cleanup fixtures use this isolation
for setup and assertions as well as the operation under test.

Run their caller-preservation regression with:

```bash
python3 scripts/ci/test_git_fixture_isolation.py
```

It creates fake caller repositories with committed, staged and unstaged work,
then runs each fixture with `GIT_DIR`, `GIT_COMMON_DIR`, `GIT_WORK_TREE`,
`GIT_INDEX_FILE` and `GIT_CONFIG_PARAMETERS` individually and together.
Every fixture must succeed without changing any caller metadata or files.
A second regression invokes a real Git hook from a disposable linked worktree.
Git supplies `GIT_DIR` itself; the old unisolated `git init` control changes
the fake caller's shared `core.bare`, while the current Level Zero test must
preserve both worktrees and all shared Git metadata byte for byte. This tests
the hook environment even when the invoking shell has no Git variables.

### Fixture identity and hook environments

A git hook runs with `GIT_DIR` and `GIT_INDEX_FILE` exported. A test that runs
`git init`, `git config user.email t@t` or `git commit` in a scratch directory then
acts on the repository that is running the hook: the identity lands in the shared
`.git/config` (every linked worktree reads it), and commits and index entries land
in the caller's history. On 2026-10-08 four commits on `master` were created with the
committer `t@t` this way; history on `master` is not rewritten. The same path put
`core.bare = true` into the shared config on 2026-10-10, which made the main checkout
refuse every work-tree command. Rules for a script that runs git on a repository
other than the caller's (a scratch repository, a clone into a temporary directory):

- Shell: `source` `scripts/lib/clean-git-env.sh` after `set -euo pipefail`. It drops every `GIT_*`
  variable, ignores global and system configuration and sets `GIT_AUTHOR_*` /
  `GIT_COMMITTER_*`. Python and Node fixtures build the same environment (no `GIT_*`
  from the caller, `GIT_CONFIG_GLOBAL=/dev/null`).
- A script that is not a fixture but clones or initialises a repository elsewhere
  sources `scripts/lib/drop-git-env.sh`: it unsets every `GIT_*` variable and changes
  nothing else, so the user's proxy and credential configuration still apply.
- Never write an identity into a config (`git config user.email`). Use the environment
  variables above, or `git -c user.name=... -c user.email=... commit` per command.
- `python3 scripts/ci/test_git_fixture_isolation.py` enforces this with three checks,
  because each one misses what another catches:
  - **Identity rule.** It rejects a `git config user.*` write in any tracked script. A
    named, expiring exception list covers Python helpers that already scrub the
    environment.
  - **Isolation rule.** A tracked script (`.sh`, `.py`, `.mjs`, `.go`, `.rs`, `.mk`,
    `Makefile`) that runs `git init` or `git clone`, or that writes any config key
    (`core.bare`, `uploadpack.*`, ...), must drop the caller's variables in the same
    file: one of the two shell helpers, or an environment built without keys that
    start with `GIT_`. This is the check for a pytest-only test and for a script that
    is not a test. A file isolated in a way the check cannot see is named in
    `ISOLATION_EXCEPTIONS` with its reason; the list expires.
  - **Sentinel run.** Every scratch-repository test script that runs on its own is
    executed under a sentinel `GIT_DIR` and fails when the sentinel's config, index,
    refs or objects change. Planted scripts that write an identity, `core.bare` or
    `uploadpack.*`, or that clone, prove the sentinel sees each of them.

  The whole gate takes about four minutes. The hook `git-fixture-isolation-static`
  runs the two static rules alone, in under a second, on every changed script.

  The sentinel run is four tests (`test_sentinel_shard_*`), because the tooling
  suite stops any single test after 300 s and a hosted runner needed more than
  that for all scripts at once. The measured slow scripts are named in
  `SLOW_SHARDS`; every other script lands in a rest shard by its position in the
  sorted list, so a new script is always covered, and a test fails when a script
  is in no shard or in two. One shard carries its own limit of 900 s: its single
  script, `test-default-model-single-source.sh`, took 396 s on a hosted runner.

Only temporary caller paths are injected. The local pre-commit/pre-push hook
runs when its inputs change. Required `Pre-Commit` CI also runs the regression.
