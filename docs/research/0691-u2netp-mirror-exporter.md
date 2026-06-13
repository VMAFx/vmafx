<!-- markdownlint-disable MD013 MD060 -->
# Research-0691 — U2NetP mirror exporter

| Field      | Value |
| ---------- | ----- |
| **Date**   | 2026-05-21 |
| **Status** | Implemented |
| **Tags**   | ai, dnn, u2netp, saliency, onnx, provenance |

Companion to [ADR-0671](../adr/0671-u2netp-mirror-exporter.md).

## Finding

The U2NetP mirror was not blocked on another design decision. The
remaining gap was concrete implementation drift:

- `docs/ai/u2netp-mirror.md`,
  `docs/ai/models/u2netp_mirror_card.md`, and
  [Research-0086](0086-u2netp-fork-mirror-license-compliance.md)
  already referenced an `ai/scripts/export_u2netp_mirror.py`
  conversion tool;
- the script was absent;
- the docs still pointed at the pre-renumbering ADR-0325 path even
  though the accepted ADR is ADR-0412.

The correct implementation is an exporter, not a checked-in model
binary. ADR-0412 remains load-bearing: `model/u2netp_mirror.onnx`
is a release asset with Sigstore verification, not a git-tracked
file.

## Implementation Notes

`ai/scripts/export_u2netp_mirror.py` imports `U2NETP` from a local
upstream checkout. This avoids copying Apache-2.0 source into the
fork while still letting operators reproduce the mirror from an
audited tree.

The exporter writes:

- ONNX opset 17, `input` float32 NCHW to `saliency_map` float32 NCHW;
- ONNX metadata with model id, upstream repo/commit, checkpoint hash,
  and conversion note;
- a `u2netp-mirror-export-manifest-v1` JSON sidecar containing
  upstream module/checkpoint/license hashes, NOTICE presence, export
  dimensions/opset, output hash, and shared `run_provenance`.

The test fixture builds a tiny fake upstream `U2NETP` module and a
`module.`-prefixed checkpoint so the exporter path, state-dict
normalisation, ONNX public tensor names, metadata, and Apache-2.0
license guard are exercised without downloading the real upstream
weights in CI.

## Follow-Ups

- Run the exporter against an audited U-2-Net checkout and upstream
  `u2netp.pth`.
- Upload/sign the generated ONNX as a release asset with the
  ADR-0412 license attachment.
- Evaluate saliency/ROI materializer output before any registry
  default change.

## References

- [ADR-0412](../adr/0412-u2netp-fork-mirror-scaffold.md)
- [ADR-0671](../adr/0671-u2netp-mirror-exporter.md)
- [Research-0086](0086-u2netp-fork-mirror-license-compliance.md)
- Source: req — "well that means do u2netp (experimental)... we can only learn i guess lol"
