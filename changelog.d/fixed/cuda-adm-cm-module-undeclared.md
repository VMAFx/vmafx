**CUDA `integer_adm_cuda.c` `adm_cm_module` undeclared identifier** — PR #516
(GPU resource-leak bundle, commit `aa177510d`) added `cuModuleUnload` teardown
via `s->adm_cm_module` correctly in `close()`, and loaded it correctly via
`cuModuleLoadData(&s->adm_cm_module, ...)` in `init()`. However, four
`cuModuleGetFunction` call-sites in the same `init()` block referenced the bare
symbol `adm_cm_module` (without `s->` prefix), which is not a local variable or
parameter — only a struct member. GCC emitted `error: use of undeclared
identifier 'adm_cm_module'` on CUDA-enabled builds. PR #693 fixed the distinct
`VmafCudaFunctions` typo in the same file but missed these four sites. Fix:
prefix all four occurrences with `s->` to match the struct-member access pattern
used consistently for `s->adm_csf_module`, `s->adm_csf_den_module`, and
`s->adm_dwt_module` in the same function.
