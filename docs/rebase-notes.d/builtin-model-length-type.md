## Built-in model lengths are `unsigned int` (2026-11-13)

- `core/src/model.c` declares every `src_*_json_len` as
  `extern unsigned int`, the exact type `xxd -i` emits (no added `const`;
  C11 6.2.7p2), and types `VmafBuiltInModel.data_len` as
  `const unsigned int *` so reads stay read-only. Issue #3440 tracks the
  other xxd declarations that still differ from the generated types.
  The two buffer readers take the length as `(int)*data_len`, the same cast
  `core/src/vmafx/model.c` already uses.
- **On sync**: Netflix's `libvmaf/src/model.c` still declares these as
  `extern const int`. Keep the fork's `unsigned int` when a port adds a model
  or conflicts in this block; a new `extern const int ..._json_len` in the
  table trips `-Wpointer-sign` under `-Dwerror=true`, and on MSVC 19.52 it
  is warning C4744 under `/LTCG`.
