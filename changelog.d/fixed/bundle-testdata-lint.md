## Testdata hygiene + GPU slab ptr-cast bundle

- **#161 (SKIPPED-CONFLICT)** — testdata hygiene pass: delete orphan JSONs,
  gitignore ad-hoc benchmark results, fix hardcoded `/home/kilian/dev/libvmaf_vulkan/`
  paths in `bench_quick.py`, `compare_combined.py`, and `test_all_backends.sh`, add
  `testdata/README.md` (ADR-0813). Branched before sweep commit 24bb5daf89; 3-way
  merge produced unresolvable conflicts across 119 hunk sites. Requires rebase before
  re-application.
- **#194 (SKIPPED-CONFLICT)** — GPU slab ptr-cast macro `SLAB_FIELD`: centralise
  `CUdeviceptr`/`uintptr_t` → typed-pointer casts; replace 21 bare
  `performance-no-int-to-ptr` NOLINTs in `integer_vif_hip.c`, `integer_vif_cuda.c`,
  and `integer_adm_cuda.c`; resolve pre-existing `.semgrepignore` conflict markers
  (ADR-0800). Branched before sweep commit 24bb5daf89; 3-way merge produced
  unresolvable conflicts. Requires rebase before re-application.
