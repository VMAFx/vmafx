<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1089: Block non-standard ONNX operator domains in the DNN wire scanner

- **Status**: Accepted
- **Date**: 2026-06-07
- **Deciders**: Lusoris
- **Tags**: `security`, `ai`, `dnn`, `fork-local`

## Context

The ONNX Runtime (ORT) dispatches operators by the tuple `(domain, op_type)`,
not by `op_type` alone. The standard built-in op set uses domain `""` (empty
string, equivalent to `"ai.onnx"`). Custom or third-party op sets register
under non-empty, non-standard domain strings (e.g. `"com.microsoft"`,
`"com.evil"`).

The fork's wire-format scanner (`core/src/dnn/onnx_scan.c`) checked only
`NodeProto.op_type` (field 4) against the allowlist, but never read
`NodeProto.domain` (field 7). A crafted model could therefore set
`op_type = "Conv"` (matching the allowlist) and `domain = "com.evil"`. The
scanner would accept it, but ORT would dispatch to whatever custom op was
registered under `("com.evil", "Conv")` — executing arbitrary code as the
VMAF process.

This is a defence-in-depth bypass: ORT's own sandboxing is the primary
barrier, but the scanner's purpose is to reject obviously hostile inputs
*before* ORT ever instantiates a session. Allowing non-standard domains
defeats that purpose entirely.

## Decision

The scanner reads `NodeProto.domain` (field 7) at every node level (including
inside control-flow subgraphs via the existing recursive descent). Any domain
string that is neither the empty string `""` nor `"ai.onnx"` causes the
scanner to return `-EPERM`, the same code returned for a disallowed op type.
The `first_bad` out-pointer is populated with the rejected domain string for
caller diagnostics.

Standard ONNX-ML (`"ai.onnx.ml"`) and all vendor/custom domains are blocked.
No exceptions are provided; if a future consumer requires an ONNX-ML op, a
separate ADR must justify it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Allowlist specific non-standard domains (e.g. `"ai.onnx.ml"`) | Supports ONNX-ML ops | Wider attack surface; every added domain requires auditing all its ops | No current consumer needs ONNX-ML; default to narrow |
| Reject domain field presence entirely (require empty/absent domain) | Simplest rule | Rejects explicitly-tagged standard-domain models (`domain="ai.onnx"`) exported by some tools | Too strict; legitimate exporters may emit `"ai.onnx"` explicitly |
| No change — rely on ORT sandboxing | Zero code change | Scanner's stated purpose is pre-ORT rejection; partial allowlist is misleading if bypassable | Violates the stated security invariant |

## Consequences

- **Positive**: The `(domain, op_type)` tuple is now fully gated before ORT
  instantiates a session. A custom op cannot shadow an allowlisted built-in
  by using a non-standard domain.
- **Negative**: Models that explicitly tag ops with any non-empty non-`"ai.onnx"`
  domain are rejected, even if ORT would run them safely. In practice all
  in-tree models use default (empty) domain, so no regression is expected.
- **Neutral / follow-ups**: The `read_domain` helper is ~50 LOC and fully
  covered by 5 new unit tests. The `onnx_scan.h` doc comment is updated to
  document the four-field scope. No public API or CLI surface changes.

## References

- ONNX protobuf spec: `NodeProto.domain` = field 7 (string), standard domain
  is `""` or `"ai.onnx"`, ONNX-ML extension is `"ai.onnx.ml"`.
- ORT dispatch: `IExecutionProvider::GetKernelRegistry()` keyed on
  `(op_type, domain, opset_version)` — see ORT source
  `onnxruntime/core/framework/op_kernel.h`.
- Prior scanner ADRs: ADR-0169 (Loop/If control-flow ops), ADR-0171
  (Loop-count cap), ADR-0258 (Resize admitted).
- Security audit: parallel agent `wf_b08e0c22-717-7`, 2026-06-07.
