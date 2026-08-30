<!-- markdownlint-disable MD013 MD060 -->
# ADR-0671: U2NetP Mirror Exporter

- **Status**: Accepted
- **Date**: 2026-05-21
- **Deciders**: Lusoris, Codex
- **Tags**: ai, dnn, u2netp, saliency, onnx, provenance, fork-local

## Context

[ADR-0412](0412-u2netp-fork-mirror-scaffold.md) landed the
license, model-card, operator-doc, and release-attachment scaffold
for an upstream U-2-Net `u2netp` mirror. The scaffold deliberately
kept the binary out of git and said the conversion script would be
a follow-up, but the user-facing docs already referred to
`ai/scripts/export_u2netp_mirror.py`.

That made the surface half-finished: operators could read how to
verify a future release asset, but they had no in-tree tool to
build the ONNX candidate, record the upstream hashes, or attach the
Apache-2.0 compliance metadata that ADR-0412 requires.

## Decision

Add `ai/scripts/export_u2netp_mirror.py` as the operator bridge from
an audited local `xuebinqin/U-2-Net` checkout plus `u2netp.pth` to
the fork mirror contract:

- import upstream `U2NETP` from the local checkout instead of
  vendoring upstream source into this repository;
- load the upstream checkpoint, including `module.`-prefixed
  `DataParallel` state dicts;
- export ONNX opset 17 with public tensors
  `input` -> `saliency_map`;
- select upstream `d0` as the saliency output;
- require an Apache-2.0 upstream `LICENSE`;
- write ONNX metadata plus a deterministic
  `u2netp-mirror-export-manifest-v1` sidecar with shared
  `run_provenance`.

The exporter still does **not** register `u2netp_mirror_v1` as the
default `mobilesal` weights and does **not** commit the generated
ONNX file. `saliency_student_v2` remains the production default until
a separate measured promotion says otherwise.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add an exporter that imports a local upstream checkout | Closes the missing-tool gap without vendoring Apache-2.0 code; records hashes and metadata for release review | Requires operators to fetch the upstream checkout and checkpoint first | Chosen: it is the smallest implementation that makes the scaffold usable and auditable |
| Vendor the U-2-Net architecture into `ai/scripts/` | Fully self-contained exporter | Copies Apache-2.0 source into this tree and broadens the license surface | Rejected: importing the audited checkout keeps the fork source clean |
| Commit `model/u2netp_mirror.onnx` directly | Immediate runtime availability | Violates ADR-0412 and bloats git history with a binary artefact | Rejected: the binary remains a signed release asset only |
| Flip the registry default to U2NetP now | Gives users the larger upstream saliency model immediately | No local release asset, no measured ROI/tune win, and changes production behaviour | Rejected: runtime promotion is a separate model decision |
| Keep docs-only scaffold status | No code risk | Leaves a referenced script missing and blocks experiments | Rejected: the user explicitly asked to do the U2NetP experiment |

## Consequences

- **Positive**: The U2NetP mirror path is now executable, test-covered,
  and carries repeatable provenance from source checkout to ONNX.
- **Negative**: Operators still need to acquire the upstream checkpoint
  through the upstream distribution channel before exporting.
- **Neutral / follow-ups**: Upload/sign the generated ONNX as a release
  asset, then evaluate it against saliency/ROI materialized tables before
  any registry or default-weight change.

## References

- [ADR-0412](0412-u2netp-fork-mirror-scaffold.md) — scaffold this
  implementation completes.
- [ADR-0265](0265-u2netp-saliency-replacement-blocked.md) — original
  U2NetP blocker chain.
- [ADR-0286](0286-saliency-student-fork-trained-on-duts.md) and
  [ADR-0364](0364-saliency-student-v2-resize-decoder.md) — fork-owned
  saliency-student defaults.
- [Research-0691](../research/0691-u2netp-mirror-exporter.md) —
  implementation digest.
- Source: req — "well that means do u2netp (experimental)... we can only learn i guess lol"
