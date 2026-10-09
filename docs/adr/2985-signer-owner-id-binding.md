<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2985: A tiny model's signer is VMAFx's owner ID, not only its name

- **Status**: Accepted
- **Date**: 2026-10-09
- **Deciders**: lusoris
- **Tags**: security, ai, supply-chain, dnn

## Context

`--tiny-model-verify` ([ADR-0211](0211-model-registry-sigstore.md)) runs
`cosign verify-blob` with `--certificate-identity-regexp
https://github.com/VMAFx/vmafx/.github/workflows/.+`. cosign searches the
certificate's identity for the expression (Go `regexp.MatchString`), so this
one accepted any identity that contains it: with text in front, after another
repository's path, and with any character where the dots are. Several guides
had the same flaw ([docs/ai/security.md](../ai/security.md), the u2netp mirror
pages, the operator and tester guides).

Even an anchored expression names the organisation only by its login. GitHub
frees a login when an organisation is renamed (GitHub docs, "Renaming an
organization"), and a rename of `VMAFx` is planned. Whoever takes the old login
next can run a workflow at the same path and get a Fulcio certificate with the
same identity. Fulcio also writes the organisation's numeric ID into extension
`1.3.6.1.4.1.57264.1.17` (Source Repository Owner Identifier); VMAFx's is
`288567244` (read from the v1.0.0-rc.3 release certificates). A new owner of the
name gets another ID. cosign 3.1.3, the pinned release, has no option for that
extension; its certificate options are identity, issuer and five
`github-workflow-*` name fields
([research digest](../research/2985-signer-owner-id-binding.md)).

## Decision

libvmaf checks the signing certificate itself before it runs cosign
(`core/src/dnn/signer_identity.c`). It reads the bundle once and accepts it only
when it holds exactly one `rawBytes` certificate and no `\u` escape, the
certificate's subject alternative name is one URI of the form
`https://github.com/VMAFx/vmafx/.github/workflows/supply-chain.yml@refs/tags/v<digit>...`
or `...@refs/heads/master`, and extension `.1.17` is the UTF8String
`288567244`. It then writes those bytes to a private file (`mkstemp(3)`) and
passes that file to cosign together with the same identity as an anchored
expression, `VMAF_DNN_SIGNER_IDENTITY_REGEXP`. A certificate of any other
identity or owner is `-EPROTO`, as a signature cosign rejects. Every identity
expression in the workflows and current guides is anchored with `^` and `$` and
escapes its dots; the guides check the owner ID through `gh attestation verify
--format json` and `jq`, which reports it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Anchor the expression only | One-line change | Still trusts whoever holds the login after a rename | Leaves the rename hole open |
| Pin the owner ID in libvmaf (chosen) | A new owner of the name cannot pass; no new dependency | A small DER and base64 reader in C; the ID changes only if VMAFx moves to another organisation, which then changes this constant | Smallest change that closes both holes |
| Pass `--certificate-github-workflow-repository VMAFx/vmafx` | cosign supports it | Also a name (extension `.1.5`), freed by a rename like the login | Same hole as the expression |
| Trust the old name only for signatures logged before the rename (Rekor time) | No certificate parsing | Needs the rename date in the binary; new signatures under the new name still need a check; time handling in the verifier | More moving parts than an ID that never changes |
| Verify with `gh attestation verify` from libvmaf | gh reports the owner ID | Models are signed with `cosign sign-blob`, not attested; adds a second external tool | Wrong artefact type and a new dependency |
| Let cosign read the bundle file itself | No temporary file | The bundle can change between libvmaf's read and cosign's | The owner check must apply to the bytes cosign verifies |

## Consequences

- **Positive**: a certificate for a renamed or squatted `VMAFx` login cannot
  load a model; the anchored expression refuses prefixed, suffixed and
  look-alike identities; guides show a check a reader can run.
- **Negative**: VMAFx's organisation ID is a constant in libvmaf
  (`VMAF_DNN_SIGNER_OWNER_ID`); moving the repository to a new organisation
  (rather than renaming) needs a release with the new ID before models signed
  there load. Bundles with more than one certificate (Sigstore bundle v0.1
  chains) are refused; `cosign sign-blob` 3.x writes v0.3 bundles with one.
- **Neutral / follow-ups**: the rename itself changes the name in the
  expression, the guides and `VMAF_DNN_SIGNER_IDENTITY_REGEXP` to the new login,
  not the owner ID. No model ships a bundle yet
  (`T-AI-MODEL-SIGNING-BUNDLES-MISSING-2026-10-05`); the workflow that signs
  models must be `supply-chain.yml` on a tag or `master`, or the constant
  changes with it.

## References

- Source: the organisation-rename inventory of 2026-10-09 found the
  unanchored expressions and the name-only identity; hardening requested by
  the maintainer's rename-planning lane ahead of the rename.
- [ADR-0211](0211-model-registry-sigstore.md) (`--tiny-model-verify`),
  [research digest](../research/2985-signer-owner-id-binding.md).
- `core/test/dnn/test_signer_identity.c`, `core/test/dnn/test_tiny_model_verify.c`,
  `core/test/test_signer_identity_regexp_contract.py`.
