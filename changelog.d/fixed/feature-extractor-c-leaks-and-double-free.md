- **`feature_extractor.c` — 3 cleanup-path leaks + 1 double-free
  (no-op-today but intent-incorrect).** Identified by c-reviewer
  agent audit 2026-05-30:
  1. `vmaf_feature_extractor_context_create` (line 485 area):
     `vmaf_fex_ctx_parse_options` failure returned the error directly,
     leaking `f->fex->priv`, `f->fex` (the `x` allocation), and `f`.
     Fixed by routing through the existing `free_x` / `free_f` chain
     plus an explicit `free(f->fex->priv)`.
  2. `get_fex_list_entry` (line 761 area):
     `vmaf_dictionary_copy(&opts_dict, &entry.opts_dict)` return value
     was silently discarded; a copy failure left `entry.opts_dict` in
     a partially-constructed state and the subsequent equality
     comparison at line 750 would lie. Now checked.
  3. `get_fex_list_entry` (line 777 area):
     realloc failure jumped to `free_ctx_list` which freed
     `entry.ctx_list` but left `entry.opts_dict` leaked. New
     `free_opts_dict` label between `free_ctx_list` and the realloc
     site closes the gap.
  4. `vmaf_fex_ctx_pool_destroy` (line 941 area):
     `vmaf_dictionary_free(&pool->fex_list[i].opts_dict)` was inside
     the per-ctx-slot `j` loop. The dict is held once per entry, not
     per slot — so the call ran `capacity` times per entry. The
     second-and-later calls were `free(NULL)` no-ops (the first call
     nulls the pointer), so no actual UB shipped, but the intent was
     clearly per-entry. Moved out of the `j` loop.
