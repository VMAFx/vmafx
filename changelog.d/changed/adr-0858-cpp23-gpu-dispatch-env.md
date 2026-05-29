## C++23 conversion of `gpu_dispatch_env.c` (ADR-0858)

`core/src/gpu_dispatch_env.c` converted to `gpu_dispatch_env.cpp` (C++23).
Improvements: `std::mutex` + `std::lock_guard` RAII eliminates the
platform `#ifdef` `CRITICAL_SECTION` / `pthread_mutex_t` bootstrap;
`std::optional<std::string>` replaces `strdup` + nullable `char *`;
`std::string_view` comparisons replace `strcmp` in the fast path;
`[[nodiscard]]` on `vmaf_gpu_dispatch_env_get`. Compiled as an isolated
`gpu_dispatch_env_cpp23_lib` static library (same pattern as ADR-0708
`metadata_handler_cpp20_lib`) so the `cpp_std=c++23` override stays
local and does not propagate to any other TU. Public C symbol unchanged;
all GPU backends link without modification.
