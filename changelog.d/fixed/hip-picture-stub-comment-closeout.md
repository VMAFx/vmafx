Corrected stale comment in `core/src/picture.h` for
`VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE`: the comment previously described
`picture_hip.{c,h}` as a stub whose pictures arrive as host-side buffers.
Per ADR-0613 the HIP picture pool is now fully implemented (`hipMalloc` /
`hipFree`); the comment has been updated to reflect the current state.
