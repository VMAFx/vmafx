- **`u2netp` fork-local release-artefact mirror — scaffold
  ([ADR-0367](../docs/adr/0367-u2netp-fork-mirror-scaffold.md)).**
  Adds the licence + documentation + release-pipeline plumbing
  for a fork-local mirror of the upstream U-2-Net `u2netp`
  checkpoint (Apache-2.0, ~4.7 MB, Google-Drive-walled upstream
  per [ADR-0265](../docs/adr/0265-u2netp-saliency-replacement-blocked.md)),
  shipped via GitHub Release attachment and signed via the
  existing Sigstore + SLSA pipeline. New top-level
  [`LICENSES/`](../LICENSES/) directory carries the full
  Apache-2.0 text + attribution block per Apache-2.0 §4 (a) +
  (c); upstream ships no NOTICE file so §4 (d) is moot
  (verified at HEAD `ac7e1c81`). Operator workflow at
  [`docs/ai/u2netp-mirror.md`](../docs/ai/u2netp-mirror.md)
  covers fetch via `gh release download`, sha256 cross-check,
  and `cosign verify-blob` against the keyless OIDC identity.
  Model card stub at
  [`docs/ai/models/u2netp_mirror_card.md`](../docs/ai/models/u2netp_mirror_card.md)
  follows the [ADR-0042](../docs/adr/0042-tinyai-docs-required-per-pr.md)
  5-point bar with placeholders for the binary's sha256 and
  Sigstore bundle URL. The release workflow at
  [`.github/workflows/supply-chain.yml`](../.github/workflows/supply-chain.yml)
  gains an idempotent guard that stages
  `model/u2netp_mirror.{onnx,pth}` + `Apache-2.0-u2netp.txt`
  into the release-artefacts directory iff the binary is
  present — missing-file path is a fast-exit no-op so
  scaffold-only releases continue to work. Binary upload is
  deferred to a sibling PR after the user confirms the three
  open compliance questions in ADR-0367 §References (Apache-2.0
  §4 read, tag scheme, artefact format). The fork's
  *recommended* saliency weights remain
  [`saliency_student_v1`](../docs/ai/models/saliency_student_v1.md)
  ([ADR-0286](../docs/adr/0286-saliency-student-fork-trained-on-duts.md));
  this mirror is the named *fallback* for users who specifically
  want upstream u2netp lineage. Companion research digest:
  [Research-0086](../docs/research/0086-u2netp-fork-mirror-license-compliance.md).
