<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3530: Reject new Bandit findings against the merge base

- **Status**: Accepted
- **Date**: 2026-11-11
- **Deciders**: VMAFx maintainers
- **Tags**: ci, security, agents

## Context

The subprocess changes in PR #3387 introduced three B603 findings in `scripts/ci/tests/test_install_rocm_from_image.py`. Ruff's `# noqa: S603` suppression did not suppress Bandit, so Codacy reported them after the push. The historical reproducer compares `20fa27ac6` with the changed blobs from `a0bc69572`; the corrected blobs from `77ca97921` pass. Local checks need the same engine and pattern policy as Codacy, without making unrelated existing findings block every change. This supports the checked-error and preflight requirements in [principles.md](../principles.md).

Codacy's native configuration option reads `bandit.yml` from the branch under analysis. Comparing both revisions under the current configuration would incorrectly grandfather newly enabled rules. A push can also target content different from the local working tree or HEAD. The shared implementation in `scripts/lib/new_scanner_findings.py` must select the same revisions for Bandit and flawfinder.

## Decision

Run pinned Bandit at pre-commit and pre-push and reject findings absent from the merge-base copy. Read the current branch's `bandit.yml` and the merge-base configuration separately; if the base lacks a configuration, use the current one for that scan. Any configuration byte change expands selection to every tracked Python source and disables the identical-source shortcut. Pre-push reads source and configuration blobs from `PRE_COMMIT_TO_REF` and computes its merge base; pre-commit uses the working tree and HEAD. Resolve the default comparison branch through the `origin/HEAD` symbolic ref, then `origin/master`, with an explicit `--base-branch` override.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Merge-base new-findings gate | No maintained baseline artifact; same scanner and branch policy as Codacy | Two scans per changed source; config changes scan all tracked sources | Chosen: catches new findings before push while retaining existing debt |
| Whole-tree baseline file | Explicit snapshot; easy whole-tree comparison | Separate artifact drifts; baseline refresh can hide additions | Adds another policy state to maintain |
| Codacy only | No local scanner dependency | Findings arrive after push and CI work | Leaves the observed feedback gap |
| Ruff S-rules only | Reuses the existing fast linter | Different rule set and suppression syntax from Bandit | Does not detect the observed B603 escape under `# noqa: S603` |

## Consequences

- **Positive**: The historical three-line B603 escape is caught locally. Shared byte-preserving Git reads cover outgoing blobs, literal paths and both scanners. Python source-line identities use PEP 263 decoding. Python and `pyi` identify tags, including Python shebangs, are accepted.
- **Negative**: Configuration changes require a complete tracked-source scan. The hook runs as one process (`require_serial`), so that scan happens once; only when pre-commit splits an over-long argument list into several command lines (for example `--all-files` on Windows) does each part repeat it, with the same verdict. A scanner error exits 2; it never counts as a clean scan. A low-similarity rename that Git reports as deletion plus addition retains no pairing, so existing findings are reported as new and block the change. This conservative behavior avoids inventing an ambiguous correspondence.
- **Neutral / follow-ups**: Finding identity remains a multiset of rule ID and stripped source line. Moved lines and Git-detected renames preserve findings; this is not a data-flow equivalence proof. Keep the configuration-file option enabled in Codacy and refresh the pattern list from the documented source. Positive, negative and boundary cases live in `scripts/ci/tests/test_check_bandit_new.py` and `test_check_flawfinder_new.py`.

## Supply-chain impact

Bandit 1.9.4 (Apache-2.0, [upstream](https://github.com/PyCQA/bandit)) is a development and test dependency, pinned in the hook and `requirements/locks/tooling-tests.in`. The tooling lock adds Bandit and its four dependencies (`markdown-it-py`, `mdurl`, `rich`, `stevedore`) with distribution hashes and a new input fingerprint; every other pin in it is unchanged. `identify==2.6.20`, already used by pre-commit and the flawfinder tests, supplies the classifier. No runtime library, service or credential is added. PyPI distribution hashes are the installation trust anchor; no new attestation claim is made.

## References

- [Bandit/Codacy research digest](../research/3530-bandit-codacy-pattern-list.md): configuration semantics, published pattern list and measured stub behavior; accessed 2026-11-11.
- [Codacy configuration-file behavior](https://docs.codacy.com/repositories-configure/configuring-code-patterns/): branch-specific native tool configuration; accessed 2026-11-11.
- [Bandit 1.9.4 configuration](https://bandit.readthedocs.io/en/1.9.4/config.html): native `tests` selection and suppression syntax; accessed 2026-11-11.
- Reproducer: base `20fa27ac6`, introduced findings `a0bc69572`, corrected source `77ca97921`; PR #3387.
- Implementation: `scripts/ci/check_bandit_new.py`, `scripts/ci/check_flawfinder_new.py`, `scripts/lib/new_scanner_findings.py`.
