- **`vmaf_picture_wrap()`: score frames you already hold, without a copy
  (Netflix/vmaf `700124a4c`).** A `VmafPicture` can now sit on planes the
  caller owns, such as a decoder's frame; a callback reports when libvmaf no
  longer reads them. Same struct and signature as upstream; the fork rounds
  odd chroma planes up, as `vmaf_picture_alloc()` does, and refuses a size of
  0, a NULL plane or a stride shorter than a row with `-EINVAL`
  ([Wrap your own planes](docs/api/pictures.md#wrap-your-own-planes-vmaf_picture_wrap)).
