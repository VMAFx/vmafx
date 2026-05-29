# Tiny AI — security model

ONNX models are **data, not code**, but that is not an excuse for a lazy
runtime. libvmaf's DNN layer applies four layers of defence before any
frame touches a graph.

## Threat model

| Class | Example | Mitigation |
| --- | --- | --- |
| Hostile `.onnx` file | Model with custom op that exfiltrates via network syscall | Operator allowlist (next section). |
| Memory-exhaustion via huge model | 10 GB `.onnx` passed to `--tiny-model` | Size cap (`VMAF_DNN_DEFAULT_MAX_BYTES`, compile-time 50 MB). |
| Path-traversal via `--tiny-model` | `--tiny-model ../../etc/shadow` | `vmaf_dnn_validate_onnx` resolves symlinks, requires `S_ISREG` + readable, and can enforce the optional `VMAF_TINY_MODEL_DIR` directory jail. |
| Silent model substitution | Attacker replaces signed model with a poisoned one | Opt-in Sigstore (`cosign`) verification against the workflow identity. |

## Layer 1 — operator allowlist

[`core/src/dnn/op_allowlist.{h,c}`](../../core/src/dnn/op_allowlist.h)
holds a curated set of ONNX operator names. Before creating an ORT session
the loader walks the graph and rejects any node whose op is not on the
list.

The list includes the common building blocks of C1/C2/C3 architectures
(`Conv`, `Gemm`, `Relu`, `BatchNormalization`, `GlobalAveragePool`,
activations, pooling, arithmetic, reshape/transpose) plus the two
control-flow ops `Loop` and `If` (added in
[ADR-0169](../adr/0169-onnx-allowlist-loop-if.md) — required by
MUSIQ / RAFT / small-VLM-class architectures) and the spatial
sampler `Resize` (added in
[ADR-0258](../adr/0258-onnx-allowlist-resize.md) — required by
U-2-Net / mobilesal / BASNet / PiDiNet / FPN-style detectors).
`Scan` remains rejected — its variant-typed input/output binding
makes static bound-checking impractical for a wire-format scanner.
Unknown op names (`custom_op_xyz`) are rejected, as is anything
that could touch the filesystem or network.

`Resize` carries multiple `mode` settings — `nearest`, `linear`,
`cubic`. Per ADR-0258 the wire scanner gates op-type only, not
attributes; consumers shipping their own ONNX are expected to keep
`mode in ("nearest", "linear")` (`cubic` is numerically less stable
on quantised inputs and not exercised by any in-tree consumer).

For `Loop` / `If`, the wire-format scanner in
[`onnx_scan.c`](../../core/src/dnn/onnx_scan.c) recurses into the
embedded subgraphs (`Loop.body`, `If.then_branch`, `If.else_branch`)
and applies the same allowlist check at every depth (capped at 8
levels of nesting as a defence-in-depth bound). A forbidden op cannot
hide inside a control-flow body.

**Bounded-iteration guard** (added in
[ADR-0171](../adr/0171-bounded-loop-trip-count.md)). Two layers
mirror the doc-drift enforcement model:

- **Export-time (`vmaf-train`)**: `vmaf_train.op_allowlist` traces
  every `Loop.M` input back to a `Constant` int64 scalar and
  rejects when the producer is a graph input, a non-Constant op,
  or carries a value outside `[0, MAX_LOOP_TRIP_COUNT]` (default
  1024, per-call overridable for legitimate longer pipelines).
  Recursion descends into nested subgraphs — a `Loop` inside a
  `Loop.body` must be statically bounded in its own scope.
- **Load-time (libvmaf wire scanner)**: a counter threaded through
  the scanner caps the total number of `Loop` nodes per model at
  `VMAF_DNN_MAX_LOOP_NODES = 16` across the top-level graph and
  every embedded subgraph. The cap is coarser than the Python
  data-flow check by design — reproducing producer-map lookup in
  a wire-format scanner would violate the ADR D39
  "bounded-auditable-scope" constraint that keeps the scanner from
  pulling in `libprotobuf-c`.

