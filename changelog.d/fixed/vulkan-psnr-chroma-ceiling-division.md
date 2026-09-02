# fix(vulkan/psnr): use ceiling division for chroma plane geometry on odd-dimension YUV420

`psnr_vulkan.c::init()` used floor division (`w / 2U`, `h / 2U`) to derive
chroma plane width and height for subsampled YUV420 inputs.  The CPU reference
(`integer_psnr.c::init`) and the CUDA twin (`cuda/integer_psnr_cuda.c:132`)
both use ceiling division — `(w + ss) >> ss` — matching the MPEG subsampling
convention and the YUV plane sizes that the picture allocator and the FFmpeg
import path actually produce.

On odd-dimension inputs (e.g. 1921×1080, 999×540) the floor form made chroma
planes one pixel narrower (and/or shorter) than the reference.  PSNR was then
summed over a different sample count, diverging from the CPU/CUDA value and
causing the cross-backend `places=4` parity gate to fail on those widths.
Even-dimension inputs were unaffected.

The fix replaces both floor expressions with the ceiling form in
`core/src/feature/vulkan/psnr_vulkan.c` and updates the surrounding
comment and the file-level header.

Surfaced by the C feature-twin dedup audit (`.workingdir/dedup-audit-c-feature-twins-2026-05-16.md`
finding #5); algorithm correctness documented in Research-0094.

Regression test: `core/test/test_psnr_vulkan_chroma_geom.c` — pure-C
geometry unit test for 1921×1081 and 999×540 YUV420 that does not require a
Vulkan device.
