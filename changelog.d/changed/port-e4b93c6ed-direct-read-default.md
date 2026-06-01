## tools/vmaf: enable direct read by default (port e4b93c6ed)

Remove the `USE_DIRECT_READ` compile-time guard; `video_input_fetch_into_vmaf_picture()`
is now the only input path. The `copy_picture_data()` / `video_input_fetch_frame()`
legacy path is removed. `fetch_picture()` no longer accepts a `depth` argument.
