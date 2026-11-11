<!-- markdownlint-disable MD013 MD041 MD060 -->
# Research-3530: Bandit configuration and Codacy pattern selection

Checked on 2026-11-11 for Bandit 1.9.4 and identify 2.6.20. The implementation is `scripts/ci/check_bandit_new.py`; the design decision is [ADR-3530](../adr/3530-bandit-new-merge-base-gate.md).

## How Codacy reads the configuration

[Codacy's native tool configuration documentation](https://docs.codacy.com/repositories-configure/configuring-code-patterns/) lists root `bandit.yml` as a supported Bandit configuration. The configuration-file option must first be activated on the tool's Code patterns page after the file is available on the default Codacy branch. Codacy then reads the configuration from the branch being analysed, including pull-request changes. A branch without the file falls back to the patterns previously configured through the UI. The coding standard still controls whether the tool is enabled. Accessed 2026-11-11.

This is distinct from `.codacy.yml`: [Codacy's general configuration documentation](https://docs.codacy.com/repositories-configure/codacy-configuration-file/) describes default-branch precedence for that file. Do not apply those precedence rules to Bandit's native configuration. Accessed 2026-11-11.

The [Codacy Bandit wrapper](https://github.com/codacy/codacy-bandit/blob/master/src/main/scala/codacy/bandit/Bandit.scala), `nativeConfigFileNames` and `runTool`, locates `bandit.yml` and passes it through `-c` when no explicit pattern set is supplied. With explicit enabled patterns, the wrapper filters results instead of passing the native configuration. This supports the documented need to enable native configuration in Codacy; adding a file alone does not prove that the live repository option is enabled. The live UI option was not inspected during this check. Accessed 2026-11-11.

## Source of the 56 IDs

The list in [`bandit.yml`](../../bandit.yml) matches every `patternId` whose `enabled` value is true in the public [`codacy-bandit/docs/patterns.json`](https://raw.githubusercontent.com/codacy/codacy-bandit/master/docs/patterns.json), version 1.9.4. A download on 2026-11-11 produced SHA-256 `1dc1275ac5c2a6aa9690f27b83d6af49bd19a91244d46ea457e6d1ce10d30f89`: 56 enabled IDs, 56 local IDs, no differences. This is the published engine default list, not a separately authenticated export of this repository's UI settings. B101 is disabled there; B603 is enabled. Accessed 2026-11-11.

For a refresh, compare the enabled IDs in that source with the local YAML, review each difference, then regenerate the pinned tooling lock and run `scripts/ci/tests/test_check_bandit_new.py`. Bandit's [1.9.4 configuration documentation](https://bandit.readthedocs.io/en/1.9.4/config.html) specifies that `tests` selects the listed IDs and that `# nosec` controls Bandit suppression; Ruff's `# noqa: S603` does not suppress Bandit. Accessed 2026-11-11.

## Python stubs and file selection

Installed identify 2.6.20 reports `python` for `.pyw` and `pyi` for `.pyi`. It also recognises Python from executable shebangs. The hook therefore delegates source classification to the gate and permits the configuration file to trigger it. The gate accepts both tags and handles the full classifier's Python filename set rather than maintaining another extension list. The cases are reproduced by `test_python_hook_types_and_shebang_are_scanned` and `test_hook_selects_configuration_and_stub_files`.

A measured Bandit 1.9.4 invocation with an explicit `example.pyi` containing a `subprocess.run` call exits 1 with B404 and B603 and no scan errors. A recursive invocation over a directory containing only that stub exits 0 with no source metrics or findings: the recursive extension defaults exclude it. [Bandit's file-discovery implementation](https://github.com/PyCQA/bandit/blob/1.9.4/bandit/core/manager.py), `discover_files`, explicitly scans named files regardless of the recursive inclusion glob. Accessed 2026-11-11.

Codacy's wrapper passes its supplied file list directly to Bandit and excludes only a short list of dependency lockfile names; it has no `.pyi` rejection. Consequently, a `.pyi` supplied to that wrapper reaches Bandit as an explicit file and is scanned. Whether the hosted platform includes a particular stub in its Python file list depends on language detection and repository settings; that live selection was not measured here. The local gate explicitly includes stubs, so it does not depend on that unverified platform setting. Wrapper source accessed 2026-11-11.

## Comparison consequences

The base scan reads the merge-base configuration; absence uses the current configuration. A configuration byte change scans every tracked Python file, including unchanged source, under the two configurations. Otherwise the byte-identical shortcut avoids both scans. Pre-push selects source and configuration from the outgoing ref; local edits do not affect the result. Missing scanners, malformed reports, decoding errors and absent merge bases exit 2.

Finding identity is a multiset of rule ID and stripped source line. Git-detected renames retain the old path; an edited rename below Git's similarity threshold is treated as a new file and can report retained findings as new. `test_low_similarity_rename_fails_closed` records that conservative limitation. The historical escape is reproduced from `20fa27ac6` and `a0bc69572`; Bandit reports the three B603 lines at 242, 249 and 258 of `scripts/ci/tests/test_install_rocm_from_image.py`. Replacing the changed files with `77ca97921` blobs yields exit 0.
