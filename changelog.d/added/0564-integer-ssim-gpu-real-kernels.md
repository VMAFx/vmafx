Real integer_ssim GPU kernels on CUDA, HIP, and SYCL backends — replaces the silent
float_ssim substitution that caused `--feature ssim` to report float_ssim scores on
every GPU backend. CUDA is bit-exact with CPU (places=6, diff=0 on Netflix golden
fixture); HIP uses the same int64 algorithm (places=6 target); SYCL uses int64 moments
with float32 SSIM formula due to the Arc A380 fp64-free constraint (ADR-0220), reaching
places=4–5. (ADR-0564.)
