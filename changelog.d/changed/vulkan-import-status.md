### Vulkan image-import entry points marked fully implemented

The public header `core/include/libvmaf/libvmaf_vulkan.h` previously
contained stale `@return -ENOSYS until T7-29 part 2 lands` comments on
`vmaf_vulkan_import_image`, `vmaf_vulkan_wait_compute`, and
`vmaf_vulkan_read_imported_pictures`. All three functions have been fully
implemented since ADR-0186 / ADR-0251 landed. The stale comment has been
removed; the `@return` lines now document only the real error codes.

The T-VK-T7-29-PART-2-IMPORT-NOT-IMPL tracking row in `docs/state.md`
is moved to the Recently Closed section.
