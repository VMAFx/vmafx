# Licence provenance check

Every file's licence is decided by where its code came from
([ADR-1250](../adr/1250-eupl-fork-relicense.md)): fork-authored files are
`EUPL-1.2`, and a file that carries Netflix, libjxl, Xiph.Org or IQA code keeps
those terms and their copyright notices. The required check `Licence Provenance`
in [`lint-and-format.yml`](../../.github/workflows/lint-and-format.yml) holds
the tree to that rule on every pull request and every push to `master`
([ADR-1474](../adr/1474-relicense-helper-headers-and-ci-check.md)).

## If the check failed on your pull request

The job log lists one line per file, `<action><TAB><path>`, then `pending: N`.

| Line | What it means | What to do |
| --- | --- | --- |
| `relicense <path>` | A fork-authored file carries another licence, or a new source file has no header | `python3 scripts/dev/relicense_fork_files.py --write` and commit the result |
| `repair <path>` | A file that keeps its terms has an identifier that does not exist (`BSD+Patent`, `BSD-3-Clause-Plus-Patent`) | The same `--write` |
| `attribute <path>` | The file sits where a reviewed rule says code of another origin lives, and it lacks that origin's notice or licence | Read the file against the reference first, then pick one of the three cases below |
| `stale-review <path>` | `scripts/dev/relicense_provenance.toml` names a file that no longer exists | Move the entry to the file's new path, or delete it |

For an `attribute` line, what the file actually contains decides:

1. **It reproduces the code its directory's family names** (a kernel, a SIMD
   twin): run `--write`. The tool adds the notices and extends the tag.
2. **It reproduces only part of it** (a helper header with one term of the
   reference): add a `[ports."<path>"]` entry to
   `scripts/dev/relicense_provenance.toml` naming the origins it holds, then run
   `--write`. A fork-created header ends as `EUPL-1.2 AND` the licences of
   exactly that code.
3. **It reproduces none of it** (an argument block, or macros and an include of
   a shared header that carries the notices itself): add a `[not_ports]` line
   with the reason. The file stays `EUPL-1.2` and is not changed.

Do not pick a tag by hand and do not remove a notice. `[ports]`, `[not_ports]`
and the family rules are reviewed data; a new entry is part of the pull
request's diff, with the reason in it.

`.toml` files are candidates like the other types the tool can comment (since
[ADR-1699](../adr/1699-root-licence-files-eupl.md)): a new fork manifest or
configuration file without a header shows up as `relicense <path>`, and
`--write` gives it the two-line `#` header (the `Copyright <year> Lusoris`
line and the EUPL-1.2 tag) the tool writes into Python files. The name
`pyproject.toml` carries no provenance signal, so upstream's
`python/pyproject.toml` does not veto a fork package's manifest.

Copyright notices must name the project holder only
([ADR-0861](../adr/0861-vmafx-copyright-policy-drop-anthropic.md)). The gate
refuses any tracked file whose header (the leading comment block within the
first 30 lines) carries a copyright line naming an AI tool or its vendor
(case-insensitive: Claude, Anthropic, OpenAI, ChatGPT, Copilot, Gemini, Codex)
with a message naming the file and line number. To fix this refusal, delete the
AI tool copyright line from the file header, keeping the `Copyright <year> Lusoris`
notice and the SPDX licence identifier intact.

## Running it locally

```bash
git remote add --no-tags upstream https://github.com/Netflix/vmaf.git   # once
git fetch upstream
pin="$(python3 scripts/ci/upstream_parity_pin.py --within upstream/master)"
python3 scripts/dev/relicense_fork_files.py --check --upstream-ref "$pin"
```

Reading the result:

- `pending: 0` and exit status 0 mean the tree is clean.
- `--list` prints the verdict of every candidate file instead.
- `--write` applies every pending change.

The run reads the history of each candidate (about a minute on four cores),
which is why it is a CI job and not a commit hook. The commit-time side is the
pair of hooks `test-relicense-fork-files` and `test-upstream-parity-pin`, which
run the tool's unit tests and the contract below when their inputs change.

!!! note "Full history required"
    The checkout needs its full history: the tool refuses a shallow clone
    (`git fetch --unshallow` fixes one), because the veto that protects an
    outside contributor's work reads every commit that touched a file.

## The upstream commit it compares against

Several vetoes ask whether a path or a file name exists in Netflix/vmaf. The
job does not ask upstream's moving `master`; it asks the commit the repository
records as the upstream head it is at parity with, the one heading of this form
in [known upstream bugs](known-upstream-bugs.md):

```text
## Upstream head the fork is at parity with: `<commit id>` (<date>)
```

[`scripts/ci/upstream_parity_pin.py`](../../scripts/ci/upstream_parity_pin.py)
reads it. A file Netflix adds therefore cannot turn the job red on an unrelated
pull request; the upstream port or sync that moves the heading is the pull
request that sees the difference.

The reader fails, and the job with it, when:

- the heading is missing, or its wording changed;
- two headings of that form exist (give the older section another title);
- the id is not 7 to 40 lowercase hex digits inside a code span;
- the id does not name exactly one commit, or Netflix's `master` does not
  contain it.

## What the job does

1. Checks out the pull request with full history (`fetch-depth: 0`).
2. Runs `scripts/dev/tests/test_relicense_fork_files.py` and
   `scripts/ci/tests/test_upstream_parity_pin.py`.
3. Fetches `master` of `https://github.com/Netflix/vmaf.git`.
4. Resolves the recorded head with `upstream_parity_pin.py --within FETCH_HEAD`.
5. Runs `relicense_fork_files.py --check --upstream-ref <that commit>`.

It has no path filter and no conditional skip. The
[required aggregator](ci-job-names.md#aggregator-gating) lists it in `required`
and in `strictMustReport`, so a run in which it never reported fails too.

## What the tool does not manage

- `scripts/ci/exact_twins.d/` (data fragments whose suffix names a backend).
- `.codex/agents/*.toml` (praetor's Codex projections of `.agents/agents/`,
  written by the engine; [ADR-1699](../adr/1699-root-licence-files-eupl.md)).
- `tools/figures/` (files of the governance engine that `praetorctl audit`
  compares with praetor's own bytes).
- `.config/agent/hooks/block_evasion.py` and `.config/lefthook/engine.sh`
  (praetor's scripts, which `praetorctl adopt` refreshes only while they are
  praetor's own text; dated exceptions in
  `.config/lint-exceptions.d/spdx.toml`).
- Verbatim mirrors of another repository (`[mirrors]`, the Pelorus files).
- A licence grant below a file's own header (the first 60 lines), for example
  the header template a sync script embeds as a string.

`reuse lint` (`make lint-reuse` locally, the `REUSE lint` job of
[`reuse.yml`](../../.github/workflows/reuse.yml) in CI) covers these files'
licence metadata through [`REUSE.toml`](../../REUSE.toml).
