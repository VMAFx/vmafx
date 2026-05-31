<!-- markdownlint-disable MD060 -->
# `binaries/` — pre-compiled `libvmaf` per-platform drop

This directory is **populated by the operator**, not by source control.
The kit ships an empty `.gitkeep` placeholder so the directory exists in
the tarball; each operator builds the libvmaf binary for their platform
once with `build-libvmaf-binaries.sh` and rsyncs the artefact into the
correct subdirectory before tarballing or running the pipeline.

## Layout

```text
tools/ensemble-training-kit/binaries/
├── linux-x86_64-cuda/vmaf       # NVIDIA CUDA path
├── linux-x86_64-sycl/vmaf       # Intel Arc / iGPU SYCL path
├── linux-x86_64-vulkan/vmaf     # vendor-neutral Vulkan path (fallback)
├── darwin-arm64-cpu/vmaf        # macOS Apple Silicon CPU path
├── darwin-x86_64-cpu/vmaf       # macOS Intel CPU path (older Macs)
└── README.md                    # this file
```

## Which subdir to use on which box

| Box                          | Subdir                  | Backend rationale                                   |
|------------------------------|-------------------------|-----------------------------------------------------|
| NVIDIA CUDA Linux            | `linux-x86_64-cuda/`    | `-Denable_cuda=true` — fastest path for NVENC corpora |
| Intel Arc 310 (Linux)        | `linux-x86_64-sycl/`    | `-Denable_sycl=true` — Arc's native compute path     |
| Intel iGPU (Linux)           | `linux-x86_64-sycl/` or `linux-x86_64-vulkan/` | SYCL preferred when oneAPI is installed; Vulkan is the vendor-neutral fallback |
| Apple Silicon (M-series Mac) | `darwin-arm64-cpu/`     | macOS has no CUDA / SYCL / Vulkan-compute path on Apple Silicon as of 2026; CPU NEON SIMD is the supported route |
| Intel Mac (T2)               | `darwin-x86_64-cpu/`    | Same as above; AVX2 SIMD on the CPU path             |

## How to populate

Run the build helper from the repo root:

```bash
bash tools/ensemble-training-kit/build-libvmaf-binaries.sh \
    --platform linux-x86_64-cuda
```

The helper invokes `meson setup` + `ninja` with the right `-D` flags
for the requested platform and copies the resulting `tools/vmaf` binary
into `tools/ensemble-training-kit/binaries/<platform>/vmaf`. Run it
once per box; rsync the populated subdirectory back to the operator who
makes the distribution tarball.

## Why the binaries aren't checked in

Pre-compiled binaries are platform-specific (libc version, GPU runtime
ABI, glibc symbol versioning) — bundling them in the source tree would
break the moment a collaborator's machine has a different toolchain.
The build helper takes ~3 minutes per platform; the operator runs it
once per box and never again.

## What the kit does when a binary is missing

`01-prereqs.sh` detects the platform, picks the matching subdirectory,
and prints which binary is missing (without aborting on _other_
platforms' binaries also being missing — only the locally-relevant one
matters per box). The corpus generator and validator both honour
`LIBVMAF_BIN` as an environment override; setting it explicitly bypasses
this layout entirely.
