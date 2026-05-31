<!-- markdownlint-disable MD060 -->
# ADR-0211: Tiny-model registry schema + Sigstore `--tiny-model-verify`

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, security, supply-chain, fork-local, t6-9

## Context

The fork's tiny-AI surface ships ONNX models in-tree under `model/tiny/`,
indexed by `model/tiny/registry.json`. ADR-0173 / ADR-0174 (PTQ audit)
added `quant_mode` and `int8_sha256` to the registry; ADR-0010 set the
project-wide policy of keyless Sigstore signing for releases. The two
strands had not yet met: the registry carried sha256 pins but no link to
its Sigstore bundles, and `docs/ai/security.md` advertised a future
`--tiny-model-verify` flag that did not exist.

T6-9 lands both halves in one PR — formalising the registry schema with
license + Sigstore-bundle metadata and wiring `--tiny-model-verify`
through to `cosign verify-blob`. Landing them together avoids a
registry-schema flip-flop where a follow-up PR would otherwise need to
extend the schema again to accommodate the verify flow.

## Decision

We adopt **JSON Schema (Draft 2020-12)** as the formal registry contract,
extend the per-entry shape with `license`, `license_url`, and
`sigstore_bundle`, and wire a new `--tiny-model-verify` CLI flag that
calls `cosign verify-blob` via `posix_spawnp(3p)`. The verifier
short-circuits model load on any failure (missing registry entry,
missing bundle file, missing `cosign`, non-zero exit). Bundle files are
declared in-tree per the registry but populated at release time by the
existing supply-chain workflow; absence of a real bundle pre-release is
benign because the flag is opt-in. `schema_version` bumps to `1`; the
loader accepts both `0` and `1` so the change is backward-compatible
with already-shipped registries.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| In-tree custom validator (no JSON Schema) | Zero deps, smallest patch | Re-invents what JSON Schema already covers; no IDE / editor integration | Rejected — JSON Schema is the industry standard and `jsonschema` is a one-line `pip install` |
| JSON Schema (chosen) | Tooling, IDE completion, Draft 2020-12 widely supported | One optional Python dep | Selected — falls back to a structural check when `jsonschema` isn't installed so CI on minimal images still gets coverage |
| External schema-spec language (e.g. CUE, Pkl) | More expressive | Adds a non-Python toolchain dep; overkill for ~6-field shape | Rejected |
| `cosign` as a runtime dep (chosen) | Reuses the Sigstore-canonical CLI; no in-tree Rekor / Fulcio code to maintain | Operator must install cosign | Selected — the binary is widely distributed; a missing cosign is detected and reported with `-EACCES` |
| Vendored cosign code (libcosign / Go-link) | No external dep | Pulls Go toolchain into the C build; large binary growth | Rejected |
| Build-time signing only (no runtime verify) | Simplest | Defeats the threat model — model substitution at deploy time goes undetected | Rejected |
| SHA-256 (chosen) | Already in use throughout the registry | — | Selected — switching to SHA-512 would add no real bits given the supply-chain bundle is the trust anchor |
| SHA-512 | Slightly larger digest | Cost without benefit; mismatches the existing `int8_sha256` convention | Rejected |
| In-process verification (link cosign-go via cgo) | Skips fork/exec | Embeds Go runtime in libvmaf; cross-platform compat nightmare | Rejected |
| Shell out via `system(3)` | Simplest spawn API | Banned by CLAUDE.md §6 (rule 30 on `principles.md`) — shell-injection risk | Rejected — using `posix_spawnp(3p)` with an explicit argv array dodges shell parsing entirely |

## Consequences

- **Positive**: the registry now carries every field needed for an
  end-to-end supply-chain check (license metadata, sha256 pin, Sigstore
  bundle path) and `--tiny-model-verify` is a real one-line CLI gate
  for production deployments.
- **Positive**: the JSON Schema is editor-discoverable via
  `"$schema": "./registry.schema.json"`, so adding a new model with a
  malformed `kind` enum is caught at lint time.
- **Negative**: `cosign` becomes a soft runtime dep for any caller that
  passes `--tiny-model-verify`. Documented in `docs/ai/security.md` and
  `docs/ai/model-registry.md`.
- **Negative**: a `schema_version: 1` registry parsed by an older
  loader that rejects unknown fields would break — mitigated by the
  bounded enum (`{0, 1}`) and the "additionalProperties: false" set:
  the loader drops new fields silently and the JSON Schema warns.
- **Neutral / follow-ups**: the supply-chain workflow
  (`.github/workflows/supply-chain.yml`) needs a follow-up PR to drop
  generated `<onnx>.sigstore.json` bundles next to the artifacts at
  release time; the verifier already finds them via the registry path.
  The MCP server (T6-12 onward) inherits the registry surface and can
  expose a `verify_model` JSON-RPC method on top of
  `vmaf_dnn_verify_signature()`.

## References

- Source: backlog item T6-9 (`.workingdir2/BACKLOG.md`).
- Related ADRs: [ADR-0010](0010-sigstore-keyless-signing.md) (Sigstore
  policy), [ADR-0042](0042-tinyai-docs-required-per-pr.md) (tiny-AI
  doc requirement), [ADR-0166](0166-mcp-server-release-channel.md)
  (cosign verify usage in the MCP release channel),
  [ADR-0173](0173-ptq-audit-quant-modes.md) /
  [ADR-0174](0174-ptq-audit-int8-sha256.md) (PTQ-era registry fields).
- Roadmap reference: `docs/ai/roadmap.md` §"Sigstore verification".
- Supply chain doc: `docs/ai/security.md` Layer 4.
