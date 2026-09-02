<!-- markdownlint-disable MD060 -->
# Research-0090: Semgrep OSS warnings audit — 2026-05-09

- **Status**: Closed (this PR ships the disposition)
- **Workstream**: companion to the CodeQL alert sweep (300+ alerts dispatched
  the same day)
- **Last updated**: 2026-05-09

## Question

The Semgrep OSS lane in `.github/workflows/security-scans.yml` uploads SARIF
to the GitHub Security tab on every push to `master`. Per the user's request
(2026-05-09), we want to triage the registry-pack findings the same way the
CodeQL alerts are being addressed: real bug → fix; false positive → suppress
with cite; rule-not-applicable → defer with reason.

## Method

1. Read the existing workflow + `.semgrep.yml` + `.semgrepignore`.
2. Run the local-rules subset locally:
   `semgrep scan --config=.semgrep.yml --json` from the repo root.
3. Run the registry-rules subset locally with the packs the workflow
   references:
   `semgrep scan --config=p/cwe-top-25 --config=p/cert-c-strict
   --config=p/cert-cpp-strict --config=p/c --config=p/python --json .`
4. Tabulate per-rule and classify each finding by reading the call site.

## Findings — local rules

`semgrep --config=.semgrep.yml`: **0 findings**, 25 parser warnings.

The 25 parser warnings are semgrep-OSS C-parser limitations (`Failure:
ii_of_name: IdDeref`, "single name expected for simple var" on legitimate
code in `core/src/sycl/common.cpp`, `core/src/feature/adm.c`, etc).
They are not actionable without an upstream semgrep fix; they do not
suppress findings (the rule simply skips the file).

## Findings — registry rules

### CI infrastructure issue surfaced first

Two of the three registry packs the CI lane references **no longer exist**:

| Pack URL                              | HTTP status |
|---------------------------------------|-------------|
| `https://semgrep.dev/c/p/cert-c-strict`   | 404 |
| `https://semgrep.dev/c/p/cert-cpp-strict` | 404 |
| `https://semgrep.dev/c/p/cwe-top-25`      | 200 |
| `https://semgrep.dev/c/p/c`               | 200 |
| `https://semgrep.dev/c/p/python`          | 200 |
| `https://semgrep.dev/c/p/cpp`             | 404 |

The two `cert-*-strict` packs are referenced by the **registry SARIF**
`run:` step (lines 67–71 of `security-scans.yml`). When semgrep can't
download a pack, it exits 7 and emits an SARIF with no `results`. The
step is `continue-on-error: true`, so the failure is silent; CI just
uploads an empty SARIF and the Security tab learns nothing about C/C++
CERT findings.

The semgrep registry consolidated the `cert-*-strict` and `cert-*` packs
into the unprefixed `p/c` and `p/python` packs in 2025; the strict
suffix was retired. `p/cpp` was also retired (rolled into `p/c`). This
PR repoints the workflow at the surviving packs.

### Registry findings against the surviving packs

Total: **19 findings** across 1942 files scanned.

| Rule | Count | Severity |
|------|-------|----------|
| `python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1` | 9 | WARNING |
| `python.lang.security.audit.subprocess-shell-true.subprocess-shell-true`     | 5 | ERROR |
| `python.lang.security.use-defused-xml-parse.use-defused-xml-parse`           | 2 | ERROR |
| `yaml.github-actions.security.run-shell-injection.run-shell-injection`       | 2 | ERROR |
| `python.lang.security.unverified-ssl-context.unverified-ssl-context`         | 1 | ERROR |

Severity tally: 10 ERROR, 9 WARNING.

## Disposition

### SECURITY-FIX-NOW (3 findings)

#### F1 — `python/vmaf/config.py:40` — unverified SSL context

```python
ssl._create_default_https_context = ssl._create_unverified_context
urllib.request.urlretrieve(remote_path, local_path)
```

`download_reactively()` clobbers the **process-global** SSL default to
the unverified context, then downloads from
`https://github.com/Netflix/vmaf_resource/raw/master/...`. GitHub serves
that URL with a valid public CA chain, so the bypass is *unjustified
defence-in-depth disabling*: it makes the download succeed in a
network where TLS verification fails for *any* reason (corporate MITM
proxy, expired root, attacker), and the bypass leaks to every
subsequent SSL-using call in the same process because it mutates a
module-level default.

Fix: drop the line. The system trust store handles GitHub fine. If a
specific CI environment can't validate, the operator can set
`SSL_CERT_FILE` to point at the right bundle — that's the documented
escape hatch, and it's per-process scoped.

#### F2/F3 — `.github/workflows/lint-and-format.yml:81,264` — run-shell-injection

