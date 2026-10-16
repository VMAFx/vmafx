## VmafPicture keeps no colour member after upstream 0b9462873 (2026-10-15)

Netflix/vmaf `0b9462873` moves `VmafColor color` to the end of upstream's
`VmafPicture`. A sync keeps the fork's struct without the member
(ADR-1822, status update 2026-10-15) and does not port
`vmaf_picture_convert_context_init()`; `test_picture_no_color_member` fails
when a `color` member returns to `core/include/libvmaf/picture.h`.
