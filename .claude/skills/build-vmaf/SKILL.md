---
name: build-vmaf
description: Build the libvmaf library + CLI with the requested backend combination (cpu, cuda, sycl, hip, all) and build type. Wraps meson setup + ninja. Reports wall time + output artifact paths.
---
<!-- markdownlint-disable MD013 -->

# /build-vmaf

Wrapper over `meson setup` + `ninja`. Enforces canonical repo command lines.

## Invocation

```text
/build-vmaf [--backend=cpu|cuda|sycl|hip|all] [--config=debug|release|relwithdebinfo]
            [--sanitizers=asan,ubsan,tsan|none] [--reconfigure] [--clean]
```

Defaults: `--backend=cpu --config=release --sanitizers=none`.

## What it does

1. `cd` `core/` (libvmaf source root after ADR-0700).
2. `--clean` -> remove `build/`.
3. `meson setup build [--reconfigure]` backend flags:
   - `cpu`: `-Denable_cuda=false -Denable_sycl=false`
   - `cuda`: `-Denable_cuda=true -Denable_sycl=false`
   - `sycl`: `-Denable_cuda=false -Denable_sycl=true`
   - `hip`: `-Denable_cuda=false -Denable_sycl=false -Denable_hip=true` (when
     backend scaffolded; else error)
   - `all`: all enabled together
4. Build type flags per `--config`:
   - `debug` -> `--buildtype=debug`
   - `release` -> `--buildtype=release`
   - `relwithdebinfo` -> `--buildtype=release -Db_ndebug=true` + `-g` via env
5. Sanitizers per `--sanitizers`:
   - `asan` -> `-Db_sanitize=address`
   - `ubsan` -> `-Db_sanitize=undefined`
   - combine: `address,undefined`. `tsan` standalone.
6. `ninja -C build -j$(nproc)`.
7. Reports: wall time, `build/tools/vmaf` path,
   `build/src/libvmaf.so.3.0.0` path.

## Constraints

- Sanitizers force `--buildtype=debug`: ASan+UBSan need debug builds, else
  overflow checks silently vanish.
- TSan never combines with ASan in one build.
- CUDA + SYCL build needs both compilers; error early if `nvcc` or `icpx`
  missing, not mid-build.

## Uses

`.claude/skills/build-vmaf/build.sh` (agent invokes directly). Outside Claude:
call `./.claude/skills/build-vmaf/build.sh --backend=cpu`.
