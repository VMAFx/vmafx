- **core/gpu_dispatch_env**: fix C++ data race on the lock-free fast path in
  `vmaf_gpu_dispatch_env_get` — add `std::atomic<bool> ready` publication flag
  per `EnvRow`; slow-path writer does a `memory_order_release` store after
  populating `var_name`/`value` under the mutex; fast-path reader does a
  `memory_order_acquire` load before reading those fields. Eliminates UB under
  [intro.races] and TSan finding. (ADR-1068)
