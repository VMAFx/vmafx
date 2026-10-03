<!-- markdownlint-disable MD013 -->
# Research-2139: what an AMD GPU tester image needs at run time, and the source its LGPL parts owe

- **Date**: 2026-10-03
- **Status**: input to [ADR-1511](../adr/1511-amd-gpu-tester-image.md)
- **Measured on**: the `gfx1036` graphics of the Ryzen 9 9950X3D in `ryzen-4090-arc`
  (RDNA2, 2 compute units, Linux 7.2.8, `amdgpu`), Docker 29.8.2, in the image built from
  `docker/Dockerfile.tester --target final-hip` (ROCm 10.0.0 runtime; the host has ROCm
  7.2.4, which the image does not use).

## Question

Which ROCm files does a HIP build of libvmaf load, under which licences, what does
their copyleft part require, which AMD GPUs can one image serve, and does WSL2 work?

## Sources

- The pinned ROCm image (`ROCM_BUILDER`): `share/therock/therock_manifest.json`
  (ROCm 10.0.0, TheRock `16adc4d875fd4f65ea23c7c84e1c66706fde3047`, rocm-systems
  `6b0e43f3…`, llvm-project `8f497e09…`), `share/therock/dist_info.json` (the 25 targets
  ROCm builds its own libraries for), `share/doc/*/LICENSE*`, `readelf -d` of every
  library below.
- TheRock at that commit, `third-party/sysdeps/*/CMakeLists.txt`: elfutils 0.195
  (SHA-512 pinned), numactl 2.0.19, libdrm 2.4.134, zlib 1.3.2, zstd 1.5.7, XZ Utils
  5.8.1, bzip2 1.0.8, from TheRock's S3 mirror; the upstream elfutils and numactl
  archives hash to the same values.
- elfutils 0.195 `libelf/*.c` headers (LGPL-3.0-or-later OR GPL-2.0-or-later); numactl
  2.0.19 `libnuma.c` (LGPL-2.1).
- AMD's ROCm-on-WSL documentation and `librocdxg`: under WSL2 the Linux HSA runtime does
  not work; AMD's `hsa-runtime-rocr4wsl-amdgpu` and `librocdxg` replace it.

## Findings

1. **The runtime closure.** `libvmaf.so` needs `libamdhip64.so.7`; ROCm 10.0.0's
   `libamdhip64` needs `libhsa-runtime64`, `libamd_comgr`, `librocprofiler-register`,
   `librocm_kpack`; `libamd_comgr` needs `libLLVM.so.23.0git` and `libclang-cpp.so.23.0git`;
   the HSA runtime and these need eight system libraries TheRock builds with renamed
   sonames (`librocm_sysdeps_{elf,numa,drm,drm_amdgpu,z,zstd,liblzma,bz2}`). Each finds the
   next through an RPATH relative to itself (`$ORIGIN/llvm/lib`,
   `$ORIGIN/rocm_sysdeps/lib`), so the files keep their directories and need no
   `patchelf`. 261 MB in all, `libLLVM` 132 MB of it. `libdrm_amdgpu` carries its ID table
   inline (`inline_amdgpu_ids`). In the built image every binary resolves every library
   (`ldd`), and AMD built them to glibc 2.28, so they run on Debian 13.
2. **Licences.** CLR (`libamdhip64`), rocprofiler-register, kpack: MIT; ROCR: NCSA;
   comgr: Apache-2.0 WITH LLVM-exception (its installed `LICENSE.txt`); LLVM and Clang:
   Apache-2.0 WITH LLVM-exception (no text installed; fetched at the llvm-project pin).
   Bundled: libelf LGPL-3.0-or-later OR GPL-2.0-or-later, libnuma LGPL-2.1-only, libdrm
   and libdrm_amdgpu MIT, zlib Zlib, zstd BSD-3-Clause OR GPL-2.0-only, liblzma 0BSD,
   libbz2 bzip2-1.0.6; ROCm installs none of their texts, so they are fetched at the
   release tags and pinned by SHA-256. The LGPL source: the upstream archive plus TheRock
   at the commit (it patches numactl and runs `patch_source.sh` on elfutils before
   building), recorded by the ELF build IDs `11dee761…` (libelf) and `ec55f204…`
   (libnuma). `licensing.py fetch-sources` downloads the three archives and checks each
   hash.
3. **Targets.** HSA loads a code object only for the device's exact gfx target, so the
   image builds 18: `gfx908`, `gfx90a`, `gfx942`, `gfx950`, `gfx1030`, `gfx1031`,
   `gfx1032`, `gfx1034`, `gfx1035`, `gfx1036`, `gfx1100`, `gfx1101`, `gfx1102`, `gfx1103`,
   `gfx1150`, `gfx1151`, `gfx1200`, `gfx1201` (ROCm 10.0.0's list without `gfx1010` to
   `gfx1012`, `gfx1033`, `gfx1152`, `gfx1153` and `gfx1250`). The HIP part of the build
   takes about one minute at `-j6`; the 110 test executables hold 1.3 GB.
4. **The gfx1036 run.** The documented command (`--read-only --cap-drop ALL
   --security-opt no-new-privileges --tmpfs /tmp --device /dev/kfd --device /dev/dri
   --group-add 988`, uid 10001), `flock ... timeout 300`, exit 0 in 1 min 43 s: verdict
   `pass`; the HSA probe finds one GPU agent (`gfx1036`, 2 CUs, HSA runtime 1.21) and the
   run is pinned with `ROCR_VISIBLE_DEVICES=0`; 17 extractors on HIP, 4332 values per run
   of the four fixtures, 0 differing; `adm` and `float_vif` keep their CPU extractor under
   `--backend hip` (`float_vif_hip` carries the HIP dispatch flag only with
   `-Denable_float_vif_hip_autodispatch=true`, off by default; `adm_hip` carries none and
   emits no AIM / `adm3`, `T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05`), so the
   twin comparison lists
   them as `not_in_run` and the gate measures them: 98 gate cells OK at 0 and 2 SKIP
   (`float_ms_ssim_chroma` on the 576x324 pairs); 73 device tests passed, 0 failed, 0
   skipped; the RDNA2 part of `T-HIP-TWINS-OTHER-TARGETS-2026-10-03` passes with 112
   items of evidence. A second run (with a planted reference) passed the GPU section too.
   Without the devices: verdict `pass`, `gpu.status` `no_device`, reason "no /dev/kfd is
   visible: add --device /dev/kfd --device /dev/dri (Linux, amdgpu driver)". With one
   planted wrong reference value: exit 1, `reference_equivalence` names `psnr_y`, frame 2
   and both values. No dropped dispatch (`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`)
   appeared.
5. **The licence gate fails closed.** On the exported tree `licensing.py check --artifact
   hip-image` passes; it fails on a planted unrecorded library, on a `libelf` of another
   build ("copyleft librocm_sysdeps_elf.so.1 (build ID 247a106c…) has no recorded source"),
   on a recorded bundled library that is gone, and on a removed `libnuma` licence text.
6. **Size.** 1.93 GB unpacked (`docker export`), 611 MB compressed content.

## Open questions

- CDNA, RDNA3, RDNA3.5 and RDNA4: the measurements the kit exists for; CDNA is the first
  wave64 run of the twins' reductions.
- WSL2: unsupported with this runtime; a Windows kit (later lane) or AMD's WSL runtime
  would be its own record.
