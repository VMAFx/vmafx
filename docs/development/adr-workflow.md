<!-- markdownlint-disable MD013 -->
# ADR Authoring Workflow

Reserve an ADR number with `scripts/adr/next-free.sh --claim`, write the ADR,
register its index fragment and regenerate the generated files. This page
describes how to create a new Architectural Decision Record (ADR) in this fork
without colliding with a number already claimed by another agent or branch.

## Quick start

1. Reserve a number atomically. A stub file
   `docs/adr/0532-my-topic-slug.md.stub`
   (the number is an example) is created immediately:

    ```bash
    N=$(scripts/adr/next-free.sh --claim my-topic-slug)
    ```

2. Edit the stub into a real ADR:

    ```bash
    $EDITOR docs/adr/${N}-my-topic-slug.md.stub
    ```

3. Rename the stub to the final filename before committing:

    ```bash
    mv docs/adr/${N}-my-topic-slug.md.stub docs/adr/${N}-my-topic-slug.md
    ```

4. Create the one-row index fragment and register its order:

    ```bash
    $EDITOR docs/adr/_index_fragments/${N}-my-topic-slug.md
    printf '%s\n' "${N}-my-topic-slug" >> docs/adr/_index_fragments/_order.txt
    ```

5. Regenerate and check all source-owned metadata:

    ```bash
    make docs-fragments-write
    make docs-fragments-check
    ```

6. Check what the regeneration changed, then stage the ADR, the fragment and the
   generated outputs:

    ```bash
    git status --short
    git add docs/adr/${N}-my-topic-slug.md \
      docs/adr/_index_fragments/${N}-my-topic-slug.md \
      docs/adr/_index_fragments/_order.txt docs/adr/README.md \
      docs/adr/by-tag/ CHANGELOG.md
    git commit -m "docs(adr): ADR-${N} my topic slug"
    ```

    `make docs-fragments-write` also regenerates the exact-twins table, the
    upstream-parity allowlist, the agents index and the hardware reports, so an
    ADR PR can dirty those files too. Stage whichever of them `git status`
    shows as changed.

## Commands

### Reserve a number

```bash
N=$(scripts/adr/next-free.sh --claim <slug>)
```

- `<slug>` must match `[a-z0-9][a-z0-9-]*` (lowercase letters, digits, hyphens).
- It prints the reserved 4-digit number on stdout.
- It creates `docs/adr/<NNNN>-<slug>.md.stub` on disk.
- It soft-fails on a network outage (fetch errors are non-fatal); the pre-commit
  hook and the CI gate (`adr-collision-check`) remain the hard backstop.

### Print the next free number (read-only, no claim)

```bash
scripts/adr/next-free.sh
```

This does not create any file. Use it only for inspection: do not use the
printed number as the basis for hand-creating an ADR file without immediately
calling `--claim`, because the number may be taken by the time you create the
file.

### Release an abandoned claim

```bash
scripts/adr/next-free.sh --release <NNNN>
```

This removes the stub file for `<NNNN>` and frees the slot. Use it if a PR is
abandoned and the stub was never promoted to a real ADR.

## Stub lifecycle

```text
--claim slug      stub created (NNNN-slug.md.stub)
                  |
edit stub         fill in ADR content in-place
                  |
mv stub -> .md    rename before committing (git tracks the .md, not the .stub)
                  |
git commit        stub is gone; real ADR lives in tree
```

Stubs are gitignored by the pre-commit hook (it only fires on `.md` files), so
they do not pollute commit history. They are also excluded from
`check-adr-numbering`: only the final `.md` is validated.

## Why use `--claim`?

Use `--claim` because the read-only mode hands several callers the same number.
On a busy session day (2026-05-18), five or more parallel Claude agents called
`scripts/adr/next-free.sh` within seconds of each other and received the same
answer, because the read-only mode prints the next free number without
reserving it. That caused about ten renumbers and rebases in a single session
(ADR-0532).

`--claim` prevents this in four steps:

1. It acquires a POSIX `mkdir` lock, atomic on Linux ext4 and tmpfs, so
   concurrent callers on the same host serialize. The lock name
   (`/tmp/vmaf_adr_claim_lock_<path>`) is built from the absolute, resolved path
   of the repository's common git directory, so a caller in `docs/`, one in the
   repository root and one in a linked worktree take the same lock. The lock
   is held by the caller that creates its owner file (exclusively, `O_EXCL`);
   the file records an owner token, and a caller only removes a lock it holds.
   A lock directory without an owner file is free.
   A caller waits up to 60 s for the lock (`VMAF_ADR_LOCK_WAIT_S` changes the
   bound) and then fails with the holder's owner line and process id. An
   owner file whose process is no longer running was left by a killed claim:
   the next caller takes a second lock that serialises takeovers, checks the
   process again, checks that the owner line is still the one it read, removes
   the file and prints a `WARNING: took over a stale ADR allocator lock` line.
   A lock whose holder still runs is never taken over.
2. It scans remote branches via `git ls-remote --heads` and `git ls-tree` to
   also skip numbers already claimed by in-flight branches on origin.
