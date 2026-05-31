<!-- markdownlint-disable MD013 MD033 MD060 -->
# Research-0086 — `u2netp.pth` fork-local mirror: Apache-2.0 §4 compliance + supply-chain surface

| Field      | Value                                                       |
| ---------- | ----------------------------------------------------------- |
| **Date**   | 2026-05-08                                                  |
| **Status** | Compliance walk recorded — scaffold approved (binary upload pending user) |
| **Tags**   | dnn, tiny-ai, u2netp, license, apache-2.0, supply-chain, fork-local |

Companion to [ADR-0412](../adr/0412-u2netp-fork-mirror-scaffold.md).
Captures the licence-compliance walk for redistributing
`u2netp.pth` (or its ONNX rewrap) under Apache-2.0 §4 from the
fork's GitHub Releases, the Sigstore + SLSA wiring this scaffold
extends, and the alternatives the ADR rules out.

## Background

[ADR-0265](../adr/0265-u2netp-saliency-replacement-blocked.md)
recorded two independent blockers against `u2netp`:
distribution (Google-Drive walled) and op allowlist
(bilinear `Resize` not on the list). The op-allowlist axis was
resolved by ADR-0258 (`Resize` added). The distribution axis
remains: there is no upstream GitHub release with the weights
attached, no pinnable raw URL.

ADR-0286 unblocked the *primary* path by training a fork-owned
saliency student on DUTS-TR. That model is wholly fork-owned,
BSD-3-Clause-Plus-Patent, no third-party licence audit needed.

This digest covers the *fallback* path: a fork-local mirror of
the upstream `u2netp.pth` checkpoint via GitHub Release
attachments, signed via Sigstore, and license-compliant under
Apache-2.0 §4.

## Upstream licence audit (re-verified 2026-05-08)

```text
$ gh api repos/xuebinqin/U-2-Net/license --jq '.license'
{ "spdx_id": "Apache-2.0", "name": "Apache License 2.0" }
```

The licence read mirrors ADR-0265's audit (HEAD `ac7e1c81` on
2026-05-03, re-checked against current HEAD on 2026-05-08 — no
relicensing in the interim). Apache-2.0 is fully compatible with
the fork's BSD-3-Clause-Plus-Patent licence under the standard
combine-and-redistribute pattern.

A recursive listing of HEAD confirms there is **no** `NOTICE`
file in the upstream tree:

```text
$ gh api 'repos/xuebinqin/U-2-Net/git/trees/master?recursive=1' \
    --jq '.tree[] | select(.path | test("NOTICE$"; "i")) | .path'
(empty)
```

This is load-bearing for the redistribution conditions. Apache-2.0
§4 (d) only triggers if upstream ships a NOTICE file; (a) + (b) +
(c) are unconditional but reduce in scope when there are no
modified-file notices to carry and no NOTICE attestations to
preserve.

## Apache-2.0 §4 redistribution conditions (per-clause walk)

§4 (a) — *"You must give any other recipients of the Work ... a
copy of this License"*. **Satisfied by** shipping the full
license text at `LICENSES/Apache-2.0-u2netp.txt`. Operators
fetching the binary from the GitHub release see the licence in
the same release attachment list (the scaffold's release-workflow
step uploads both the binary and the licence text alongside).

§4 (b) — *"You must cause any modified files to carry prominent
notices stating that You changed the files"*. **Conditionally
applies** to the ONNX rewrap (the binary derivative). The
fork-local conversion script `ai/scripts/export_u2netp_mirror.py`
(implemented by ADR-0671) emits an ONNX
graph with a top-level `model.metadata_props` entry recording:
"derived from xuebinqin/U-2-Net @ <sha>; converted via
torch.onnx.export opset 17; no graph-topology modifications;
weights byte-identical to upstream `u2netp.pth`". When the binary
upload PR lands, the model-card SHA includes this metadata block.
For the verbatim `.pth` redistribution path, (b) is moot — no
file modification.

