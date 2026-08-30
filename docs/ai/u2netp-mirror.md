<!-- markdownlint-disable MD046 MD055 MD060 -->
# `u2netp` fork-local mirror — operator workflow

> **Status — exporter ready; release asset pending.** Per
> [ADR-0412](../adr/0412-u2netp-fork-mirror-scaffold.md) and
> [ADR-0671](../adr/0671-u2netp-mirror-exporter.md), this page
> documents the operator workflow for the fork's mirror of upstream
> U-2-Net's `u2netp` checkpoint. The generated binary remains a signed
> release asset, not a git-tracked file.
>
> **Recommended default**: most consumers should reach for
> [`saliency_student_v2`](models/saliency_student_v2.md), the
> fork-trained DUTS student
> ([ADR-0364](../adr/0364-saliency-student-v2-resize-decoder.md)).
> See [the model card's "When to use this" section](models/u2netp_mirror_card.md#6-when-to-use-this)
> for the cases where this mirror is the right choice instead.

This page is the operator-facing complement to the model card
at [`docs/ai/models/u2netp_mirror_card.md`](models/u2netp_mirror_card.md).
The model card explains *what* the mirror is; this page explains
*how* to fetch it, verify it, and load it.

## 1. Where the binary lives

The mirror is **not** committed to git (it would bloat the
history with a 4.7 MB binary). It is shipped as a GitHub
Release asset attached to the VMAFx/vmafx repository:

| Aspect          | Value                                                          |
| --------------- | -------------------------------------------------------------- |
| Repository      | <https://github.com/VMAFx/vmafx>                              |
| Release tag     | `u2netp-mirror-v1`                                             |
| Asset filenames | `u2netp_mirror_v<N>.onnx` (binary)                             |
|                 | `u2netp_mirror_v<N>.onnx.bundle` (Sigstore signature bundle)   |
|                 | `Apache-2.0-u2netp.txt` (license text + attribution block)     |
| In-tree path    | `model/u2netp_mirror.onnx` (gitignored; conventionally where

                    the operator drops the downloaded asset for the C-side loader
                    to find) |

If the upstream upstream itself ever cuts a GitHub release with
the weights as an artefact, prefer fetching from upstream
directly — the fork's mirror exists because no such release
exists today. See `gh api repos/xuebinqin/U-2-Net/releases`.

## 2. Where to download the original

The upstream `u2netp.pth` lives at
<https://github.com/xuebinqin/U-2-Net> with the download link in
`README.md` pointing at Google Drive. The fork's mirror is built
from that upstream artefact (or its ONNX rewrap), redistributed
under Apache-2.0 §4 with the attribution receipt at
[`LICENSES/Apache-2.0-u2netp.txt`](../../LICENSES/Apache-2.0-u2netp.txt).

You can audit the lineage at any time: download the upstream
`u2netp.pth` from Google Drive, run

```bash
sha256sum u2netp.pth
```

and compare against the sha256 recorded in the mirror's model
card (filled in at the binary upload PR). For the ONNX rewrap
path, the export script's metadata block records the conversion
provenance.

## 3. How to export the ONNX mirror locally

Start from an audited local checkout of
<https://github.com/xuebinqin/U-2-Net> and a local `u2netp.pth`
download. The exporter imports upstream `model/u2net.py`, selects
the `d0` saliency output, writes the fork tensor contract
(`input` -> `saliency_map`), and emits a provenance manifest:

```bash
.venv/bin/python ai/scripts/export_u2netp_mirror.py \
  --upstream-dir /path/to/U-2-Net \
  --checkpoint /path/to/u2netp.pth \
  --output model/u2netp_mirror.onnx \
  --manifest-out model/u2netp_mirror.manifest.json
```

The exporter refuses non-Apache upstream license text and records
the upstream module, checkpoint, license, output hashes, and
`run_provenance` in the manifest. The output paths above are
gitignored by design; do not commit either file. Use them for local
evaluation or for the release-asset upload/signing step.

## 4. How to fetch the released mirror

You need [`gh`](https://cli.github.com/) and
[`cosign`](https://docs.sigstore.dev/cosign/installation/)
v3.0.0 or newer.

```bash
# Pin a release tag
TAG="u2netp-mirror-v1"

# Pull the binary, the Sigstore bundle, and the license text
gh release download "$TAG" --repo VMAFx/vmafx \
  --pattern 'u2netp_mirror_v*.onnx' \
  --pattern 'u2netp_mirror_v*.onnx.bundle' \
  --pattern 'Apache-2.0-u2netp.txt' \
  --dir ~/Downloads/u2netp-mirror
```

You should now have three files in `~/Downloads/u2netp-mirror/`:

```text
u2netp_mirror_v1.onnx
u2netp_mirror_v1.onnx.bundle
Apache-2.0-u2netp.txt
```

The `.bundle` is a Sigstore single-file bundle (signature +
certificate + Rekor entry) emitted by `cosign sign-blob` v3+.

## 5. How to verify the signature

This step is **mandatory** before the binary is loaded by any
production pipeline. The verify-blob check gates on Sigstore's
keyless OIDC identity, so a tampered or wrong-origin binary
fails verification:

```bash
cosign verify-blob \
  --bundle u2netp_mirror_v1.onnx.bundle \
  --certificate-identity-regexp '^https://github\.com/VMAFx/vmafx' \
  --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' \
  u2netp_mirror_v1.onnx
```

Expected output on success:

```text
Verified OK
```

Any other output (especially `Error: failed to verify`) means
the binary is not the one this fork released. Do not load it.

## 6. How to verify the expected hash

Beyond the Sigstore signature, the model card records a sha256
for the binary. Cross-check after fetch:

```bash
sha256sum u2netp_mirror_v1.onnx
# Expected: <sha256 from docs/ai/models/u2netp_mirror_card.md §1>
```

If the hashes do not match, the asset has been tampered with or
the model card is stale. In either case, do not proceed.

## 7. How to use the binary at runtime

After verification, drop the binary into the fork's `model/`
directory at the canonical path:

```bash
install -m 0644 u2netp_mirror_v1.onnx \
  /path/to/VMAFx/vmafx/model/u2netp_mirror.onnx
```

Loading the model from C-side `feature_mobilesal.c` requires
either:

(a) A registry follow-up PR registering `u2netp_mirror_v1` in
`model/tiny/registry.json` as an alternative weights drop-in.
The C-side extractor is unchanged — it loads whichever ONNX
the registry resolves to. *(This follow-up is filed but not
bundled with the scaffold PR.)*

(b) Manual override via the existing model-resolver path —
useful for ad-hoc evaluation without flipping the default.
See [`docs/ai/model-registry.md`](model-registry.md).

For table-side experiments before any registry promotion, point the saliency
materializer at the verified ONNX and record the candidate id explicitly:

```bash
.venv/bin/python ai/scripts/materialize_saliency_features.py \
  --input runs/full_features_chug_hdr.jsonl \
  --output runs/full_features_chug_hdr.u2netp.jsonl \
  --model-path model/u2netp_mirror.onnx \
  --model-id u2netp_mirror_v1 \
  --temporal-aggregator ema \
  --audit-json runs/full_features_chug_hdr.u2netp.audit.json
```

Those output rows carry `saliency_model_id` and `saliency_aggregator`, so
later signal-mix audits and MOS-head retrains can distinguish U2NetP/EMA
experiments from the bundled saliency-student mean baseline.

## 8. License compliance — what the operator must do

The mirror redistributes Apache-2.0-licensed work. If the
operator further redistributes the binary (e.g. baking it into
their own product or research artefact), they inherit
Apache-2.0 §4's redistribution conditions:

- Ship the licence text alongside (the
  [`LICENSES/Apache-2.0-u2netp.txt`](../../LICENSES/Apache-2.0-u2netp.txt)
  file from the same release is sufficient).
- Preserve the attribution block (the same file carries it).
- For modified ONNX rewraps: state the modification (Apache-2.0
  §4 (b)).

The fork's redistribution itself follows the same rules; see
[Research-0086](../research/0086-u2netp-fork-mirror-license-compliance.md)
for the per-clause walk.

## 9. Troubleshooting

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| `gh release download` returns no assets | Binary not uploaded yet | Export locally with `ai/scripts/export_u2netp_mirror.py` or wait for the signed release asset per ADR-0412 |
| `cosign verify-blob` fails with "no matching signatures" | Wrong `--certificate-identity-regexp` | Use the regex shown above; the cert binds to `VMAFx/vmafx` workflow runs |
| `cosign verify-blob` fails with "expired certificate" | Sigstore short-lived certs need a fresh Rekor lookup | Cosign v3+ does this automatically; ensure your `cosign` is up to date |
| sha256 mismatch | Wrong asset, partial download, or stale model card | Re-download; re-check model card on the same release tag |
| ONNX load fails with "unsupported op" | Wire-format scanner rejecting an op | Check [`core/src/dnn/op_allowlist.c`](../../core/src/dnn/op_allowlist.c); `Resize` was added by ADR-0258 — older fork commits won't load this graph |

## References

- [ADR-0412](../adr/0412-u2netp-fork-mirror-scaffold.md) — the
  scaffold decision.
- [ADR-0671](../adr/0671-u2netp-mirror-exporter.md) — the exporter
  implementation.
- [Research-0086](../research/0086-u2netp-fork-mirror-license-compliance.md)
  — Apache-2.0 §4 walk + Sigstore wiring.
- [ADR-0286](../adr/0286-saliency-student-fork-trained-on-duts.md)
  — the recommended primary path (`saliency_student_v1`).
- [ADR-0265](../adr/0265-u2netp-saliency-replacement-blocked.md)
  — the original blocker chain.
- Sigstore cosign verify-blob:
  <https://docs.sigstore.dev/cosign/verifying/verify/>.
- Upstream u2netp: <https://github.com/xuebinqin/U-2-Net>.
