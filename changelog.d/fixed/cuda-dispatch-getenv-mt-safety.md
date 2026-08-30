- **CUDA `dispatch_strategy.c` `getenv()` thread-safety (round-5 clang-tidy
  `concurrency-mt-unsafe` sweep).** `vmaf_cuda_select_strategy()` called
  `getenv("VMAF_CUDA_DISPATCH")` on every invocation, which is not MT-safe per
  POSIX.1-2008 §2.2.2 if another thread concurrently calls `setenv`/`putenv`.
  Fixed by caching the env-var value at the first call using a `pthread_once`
  guard (`g_env_once` / `cache_env_dispatch`). The cached string is
  `strdup`-copied so it remains valid even if the calling application later
  modifies the environment. Caller contract documented: set
  `VMAF_CUDA_DISPATCH` before the first CUDA frame is submitted.