§4 (c) — *"You must retain ... all copyright, patent, trademark,
and attribution notices from the Source form of the Work ...
excluding those notices that do not pertain to any part of the
Derivative Works"*. **Satisfied by** the attribution block in
`LICENSES/Apache-2.0-u2netp.txt` and the model card. The block
cites:

- Upstream copyright: 2020 The U^2-Net Authors (per the LICENSE
  header).
- Paper citation: Qin et al. 2020.
- Source URL: `https://github.com/xuebinqin/U-2-Net`.
- Upstream commit pin: `ac7e1c81` (current HEAD on the audit
  date; re-pinned at the binary upload PR).

§4 (d) — *"If the Work includes a "NOTICE" text file ... any
Derivative Works that You distribute must include a readable copy
of the attribution notices contained within such NOTICE file"*.
**Moot** — upstream ships no NOTICE file (verified above).

## Sigstore + SLSA wiring inheritance

`.github/workflows/supply-chain.yml` already does the heavy
lifting for `libvmaf.so`, the `vmaf` CLI, and `models.tar.gz`:

- `cosign-installer@v4.1.1` + `cosign v3.0.6` — keyless OIDC
  sign-blob with `--bundle <f>.bundle` output (single-file
  Sigstore bundle = signature + certificate + Rekor entry).
- `slsa-framework/slsa-github-generator/.github/workflows/generator_generic_slsa3.yml@v2.0.0`
  — SLSA L3 provenance.
- `softprops/action-gh-release@v3.0.0` — attach all signatures +
  SBOM to the release.

The scaffold's release-workflow step extends this by:

1. Detecting `model/u2netp_mirror.onnx` (and optionally
   `model/u2netp_mirror.pth`) at release time. If absent, fast-exit
   no-op.
2. If present, copy into the `artifacts/` dir before the existing
   `Compute SHA256 hashes` step, so SLSA provenance includes them
   as subjects.
3. The existing `cosign sign-blob --yes --bundle` loop signs every
   file in `artifacts/`, including the mirror — no new signing
   logic.

This intentionally reuses `release:published` triggering rather
than introducing a separate `u2netp-mirror-v1`-only release. The
scaffold writes the asset name pattern as `u2netp_mirror_v<N>.onnx`
(N = monotonic integer per binary upload PR), so multiple
versions of the upstream weights can co-exist in the release
asset list across releases.

Verification recipe documented in `docs/ai/u2netp-mirror.md`
(operator doc):