3. It creates the claim file `.git/adr-claims/NNNN` exclusively (`O_EXCL`, the
   shell's `noclobber`). If another caller already created it, the number is
   taken and the next one is tried, up to 100 times. This step decides the
   number even if two callers ever hold the lock at once.
4. It writes a `docs/adr/NNNN-<slug>.md.stub` placeholder that all later callers
   (including read-only mode) treat as a taken number.

### The invariant for agents

All fork-local agents that create ADRs must call `--claim` before creating the
file. This is documented in the root `AGENTS.md` and is hard rule 8 of the
[agent hard rules](agent-hard-rules.md). An agent that hand-picks a number
without calling `--claim` passes the local pre-commit hook but may collide at
the CI `adr-collision-check` gate.

## Generated metadata

Edit ADR files and index fragments as the sources. The README index and the
by-tag pages are rendered outputs; never correct those generated files by hand.

The site's sidebar lists only the ADR index, the template and the tag index
([ADR-1510](../adr/1510-adr-nav-collapse-behind-index.md)), so a new ADR does
not touch `mkdocs.yml`: readers find it through the index, its tag pages and
search.

| Item | Behaviour |
| --- | --- |
| Generation | `make docs-fragments-write` regenerates, in order: the changelog, the ADR index, then the tag pages. |
| Check | `make docs-fragments-check` is read-only and fails on missing, changed or obsolete generated files. It also checks that every ADR has one correctly named fragment, fragment ADR links resolve, and the order manifest has no duplicate or missing-fragment entries. |
| Where the check runs | The local pre-commit hook, the required `Docs` CI job and the Pages build. A clean MkDocs build alone does not prove metadata freshness. |
| Generator tests | `python3 scripts/docs/tests/test_generators.py` runs the disposable fixture tests. |
| After a rebase | If the rebase brings in other ADRs, regenerate again from the combined sources before committing. |

Tag rules, from each ADR's front matter
([ADR-1242](../adr/1242-generated-adr-freshness.md)):

- existing case and backtick normalisation is preserved, and repeated
  equivalent tags contribute one row per ADR;
- unsafe filename characters and the reserved tag `index` are rejected;
- ordinary tags, including `c++`, underscore names and version tags, retain
  their spelling after normalisation;
- the generator escapes title text for Markdown tables without changing
  accepted ADR bodies or stripping meaningful spaces from code examples.

## Linking to an ADR

Cite an ADR by number and link to its file, then run the link checker:

```markdown
See [ADR-0165](../adr/0165-state-md-bug-tracking.md) for the bug-ledger rule.
```

`scripts/ci/check-adr-links.py` runs as the `check-adr-links` pre-commit hook
on any change under `docs/`, and resolves slug first, then number:

```bash
python3 scripts/ci/check-adr-links.py         # report
python3 scripts/ci/check-adr-links.py --fix   # repair what resolves unambiguously
```

A link carries the decision's identity twice, as the number and as the slug,
and either half can go stale on its own:

| Half that rotted | How it happens | Repair |
| --- | --- | --- |
| slug | the ADR was renamed; the number still names the right decision | from the number |
| number | a collision sweep renumbered the file; the slug still names the right decision | from the slug, and the `[ADR-NNNN]` text |

`--fix` refuses two cases and reports them instead: a citation where neither
the number nor the slug matches anything, and one where the two halves disagree
about which file they mean. Both need a person.

When a number genuinely has no ADR (planned and never written), drop the link
and cite the number in plain text, so a reader is not sent to a 404.
`docs/state.md` does this for ADR-0846, which the tree skips entirely.

!!! note "Why slug first"
    The second row is not hypothetical: it is the larger half. `af227b026`
    (lusoris/vmaf#310) and `fb14bc332` (lusoris/vmaf#752) were ADR collision
    sweeps that renumbered duplicate-numbered ADRs; the second renamed 50
    files, moving `0241-vmaf-tiny-v3-mlp-medium.md` to
    `0389-vmaf-tiny-v3-mlp-medium.md` and 27 others into the 0388 to 0415
    band. Each sweep moved the file and its index fragment and left every
    inbound citation on the old number.

Repairing every broken link from its number was tried first. For the 35
links the sweeps had renumbered, it silently repointed the citation at
whatever unrelated ADR now holds the old number: `[ADR-0241]` in a tiny-AI
evaluation digest became a link to the HIP PSNR kernel-template ADR. Those
links resolve, so they read as authoritative and no checker complains
afterwards. A dead link is better than a confident wrong one.

!!! note "What the gate does not check"
    A citation whose number and slug agree and are both the wrong decision
    needs review, not a parser. It is tracked as
    `T-STALE-ADR-CITATIONS-2026-09-16`.

## Running the smoke tests

```bash
bash scripts/adr/test-next-free.sh
bash scripts/adr/tests/test-next-free-concurrent.sh
```

The first suite covers sequential claims, parallel (race) claims, `--release`
and invalid-slug rejection. The second starts parallel claims from the
repository root and from `docs/` in a throwaway repository, repeats the round,
and fails when two callers receive the same number. All assertions must pass. Run it after any change
to `scripts/adr/next-free.sh`.

## References

- [ADR-0386](../adr/0386-adr-numbering-collision-prevention.md): original
  three-piece defence (hook, CI gate and helper script).
- [ADR-0535](../adr/0535-adr-atomic-allocator.md): this atomic-claim extension.
- [docs/adr/README.md](../adr/README.md): ADR index and conventions.
- [docs/adr/0000-template.md](../adr/0000-template.md): ADR file template.
