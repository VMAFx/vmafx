<!-- markdownlint-disable MD013 MD060 -->
# Research 1277 — local workspace contract cleanup

## Question

Which tracked references are live, which local data belongs in the state root,
and how should public documentation cite evidence that is currently ignored?

## Findings

The pre-change tree contained 895 references across 368 tracked files after
generated indexes were excluded. Seventy-four Markdown links targeted ignored
workspace paths and therefore could not resolve from a fresh clone. The live
state CLI and the freshest local ledgers already used `.workingdir/`, while the
machine's 565 GiB corpus store already used `.corpus/`.

The retired local tree mixed state, corpus data, generated feature tables,
recovery material, external checkouts, and cache. Its durable portion was
archived before cleanup; the 200,724,946-byte zstd archive passed SHA-256 and
full tar-list verification. The rebuildable cache and clean external checkout
were not duplicated. No retired source directory remains in the main checkout.

## Classification

| Data class | Canonical destination | Examples |
|---|---|---|
| Agent continuity and local receipts | `.workingdir/` | OPEN/BACKLOG/BUGS, run logs, recovery bundles |
| Bounded disposable output | `.workingdir/cache/` | hook replay output, temporary encodes, extraction scratch |
| Dataset and reusable derived data | `.corpus/` | Netflix/KoNViD/CHUG media, feature JSONL, reusable encodes |
| Public durable evidence | tracked `docs/` or manifests | ADR rationale, research results, current bug status |

This classification shows why a global old-name to `.workingdir` substitution
is invalid: it moves corpus and derived-data defaults into the state ledger.

## Repository enforcement

The contract check verifies that:

1. `.workingdir/` and `.corpus/` remain ignored and untracked;
2. the retired numbered root is not ignored or tracked;
3. active source, configuration, tests, and operator docs do not name the
   retired root;
4. no tracked Markdown link points into an ignored local root;
5. representative runtime defaults resolve to the role-appropriate root.

Historical ADR and changelog prose is not rewritten into a false path. The
link target is removed when the source was local-only, and current public
claims cite tracked material instead.

## Verification

```bash
bash scripts/ci/tests/test-check-local-data-contract.sh
bash scripts/ci/check-local-data-contract.sh
python3 -m unittest discover -s scripts/ci/tests -p 'test_*.py'
praetorctl compile-context --verify
make docs-fragments-check
```

Package-focused tests cover Python and Go defaults changed by the migration.
No Netflix golden assertion or score computation changes.

The strict touched-file replay also exposed two error-propagation holes in
tools already changed by the migration. The dispatch precheck treated missing
backlog rows and unavailable GitHub/task scans as eligible, while the hardware
corpus producer printed `[skip]` for failed quality points and still returned
zero. Both now fail closed, with explicit offline skip flags retained only for
intentional operator use and successful corpus rows retained solely for
diagnosis. Focused positive, negative, and boundary tests pin those outcomes.

## Limits

Git cannot move another clone's ignored local data. The repository can make a
stale path visible and reject new tracked references, but operators remain
responsible for relocating private datasets once and verifying their own
archives before pruning them.
