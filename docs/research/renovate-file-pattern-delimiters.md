# Research: Renovate custom manager file-pattern delimiters

## Scope and reproduction

The `build/base-image-single-source` branch at `51fb602d39fe60a6c8a72dfcb58e8036303e486c`
contains 43 `managerFilePatterns` across nine custom managers. Every pattern
has doubled slash delimiters, such as `//^build-config\.env$//` after JSON
decoding. This remains valid regex syntax but selects no repository-relative
path. The base-image manager therefore loses discovery of its authority file
and eight mirrors while the built-in Docker manager is disabled for those
mirrors.

Verified on 2026-09-08 against the installed **Renovate 44.56.3** package:

- `dist/util/string-match.js`, SHA-256
  `ffa8dcc2d16588a3bf73b9de39f3a5051439e158f889103a803997296d138767`.
- Its source map, `dist/util/string-match.js.map`, SHA-256
  `338ac2ad314d22d0f5cfa1584a619379b6226ecde6b4dfbb4c529b9f9f8a2a24`.
- `parseRegexMatch` in the installed source map (lines 82–87) removes exactly
  one delimiter from each end. The pinned
  [upstream source](https://github.com/renovatebot/renovate/blob/44.56.3/lib/util/string-match.ts)
  and [documented syntax](https://docs.renovatebot.com/string-pattern-matching/#regex-matching)
  agree with this behavior.

The installed `matchRegexOrGlob` function selected **0/43** patterns before
the correction and **43/43** afterward. The installed regex manager's
`extractPackageFile` then extracted all **13** central image pins from
`build-config.env` and **29** corresponding declarations across the eight
Dockerfile mirrors. No registry lookup or dependency PR was needed for these
file-selection and extraction checks.

## Validation and limits

The installed `renovate-config-validator --no-global --strict renovate.json`
exited zero both before and after the correction. Schema validation cannot
prove that a valid regex actually selects a file.

The installed package ran under Node.js 26.8.1. Its optional native RE2 binding
was absent, so Renovate used its JavaScript RegExp fallback and the validator
reported that limitation. The corrected file patterns use only anchors,
literal characters and escaped dots, shared by the Python fixture engine,
JavaScript RegExp and RE2; this run does not claim native-RE2 or hosted-App
execution. The source hashes above bind the actual local code inspected.

The permanent, network-free regression command is:

```bash
python3 -m unittest discover -s scripts/ci/tests -p test_renovate_file_patterns.py -v
```

Its fixtures require a positive tracked-file match for each configured
pattern, reject archive/backup lookalikes, and pair the base manager's exact
config-plus-mirror set with the built-in Docker exclusions. The original
configuration produced 44 fixture failures; the corrected configuration
passes. The pre-commit hook runs the suite in local and CI all-files checks.

## Resolution

Remove one slash at each end of every affected custom-manager file pattern.
This implements the existing Renovate format and ADR-1231 ownership contract;
it introduces no new dependency, version policy or update schedule. Removing
an obsolete manager is independent of this correction and should preserve
coverage for the remaining configured managers.
