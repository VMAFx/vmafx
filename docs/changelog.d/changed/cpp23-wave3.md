<!-- markdownlint-disable MD041 -->
## C++23 Wave 3: feature_name, picture_copy, model (ADR-0729)

- `core/src/feature/feature_name.c` → `.cpp`: `goto`-cleanup replaced with
  `std::unique_ptr<VmafDictionary, DictDeleter>` RAII; `[[nodiscard]]` on
  allocation functions; fixes a latent use-after-free in the dict-realloc
  error path (dict pointer could be double-freed under RAII if naively reset
  inside the loop).
- `core/src/feature/picture_copy.c` → `.cpp`: `std::span` for per-row
  iteration bounds; `static_cast<>` replaces C-style casts; `else`-if chain
  replaced with early returns.
- `core/src/model.c` → `.cpp`: multi-label `goto` in
  `vmaf_model_collection_append` replaced with structured early-return flow;
  `malloc`+`memset` pairs replaced with `calloc`; `[[nodiscard]]` on
  `vmaf_model_generate_name`.
- Internal headers `log.h`, `read_json_model.h`, `opt.h`, `alias.h`,
  `feature_extractor.h` gain `extern "C"` guards, enabling direct inclusion
  from C++ translation units.
- `picture_copy.h` gains include guards (previously missing).
- Test files `test_feature.c` and `test_model.c` renamed to `.cpp` to match
  their unity-included implementation sources.
