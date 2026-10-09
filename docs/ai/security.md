<!-- markdownlint-disable MD060 -->
# Tiny AI — security model

ONNX models are data, not code, but that is no excuse for a lazy runtime.
libvmaf's DNN layer applies four layers of defence before any frame touches a
graph:

| Layer | What it does | Default |
| --- | --- | --- |
| 1. Operator allowlist | Rejects any graph node whose op is not on a curated list | always on |
| 2. Resource bounds | Size cap, path validation, optional directory jail, output sanity | always on (jail opt-in) |
| 3. ORT sandbox | ONNX Runtime executes the graph with no interpreter or shell-out | always on |
| 4. Signature verification | `cosign verify-blob` of the model's Sigstore bundle | off; opt in with `--tiny-model-verify` |

## Threat model

| Class | Example | Mitigation |
| --- | --- | --- |
| Hostile `.onnx` file | Model with a custom op that exfiltrates via a network syscall | Operator allowlist (layer 1). |
| Memory exhaustion via a huge model | 10 GB `.onnx` passed to `--tiny-model` | Size cap `VMAF_DNN_DEFAULT_MAX_BYTES`, compile-time 50 MB (layer 2). |
| Path traversal via `--tiny-model` | `--tiny-model ../../etc/shadow` | `vmaf_dnn_validate_onnx` resolves symlinks, requires `S_ISREG` and readable, and can enforce the `VMAF_TINY_MODEL_DIR` jail (layer 2). |
| Silent model substitution | Attacker replaces a signed model with a poisoned one | Opt-in Sigstore (`cosign`) verification against the workflow identity (layer 4). |

## Layer 1: operator allowlist

[`core/src/dnn/op_allowlist.{h,c}`](../../core/src/dnn/op_allowlist.h) holds a
curated set of ONNX operator names, 74 entries (73 distinct names; `Clip` is
listed twice). Before creating an ORT session the loader walks the graph and
rejects any node whose op is not on the list. Extending the list is a
conscious, reviewed act: a change to `op_allowlist.c` must be called out in the
PR description and backed by a concrete model that needs the addition.

### What is allowed

The list covers the building blocks of C1, C2 and C3 architectures:

- `Conv`, `Gemm`, `MatMul`, normalisation layers such as `BatchNormalization`,
  and pooling such as `GlobalAveragePool`;
- activations such as `Relu`, `Sigmoid`, `Softmax` and `Gelu`;
- arithmetic, reduce, reshape and transpose ops;
- the five quantisation ops `QuantizeLinear`, `DequantizeLinear`,
  `DynamicQuantizeLinear`, `MatMulInteger` and `ConvInteger`, see
  [quantization.md](quantization.md);
- the control-flow ops `Loop` and `If`
  ([ADR-0169](../adr/0169-onnx-allowlist-loop-if.md); needed by MUSIQ, RAFT and
  small-VLM-class architectures);
- the spatial sampler `Resize`
  ([ADR-0258](../adr/0258-onnx-allowlist-resize.md); needed by U-2-Net,
  mobilesal, BASNet, PiDiNet and FPN-style detectors).

These are rejected: `Scan`, unknown names (`custom_op_xyz`), and anything that
could touch the filesystem or network. `Scan` stays out because its
variant-typed input and output binding makes static bound-checking impractical
for a wire-format scanner.

!!! note
    `Resize` has a `mode` attribute (`nearest`, `linear`, `cubic`). Per
    ADR-0258 the wire scanner gates the op type, not its attributes. Consumers
    shipping their own ONNX are expected to keep `mode` at `nearest` or
    `linear`: `cubic` is numerically less stable on quantised inputs and no
    in-tree consumer uses it.

### Control-flow subgraphs

For `Loop` and `If`, the wire-format scanner in
[`onnx_scan.c`](../../core/src/dnn/onnx_scan.c) recurses into the embedded
subgraphs (`Loop.body`, `If.then_branch`, `If.else_branch`) and applies the
same allowlist check at every depth, capped at 8 levels of nesting
(`VMAF_DNN_MAX_SUBGRAPH_DEPTH`) as a defence-in-depth bound. A forbidden op
cannot hide inside a control-flow body.

### Bounded-iteration guard

[ADR-0171](../adr/0171-bounded-loop-trip-count.md) bounds loops at two points:

| Layer | Where | Limit |
| --- | --- | --- |
| Export time | `vmaf_train.op_allowlist` in `vmaf-train` | Every `Loop.M` input must trace back to a `Constant` int64 scalar in `[0, MAX_LOOP_TRIP_COUNT]` (default 1024, overridable per call). Rejects a graph-input or non-Constant producer. Recurses into nested subgraphs, so a `Loop` inside a `Loop.body` is bounded in its own scope. |
| Load time | libvmaf wire scanner (`onnx_scan.c`) | `VMAF_DNN_MAX_LOOP_NODES = 16` `Loop` nodes per model across the top-level graph and every embedded subgraph; nesting depth 8. |

