# Research-1311: Source ADR citation provenance audit

<!-- markdownlint-disable MD013 MD060 -->

## Question

Which plain `ADR-NNNN` references in implementation and build/control files
still name a missing or unrelated decision, what does Git history prove each
one meant, and what deterministic gate can prevent the same drift without
treating ordinary Markdown prose as source?

This closes `T-STALE-ADR-CITATIONS-2026-09-16`. It does not alter numerical
algorithms or outputs, Netflix golden assertions, benchmark baselines, or
training inputs.

## Corpus and method

The audit starts from Git's tracked-file list, then selects implementation and
build/control paths through the suffix/basename allowlist in
`scripts/ci/check-source-adr-citations.py`. It intentionally excludes Markdown,
changelog prose, patches, binary/model data, and `mkdocs.yml` (a prose navigation
index containing almost every ADR number). The selected corpus includes C,
C++, CUDA, HIP, SYCL, Metal, Python, Go, Rust, shell, Meson, Make, Dockerfiles,
and CI/control YAML.

For every token the audit performed these checks:

1. resolve `docs/adr/NNNN-*.md` and require one exact filename;
2. compare the source context with the candidate ADR's title and decision;
3. for every missing or suspect number, inspect the citing line's `git blame`,
   the introducing commit, the candidate ADR's creation/rename history, and
   later supersession records;
4. separate real historical names from synthetic test data;
5. record the final live filename and exact `path -> occurrence count` in the
   committed registry.

After correction, the gate reports 652 live identities, four retired identities,
four fixture identities, and 6,909 governed occurrences across 2,690 selected
tracked files. The larger count than the 2026-09-16 audit comes from deliberately
including build and CI control files as well as language source files.

## Proven corrections

| Stale number | Correct identity | Evidence |
|---|---|---|
| ADR-0049 | [ADR-0415](../adr/0415-cambi-sycl-port.md) | The comment is in the CAMBI SYCL parity test and quotes the bit-exact contract. `e76b7823a` created the decision as ADR-0371 with the CAMBI SYCL port; collision sweep `fb14bc332` renamed that exact file to ADR-0415. ADR-0415 lines 56–57 state places=4, integer-only, bit-exact CPU parity. |
| ADR-0322 | [ADR-0326](../adr/0326-vmaf-tune-phase-b-bisect.md) | `17a97531b` introduced both the `compare.py` sentence and `0326-vmaf-tune-phase-b-bisect.md`. The ADR explicitly owns the target-VMAF predicate and the geometry-bound closure described by the source comment. |
| ADR-0572 | [ADR-0574](../adr/0574-hdr-features-cuda-twins-phase-1.md) | `2a63c41ad` introduced the CUDA `float_adm_score.cu` accumulator comment and ADR-0574 in one commit. ADR-0574 defines `aim_cm` in slots 6–8 with `noise_weight = 0`, exactly the comment's layout. |
| ADR-0715 | [ADR-0714](../adr/0714-vmafx-operator-skeleton.md) | `802ea1067` created ADR-0714 with the operator API, CRDs, stub reconcilers, Helm integration, and tests. `267531cf25` later added the operator Dockerfile and miscopied the skeleton number as 0715. |
| ADR-0814 | [ADR-0786](../adr/0786-vmafx-operator-stage2-reconcilers.md) | `eae7ccbd4` created ADR-0786 for the Stage-2 reconciler loops, webhooks, and per-controller RBAC. The Dockerfile's “operator reconciler scope” label matches that title and body; no ADR-0814 exists. |

Three earlier corrections from PR #1425 remain valid and are now locked by the
same registry: ADR-0900 to ADR-0913 (changelog splice contract), ADR-0553 to
ADR-0564 (real integer-SSIM GPU kernels), and ADR-0204 to ADR-0206
(SSIMULACRA2 CUDA/SYCL twins).

ADR-1214 is no longer missing. `8d0cdd7c4` added
`1214-float-adm-csf-scale-watson-mode-and-aliases.md` and its regressions; the
existing source citations now resolve to that exact file and need no rewrite.

## Governed historical identities

