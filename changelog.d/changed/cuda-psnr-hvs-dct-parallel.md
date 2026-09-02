Parallelise the integer DCT stage inside `psnr_hvs_cuda` blocks while
preserving the existing thread-0 float reduction order and feature
output contract.
