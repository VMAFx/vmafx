- **Semgrep OSS warnings — 19/19 triaged (Research-0090)** — three real
  fixes plus sixteen line-level `# nosemgrep` suppressions, each citing
  the call-site reasoning. Fixed: (1) `python/vmaf/config.py`
  `download_reactively()` no longer clobbers the process-global SSL
  default with `ssl._create_unverified_context` — GitHub's
  `vmaf_resource` URL serves a valid public-CA chain, so the bypass was
  unjustified defence-in-depth disabling that masked legitimate TLS
  failures and leaked to every later SSL call in the same process; (2/3)
  `lint-and-format.yml`'s two `clang-tidy` `run:` blocks now alias
  `${{ github.* }}` interpolations through `env:` per GitHub's hardening
  guide, defusing the `yaml.github-actions.security.run-shell-injection`
  rule. Suppressed (false positives): nine `hashlib.sha1(...)` cache-key
  sites in the upstream Netflix Python harness (memoization /
  filename-shortening, not security; switching to SHA-256 invalidates
  every existing user's on-disk cache), five `subprocess.call(shell=True)`
  test fixtures (hardcoded test-YUV paths, no attacker-controlled
  string, `shell=True` needed for `>/dev/null 2>&1` redirection), and
  two `ElementTree.parse()` of the libvmaf C tool's own log XML (we own
  both the path and the writer, no XXE surface).
- **Security-scans CI workflow — registry-pack list de-rotted** — the
  `p/cert-c-strict` and `p/cert-cpp-strict` packs the workflow had been
  citing were retired by the Semgrep registry in 2025 and now return
  HTTP 404; semgrep was silently exiting 7 and uploading empty SARIF
  for the registry lane. Repointed at the surviving consolidated packs:
  `p/cwe-top-25` + `p/c` (where the strict-CERT rules were rolled into)
  + `p/python` (so the harness paths actually get coverage). The
  `--config=.semgrep.yml` local-rules lane is unchanged and continues to
  gate (`--error`).