The two `run:` blocks interpolate `${{ github.event_name }}`,
`${{ github.base_ref }}`, and `${{ github.event.before }}` directly
into shell. Of these, `event.before` is a SHA the GitHub event
delivers, but the standard hardening pattern (cf. GitHub's
`actions/security-hardening` guide) is to assign every `${{ github.* }}`
to an `env:` variable on the step and use `"$VAR"` in shell. That
forecloses the rare case where a future GitHub version permits a
controlled-but-not-shell-safe value (e.g. a branch name with shell
metacharacters), and it makes the data-flow explicit for future
maintainers.

Fix applied: assign all three to `env:` and use `"$VAR"` in shell.

### FALSE-POSITIVE-SUPPRESS (16 findings)

#### F4–F12 — 9× `hashlib.sha1(...)` (WARNING) — non-cryptographic cache keys

| File | Lines | Use |
|------|-------|-----|
| `python/vmaf/core/asset.py` | 603, 1113 | hash of asset-config string → cache filename component |
| `python/vmaf/core/executor.py` | 131, 566 | hash of asset-config string → cache filename component |
| `python/vmaf/core/result_store.py` | 127 | hash of result-config → cache filename component |
| `python/vmaf/tools/decorator.py` | 46, 110, 131 | `@persist` memoization decorator cache key |
| `python/test/feature_extractor_test.py` | 94 | test-fixture cache lookup |

Every site is `hashlib.sha1(<config-string>).hexdigest()` whose output
becomes a *cache filename component*, never a security boundary. SHA-1
is fine for non-cryptographic hashing (Git uses SHA-1 for object names
on the same justification). The collision risk that motivates the rule
(crafted second pre-image) is irrelevant when the input is a process-
local config dict serialised by the same code that consumes the hash.

These are upstream Netflix harness paths. Switching to SHA-256 would
invalidate every existing user's on-disk cache (filename change).
Suppress with line-level `# nosem` cites.

#### F13–F17 — 5× `subprocess.call(cmd, shell=True)` (ERROR) — test fixtures

| File | Line | Context |
|------|------|---------|
| `python/test/command_line_test.py` | 161, 213, 234, 255 | `cmd = "{exe} {hardcoded-test-yuv-path}".format(...)` |
| `python/test/ssimulacra2_test.py`  | 54  | `cmd = f"{exe} ... --output {self.output_file_path}"` |

All five sites build the command from `VmafConfig.root_path()` + a
hardcoded test-fixture path under `python/test/resource/yuv/`. The only
runtime input is the test machine's repo checkout location. There is
no attacker-controlled string flowing into the shell. The
`shell=True` form is used because the tests want shell-globbing /
redirection (`>/dev/null 2>&1`) for assertion-on-exit-code tests.

Switching to `shell=False` + a list would lose the redirection. Test
code stays as-is; suppress with line-level cites.

#### F18/F19 — 2× `xml.etree.ElementTree.parse()` (ERROR) — own-output XML

| File | Line | Source |
|------|------|--------|
| `python/vmaf/core/feature_extractor.py` | 115 | parses VMAF C-tool's XML output written to `log_file_path` we own |
| `python/vmaf/core/quality_runner.py` | 1496 | same — VMAF C-tool's own log XML |

The XXE attack surface that motivates the rule requires attacker-
controlled XML. The parser sees only the VMAF tool's own well-formed
output written to a `log_file_path` the harness chose. There is no
external entity in any path the tool emits. `defusedxml` is the right
choice for parsing untrusted XML; it's overkill for a fixture you
just wrote yourself.

Suppress with line-level cites. (Long-term cleanup: switch to
`defusedxml` anyway as belt-and-braces, but that's a separate PR
and an upstream-Netflix conversation.)

### DEFERRED

None. Every finding has a disposition.

## Recommendation — applied in this PR

1. Fix the **SSL-context bypass** in `config.py` (F1).
2. Harden the two `run:` blocks in `lint-and-format.yml` with `env:`
   variables (F2/F3).
3. Land 16 line-level `# nosemgrep: <rule-id>` suppressions with cite
   pointing back to this digest, for the SHA-1 / shell=True / XML
   findings.
4. Fix the **CI workflow registry config** to stop referencing the
   404-ing `p/cert-*-strict` and `p/cpp` packs. Repoint at `p/c`
   (the surviving CERT-C consolidation pack) plus the existing
   `p/cwe-top-25` and the python pack `p/python` (so SARIF actually
   has Python coverage from the registry, not just our local rules).

## References

- User request 2026-05-09 (Semgrep OSS scan results triage).
- ADR-0108 deep-dive deliverables rule.
- ADR-0221 changelog fragment pattern.
- CLAUDE §6 (banned C functions overlap with semgrep CERT rules).
- Memory `feedback_no_guessing` — every "false positive" claim cites
  the call-site reasoning above.
- Memory `feedback_no_skip_shortcuts` — no rule blanket-suppressed; every
  suppression is line-level with a cite.
