- **RC scaffold-stub completion** (ADR-0928 cycle N+1): implemented all five
  `VmafPicture` v2 entry points (`vmaf_picture2_alloc`, `vmaf_picture2_unref`,
  `vmaf_picture_v1_to_v2`, `vmaf_picture_v2_to_v1`, `vmaf_backend_handle_name`)
  in `core/src/picture_v2.c`; wired the header into the meson install target;
  added 10-case unit test (`test_picture_v2`). Previously the header declared
  five symbols that returned `-ENOSYS` and were not linked into `libvmaf.so`.
- **Fixed 13 `ai/scripts` stub scripts** that exited with code 0 (success)
  despite being unimplemented, misleading callers and CI pipelines into
  believing work had completed. All 13 now exit with code 1 and print a
  clear guidance message to stderr.
