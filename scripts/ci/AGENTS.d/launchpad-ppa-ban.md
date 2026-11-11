---
paths:
  - scripts/ci/test_fail_closed_ci.py
  - .pre-commit-config.yaml
  - .github/workflows/lint-and-format.yml
  - .github/workflows/nightly.yml
invariant: ADR-3533 text ban covers tracked YAML, shell, Dockerfile*/Containerfile* sources; comments and data count.
area: gates
---
# Launchpad source text ban (ADR-3533)

`LaunchpadPPAContract` owns source-registration text ban. One case-insensitive
regex: `add-apt-repository`, `apt-add-repository`, `\bppa:`,
`ppa\.launchpad(?:content)?\.net`. One finding per matching physical line:
path, line, matched text, archive-install guidance.

Scope = `git ls-files -z`, repository-wide: `.yml`, `.yaml`, `.sh`, `.bash`
suffixes; basenames starting `Dockerfile` or `Containerfile`. No directory
exemptions. Tracked non-regular paths, symlinks included -> explicit skip
diagnostic, target never read. UTF-8 reads -> replacement decoding. Git
failure -> error, never empty success.
File bound = 1,864; measured 466 tracked source paths on 2026-11-11 x4.

No YAML or shell parsing. Comments, echo output, heredoc data, variable
assignments, nested commands, anchors and merge keys -> text rules only.
Mentions -> ADR-3533 wording without banned text. Empty files and unmatched
quotes without banned text -> allowed.

Keep `fail-closed-ci-contract` hook trigger aligned with source-path scope;
retain test-file and tox triggers. Fixtures: positive, negative, boundary;
tracked temporary repositories, exact file:line diagnostics, failed Git
listing, count overflow, relocated actions, aliases, both source hosts.
Planted tidy registration -> rejection; current tracked tree -> no findings.
Temporary repositories strip every inherited `GIT_*` variable, disable caller
system/global configuration, hooks and signing; poisoned repository, index and
configuration variables regression keeps that isolation. Listing env =
`listing_git_environment(root)`: this checkout -> caller env (hook index);
any other root -> no `GIT_*`, `GIT_CONFIG_NOSYSTEM=1`,
`GIT_CONFIG_GLOBAL=os.devnull`. Malformed caller `~/.gitconfig` and XDG config
regression keeps fixture listing working.
