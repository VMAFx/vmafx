# Praetor documentation gate and engine pin

Praetor is the standards engine that `make verify-all` and the required
`Standards` job run. This page covers what a contributor needs when the gate
touches documentation, and how a maintainer moves the engine pin. For the
other local checks see [CI overview](ci.md#before-you-push).

## Praetor documentation gate

`make verify-all` runs two targets from praetor's Documentation Governance
gate, which the `docs:seo-portal` facet in `.standards.yaml` requires:

| Target | Runs | Notes |
| --- | --- | --- |
| `make docs-lint` | `node tools/markdownlint/verify.mjs` | Installs its pinned `markdownlint` library (0.41.1, [ADR-1506](../adr/1506-praetor-pin-move-markdownlint-braces.md)) from the npm registry into a temporary directory, so it needs network access and leaves no `node_modules` in the tree. |
| `make docs-figures` | `node tools/figures/build.mjs check` and `sources` | Skips, and says why, while the repository has no figure spec under `docs/figures/` and no output under `docs/assets/figures/`. |

Both need Node.js 24. The hosted copy is
[`praetor-docs.yml`](../../.github/workflows/praetor-docs.yml), whose check name
is `Documentation Governance`.

### Files praetor owns

!!! warning "Do not edit praetor-owned files by hand"
    `praetorctl audit` locks them byte for byte. A praetor pin move in
    `standards-gate.yml` brings their updates, and Renovate is told to leave
    them alone.

The locked files are:

- `.github/workflows/praetor-docs.yml`;
- everything under `tools/markdownlint/` and `tools/figures/`;
- the `# BEGIN praetor documentation gate` block in the `Makefile`;
- the `# BEGIN praetor managed attributes` block in `.gitattributes`.

Consequences worth knowing:

- The workflow keeps praetor's name, which is two characters over the
  30-character budget in [CI job display names](ci-job-names.md).
- `.gitignore` re-includes `tools/figures/dist/`, which the repository-wide
  `dist/` rule would otherwise hide from a checkout that audit then fails on.
- The repository's own black, ruff and markdownlint pre-commit hooks skip
  `tools/figures/`, since a rewrite there fails the audit
  ([cordanaLLM/praetor#578](https://github.com/cordanaLLM/praetor/issues/578)).
- Praetor requires Git to ignore the retired numbered workspace root, so the
  [ADR-1277](../adr/1277-workingdir-contract-cleanup.md) contract check accepts
  praetor's managed rule for it and nothing else
  ([ADR-1351](../adr/1351-praetor-engine-pin-move.md)).

### Settings

The gate's settings live in the `documentation` block of `.standards.yaml`:

| Setting | Value | Reason |
| --- | --- | --- |
| `max_files` | 8192 | The tree holds about 4,000 Markdown files, close to the default bound of 4,096. |
| `max_file_bytes` | 4194304 | `docs/rebase-notes.md`, `docs/changelog-archive/1.0.0-rc.1.md` and `docs/state.md` exceed the default 1 MiB. 4 MiB is praetor's ceiling. |
| `style_exclude` | `docs/adr/README.md`, `docs/adr/_index_fragments/[0-9]*.md`, `**/testdata/**` | The generated ADR index, the per-ADR row fragments and test fixtures, which the repository's own markdownlint hook in `.pre-commit-config.yaml` also skips. |

`docs/rebase-notes.md` grows with every rebase-sensitive change. It is about
3.2 MB and must be split before it reaches the 4 MiB ceiling.

### Style rules and line length

Excluded files still go through the gate's private-link rule. The style rules
come from praetor's locked `tools/markdownlint/markdownlint-cli2.yaml`, not from
the root `.markdownlint.json`, which the gate ignores.

The two disagree on `MD013`: praetor turns it off, the root file limits lines
to 80 characters. To exempt a block from line length, wrap it:

```markdown
<!-- markdownlint-capture -->
<!-- markdownlint-disable MD013 -->
long lines here
<!-- markdownlint-restore -->
```

A closing `markdownlint-enable MD013` switches the rule on with markdownlint's
defaults under praetor's configuration, tables included.

## Moving the praetor pin

`PRAETOR_REF` in
[`standards-gate.yml`](../../.github/workflows/standards-gate.yml)
names the praetor commit that CI installs with `go install`. The same engine has
to run in your hooks: lefthook calls whatever `praetorctl` is first on `PATH`,
not the pin. Check yours before you commit or push:

```bash
praetorctl version   # prints the first 12 hex digits of PRAETOR_REF
```

To move the pin, change `PRAETOR_REF` to a commit whose own CI is green, install
that engine, and let it regenerate what it owns:

1. Install the engine and put it first on `PATH`:

    ```bash
    GOBIN=<engine>/bin go install github.com/cordanaLLM/praetor/cmd/standardsctl@<sha>
    ln -s standardsctl <engine>/bin/praetorctl
    export PATH=<engine>/bin:$PATH
    ```

2. Regenerate the locked and compiled files:

    ```bash
    praetorctl profile set --lock-source-root=<praetor checkout at sha>
    praetorctl compile-context
    ```

3. Record the baseline with the new engine, on the final tree:

    ```bash
    praetorctl baseline -record -allow-increase -reason "<what the engine measures that it did not>"
    ```

4. Verify:

    ```bash
    praetorctl audit
    praetorctl hiss coverage --verify
    praetorctl dedupe scan .
    make verify-all
    ```

!!! warning "Run `adopt --force` only in a throwaway copy"
    `praetorctl adopt --force --lock-source-root=<praetor checkout>` rewrites
    the `AGENTS.md` harness with a table that marks every invariant as not
    enforced, adds `praetorctl hook` entries to the agent settings files and
    writes `.gemini/settings.json`, which the local exclude file hides. None of
    that is wanted here ([ADR-1351](../adr/1351-praetor-engine-pin-move.md)).
    Run it in a copy of the tree, never the real one, and copy back only the
    files that audit reports as stale (the documentation gate assets and the
    `.devcontainer/` bootstrap in the last move).

### Rules for a move

Three rules hold the move together:

- **Record the baseline with the new engine, on the final tree.** The baseline
  is keyed by file and line, and the engine decides what counts. A move may
  raise it with `--allow-increase` only when the old engine records no growth
  on the same tree and every added entry traces to an engine change, as
  ADR-1351 shows for the last move. Growth caused by code stays forbidden.
- **Update the README governance block by hand if audit says it is stale.** The
  line `<n> recorded infractions; audit forbids growth.` must equal
  `total_infractions` in `.standards-baseline.json`.
- **Move every hook engine at the same time, and rebase.** The two engines do
  not read each other's trees. A branch moves by rebasing onto the merged pin
  and switching `PATH` to the new engine. Measured on the move to
  `6c772713a133`:
  - the new engine fails `compile-context --verify`, `audit` and
    `hiss coverage --verify` on a branch at the old pin (register block, 244
    entries the old baseline does not record, a renamed HISS-01 catalog
    title);
  - the old engine fails the same three and `flavor audit` on the new tree,
    because it cannot parse `repository.default_branch`, `documentation` or
    `register.sources` in `.standards.yaml`;
  - `dedupe scan .` passes in both directions.

### Audit in hooks

`scripts/git-hooks/hiss-audit.sh` runs the audit in both the pre-commit and the
pre-push hook. Since praetor `6c772713a133` an audit reads the live Actions
permissions and the last workflow runs from GitHub, which takes about 40
seconds here against 2 seconds offline, so the hooks pass `--offline` when the
engine has the flag. CI and `make verify-all` keep the forge read.

### The current pin

The pin is `d141862c430b`
([ADR-3061](../adr/3061-praetor-pin-d141862c.md)). Against `3a766f2d56ad` it
changes one file here, `tools/apicompat/gate/main.go`. The API gate no longer
installs go-apidiff as `module@version`. It writes a small build module that
requires go-apidiff at the same commit and `golang.org/x/tools` v0.51.0
exactly, and builds the checker there. Go 1.27.2 writes export data version 5.
The x/tools v0.49.0 that go-apidiff requires reads up to version 4, so the
checker read every package as empty, and the gate's canary failed the
`Go API Compatibility` check on every pull request once setup-go's `stable`
resolved to 1.27.2 ([cordanaLLM/praetor#1051](https://github.com/cordanaLLM/praetor/pull/1051)).
`praetor-api.yml` still installs Go as `stable`, which is praetor's template
text ([cordanaLLM/praetor#1037](https://github.com/cordanaLLM/praetor/issues/1037)).
The engine's audit adds one advisory warning, about the missing cache-band
markers in `AGENTS.md`.

The pin before was `3a766f2d56ad`
([ADR-2784](../adr/2784-praetor-pin-3a766f2d.md)). Against `7458a220e1c9` it
changed four things here:

- **REUSE gates.** Because the root carries `REUSE.toml` and `LICENSES/`,
  `adopt` writes `.github/workflows/reuse.yml`, and audit checks that no
  `REUSE.toml` annotation is shadowed by a later one and that `LICENSE` holds
  the text of the declared licence (`LICENSES/EUPL-1.2.txt`) with no second
  root file named like a licence. Both checks pass. The `REUSE lint` job of
  `reuse.yml` is the one CI run of `reuse lint`: it is required through the
  aggregator (marker in `standards-gate.yml`), the Pre-Commit job skips its
  `reuse-lint` hook, and the commit hook and `make lint` keep it locally.
  `reuse.yml` is not byte-locked: praetor keeps an edited copy, so its two
  actions are pinned to commits here where praetor writes tags
  ([cordanaLLM/praetor#899](https://github.com/cordanaLLM/praetor/issues/899)).
  A planted file naming a licence without a text in `LICENSES/` fails it; a
  file without any SPDX line is covered by the `REUSE.toml` default and fails
  `scripts/ci/check-copyright.sh` instead.
- **Build-warnings gate (HISS-10).** Audit fails a CI lane that compiles C,
  C++, Rust or Go without its toolchain's warnings-as-errors form. It reads
  the form only as a literal on the command (`--werror` or `-Dwerror=true` on
  `meson setup`, `-Werror` in `CGO_CFLAGS`, `RUSTFLAGS: -D warnings`), and
  not what `$(scripts/ci/werror-args.sh ...)`, a step output or a matrix value
  prints. Since [ADR-2828](../adr/2828-ci-werror-every-lane.md) every one of
  the 50 lanes it reads spells its switch that way and builds warning-free
  ([CI](ci.md#warnings-are-errors-adr-2170-adr-2828)); the 16 workflow entries
  ADR-2784 declared in `.config/lint-exceptions.d/HISS-10.toml` are gone. A
  lane that cannot be gated yet would take an entry there, which
  `scripts/ci/praetor_tidy_coverage.py` copies into `.standards.yaml`
  ([tidy lanes](tidy-lanes.md#praetors-copy-of-the-same-facts)). Praetor is
  asked to read a declared wrapper script
  ([cordanaLLM/praetor#924](https://github.com/cordanaLLM/praetor/issues/924)).
- **Workflow trigger report (HISS-18).** Audit warns, without failing, about
  136 jobs in 28 workflows that skip a draft at job level or wait on a job
  that does. It recommends a failing first step instead, while this
  repository's tier design skips drafts at job level and lets the aggregator
  fail them ([ADR-2169](../adr/2169-ci-fewer-runs.md)). Three warnings name
  jobs that never run on a pull request
  ([cordanaLLM/praetor#922](https://github.com/cordanaLLM/praetor/issues/922)).
- **Engine-written files.** The managed workflows name `shell: bash` on their
  draft step; the API gate program is gofumpt-formatted and gains
  `tools/apicompat/gate/placeholder.go`; the figure-engine hook and notes are
  reformatted. All are regenerated by the engine.

An engine at this pin also writes an optional kind on bug-ledger rows, which
older engines refuse; move every engine that reads the shared ledger before
using `praetorctl state bug add --kind`.

The pin before was `7458a220e1c9`
([ADR-2440](../adr/2440-praetor-pin-7458a220.md)). Against `afb739ed81f3` it
changed three things here:

- The emitted `praetor-api.yml` and `praetor-docs.yml` push only on `master`,
  listen for `ready_for_review` and stop a draft pull request with a failing
  first step. The engine regenerates them, so audit still locks them byte for
  byte, and `push_branch_exceptions` in `.github/ci-tier.json` is empty
  ([CI routing](ci.md)). The jobs still start on a draft, so they stay in
  `untiered_jobs`.
- Audit fails when `core.hooksPath` leaves the managed hooks directory. A
  throwaway clone made with `git clone -c core.hooksPath=...` persists the
  value and fails it; pass `-c` on each command instead.
- The caveman article count ignores model names such as `A380`.

The pin before was `afb739ed81f3`
([ADR-2321](../adr/2321-praetor-pin-afb739ed.md)). Against its predecessor it
adds five checks this repository had to meet:

| Check | What it asks here | Where it is met |
| --- | --- | --- |
| Caveman lint of every tracked nested `AGENTS.md` | the internal register in 72 files | the converted files and `AGENTS.d/` pages ([text register](#what-the-text-register-checks)) |
| HISS-11 SLSA level, declared against measured | Level 3 declared by the `native-gpu-systems` profile and the `security:high` facet; the workflows reach Level 2 | a declared gap for `.github/workflows/docker-publish-operator-node.yml` in `.config/lint-exceptions.d/HISS-11.toml`, rendered into `.standards.yaml` ([tidy-lanes](tidy-lanes.md#praetors-copy-of-the-same-facts)); expires 2027-01-04 |
| Stale `.paperclip/rules.md` and harness rows | the harness praetor synthesises for the declared forge | `.paperclip/harness.json` and `rules.md` regenerated by `praetorctl adopt` in a throwaway copy (see the warning above) |
| Register skills | the register block names a skill only when `.agents/skills/` carries it | `compile-context` re-spliced the block without skill names |
| Markdown gate lock | `katex` overridden to 0.19.0, Node 22.12.0 or later | `tools/markdownlint/` regenerated by the engine |

The pre-commit hook must come from a known runner (lefthook here), and the Go
vulnerability gate (`praetorctl security govuln`) passes with the existing
OpenVEX document `security/vex/go.openvex.json`. The live branch-protection
comparison does not run, because `adoption.decline` lists `branch-ruleset`.

The pin before was `04cc813ff054`
([ADR-2153](../adr/2153-praetor-pin-04cc813.md)): the documentation gate's
lint time budget (`documentation.lint_timeout_seconds: 240` in
`.standards.yaml`, measured maximum of a lint child 11.4 s on this
repository), line-ending-neutral digests, and a clang-tidy translation-unit
coverage gate whose inputs are rendered from the repository's own tidy lists
([tidy-lanes](tidy-lanes.md#praetors-copy-of-the-same-facts)).

The pin before that was `0af07a733e65`
([ADR-1506](../adr/1506-praetor-pin-move-markdownlint-braces.md)).
It dropped `markdownlint-cli2` and the `braces` chain from the gate's lock, and
added praetor's `Go API Compatibility` workflow
(`.github/workflows/praetor-api.yml`, `tools/apicompat/`), which audit now
requires.

- The Required Checks Aggregator lists it in `required` (maintainer decision,
  2026-10-03).
- The workflow has no path filter, so a pull request without a Go change still
  reports and the comparison passes.
- It is in `strictMustReport` since the pin `7458a220e1c9`
  ([ADR-2440](../adr/2440-praetor-pin-7458a220.md)). Before, praetor's locked
  file did not run on `ready_for_review`, so a draft that became ready kept the
  result of its last push; now it is re-run. `test_go_api_compat_required.py`
  holds the list entry.
- Audit locks the workflow byte for byte, so its
  `# required-aggregator-job: Go API Compatibility` marker sits in
  `standards-gate.yml`; `scripts/ci/check-aggregator-names.sh` fails when no job
  reports a name marked that way, which a rename of the job would cause.
- The engine also reads shell, workflow and systemd files, so the baseline went
  from 227 to 503 with the old engine recording no growth on the same tree.

## What the text register checks

The `register:` section of `.standards.yaml` declares who reads which text.
`forge` is `social`, `docs` is `docs`; `agent`, `context`, `ledger`, `hooks`,
`prompts` and `mcp` are `internal`, the terse `caveman` form. The gate measures
only what it can read as a file:

| Text | Checked by | Gate |
| --- | --- | --- |
| Root `AGENTS.md` and the compiled vendor files | `praetorctl compile-context --verify`, `praetorctl audit` | blocks |
| Every tracked nested `AGENTS.md` (72 files) | the same commands | blocks |
| Canonical personas under `.agents/agents/` | the same commands | blocks |
| `.paperclip/harness.json` strings (`register.sources`) and `.paperclip/rules.md` | `praetorctl audit` | blocks |
| `AGENTS.d/` topic pages (they render into the nested `AGENTS.md` files), `.claude/` skills, agents and workflows | `praetorctl caveman check --kind=context <file>` | none; run it by hand |
| `.workingdir` ledger text, briefs, PR bodies, commit bodies | nothing | none |

Since the pin `afb739ed81f3` the gate lints the nested `AGENTS.md` files as
context, with no baseline. The move to that pin converted the 19 nested files
and 195 `AGENTS.d/` pages that failed `praetorctl caveman check --kind=context`
(article density, sentences over 30 prose words, hedges), so every page passes
the same command as well ([agents index](agents-index.md#add-a-page)).

The register block names no skill. Praetor names the `social-text` and
`caveman` skills only in a repository that carries them under
`.agents/skills/`, and this repository ignores that directory.
