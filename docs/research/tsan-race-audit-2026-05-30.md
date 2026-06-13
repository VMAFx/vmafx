# TSan race audit — libvmaf threadpool + dispatcher paths (2026-05-30)

- **Trigger**: agent task on branch `chore/mcp-tools-audit-20260529` —
  "Run TSan (ThreadSanitizer) on the libvmaf threadpool + dispatcher
  paths to surface latent race conditions". ADR-0138/0139 elevate
  bit-exactness to a load-bearing invariant — a race can silently
  produce wrong scores without crashing.
- **Worktree**: `fix/tsan-race-audit` based on `origin/master`
  (master tip `bbcaa8d127`, "fix(common): init_blur_array — free
  partial allocations on failure (#297)").
- **Build**: `meson setup build-tsan core -Denable_cuda=false
  -Denable_sycl=false -Db_sanitize=thread`. Verified TSan
  instrumentation via `nm | grep __tsan` (14 symbols) and
  `ldd | grep libtsan` (`libtsan.so.2`).

## Reproducer

```text
TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 history_size=7" \
./build-tsan/tools/vmaf \
  -r python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv \
  -d python/test/resource/yuv/checkerboard_1920_1080_10_3_1_0.yuv \
  -w 1920 -h 1080 -p 420 -b 8 --threads 16 \
  --feature psnr --feature float_ssim \
  --feature float_ms_ssim --feature ciede \
  -o /tmp/vmaf-bigstress.json --json
```

Smaller invocations (`--threads 4` with a single feature, the
`fast` test suite, `test_framesync`, `test_thread_pool`) did not
fire the race; the stressor needs both float_ssim and float_ms_ssim
enabled plus enough workers (>=4) to overlap the two extractors'
`init()` callbacks.

## Findings

**Pre-fix: 10 TSan warnings.** All on the same four globals,
defined in `core/src/feature/iqa/ssim_tools.c`:

| Global | Site (write thread A) | Site (write thread B) |
| --- | --- | --- |
| `g_ssim_precompute` | `iqa_ssim_set_dispatch` from `float_ssim::init` | same, from `float_ms_ssim::init` |
| `g_ssim_variance` | same | same |
| `g_ssim_accumulate` | same | same |
| `g_iqa_convolve` | `iqa_convolve_set_dispatch` from `float_ssim::init` | same, from `float_ms_ssim::init` |

### Root cause

`vmaf_feature_extractor_context_extract`
(`core/src/feature/feature_extractor.c:580`) lazy-initialises each
fex_ctx on first extract:

```c
if (!fex_ctx->is_initialized) {
    int err = vmaf_feature_extractor_context_init(fex_ctx, ...);
    if (err) return err;
}
```

The `fex_ctx_pool` gives each worker a distinct `fex_ctx`, so the
`is_initialized` race itself is benign within one extractor. But
the dispatch globals are **process-wide**: when worker A runs
`float_ssim::init` and worker B runs `float_ms_ssim::init`
concurrently, both call `iqa_ssim_set_dispatch(...)` /
`iqa_convolve_set_dispatch(...)` and race-write the same 8 bytes.

The races are *value-benign* on current x86-64 hardware (8-byte
aligned, all writers install the same ISA-best pointer) but
**TSan correctly flags them**, and:

- On weakly-ordered hardware (ARM, POWER) a non-aligned racing
  store could publish a torn pointer.
- ADR-0138 / ADR-0139 treat any dispatch-pointer race as
  load-bearing.

### Other globals scanned, no race surfaced

- `g_flags` / `g_flags_mask` (`core/src/cpu.cpp`) — already
  `std::atomic`, initialised before workers start.
- `prev_ref` shared state in `feature_extractor.c` — already
  covered by ADR-0795 with documented assertions.
- `vmaf_thread_pool_*` — `pthread_mutex_t` and condvars correctly
  scope the worker queue; no races on the queue or refcounts.
- `vmaf_fex_ctx_pool_*` — `pthread_mutex_t(pool->lock)` plus
  `atomic_load/atomic_fetch_add` on entry refcounts; no races.
- `vmaf_feature_collector` — no concurrent writes flagged.
- DNN / ORT init paths — no concurrent writes flagged.

## Fix

Single process-wide `pthread_once_t` owned by `ssim_tools.c`,
shared between `float_ssim.c` and `float_ms_ssim.c` via the new
`iqa_ssim_install_dispatch_once(pthread_once_t *, void (*)(void))`
helper declared in `core/src/feature/iqa/ssim_simd.h`.

- The helper's `guard` parameter is retained for API symmetry but
  ignored; the actual guard is the file-static
  `g_ssim_dispatch_once`.
- The installer-callback pointer is published via an
  atomic CAS (`memory_order_release`) so only the first publisher
  writes the slot and TSan sees a sequenced store.
- `pthread_once` runs the trampoline exactly once and emits a
  full memory barrier; all workers (any TU) read the four
  dispatch globals through that barrier.

See [ADR-0871](../adr/0871-ssim-dispatch-pthread-once.md) for the
full decision record, alternatives matrix, and consequences.

## Verification

```text
$ TSAN_OPTIONS=... ./build-tsan/tools/vmaf ...           # post-fix
$ grep -cE 'WARNING: ThreadSanitizer' /tmp/tsan-bigstress3.log
0

$ meson test -C build-tsan
Ok:                63
Fail:              0

$ python3 -c "import json; d=json.load(open('/tmp/vmaf-fixed-src01.json')); print(d['pooled_metrics']['vmaf'])"
{'min': 71.174765, 'max': 87.180955, 'mean': 76.66783, 'harmonic_mean': 76.508906}
# bit-for-bit identical to the pre-fix run on the src01_hrc00/src01_hrc01 pair
```

## Files touched

- `core/src/feature/iqa/ssim_simd.h` — declare
  `iqa_ssim_install_dispatch_once`, include `<pthread.h>`.
- `core/src/feature/iqa/ssim_tools.c` — file-static
  `pthread_once_t`, atomic publish of installer, trampoline.
- `core/src/feature/float_ssim.c` — extract installer to
  `ssim_install_dispatch_for_host_isa`, call once-helper.
- `core/src/feature/float_ms_ssim.c` — call once-helper instead of
  direct dispatch install.
- `docs/adr/0871-ssim-dispatch-pthread-once.md` — decision record.
- `docs/adr/README.md` — index row.
- `docs/adr/_index_fragments/_order.txt` — manifest entry.
- `changelog.d/fixed/tsan-race-audit.md` — release-please fragment.
- `docs/research/tsan-race-audit-2026-05-30.md` — this digest.
- `docs/rebase-notes.md` — rebase-impact entry.
- `docs/state.md` — closed-bug row.

## Audit clean status

No further races, deadlocks, or heap-use-after-free in thread
context were observed across:

- 49 / 49 `fast` suite tests
- 63 / 63 full suite (including `dnn` and `slow`)
- `--threads 4` baseline scoring (src01)
- `--threads 16` stress with 4 features on 1080p checkerboard
- `--threads 16` stress with `report_atomic_races=1`
- `vmaf_bench` (no-config exit)
- `test_framesync`, `test_thread_pool` standalone
