<!-- markdownlint-disable MD013 MD027 MD029 MD033 MD060 -->
# ADR-0412: Fork-local release-artefact mirror scaffold for `u2netp.pth` (Apache-2.0)

- **Status**: Accepted
- **Status update 2026-05-15**: scaffold implemented;
  `LICENSES/Apache-2.0-u2netp.txt` and
  `docs/ai/models/u2netp_mirror_card.md` present on master;
  attribution + licence compliance recipe landed. Binary upload
  (`model/u2netp_mirror.onnx`) is a user-triggered release step
  per the scaffold design.
- **Status update 2026-05-21**:
  [ADR-0671](0671-u2netp-mirror-exporter.md) adds the missing
  exporter that produces the ONNX candidate and provenance manifest
  from an audited local upstream checkout. The binary remains a
  release asset, not a committed file.
- **Date**: 2026-05-08
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, u2netp, saliency, license, apache-2.0, supply-chain, fork-local, docs

## Context

[ADR-0265](0265-u2netp-saliency-replacement-blocked.md) recorded
two independent blockers against switching the `mobilesal`
extractor's underlying weights to U-2-Net's `u2netp` checkpoint:

1. **Distribution**: `u2netp.pth` is published only behind a Google
   Drive viewer URL on `xuebinqin/U-2-Net` — no GitHub release, no
   pinnable raw URL, so the FastDVDnet pattern (pin upstream commit
   curl -L -O <raw>` in CI) does not reproduce.
2. **ONNX op allowlist**: U-2-Net's bilinear `F.upsample` lowers to
   `Resize`, which was not on `core/src/dnn/op_allowlist.c` at
   the time.

The op-allowlist blocker (axis 2) was resolved by
[ADR-0258](0258-onnx-allowlist-resize.md) (`Resize` added under a
bounded contract). The distribution blocker (axis 1) remains.

[ADR-0286](0286-saliency-student-fork-trained-on-duts.md) +
PR #341 shipped `saliency_student_v1` — a fork-trained
~113 K-parameter U-Net on DUTS-TR — as a drop-in replacement for
`mobilesal_placeholder_v0`. That model is wholly fork-owned under
BSD-3-Clause-Plus-Patent, so the *primary* path to a
content-dependent `saliency_mean` does not require an upstream
weights mirror at all.

This ADR records the **fallback** path: a fork-local release
artefact that mirrors `u2netp.pth` from upstream Google Drive
under Apache-2.0 §4 NOTICE attribution, so a future maintainer
who wants the upstream u2netp weights specifically — for lineage
audit, comparative evaluation, or as an alternative to the DUTS
student — can pin a stable URL the fork controls. The PR ships
the *scaffold* only: licence text, model-card stub, operator
documentation, and a release-workflow upload step that becomes
active when the binary lands at `model/u2netp_mirror.onnx`. The
binary itself is **not committed** in this PR (the user uploads
it once to the dedicated release tag); the scaffold has to land
first so the release pipeline knows what to do when it appears.

The Apache-2.0 §4 redistribution conditions are explicit:

> 4. Redistribution. You may reproduce and distribute copies of
> the Work or Derivative Works thereof in any medium, with or
> without modifications, ... provided that You meet the following
> conditions:
>
> (a) You must give any other recipients of the Work ... a copy
>     of this License; and
> (b) You must cause any modified files to carry prominent notices
>     stating that You changed the files; and
> (c) You must retain, in the Source form of any Derivative Works
>     that You distribute, all copyright, patent, trademark, and
>     attribution notices from the Source form of the Work ...; and
> (d) If the Work includes a "NOTICE" text file as part of its
>     distribution, then any Derivative Works that You distribute
>     must include a readable copy of the attribution notices
>     contained within such NOTICE file ...

Upstream `xuebinqin/U-2-Net` ships an SPDX `LICENSE` file
(`Apache-2.0`) and **no** `NOTICE` file (verified against HEAD
`ac7e1c81` in [Research-0086](../research/0086-u2netp-fork-mirror-license-compliance.md)).
The fork's redistribution obligation therefore reduces to (a) +
(b) + (c) — ship the licence text and an attribution block citing
upstream copyright, paper, and source URL. (d) is moot.

The mirror is a *binary redistribution* of an ONNX-converted
artefact derived from upstream `.pth` weights. Per Apache-2.0 §4
the same conditions apply to "the Work or Derivative Works
thereof in any medium". The ONNX rewrap is a derivative work
(graph topology preserved, weights re-encoded into a different
container format); the licence carries through unchanged.

## Decision

We will scaffold a fork-local mirror for `u2netp` weights under
the path `model/u2netp_mirror.onnx` (gitignored — binary blob,
shipped via GitHub Release attachment, not source control), with
the following pieces landing in this PR:

1. **`LICENSES/Apache-2.0-u2netp.txt`** — full Apache-2.0 licence
   text + attribution block citing upstream copyright, paper, and
   source URL. Satisfies §4 (a) + (c). New top-level directory
   `LICENSES/` per the SPDX licenses-list convention; future
   third-party licence bundles drop alongside.
2. **`docs/ai/models/u2netp_mirror_card.md`** — model card stub
   per [ADR-0042](0042-tinyai-docs-required-per-pr.md)'s 5-point
   bar, with placeholders for the binary's sha256 and the
   Sigstore bundle URL that fill in once the user uploads.
   Documents the upstream source URL verbatim, the conversion
   recipe (`.pth → .onnx`), the `(input shape, op allowlist
   coverage, opset)` triple, and the licence-compliance receipt.
3. **`docs/ai/u2netp-mirror.md`** — operator-facing workflow doc
   under [`docs/ai/`](../ai/) (per CLAUDE.md §12 r10): where to
   download the original from upstream, how to convert (or skip
   conversion if the user already has ONNX bytes), how to verify
   the expected sha256, how to fetch the fork's mirror via
   `gh release download`, and the Sigstore signature verification
   command.
4. **`.github/workflows/release-please.yml`** gains a
   `u2netp-mirror-attach` job that runs only when
   `model/u2netp_mirror.onnx` is present in the working tree at
   release time. Idempotent: missing-file path is a no-op (fast
   exit), present-file path uploads the binary to the release
   under the tag scheme `u2netp-mirror-v<N>` and triggers the
   existing Sigstore sign step. Until the binary lands, the job
   is a fast-exit shell guard — no broken release.
5. **`changelog.d/added/0367-u2netp-fork-mirror-scaffold.md`**
   per [ADR-0221](0221-changelog-adr-fragment-pattern.md).
6. **`docs/adr/_index_fragments/0367-u2netp-fork-mirror-scaffold.md`**
   _order.txt` append per the index-fragment pattern.
7. **`docs/state.md`** Deferred row T6-2a updated to record this
   scaffold landing as partial progress on path (b)
   (`T6-2a-mirror-u2netp-via-release`).
8. **`docs/rebase-notes.md`** entry — describes the new
   `LICENSES/` directory and the release-workflow guard so a
   rebase against upstream Netflix/vmaf knows the surface is
   fork-local-only.
9. **`ai/AGENTS.md`** invariant — `model/u2netp_mirror.onnx` is
   *gitignored* and *never* committed to source control; only
   the metadata + scaffold lives in-tree.
10. ADR-0257 + ADR-0265 each receive a
    `### Status update 2026-05-08: path B scaffold landed`
    appendix per [ADR-0028](0028-adr-maintenance-rule.md)'s
    immutability rule (the body stays frozen; updates land as
    dated appendices, not body edits).

The PR is opened **draft** — final merge gates on the user
confirming three compliance questions captured in §References:
the redistribution-permission read of Apache-2.0 §4, the
release-tag-scheme choice, and the file-format choice
(redistribute the upstream `.pth` verbatim, or redistribute the
ONNX rewrap, or both).

The mirror is positioned as a *fallback* to ADR-0286's
fork-trained student. The recommended path for any new consumer
of `mobilesal`/`saliency_mean` remains `saliency_student_v1`
(fork-owned, BSD-3-Clause-Plus-Patent, no third-party licence
audit needed). The mirror exists for the cases where users
specifically want the upstream u2netp lineage — e.g. citing the
2020 PR paper's published numbers, comparative evaluation
against a recent upstream model release, or because their
downstream pipeline already pins to upstream u2netp behaviour.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Scaffold the mirror in this PR; user uploads binary later** (this ADR) | Releases the scaffold-vs-binary handoff into two reviewable steps; the licence + doc + workflow surface is reviewable now without the binary; release pipeline is idempotent until the binary actually lands | Two PRs to fully ship the mirror; the scaffold sits inactive until the user uploads | **Chosen** — matches the task brief directive ("Do NOT actually upload binary content in this PR ... ships the SCAFFOLD"); preserves the user-uploads-binary-once workflow established by `tools/ensemble-training-kit/prepare-gdrive-bundle.sh` |
| Bundle scaffold + binary into one PR | One-shot ship; immediate availability of the mirror | Requires the binary to be generated and reviewed in the same PR; user-uploads-once workflow doesn't fit a single-PR shape; reviewers asked to examine a 4.7 MB blob | Rejected — task brief explicitly forbids ("Do NOT actually upload binary content") |
| Don't mirror; rely solely on `saliency_student_v1` (ADR-0286) | Zero new licence-audit surface; one fewer artefact to maintain | Closes off lineage-audit + comparative-evaluation use cases; T6-2a row stays open on path (b); future researcher repeats the licence-compliance walk | Rejected — ADR-0286 is the *primary* path; this ADR is the named *fallback* in the same row |
| Vendor `u2netp.pth` directly into `model/` and commit it as a tracked artefact | One-shot ship; immediate availability | Bloats the git history with a 4.7 MB binary; CI clones get bigger forever; conflicts with `.gitignore` patterns under `model/` | Rejected — the fork's release-attachment + Sigstore pattern (used for `libvmaf.so`, `vmaf` CLI, `models.tar.gz`) is the right shape |
| Mirror via `git-lfs` | Tracked-but-not-bloated; clone-on-demand | Adds `git-lfs` to the fork's contributor toolchain; LFS storage is GitHub paid feature; release attachments are free + already wired to Sigstore | Rejected — release attachments are the cheaper, simpler shape that already exists |
| Email `xuebinqin/U-2-Net` upstream and ask them to cut a GitHub release | Cleanest; removes the mirror need entirely | Out-of-band ask with no commitment; upstream maintainer turnaround time unknown; doesn't unblock the path in any defined timeline | Filed as long-shot follow-up; this PR does not depend on it |
| Mirror through a non-GitHub artefact host (S3, IPFS, Drive) | Bypasses GitHub release-attachment storage limits | Adds a new infrastructure surface the fork doesn't have today; complicates licence-attribution traceability; loses Sigstore + SLSA wiring this PR inherits for free | Rejected — GitHub release attachments are already in the fork's supply-chain pipeline (see `.github/workflows/supply-chain.yml`) |
| Redistribute only the ONNX rewrap, not the upstream `.pth` | Smaller asset; matches the format the C-side actually loads | Loses lineage clarity for downstream consumers verifying the rewrap was honest; the ONNX-export script becomes the source of truth for "what the weights actually are" | Open question — flagged in §References as a compliance question for the user |
| Redistribute both `.pth` and `.onnx` | Best lineage; verifiable rewrap | Doubles the artefact count + Sigstore-sign surface | Open question — flagged as compliance question for the user |

## Consequences

- **Positive**:
  - T6-2a row records progress on path (b) — the scaffold lands
    as a defined milestone even before the binary upload.
  - Future researchers wanting the upstream u2netp lineage have
    a stable internal URL to pin against.
  - `LICENSES/` becomes the canonical directory for third-party
    license bundles, ready to absorb future tiny-AI imports
    without re-litigating directory structure.
  - The scaffold is *reviewable now* — licence text, model card,
    operator doc, workflow guard — without waiting on the binary.
  - Apache-2.0 §4 NOTICE compliance is documented inline so the
    next person doing this for another model sees the worked
    example.
- **Negative**:
  - Two-PR shape means the mirror isn't fully shipped until the
    binary lands. Until then, `model/u2netp_mirror.onnx` is
    documented but not present.
  - One more piece of release-pipeline plumbing to maintain. If
    the upload step's idempotence guard breaks, releases could
    fail silently when the binary is absent.
- **Neutral / follow-ups**:
  - **Binary upload PR** — the user uploads `u2netp_mirror.onnx`
    (or `.pth`, or both — pending compliance-question answer) to
    the dedicated release tag. The scaffold's idempotent guard
    flips from no-op to active.
  - **Sibling consumer ADR** (deferred) — once the binary lands
    and is signed, a follow-up ADR registers `u2netp_mirror_v1`
    in `model/tiny/registry.json` as an alternative weights
    drop-in for `feature_mobilesal.c`. Not bundled here because
    the scaffold-only PR's review surface is the licence + the
    plumbing, not the runtime registration.
  - **`saliency_student_v1` remains the recommended path**.
    `docs/ai/u2netp-mirror.md` makes this prominently clear in
    its "When to use this" section.

## References

- [ADR-0218](0218-mobilesal-saliency-extractor.md) — original
  MobileSal extractor design with the smoke-only placeholder.
- [ADR-0257](0257-mobilesal-real-weights-deferred.md) — defers
  MobileSal real weights; points at u2netp as the recommended
  replacement; this ADR is partial progress on T6-2a path (b).
  Receives a `### Status update 2026-05-08: path B scaffold
  landed` appendix in the same PR.
- [ADR-0258](0258-onnx-allowlist-resize.md) — `Resize` added to
  the op allowlist; resolves ADR-0265's axis-2 blocker.
- [ADR-0265](0265-u2netp-saliency-replacement-blocked.md) — the
  decision this ADR partially unblocks (path-b distribution
  axis); receives the same status-update appendix.
- [ADR-0286](0286-saliency-student-fork-trained-on-duts.md) —
  ADR-0286 ships the *primary* replacement path
  (`saliency_student_v1`); this ADR is the explicit *fallback*.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — 5-point
  doc-substance bar this PR's model card satisfies.
- [ADR-0100](0100-project-wide-doc-substance-rule.md) —
  per-surface doc minimum bar; the operator workflow doc lands
  under `docs/ai/`.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six
  deliverables; the digest is
  [Research-0086](../research/0086-u2netp-fork-mirror-license-compliance.md).
- [ADR-0221](0221-changelog-adr-fragment-pattern.md) — fragment
  format for the `changelog.d/` entry.
- [Research-0086](../research/0086-u2netp-fork-mirror-license-compliance.md)
  — Apache-2.0 §4 compliance walk + redistribution surface
  audit + Sigstore-vs-cosign verify-blob recipe survey.
- [`.github/workflows/supply-chain.yml`](../../.github/workflows/supply-chain.yml)
  — the existing Sigstore + SLSA pipeline this scaffold extends.
- [`tools/ensemble-training-kit/prepare-gdrive-bundle.sh`](../../tools/ensemble-training-kit/prepare-gdrive-bundle.sh)
  — the user-uploads-binary-once workflow this PR's operator
  documentation mirrors structurally.
- Upstream code: <https://github.com/xuebinqin/U-2-Net> (HEAD
  `ac7e1c81`, SPDX = Apache-2.0).
