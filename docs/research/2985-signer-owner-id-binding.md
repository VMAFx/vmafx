<!-- markdownlint-disable MD013 -->
# Research-2985: Binding a Sigstore signer to an owner ID, not a login

Date: 2026-10-09. Decision: [ADR-2985](../adr/2985-signer-owner-id-binding.md).

## Question

Can cosign or gh bind a keyless signature to the GitHub organisation that owns
the repository, so that a later owner of the `VMAFx` login cannot pass, and
what exactly does a VMAFx certificate carry?

## Findings

1. **What a VMAFx certificate carries.** The Fulcio certificate in the
   v1.0.0-rc.3 release bundle `vmaf.bundle` (`gh release download v1.0.0-rc.3
   -R VMAFx/vmafx -p vmaf.bundle`, read with `openssl x509 -text`):
   subject alternative name
   `https://github.com/VMAFx/vmafx/.github/workflows/supply-chain.yml@refs/tags/v1.0.0-rc.3`,
   extension `.1.12` (Source Repository URI) `https://github.com/VMAFx/vmafx`,
   `.1.16` (Source Repository Owner URI) `https://github.com/VMAFx`, and
   `.1.17` (Source Repository Owner Identifier) `288567244`. The `.1.17` value
   equals `gh api repos/VMAFx/vmafx --jq .owner.id`; the repository ID is
   1252235334. Extensions from `.1.8` on are DER UTF8Strings inside the
   extension's OCTET STRING (Fulcio's OID document); the bytes confirm `0C 09`
   before `288567244`.
2. **cosign 3.1.3 has no owner option.** `cmd/cosign/cli/options/certificate.go`
   at tag v3.1.3 registers `--certificate-identity(-regexp)`,
   `--certificate-oidc-issuer(-regexp)` and `--certificate-github-workflow-`
   `trigger`, `sha`, `name`, `repository`, `ref` (the deprecated extensions
   `.1.2` to `.1.6`, all names). Nothing reads `.1.17`.
3. **cosign's identity expression is a search.** Go's `regexp.MatchString`
   matches anywhere in the text, so an expression without `^` and `$` accepts a
   longer identity. The library's expression
   `https://github.com/VMAFx/vmafx/.github/workflows/.+` therefore accepted
   `https://evil.example/https://github.com/VMAFx/vmafx/.github/workflows/x`,
   `...supply-chain.yml@refs/heads/master/evil`, and `githubXcom` in place of
   `github.com`.
4. **gh reports the owner ID.** `gh attestation verify vmaf -R VMAFx/vmafx
   --format json` (gh 2.102.0) returns
   `.verificationResult.signature.certificate.sourceRepositoryOwnerIdentifier`
   = `"288567244"` for the rc.3 binary. gh has no flag that enforces it;
   `jq -e` on that field does. This works only for attested artefacts, not for
   `cosign sign-blob` bundles such as a model's.
5. **openssl cannot isolate the extension.** `openssl x509 -ext
   1.3.6.1.4.1.57264.1.17` (OpenSSL 3.6.5) prints "No extensions in
   certificate"; only `-text` shows it, among extensions whose values an
   attacker controls (the workflow name is free text), so a `grep` over the
   text is spoofable.
6. **Bundle shape.** The rc.3 bundles are Sigstore bundle v0.3 with one
   `verificationMaterial.certificate.rawBytes`, no `raw_bytes` spelling and no
   `\u` escape; their only backslashes are `\n` in the Rekor checkpoint. A
   decoder of the bundle JSON (protojson) also accepts the proto field name and
   `\u` escapes in keys, so a reader that looks for one literal key must refuse
   both to read the certificate cosign reads.
7. **Renames free the login.** GitHub's "Renaming an organization" page: the
   old name becomes available to anyone, repository redirects stop when a
   repository of the same name is created there, and API requests with the old
   name return 404. Container packages move to the new name without a
   redirect.

## Conclusion

Bind the owner ID in libvmaf, on the bytes cosign verifies, and anchor every
expression. Guides add the `gh` + `jq` owner check where an attestation exists.