| Number | Disposition | Durable evidence |
|---|---|---|
| ADR-0557 | Abandoned CUDA-only SpEED plan | ADR-0559 and its research digest record the parallel-agent claim. `5b75ddb34` later delivered the accepted all-backend implementation under [ADR-0567](../adr/0567-speed-chroma-temporal-real-gpu.md); ADR-0964/0965 record the later wiring/repair. The AI comments were also corrected because “CPU-only until GPU twins land” had become false. |
| ADR-0558 | Abandoned HIP-only SpEED plan | Same history as ADR-0557: the split plan never produced an ADR, and ADR-0567 replaced it with the combined implementation. |
| ADR-0722 | Superseded C++11 logging attempt | [ADR-0725](../adr/0725-cpp23-pilot-log-v2.md) names ADR-0722 in its title, context, decision, alternatives, consequences, and references. The two source comments intentionally explain what the C++23 implementation supersedes; repointing or deleting them would erase that comparison. |
| ADR-0864 | Unfiled historical Markdown-lint cleanup | [ADR-0866](../adr/0866-wire-markdownlint-into-lint-pipeline.md) distinguishes ADR-0864's config/cleanup work from the later gate wiring, while [ADR-0980](../adr/0980-markdown-lint-full-ruleset-discharge.md) preserves its autofix-damage evidence and partially supersedes only its gate-narrowing disposition. Repointing to either live ADR would collapse decisions the live records deliberately distinguish. |

The registry reserves these four numbers. A new `docs/adr/NNNN-*.md` allocation
for any of them fails, as does a source occurrence not present in the governed
site map. ADR-0557/0558 remain reserved even though the false current-source
comments were removed.

## Fixture boundary

ADR-0099 and ADR-9997/9998/9999 occur only as synthetic inputs to checker
tests. They are not project decisions. Each number is allowed only at exact
fixture paths and counts; an occurrence elsewhere fails. This keeps tests
expressive without teaching the gate that an impossible number is globally
valid.

## Gate design and limitations

The registry stores:

- a live number's exact ADR filename and exact source-site counts;
- a retired identity's status, reason, repository evidence, full Git commit
  IDs, successor/related ADRs, and exact source sites;
- a fixture identity's reason and exact source sites.

The checker fails on a missing number, filename/slug reallocation, duplicate ADR
number, added/removed citation occurrence, missing retirement evidence,
unresolvable history commit, reused retired number, or escaped fixture. Git
enumeration and history lookup failures are fatal. `--write` regenerates only
live bindings and refuses to invent a retirement or fixture exception.

The fixture suite also covers its own Git boundary. During the first signed
commit attempt, fixture `git add`/`git commit` inherited the hook's alternate
`GIT_INDEX_FILE` and replaced the caller index with the fixture's four paths.
The regression poisons `GIT_INDEX_FILE`, `GIT_DIR`, `GIT_WORK_TREE`, and
`GIT_PREFIX`, then proves the caller index and repository configuration are
byte-identical while both fixture Git and checker subprocesses operate on the
disposable repository. The config assertion covers the same leak's corrupted
`user.name` / `user.email`, which the final signed-commit check exposed. All
fixture Git calls now discard inherited `GIT_*`, disable system/global
configuration, hooks, and commit signing.

Changing the CUDA float-ADM citation made the whole touched file subject to the
60-line function cap. The two existing stage-3 kernels were split into forced-
inline device helpers for decoupling, thresholding, and reduction. The helper
boundaries retain the original arithmetic and reduction order; CUDA parity is
the executable guard against numerical drift. An exact-worktree all-backend
container build compiled the changed CUDA translation unit and its focused test;
the resulting library then passed all four `test_cuda_float_adm_parity` cases on
the local RTX 4090, including non-default `adm_p_norm`, `adm_bypass_cm`, and
`adm_csf_scale` coverage.

The gate does not claim to understand prose semantics. It cannot decide whether
a sentence accurately summarises an ADR; that remains a review responsibility.
It does guarantee that the decision identity reviewed here cannot silently
change underneath the same four digits.

## Reproduction

```bash
python3 scripts/ci/tests/test_check_source_adr_citations.py
python3 scripts/ci/check-source-adr-citations.py
python3 scripts/ci/check-adr-links.py --docs docs
```

To intentionally add or remove a live citation, audit the source context first,
then regenerate and review the registry diff:

```bash
python3 scripts/ci/check-source-adr-citations.py --write
python3 scripts/ci/check-source-adr-citations.py
```

Retirements and fixture exceptions are always hand-authored; `--write` refuses
to create or broaden them.
