- **`--tiny-model-verify` accepts only VMAFx's signer, by owner ID.** The
  identity expression libvmaf passed to `cosign verify-blob`
  (`https://github.com/VMAFx/vmafx/.github/workflows/.+`) was not anchored:
  cosign searches the identity for it, so an identity with text before or
  after it passed, and its dots matched any character. libvmaf now reads the
  bundle's one signing certificate first and refuses it (`-EPROTO`) unless its
  identity is the supply-chain workflow on a release tag or `master` and its
  Fulcio owner ID (extension `1.3.6.1.4.1.57264.1.17`) is VMAFx's `288567244`,
  which a later owner of the `VMAFx` login cannot have. cosign then verifies a
  private copy of those bytes with the expression anchored at both ends. The
  guides' identity expressions are anchored too, and
  [the release guide](docs/development/release.md#the-signers-owner-not-only-its-name)
  shows the owner-ID check with `gh attestation verify` and `jq`
  ([ADR-2985](docs/adr/2985-signer-owner-id-binding.md)).
