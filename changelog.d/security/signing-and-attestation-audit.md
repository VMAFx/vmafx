- **Signing and attestation audit closes three residual gaps (ADR-0902)** —
  Container images now carry a GitHub-native
  `actions/attest-build-provenance@v4.1.0` attestation alongside the existing
  cosign keyless signature, so consumers can verify with
  `gh attestation verify oci://ghcr.io/vmafx/vmafx@sha256:DIGEST --repo VMAFx/vmafx`.
  The post-push `smoke-test` job now verifies the cosign signature with
  `cosign verify --certificate-identity-regexp …` before pulling and
  running the image, closing the gap where a compromised CI token could
  push an unsigned image and still pass smoke tests.
  `docs/development/release.md` ships a copy-pasteable consumer
  verification recipe for release blobs, the `vmaf-mcp` wheel, and
  container images (both cosign and `gh attestation verify` routes).
  Tag signing, DCO sign-off, Helm chart signing, and standalone Go
  binary releases remain scoped out — see ADR-0902 §Alternatives
  considered and the audit digest under `docs/research/` for the full
  rationale.