A model has to clear **both** layers to load. Models that bypass
the trainer (HTTP-fetched, MCP-uploaded, third-party tiny-AI
registries) still hit the load-time cap.

Extending the list is a conscious, reviewed act — changes to
`op_allowlist.c` must be called out in the PR description and backed by a
concrete model that needs the addition.

## Layer 2 — resource bounds

- **Size cap.** Loader refuses files larger than
  `VMAF_DNN_DEFAULT_MAX_BYTES` (50 MB, compile-time constant in
  [`core/src/dnn/model_loader.h`](../../core/src/dnn/model_loader.h)).
  Applies before mapping the file. The historical
  `VMAF_MAX_MODEL_BYTES` env override was retired in T7-12 once two
  release cycles passed without a shipped model approaching the cap;
  callers that genuinely need a larger envelope must bump the
  constant and rebuild.
- **Path validation.** `vmaf_dnn_validate_onnx`:
  - resolves symlinks,
  - asserts `S_ISREG` (no devices, pipes, directories),
  - returns `-errno` on any failure — caller must check.

  `VMAF_TINY_MODEL_DIR` is the optional deployment jail. When set to a
  directory, the loader canonicalises both the jail and the requested
  `.onnx` path, then requires the model to sit below that directory
  before any stat/read of the model file. Sibling-prefix escapes
  (`/models` vs `/models-evil`), symlink escapes, missing jail paths,
  and non-directory jail paths all fail closed with `-EACCES`. When the
  variable is unset or empty, the jail is a no-op and the normal
  symlink + regular-file + size + operator checks still apply.
  MCP callers also keep their separate path allowlist
  ([mcp/index.md](../mcp/index.md#security-model)).
- **Shape sanity.** The sidecar JSON declares `input_name`,
  `output_name`, and `expected_output_range`. Runtime values outside the
  range raise a warning to stderr; persistent violation aborts scoring
  for the frame.

## Layer 3 — sandbox via ORT

ORT itself sandboxes graph execution — there is no interpreter, no
shell-out, no arbitrary file I/O from inside a graph. Our layers 1/2
harden the envelope around ORT so that even a clever graph cannot
consume unbounded memory or divert through a non-allowlisted op.

## Layer 4 — signature verification (opt-in)

Release artifacts (including models shipped under `model/tiny/`) are
signed by [`.github/workflows/supply-chain.yml`](../../.github/workflows/supply-chain.yml)
using Sigstore's keyless flow. The workflow emits `<artifact>.sig` and
`<artifact>.pem` beside each artifact. To verify locally before loading:

```bash
cosign verify-blob \
    --certificate-identity-regexp "https://github.com/VMAFx/vmafx/.github/workflows/supply-chain.yml@.*" \
    --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
    --bundle vmaf_tiny_fr_v1.onnx.bundle \
    vmaf_tiny_fr_v1.onnx
```

**Now implemented (T6-9 / [ADR-0211](../adr/0211-model-registry-sigstore.md))**:
the `--tiny-model-verify` flag invokes `cosign verify-blob` at load time
via `posix_spawnp(3p)` and fails closed if the signature is missing or
bad. Off by default for dev-friendliness; strongly recommended on for
production deployments. The flag drives `vmaf_dnn_verify_signature()` in
[`core/include/libvmaf/dnn.h`](../../core/include/libvmaf/dnn.h),
which looks up the model's `sigstore_bundle` field in
[`model/tiny/registry.json`](../../model/tiny/registry.json) — see
[model-registry.md](model-registry.md) for the full schema and CLI flow.

## Reporting

If you believe a shipped model is hostile or find a way to bypass the
allowlist, follow the disclosure process in
[`SECURITY.md`](../../SECURITY.md) (90-day coordinated, PGP key listed).