```bash
# Fetch the mirror
gh release download <tag> --repo VMAFx/vmafx \
  --pattern 'u2netp_mirror_v*.onnx' \
  --pattern 'u2netp_mirror_v*.onnx.bundle' \
  --pattern 'Apache-2.0-u2netp.txt'

# Verify the Sigstore bundle (cosign v3+)
cosign verify-blob \
  --bundle u2netp_mirror_v1.onnx.bundle \
  --certificate-identity-regexp '^https://github\.com/VMAFx/vmafx' \
  --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' \
  u2netp_mirror_v1.onnx
```

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Scaffold + user-uploads-binary-later (this digest)** | Two-PR shape lets reviewers focus on licence + plumbing now without a 4.7 MB binary in the diff; matches the user's existing Google-Drive-recipient bundler workflow shape | Scaffold is inactive until the user uploads | **Chosen** — see ADR-0412 |
| Single-PR scaffold + binary | One-shot ship | Reviewers asked to examine licence + plumbing + binary all at once; binary review tooling is GitHub's anyway, just less concentrated; explicitly forbidden by task brief | Rejected — task brief says scaffold-only |
| `.pth` verbatim only | Simplest licence story (no derivative-work argument); smallest scaffold | Adds a `.pth → .onnx` runtime conversion burden to every consumer, complicating the C-side load path | Deferred; ADR-0671 implements the ONNX-export path |
| `.onnx` rewrap only | Loads directly via the existing C-side ONNX path; no runtime conversion | Loses lineage clarity; reviewers must trust the export script for "this is what upstream actually trained" | Chosen for the exporter path; release upload remains separate |
| `.pth` + `.onnx` both | Best lineage; verifiable rewrap | Doubles the asset count and Sigstore-sign count | Open question for the user — flagged |
| Vendor `u2netp.pth` directly into `model/` (committed binary) | One-shot ship | Bloats git history with a 4.7 MB binary forever | Rejected — release attachments are the right shape |
| `git-lfs` mirror | Tracked but lazy-clone | Adds `git-lfs` to fork toolchain; LFS storage is paid GitHub feature | Rejected — release attachments are free + already wired to Sigstore |
| Mirror via S3 / IPFS / external host | Bypasses GitHub storage limits | New infrastructure surface; complicates Sigstore wiring; weakens the licence-attribution traceability (asset-fetch URL is not under the fork's GitHub identity) | Rejected — GitHub release attachments are already in the supply-chain pipeline |
| Email upstream maintainer to cut a release | Cleanest solution if granted | Out-of-band, no commitment, no timeline | Filed as long-shot follow-up; this digest does not depend on it |
| Don't mirror at all; rely solely on `saliency_student_v1` | Zero new audit surface | Closes off lineage-audit + comparative-evaluation use cases | Rejected — `saliency_student_v1` is the *primary* path; this is the named *fallback* |

## Recommendation

Land [ADR-0412](../adr/0412-u2netp-fork-mirror-scaffold.md) and
the scaffold deliverables (licence text, model card stub,
operator doc, release-workflow guard) in this PR. Defer the
binary upload to a sibling PR after the user confirms the three
compliance questions in ADR-0412 §References (redistribution read,
tag scheme, artefact format). ADR-0671 adds the missing exporter
while preserving the release-asset-only binary contract.

## References

- [ADR-0218](../adr/0218-mobilesal-saliency-extractor.md) —
  original MobileSal extractor design.
- [ADR-0257](../adr/0257-mobilesal-real-weights-deferred.md) +
  [Research-0053](0053-mobilesal-real-weights-blocker.md) — the
  prior MobileSal blocker chain.
- [ADR-0258](../adr/0258-onnx-allowlist-resize.md) — `Resize`
  added to the allowlist; resolves ADR-0265's axis-2 blocker.
- [ADR-0265](../adr/0265-u2netp-saliency-replacement-blocked.md)
  [Research-0054](0055-u2netp-saliency-replacement-survey.md)
  — the U-2-Net survey + blocker decision this digest extends.
- [ADR-0286](../adr/0286-saliency-student-fork-trained-on-duts.md)
  — fork-trained `saliency_student_v1`; the recommended primary
  path.
- [ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md) — the
  doc-substance bar the PR's model-card stub satisfies.
- [`.github/workflows/supply-chain.yml`](../../.github/workflows/supply-chain.yml)
  — Sigstore + SLSA pipeline reused.
- Apache-2.0 licence text: <https://www.apache.org/licenses/LICENSE-2.0>
- Upstream code: <https://github.com/xuebinqin/U-2-Net>
  (HEAD `ac7e1c81`, SPDX = Apache-2.0, no NOTICE file).
- Upstream paper: Qin, Zhang, Huang, Dehghan, Zaiane, Jagersand,
  *"U^2-Net: Going Deeper with Nested U-Structure for Salient
  Object Detection"*, Pattern Recognition 2020.
- Sigstore cosign verify-blob spec:
  <https://docs.sigstore.dev/cosign/verifying/verify/>.
- Source: paraphrased — task brief directive ("Apache-2.0 §4
  NOTICE compliance ... documented in `docs/ai/u2netp-mirror.md`
  ... ships the SCAFFOLD").
