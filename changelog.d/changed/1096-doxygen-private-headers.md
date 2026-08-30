<!-- ADR-1096 -->
Add Doxygen `@brief`/`@param`/`@return` comments to the 10 highest-traffic
core internal headers (`framesync.h`, `thread_pool.h`, `picture_pool.h`,
`predict.h`, `fex_ctx_vector.h`, `ref.h`, `mem.h`, `log.h`, `opt.h`,
`dict.h`). Purely additive; no logic or ABI change. IDE hover-docs and
`doxygen -q` now populate for all covered APIs.