A model has to clear both to load. The load-time cap is coarser than the
Python data-flow check by design: reproducing producer-map lookup in a
wire-format scanner would violate the ADR D39 "bounded-auditable-scope"
constraint that keeps the scanner free of `libprotobuf-c`. Models that bypass
the
trainer (HTTP-fetched, MCP-uploaded, third-party registries) still hit the
load-time cap.

## Layer 2: resource bounds

### Size cap

The loader refuses files larger than `VMAF_DNN_DEFAULT_MAX_BYTES` (50 MB,
compile-time constant in
[`core/src/dnn/model_loader.h`](../../core/src/dnn/model_loader.h)). The check
runs before the file is mapped. The historical `VMAF_MAX_MODEL_BYTES` env
override was retired in T7-12 after two release cycles without a shipped model
near the cap. Callers that need a larger envelope must bump the constant and
rebuild.

### Path validation

`vmaf_dnn_validate_onnx`:

- resolves symlinks;
- asserts `S_ISREG` (no devices, pipes or directories);
- returns `-errno` on any failure, which the caller must check.

### Directory jail

`VMAF_TINY_MODEL_DIR` is the optional deployment jail:

- When set to a directory, the loader canonicalises the jail and the requested
  `.onnx` path, and requires the model to sit below the jail before any stat or
  read of the file.
- Sibling-prefix escapes (`/models` against `/models-evil`), symlink escapes,
  missing jail paths and non-directory jail paths fail closed with `-EACCES`.
- When the variable is unset or empty, the jail is a no-op and the normal
  symlink, regular-file, size and operator checks still apply.
- MCP callers keep their own path allowlist
  ([mcp/index.md](../mcp/index.md#security-model)).

### Shape sanity

The sidecar JSON declares `input_name`, `output_name` and
`expected_output_range`. A runtime value outside the range raises a warning to
stderr. A persistent violation aborts scoring for the frame.

## Layer 3: sandbox via ORT

ONNX Runtime sandboxes graph execution: there is no interpreter, no shell-out
and no arbitrary file I/O from inside a graph. Layers 1 and 2 harden the
envelope around ORT, so that even a clever graph cannot consume unbounded
memory or divert through a non-allowlisted op.

## Layer 4: signature verification (opt-in)

Release artifacts, including models under `model/tiny/`, are signed by
[`.github/workflows/supply-chain.yml`](../../.github/workflows/supply-chain.yml)
with Sigstore's keyless flow. The workflow produces a single Sigstore bundle per
artifact (signature, certificate and Rekor entry in one file), not the legacy
split `.sig` and `.pem` pair. The registry names a model's bundle
`<name>.onnx.sigstore.json`, see [model-registry.md](model-registry.md).

To verify a model by hand before loading it:

```bash
cosign verify-blob \
    --certificate-identity-regexp '^https://github\.com/VMAFx/vmafx/\.github/workflows/supply-chain\.yml@refs/(heads/master|tags/v[0-9][0-9A-Za-z.+-]*)$' \
    --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
    --bundle model/tiny/vmaf_tiny_v2.onnx.sigstore.json \
    model/tiny/vmaf_tiny_v2.onnx
```

The expression is anchored at both ends (`^` and `$`): cosign searches the
certificate's identity for it, so an unanchored pattern also accepts an
identity with anything before or after it.

The identity is a name: the organisation's login, `VMAFx`, which GitHub frees
when an organisation is renamed. The signing certificate also carries the
organisation's numeric ID, `288567244` (Fulcio extension
`1.3.6.1.4.1.57264.1.17`), which a later owner of the name cannot have.
cosign 3.1.3 has no option for it; see
[the signer's owner](../development/release.md#the-signers-owner-not-only-its-name)
for the check by hand.

The `--tiny-model-verify` flag does all of this at load time
(T6-9, [ADR-0211](../adr/0211-model-registry-sigstore.md);
[ADR-2985](../adr/2985-signer-owner-id-binding.md)):

- It reads the bundle once and refuses it unless it holds exactly one
  certificate, whose identity matches the expression above and whose owner ID
  is `288567244`. It then invokes `cosign verify-blob` through
  `posix_spawnp(3p)` on a private copy of the bytes it checked, with the same
  anchored expression, and fails closed when the signature is missing or bad.
  A certificate of any other identity or owner is refused like a bad
  signature (`-EPROTO`).
- It is off by default for dev-friendliness and strongly recommended in
  production.
- It drives `vmaf_dnn_verify_signature()` in
  [`core/include/libvmaf/dnn.h`](../../core/include/libvmaf/dnn.h), which looks
  up the model's `sigstore_bundle` field in
  [`model/tiny/registry.json`](../../model/tiny/registry.json).

## Reporting

If you believe a shipped model is hostile or find a way to bypass the
allowlist, follow the disclosure process in [`SECURITY.md`](../../SECURITY.md).
Sensitive reports should be PGP-encrypted; the policy states the response
timeline.
