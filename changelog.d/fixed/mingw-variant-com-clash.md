- Test: the Windows MinGW CUDA build compiles the test suite again. `test_metal_psnr_hvs_math.c`
  declared a static array named `VARIANT`, which collided with the Windows COM `VARIANT` typedef
  that reaches the translation unit through `nvEncodeAPI.h`; the array is now named `VARIANT_TABLE`.