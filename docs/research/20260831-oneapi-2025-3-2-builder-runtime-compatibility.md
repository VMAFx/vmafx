<!-- markdownlint-disable MD013 -->
# Research: oneAPI 2025.3.2 production builder compatibility

## Question

Can the production `-oneapi2025` image adopt Intel's oneAPI Base Toolkit
2025.3.2 patch when Intel has not published a corresponding 2025.3.2 runtime
image?

## Method

The audit queried the live Docker Hub repository APIs for the exact Ubuntu
24.04 basekit and runtime tags and checked the returned platform and digest.
It then built the Dockerfile's `final-oneapi2025` target and exercised its
configured entrypoint without accelerator hardware. The failed first build was
kept as falsifying evidence: Ubuntu supplied Meson 1.3.2 while the source tree
requires Meson 1.4.0 or newer. PyPI's live package API supplied the exact Meson
1.12.0 wheel URL and SHA-256 used by all Ubuntu 24.04 vendor builders.

## Evidence

On 2026-08-31, Docker Hub reported exactly one amd64/Linux manifest for
`intel/oneapi-basekit:2025.3.2-0-devel-ubuntu24.04`, with digest
`sha256:79a5e333ff6f773793d124b78047001c51cbcd53035e5100313abf2f771af95a`.
The same API returned zero tags matching `2025.3.2` in
`intel/oneapi-runtime`; direct manifest inspection of the expected Ubuntu
24.04 tag returned `not found`.

The production tag is therefore explicit about its split patch levels: source
is compiled by the 2025.3.2 basekit and the final stage remains Intel's latest
published Ubuntu 24.04 runtime in the same 2025.3 family, 2025.3.1. Both are
digest-pinned. This is not described as a matching 2025.3.2 runtime.

The first exact-target build stopped at Meson configuration with
`Meson version is 1.3.2 but project requires >= 1.4.0`. CUDA and ROCm use the
same Ubuntu 24.04 package floor, so this was a production GPU-image problem,
not an Intel compiler regression. The Dockerfile now downloads the
`meson-1.12.0-py3-none-any.whl` artifact once through Dockerfile `ADD`, verifies
SHA-256 `71f133147fa0fcfe8f4df49fa1045771064947834538409e5d97b3613aac8b4e`,
and installs that exact wheel without dependencies in each vendor builder.
The 2025.3.2 basekit image's installed compiler identifies itself as
`2025.3.3.20260319`; documentation therefore names the immutable vendor image
tag explicitly rather than presenting the tag as the compiler binary version.

A real score smoke then falsified the image's documented model location:
`cp -r model/ /dist/model/` produced
`/usr/local/share/vmafx/model/model/vmaf_v0.6.1.json`, while both
`VMAF_MODEL_PATH` and the operator guide point to
`/usr/local/share/vmafx/model/vmaf_v0.6.1.json`. Every builder now copies the
contents with `cp -r model/. /dist/model/`, eliminating the unintended nested
directory in the standard CPU image and the CPU, CUDA, ROCm, and oneAPI targets
of the GPU production Dockerfile.

## Alternatives considered

| Alternative | Benefit | Cost / risk | Decision |
| ----------- | ------- | ----------- | -------- |
| Use the 2025.3.2 basekit with the 2025.3.1 runtime and smoke the final image | Takes the patch update while preserving the smaller runtime image | Requires explicit split-version documentation and an executable compatibility check | Selected |
| Defer the basekit update until a 2025.3.2 runtime appears | Keeps identical patch numbers | Leaves the available compiler patch unapplied for an unbounded period | Rejected |
| Ship the 2025.3.2 basekit as the final image | Guarantees the compiler's own runtime files | Greatly enlarges the production image with development tooling | Rejected |
| Copy a hand-selected runtime library closure from the basekit | Could make both sides 2025.3.2 | Recreates the brittle partial-runtime assembly removed by ADR-1129 | Rejected |
| Keep Ubuntu's Meson 1.3.2 | Avoids a Python wheel in the builder | Cannot configure the current source tree, so every affected release image fails before compilation | Rejected |
| Install an unversioned `meson` from PyPI | Clears the immediate version floor | Makes future image rebuilds select a moving build tool | Rejected |

## Verification

The standard `cli` target and all four `linux/amd64` GPU-Dockerfile targets
(`final-cpu`, `final-cuda13`, `final-rocm7`, and `final-oneapi2025`) built
successfully. Each resulting image configured UID/GID `65532:65532`, kept
`/usr/local/bin/vmaf` as its entrypoint, and returned `3.2.1` from
`vmaf --version` without accelerator hardware. All five images then scored the
48-frame fixture through the documented bundled model path; their pooled means
were `94.323010` (standard CPU, GPU-Dockerfile CPU, CUDA, and ROCm) and
`94.323011` (oneAPI). The oneAPI final image also resolved every dynamic
dependency of both `/usr/local/bin/vmaf` and `/usr/local/lib/libvmaf.so.3` with
no `not found` entries, including `libsycl.so.8`, `libur_loader.so.0`, and the
Intel math runtime libraries supplied by the 2025.3.1 runtime image. The
release workflow repeats the driver-independent version check on each exact
published digest.
