- **The published GPU images were mislabelled and built against a dead runtime.**
  `docker/Dockerfile.node` and `docker/Dockerfile.production-gpu` pulled their
  HIP/HSA runtime libraries from `rocm/rocm-terminal:6.4` — a repository AMD
  abandoned on 2025-04-14 — while the build toolchain is ROCm 7.2.4. That is a
  KFD-ABI major mismatch: 6.x userspace against a 7.x build. Replaced with
  `rocm/dev-ubuntu-24.04:7.2.4`, which matches the toolchain exactly and is
  current (`repo.radeon.com/rocm/apt/latest` serves `rocm-core 7.2.4.70204`).
  ROCm 10 exists as a Docker tag but is not installable through the apt channel
  the image builds with, so pairing it with a 7.2.4 build would recreate the same
  mismatch inverted.
- Published tags no longer lie about their contents: `-cuda12` shipped CUDA
  13.3.1 and `-rocm6` shipped ROCm 7.2.4. The job names, Docker targets, SBOM
  filenames and tag suffixes are renamed to `cuda13` / `rocm7` / `oneapi2025`.
  The CUDA runtime `COPY --from` also moves off a stray `ubuntu22.04` base to
  `ubuntu24.04`, matching its siblings.
- **The GPU image builds were never gated.** `docker-publish-production.yml`'s
  `all-images` job failed only on `build-cpu` and `smoke-test`; the CUDA, ROCm,
  oneAPI and server results were echoed to stdout and discarded. They could fail
  on every single publish without turning the workflow red — which is how a CUDA
  12.0 pin, an `intel/oneapi-basekit:2026.0` reference to an image that does not
  exist, and the ROCm 6.4-vs-7.2.4 mismatch all survived. They gate now.
- Deletes four dead wrapper Dockerfiles (`Dockerfile.node-cpu`, `-cuda12`,
  `-rocm6`, `-sycl-oneapi2026`). Their `FROM docker/Dockerfile.node` is not a
  valid image reference, so none of them could ever build; nothing referenced
  them but a line in `docs/rebase-notes.md`. The publishing workflow already
  uses `-f docker/Dockerfile.node --target <stage>` directly.
