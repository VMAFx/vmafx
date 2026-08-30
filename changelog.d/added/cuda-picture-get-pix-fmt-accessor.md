Add `vmaf_cuda_picture_get_pix_fmt` accessor to `picture_cuda.h/c`.
Provides a clean, parallel accessor to the existing `vmaf_cuda_picture_get_stream`,
`vmaf_cuda_picture_get_ready_event`, and `vmaf_cuda_picture_get_finished_event`
helpers, consolidating the repeated `pic->pix_fmt` open-coded field accesses
across integer_psnr_cuda.c, integer_psnr_hvs_cuda.c, integer_ciede_cuda.c,
and future extractors.
