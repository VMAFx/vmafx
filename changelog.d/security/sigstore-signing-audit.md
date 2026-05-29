- **Sigstore signing audit**: resolved stale merge conflict marker in `docs/ai/security.md`;
  updated signing docs to reflect cosign v3 `.bundle` format (replaces legacy `.sig`/`.pem`);
  added full `cosign verify-blob`, `cosign verify`, `cosign verify-attestation`, and
  `slsa-verifier` examples to `docs/development/release.md`; added missing
  `attestations: write` permission and syft SBOM + `cosign attest` steps to the
  `build-rocm6`, `build-oneapi2026`, and `build-vulkan` jobs in
  `docker-publish-production.yml`; replaced the fragile
  `pip install --quiet syft || true` hack in the cuda12 job with the same
  `anchore/sbom-action/download-syft` action used by the cpu job.
