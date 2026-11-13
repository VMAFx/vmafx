---
paths:
  - core/src/picture.h
  - core/src/picture_pool.h
invariant: Internal core/src headers carry Doxygen file briefs and parameter comments.
area: engine
---
<!-- markdownlint-disable MD013 -->
# Doxygen internal header briefs and parameter documentation

## Doxygen comment invariant (ADR-1096)

Following `core/src/*.h` internal headers now carry Doxygen `@brief`,
`@param`, and `@return` annotations: `framesync.h`, `thread_pool.h`,
`picture_pool.h`, `predict.h`, `fex_ctx_vector.h`, `ref.h`, `mem.h`,
`log.h`, `opt.h`, `dict.h`.

**Invariant for rebases and follow-up branches**: when adding, renaming, or
removing function signatures in these headers, update corresponding
Doxygen block in same commit. Dangling `@param` for deleted argument
or missing `@param` for new one = docs regression. Run
`doxygen Doxyfile 2>&1 | grep warning` to check — zero new warnings =
bar.
