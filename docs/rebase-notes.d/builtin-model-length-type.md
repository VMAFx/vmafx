## Built-in model lengths are `unsigned int` (2026-11-13)

- `core/src/model.c` declares every `src_*_json_len` as
  `extern const unsigned int` and types `VmafBuiltInModel.data_len` as
  `const unsigned int *`, matching the `unsigned int` that `xxd -i` emits.
  The two buffer readers take the length as `(int)*data_len`, the same cast
  `core/src/vmafx/model.c` already uses.
- **On sync**: Netflix's `libvmaf/src/model.c` still declares these as
  `extern const int`. Keep the fork's `unsigned int` when a port adds a model
  or conflicts in this block; a new `extern const int ..._json_len` in the
  table trips `-Wpointer-sign` under `-Dwerror=true`, and on MSVC 19.52 it
  is warning C4744 under `/LTCG`.
