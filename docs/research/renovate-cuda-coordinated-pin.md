<!-- markdownlint-disable MD013 -->
# Research: what Renovate can and cannot rewrite for the CUDA pin

**Date**: 2026-09-21
**Renovate read**: 44.103.6, from the installed package under
`~/.npm/_npx/*/node_modules/renovate/dist/` (the newest of the fifteen copies
the repository's `npx` runs have left on this host)
**Question**: can a Renovate configuration alone keep all sixteen CUDA release
literals in lockstep, so a bump is proposed whole or not at all?
**Answer**: for ten of them, yes. For the other five, no — not without behaviour
that cannot be verified without running Renovate against a live registry.
Supporting ADR: [ADR-1285](../adr/1285-cuda-coordinated-pin-lockstep.md).

## The inventory, measured rather than assumed

The bug report listed seven sites. `scripts/ci/check-cuda-pin-lockstep.py` run
against the tree finds sixteen:

| Kind | Count | Where |
| --- | --- | --- |
| `config` | 1 | `build-config.env:86` |
| `image` | 6 | `build-config.env:122,123`; `Dockerfile:12`; `docker/Dockerfile.node:50`; `docker/Dockerfile.production-gpu:31,32` |
| `action` | 2 | `build.yml:267`, `libvmaf-build-matrix.yml:419` |
| `installer` | 2 | `build.yml:338`, `libvmaf-build-matrix.yml:905` |
| `series` | 2 | `build.yml:339`, `libvmaf-build-matrix.yml:906` |
| `apt` | 2 | `build-config.env:87`, `dev/Containerfile:291` |
| `label` | 1 | `docker/Dockerfile.production-gpu:296` |

Three of these were not in the report. The two `$cudaMajorMinor = '13.3'`
literals sit one line below the `$cudaVersion` literals that *were* reported,
and the OCI description label — `"VMAFX production CUDA 13.3.1 runtime"`, a
string published on `ghcr.io` — was in no inventory at all. The gate's residual
sweep found it on its first run, before any of the checks below existed. That is
the argument for the sweep: an inventory written by hand is the thing that goes
stale.

## What the datasource does with `extractVersion`

`nvidia/cuda` publishes no bare `13.3.1` tag; every tag carries a flavour and a
distro (`13.3.1-devel-ubuntu26.04`). A custom manager whose `currentValue` is
`13.3.1` therefore looks up nothing useful unless the tag list is normalised
first.

`dist/modules/datasource/common.js`:

```js
function applyExtractVersion(releaseResult, extractVersion) {
    if (!extractVersion) return releaseResult;
    const extractVersionRegEx = regEx(extractVersion);
    releaseResult.releases = filterMap(releaseResult.releases, (release) => {
        const version = extractVersionRegEx.exec(release.version)?.groups?.version;
        if (!version) return null;
        release.versionOrig = release.version;
        release.version = version;
        return release;
    });
    return releaseResult;
}
```

`dist/modules/datasource/index.js` applies it first in the filter chain, before
`filterValidVersions` runs the configured versioning:

```js
res = applyExtractVersion(res, config.extractVersion);
res = applyVersionCompatibility(res, …);
res = filterValidVersions(res, config);
```

So `extractVersionTemplate: "^(?<version>[0-9]+\\.[0-9]+\\.[0-9]+)-devel-"`
rewrites each release to its bare version and drops the tags that do not match
(`latest`, the `-runtime-` and `-base-` flavours), leaving one canonical stream
that `13.3.1` can be compared against. The regexes go through Renovate's
`regEx()`, which prefers RE2, so the pattern uses only anchors, classes and a
named group — no lookaround, no backreference.

## Why the other five spellings cannot be Renovate's

Renovate's replacement path is `doAutoReplace` in
`dist/workers/repository/update/branch/auto-replace.js`. By default it swaps
`currentValue` for `newValue` inside the matched string — which means the slot
must want the whole looked-up version. `$cudaMajorMinor = '13.3'` wants
`13.4`, `cuda-toolkit-13-3` wants `13-4`, and the label wants a sentence.

`autoReplaceStringTemplate` renders an arbitrary string instead:

```js
if (autoReplaceStringTemplate && !newName) newString = compile(autoReplaceStringTemplate, upgrade, false);
```

and Renovate registers a `replace` helper, so
`{{{replace "\\.\\d+$" "" newValue}}}` would render `13.4` from `13.4.0`. The
write is then re-verified:

```js
newContent = replaceAt(newContent, searchIndex, replaceString, newString);
await writeLocalFile(upgrade.packageFile, newContent);
if (await confirmIfDepUpdated(upgrade, newContent)) return newContent;
await writeLocalFile(upgrade.packageFile, existingContent);   // reverted
```

and `confirmIfDepUpdated` compares the *rendered template* against the
*re-extracted* `currentValue`:

```js
if (upgrade.newValue && upgrade.newValue !== newUpgrade.currentValue) {
    let templateMatchesExtractedValue = false;
    if (upgrade.autoReplaceStringTemplate) try {
        templateMatchesExtractedValue = compile(upgrade.autoReplaceStringTemplate, upgrade, false) === newUpgrade.currentValue;
    } …
    if (!templateMatchesExtractedValue) { … return false; }
```

For `$cudaMajorMinor = '13.4'` the re-extracted `currentValue` is `13.4` while
the compiled template is the whole replacement string, so the comparison fails
and the write is rolled back. Making the two agree requires the match to be
*only* the value — no anchor to the variable name — which RE2 cannot express
without lookbehind, or a `matchStringsStrategy: "recursive"` arrangement whose
`replaceString` semantics are not observable without running Renovate end to
end against a token and a registry.

Conclusion: a configuration that appears to cover these five would either be
silently inert or would revert its own writes, and neither failure is visible
from `renovate-config-validator`. They belong to a gate that can be run and
tested offline.

## What was verified, and what was not

Verified here:

- the site inventory (`check-cuda-pin-lockstep.py` against the tree, 16 sites);
- `renovate.json` validates: `node dist/config-validator.js` →
  "Config validated successfully against 1 file(s)" (Renovate 44.103.6);
- the manager's file patterns select tracked files and every `matchString` hits
  a real line carrying an `x.y.z` value
  (`test_every_matchstring_hits_a_real_line_in_the_tree`);
- the `extractVersion` regex extracts `13.4.0` from `13.4.0-devel-ubuntu26.04`
  and rejects `13.4.0-runtime-ubuntu26.04` and `latest`;
- the gate catches drift in all seven spellings and `--write` repairs exactly
  the five derived ones (12 tests in
  `scripts/ci/tests/test_cuda_pin_single_source.py`).

**Not** verified here, and deliberately stated rather than implied: no Renovate
*run* was performed. Grouping behaviour, the Docker tag list and the update
Renovate would actually propose were not observed — they need a GitHub token and
network access to the registry. The claims above about grouping rest on the
documented meaning of `groupName` plus the config validating, not on a
reproduction. The first real `CUDA release (coordinated pin)` pull request is
the observation that closes that gap.
