
## ADR-3359 — SYCL primary queue on immediate command lists (2026-10-02)

`pr/sycl-zerocopy-hardening`, [ADR-3359](adr/3359-sycl-primary-queue-immediate-cmdlist.md);
row `T-SYCL-ZEROCOPY-IMPORT-DROPPED-2026-10-02` in [state.md](state.md).

- `core/src/sycl/common.cpp::sycl_queue_props()` adds
  `sycl::ext::intel::property::queue::immediate_command_list` (under
  `SYCL_EXT_INTEL_QUEUE_IMMEDIATE_COMMAND_LIST`) to the primary queue in both
  the profiling and the plain form. The VA import in `dmabuf_import.cpp` must
  stay on that queue (`vmaf_sycl_get_queue_ptr()`): a separate immediate
  import-only queue next to a batched primary queue still drops the import.
- A rebase that restructures queue creation (another adapter, a queue pool, a
  merge with the copy or combined queue) keeps the property on the queue the
  imports are made against and `vmaf_sycl_queue_wait()` waits on, and re-runs
  `scripts/test/zerocopy-e2e.sh --stage 1 --repeat 10 --cases cambi,vif,model-vmaf_v0.6.1`
  at 8 and 10 bit under `UR_L0_USE_IMMEDIATE_COMMANDLISTS=0` (0 differing runs).
- No Netflix golden-data, public API or FFmpeg patch impact.