- Upstream paper: Qin, Zhang, Huang, Dehghan, Zaiane, Jagersand,
  *"U^2-Net: Going Deeper with Nested U-Structure for Salient
  Object Detection"*, Pattern Recognition 2020.
- **Open compliance questions** for the user to confirm before
  merging this draft (paraphrased — collected for reviewer
  visibility, not yet answered):
  1. Apache-2.0 §4 redistribution: the fork's read is that the
     mirror needs `LICENSES/Apache-2.0-u2netp.txt` + an
     attribution block in the model card; no `NOTICE` file
     pass-through is required because upstream ships none.
     Confirm or flag a stricter read.
  2. Tag scheme: `u2netp-mirror-v1` (this ADR's recommendation),
     or fold into the main `vX.Y.Z-lusoris.N` tag, or a separate
     scheme entirely?
  3. Artefact format: redistribute the upstream `.pth` verbatim,
     the ONNX rewrap only, or both? Compliance treats the rewrap
     as a derivative work either way; the question is review
     surface vs lineage clarity.
- Source: paraphrased — task brief directive ("establish a
  fork-local release-artefact mirror for `u2netp.pth` under
  Apache-2.0 §4 NOTICE compliance ... ships the SCAFFOLD:
  license, doc, release-workflow step, model card stub").
