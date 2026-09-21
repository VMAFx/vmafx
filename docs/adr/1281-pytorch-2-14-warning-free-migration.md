<!-- markdownlint-disable MD013 MD060 -->
# ADR-1281: Migrate the Python ML stack without warning suppressions

- **Status**: Accepted
- **Date**: 2026-09-21
- **Deciders**: VMAFx maintainers
- **Tags**: `ai`, `ci`, `dependencies`, `python`, `testing`, `fork-local`

## Context

ADR-1278 made Python warnings fatal, but the exact PyTorch 2.14 development image
then exposed code paths that still depended on deprecated APIs. The QAT path used
`torch.ao.quantization.quantize_fx.prepare_qat_fx`, ONNX exporters mixed the
deprecated TorchScript path with `dynamic_axes`, and package-local pytest configs
declared a pytest-asyncio option in environments that did not install that plugin.
The result was a nominal warning policy that could not pass the complete AI and
vmaf-tune package suites.

ADR-0208 already names `torchao.quantization.pt2e` as the required QAT migration
target while requiring the QAT-conditioned-weight to fp32 ONNX to ORT static-QDQ
bridge to remain intact. PyTorch 2.14 and Python 3.14 add one compatibility
constraint: torchao 0.17 fails during import, while torchao 0.18 imports and its
PT2E X86 quantizer preserves the original module state-dict keys needed by the
existing transfer guard.

## Decision

VMAFx will use torchao 0.18 PT2E with `X86InductorQuantizer` and its symmetric
QAT configuration for fake-quant preparation. The existing state-key and shape
transfer into a fresh fp32 module and the ORT static-QDQ export remain mandatory.
Torch ONNX exports use the default dynamo exporter and `dynamic_shapes`. Because
PyTorch 2.14's native exporter targets opset 18 and warns when asked to perform
its own conversion to the repository's opset-17 model contract, the shared export
path writes a private opset-18 graph and then explicitly converts it with ONNX's
version converter before atomically publishing one self-contained opset-17 file.
Callers must correct warning-producing behavior and tests must capture deliberately
public warnings explicitly. No warning filter, deprecated-API wrapper, version
rollback, or touched-file exception is permitted.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `prepare_qat_fx` and filter its warning | Smallest diff | Retains an API PyTorch says will be removed and makes the fatal-warning gate dishonest | Directly violates ADR-1278 and the user requirement |
| Pin an older PyTorch | Avoids the newly visible diagnostics | Reverts the release dependency train and postpones known breakage | A version rollback is not a source fix |
| Use torchao `QATConfig` on modules directly | Stable public high-level API | Current support is limited to linear and embedding modules; the shipped learned filter is convolutional | Does not preserve the existing CNN QAT contract |
| Use torchao PT2E with its testing-only XNNPACK quantizer | Closest to older x86 qconfig wording | Imports a private testing namespace and couples production code to an unstable helper | The supported X86Inductor quantizer covers the required convolution and linear patterns |
| Use torchao PT2E with `X86InductorQuantizer` | Public torchao path; symmetric QAT; preserves state keys; supports current CNN | Adds a runtime training dependency and exported-graph preparation step | Chosen |
| Ask PyTorch to export opset 17 directly | One export call | PyTorch 2.14 warns that its native implementations are opset 18 and performs an implicit conversion | The explicit ONNX conversion keeps warnings fatal and the final registry contract inspectable |
| Change the model registry to opset 18 | Avoids down-conversion | Widens the deployed runtime contract and requires requalification of every consumer | The warning cleanup does not authorize a model-format migration |

## Consequences

- **Positive**: Complete AI and vmaf-tune tests can run with warnings promoted to
  errors on the shipped PyTorch version; the QAT path no longer calls the removed
  `torch.ao` surface; ONNX export follows the current dynamo contract while the
  published models remain self-contained opset-17 graphs.
- **Negative**: The training extra gains the torchao wheel and the development
  image grows by roughly one compressed 3.2 MiB wheel plus installed files.
- **Neutral / follow-ups**: Keep the ORT bridge and the non-zero state-transfer
  guard. A future torchao or PyTorch bump must replay the QAT smoke test and verify
  that module state keys still match before it can land.

## Supply-chain impact

- **New dependencies**: `torchao>=0.18.0,<0.19` (runtime training dependency,
  BSD-3-Clause, <https://github.com/pytorch/ao>).
- **Removed dependencies**: none. The migration removes imports from PyTorch's
  deprecated `torch.ao.quantization` package but does not remove PyTorch.
- **Build-time fetches**: the existing `pip install -e '/build/vmaf/ai[dev]'`
  layer in `dev/Containerfile` resolves the new wheel; no new fetch mechanism is
  introduced.
- **Sigstore-signable**: the wheel remains covered by the release image SBOM and
  provenance flow; no repository-local binary is added.
- **CVE surface delta**: no network listener or privileged runtime surface is
  added. torchao executes only in the training/QAT path.

## SBOM delta

```yaml
components:
  - type: library
    name: torchao
    version: 0.18.0
    purl: pkg:pypi/torchao@0.18.0
    licenses:
      - license:
          id: BSD-3-Clause
```

## References

- [ADR-0207](0207-tinyai-qat-design.md) and
  [ADR-0208](0208-learned-filter-v1-qat-impl.md) define the retained QAT-to-ORT
  bridge.
- [ADR-1278](1278-python-safe-parallel-execution.md) makes warnings fatal.
- [PyTorch 2 Export QAT](https://docs.pytorch.org/ao/stable/pt2e_quantization/pt2e_quant_qat.html).
- [torchao 0.18](https://github.com/pytorch/ao/releases/tag/v0.18.0).
- [Research digest](../research/pytorch-2-14-warning-free-migration-2026-09-21.md).
- Source: `req` — "there is no on touch rule anymore, no fucking warning or error is just ignored because of being og netflix code, fix them all ffs".
