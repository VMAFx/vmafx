<!-- markdownlint-disable MD013 MD060 -->
# Research-1590: GPU device code compression

- **Status**: Active
- **Workstream**: [ADR-1590](../adr/1590-device-code-compression.md)
- **Last updated**: 2026-10-04

## Question

Which compression does each device compiler offer for the GPU code it embeds,
which setting is the strongest, which drivers and runtimes load the result,
and does any of it change the code the GPU runs?

## Sources

- nvcc 13.4.92 `--help` and the nvcc 13.4 manual
  (<https://docs.nvidia.com/cuda/cuda-compiler-driver-nvcc/index.html>):
  `--compress-mode {default,size,speed,balance,none}` ("not compatible with
  drivers released before CUDA Toolkit's 12.4 Release"; `default` "is currently
  equivalent to `speed`"), `--no-compress`, `--concat` (no driver statement).
  The archived nvcc manuals of 12.3.2 to 12.6.0 do not have `--compress-mode`;
  12.8.0 does.
- `fatbinary --help` (CUDA 13.4.92): `--compress` "Compress ptx and debug
  images in fatbinary", default `true`; `--compress-all` "Compress all images
  in fatbinary".
- CUDA 13.4 release notes
  (<https://docs.nvidia.com/cuda/cuda-toolkit-release-notes/index.html>),
  table 3: 13.x applications run on drivers >= 580 under minor version
  compatibility; CUDA 13.4 features may need R615.
- RAPIDS `enable_fatbin_compression.cmake`
  (<https://github.com/rapidsai/rapids-cmake/blob/main/rapids-cmake/cuda/enable_fatbin_compression.cmake>):
  always `-Xfatbin=-compress-all`; size mode needs driver 550.54.14+ under
  CUDA 12, 580+ under CUDA 13.
- ROCm clang 22 (ROCm 7.2.4) `--help-hidden`: `--offload-compress`,
  `--offload-compression-level=<value>` ("HIP only"). LLVM
  `clang/lib/Driver/OffloadBundler.cpp`: zstd with long-distance matching,
  default level 3, zlib fallback; `Options.td`: the level option exists from
  LLVM 19, `--offload-compress` from LLVM 18. ROCm "Code portability and
  compression"
  (<https://rocm.docs.amd.com/projects/llvm-project/en/docs-7.2.4/conceptual/code-portability.html>):
  decompression happens once per bundle at load time.
- icpx 2026.0 `--help-hidden` and the intel/llvm SYCL users manual
  (<https://github.com/intel/llvm/blob/sycl/sycl/doc/UsersManual.md>):
  `--offload-compress` (zstd, images over 512 bytes) and
  `--offload-compression-level=<int>` (default 10).
- AdaptiveCpp 25.10 `acpp --help`: no compression option; its deployment
  guide suggests a binary packer for the runtime libraries instead.

## Findings

Measurements are in [ADR-1590](../adr/1590-device-code-compression.md); the
reproduction commands are below.

1. nvcc stored the cubins raw. Of the 14.6 MB of the 21 uncompressed fatbins,
   nvcc's default compressed only the two PTX entries per kernel (10.97 MB).
   `--compress-mode=size` compresses every entry; with it `-Xfatbin=-compress-all`
   produced byte-identical fatbins here, and is kept so that no entry is left
   out by a size heuristic of another nvcc.
2. `--concat` packs the six cubins of a kernel before compressing them and
   saves another 24%, and the RTX 4090 on driver 615 loads it, but NVIDIA
   gives no driver floor for this nvcc 13.4 addition.
3. hipcc's bundle compression is zstd: level 22 gives 1.088 MB for 17.9 MB of
   code objects (level 3, clang's default: 1.29 MB). Level 19 and 22 differ by
   a few hundred bytes; 22 is zstd's maximum and costs no measurable compile
   time on these sizes.
4. icpx generates the SPIR-V fallback image at the final link, also in an AOT
   build under `-fno-sycl-rdc`: an AOT object linked without
   `--offload-compress` gives a binary whose `__CLANG_OFFLOAD_BUNDLE__sycl-spir64`
   section starts with the SPIR-V magic, the same object linked with the flag
   gives a zstd frame. ADR-1360 put the flag on the compile line only, so every
   binary carried a raw SPIR-V image (1.9 MB in `libvmaf.so`).
5. Compressing changes no device code: the decompressed cubins and PTX are
   byte-identical to an uncompressed build's, and the HIP code objects differ
   only in the name of `__hip_cuid_<hash>`, a hash of the compile options.

## Reproduction

```bash
# CUDA: fatbins of one tree, compressed and not, then the extracted cubins and PTX.
meson setup b-on core -Denable_cuda=true
meson setup b-off core -Denable_cuda=true -Dcompress_device_code=false
ninja -C b-on && ninja -C b-off   # or only the src/*.fatbin targets
for d in b-on b-off; do for f in $d/src/*.fatbin; do
  n=$(basename "$f" .fatbin); mkdir -p "x/$d/$n"
  (cd "x/$d/$n" && cuobjdump -xelf all -xptx all "$OLDPWD/$f"); done; done
diff -r x/b-on x/b-off && echo identical

# HIP: unbundle every target of every bundle and compare the sections.
clang-offload-bundler --type=o --input=b-on/src/psnr_score.hsaco --list
clang-offload-bundler --type=o --input=b-on/src/psnr_score.hsaco \
  --unbundle --targets=hipv4-amdgcn-amd-amdhsa--gfx1036 --output=on.co

# The build-time check, on any build directory.
python3 core/src/check_device_compression.py --stamp /dev/null \
  --fatbin b-on/src/*.fatbin
```

## Alternatives explored

- nvcc `speed` and `balance` modes: 4.35 MB and 3.20 MB against 2.60 MB in
  `size` mode; the module load time of all 21 fatbins is 1.4 ms raw and 3.0 ms
  in `size` mode, which a process start does not notice.
- Leaving clang CUDA and AdaptiveCpp builds uncompressed without an error:
  rejected, a build that cannot honour the option must say so.

## Open questions

- The driver floor of nvcc `--concat`. Enable it once NVIDIA documents the
  floor or an R580 driver is measured loading it.
- The Windows CUDA tester zip itself is measured by its workflow run; the
  figures here are from the Linux build of the same test lists.

## Related

- ADRs: [ADR-1590](../adr/1590-device-code-compression.md),
  [ADR-1360](../adr/1360-sycl-aot-compile-time-device-codegen.md)
