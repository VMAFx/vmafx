**fix(build): error on `enable_nvtx=true` without `enable_cuda=true` (build-matrix audit §1a)**

`meson setup` previously hard-errored with an opaque "Include dir does not
exist" message when `enable_nvtx=true` and `enable_cuda=false` on a host
without a CUDA installation. A new explicit `error()` call now provides a
clear diagnostic: `enable_nvtx=true requires enable_cuda=true`.
