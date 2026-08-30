# Dependency audit 2026-05-30

**Status**: Complete.
**Scope**: Routine cross-ecosystem vulnerability sweep (Python pip-audit,
Go govulncheck, container Trivy probe). No new architectural decisions.
**Outcome**: 7 Go advisories cleared via a transitive-bump of
`golang.org/x/net` `v0.53.0 → v0.55.0` and `golang.org/x/sys`
`v0.43.0 → v0.45.0`. All Python ecosystems clean.

## 1. Scope

Audit ecosystems and the manifests scanned:

| Ecosystem | Tool | Manifests |
| ----------- | --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Python | `pip-audit 2.10.0` | `docs/requirements.txt`, `python/requirements.txt`, `python/test/requirements.txt`, `tools/ensemble-training-kit/requirements-frozen.txt`, plus `pyproject.toml` for `ai/`, `mcp-server/vmaf-mcp/`, `dev-llm/`, `tools/vmaf-tune/`, `tools/vmaf-roi-score/`, `tools/ensemble-training-kit/`. |
| Go | `govulncheck` | Module-level scan against `./...` (one `go.mod` at repo root, Go 1.25). |
| Node | n/a | No `package.json` / `package-lock.json` in tree. |
| Container | `trivy image` | `vmafx-dev-mcp:latest` not present locally; deferred to CI image-scan job. |
| Rust | (excluded) | Out of scope for this audit — PR #323 (Cargo.lock regen) covers it. |

## 2. Findings

### 2.1 Python — clean

`pip-audit` returned "No known vulnerabilities found" for every
manifest scanned. This includes both the pinned development
requirements (`docs/`, `python/`, `python/test/`) and the project
manifests under `ai/`, `mcp-server/vmaf-mcp/`, `dev-llm/`,
`tools/vmaf-tune/`, `tools/vmaf-roi-score/`,
`tools/ensemble-training-kit/`.

### 2.2 Go — 7 advisories, 1 reachable

`govulncheck ./...` against the pre-fix workspace surfaced one
symbol-reachable advisory and six module-level findings:

| ID | Module | Symbol path | Severity / class | Reachable on the fork? |
| --------------- | ------------------------------- | ----------------------------------- | --------------------------- | ------------------------ |
| GO-2026-5026 | `golang.org/x/net@v0.53.0` | `idna.ToASCII` | Punycode label validation | **Yes** — reached from `cmd/vmafx-operator/internal/controller/vmafxnode_controller.go:111` `controller.VmafxNodeReconciler.probeHealthz` → `http.Client.Do` → `idna.ToASCII`. |
| GO-2026-5025 | `golang.org/x/net/html` | namespaced foreign-content parser | HTML parser correctness | Module-only — no call site reached. |
| GO-2026-5027 | `golang.org/x/net/html` | foreign-content elements | HTML parser correctness | Module-only. |
| GO-2026-5028 | `golang.org/x/net/html` | arbitrary HTML DoS | DoS | Module-only. |
| GO-2026-5029 | `golang.org/x/net/html` | DOCTYPE character references | HTML parser correctness | Module-only. |
| GO-2026-5030 | `golang.org/x/net/html` | duplicate attributes | XSS | Module-only. |
| GO-2026-5024 | `golang.org/x/sys/windows` | `NewNTUnicodeString` | Integer overflow (Windows) | Module-only; Windows-only platform. |

**Reachable advisory triage.** The `idna.ToASCII` hit fires on the
operator's healthz probe path, which constructs an HTTP request to a
runtime-determined node address. Maliciously-encoded Punycode in the
node's hostname could bypass IDNA's ASCII-only check, but the node-
address surface in the operator is supplied by Kubernetes
`status.addresses` (cluster-trusted) and not by untrusted external
input. Real exploitation requires both (a) write access to the
`VmafxNode` CR's status block and (b) a downstream consumer of the
operator's outbound HTTP client that misuses the encoded label.
Severity in our context is **low**, but the bump cost is also low.

**Module-level triage.** The six remaining findings live in
`x/net/html` and `x/sys/windows`. govulncheck's reachability analysis
shows zero call sites today. They're pulled in transitively by
`k8s.io/apimachinery` and `sigs.k8s.io/controller-runtime`. We bump
them anyway because (a) one minor bump clears the entire batch and
(b) keeping the dependency graph current is the cheapest way to
prevent a future refactor accidentally lighting up a known-bad
symbol.

### 2.3 Container — deferred

`docker images` shows `vmaf-dev-mcp:cuda13.3` and
`vmaf-dev-mcp:local` locally, but no `vmafx-dev-mcp:latest`. Per the
task brief, container scanning is skipped when the image is absent.
The CI image-scan workflow already runs Trivy against pushed images.

## 3. Fix

```bash
go get golang.org/x/net@v0.55.0 golang.org/x/sys@v0.45.0
go mod tidy
```

Resulting `go.mod` deltas:

```diff
-       golang.org/x/net v0.53.0 // indirect
+       golang.org/x/net v0.55.0 // indirect
-       golang.org/x/sys v0.43.0 // indirect
-       golang.org/x/term v0.42.0 // indirect
-       golang.org/x/text v0.36.0 // indirect
+       golang.org/x/sys v0.45.0 // indirect
+       golang.org/x/term v0.43.0 // indirect
+       golang.org/x/text v0.37.0 // indirect
```

`x/net@v0.55.0` requires `x/sys@>=v0.45.0`; the implied `x/term` and
`x/text` upgrades follow from minimum-version selection and pass
`go build ./...` and `govulncheck ./...` with no further findings.

## 4. Verification

```text
$ govulncheck ./...
No vulnerabilities found.

$ go build ./...
(clean exit)
```

Smoke-test command — see PR body Reproducer.

## 5. Alternatives considered

Only-one-way fix. The advisories cite a fixed version; the project
already tracks the `golang.org/x/*` family pinned via go.mod; the
minimum-version bump is the standard remediation. We considered
pinning just `x/net` and leaving `x/sys` at `v0.43.0`, but `x/net`'s
own go.mod forces `x/sys@>=v0.45.0`, so the bump is a single
coherent set. No ADR needed: the decision is mechanical, the
diff is two version strings.

## 6. Rebase impact

None — the entire change is in `go.mod` / `go.sum`. No upstream
Netflix/vmaf code touched. The Go workspace is a fork-only addition;
there is no upstream baseline to rebase against.
