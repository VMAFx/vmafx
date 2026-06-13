Fix UBSan `-fsanitize=function` violations in `vidinput.c` vtbl dispatch:
`yuv_input.c` and `y4m_input.c` functions registered in `YUV_INPUT_VTBL` /
`Y4M_INPUT_VTBL` used concrete `yuv_input *` / `y4m_input *` parameter types
instead of the erased `void *` mandated by the typedef in `vidinput.h`.
UBSan detected these as type-mismatched indirect calls on every frame read.
Added vtbl-compatible static wrapper functions; removed C-style casts from
both VTBL initialisers. Netflix golden scores unchanged.
