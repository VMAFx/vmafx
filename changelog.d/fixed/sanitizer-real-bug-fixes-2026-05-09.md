- **Three real-bug findings cross-confirmed by the nightly-triage
  (#537) and sanitizer-matrix-scope (#540) agents now closed.** All
  three are concrete defects exercised by the standing TSan / ASan
  test matrix; the fixes harden the implementation rather than
  relaxing any sanitizer gate (per `feedback_no_test_weakening`).
  - `SAN-INTEGER-ADM-DIV-LOOKUP-RACE` — `div_lookup_generator()` in
    `core/src/feature/integer_adm.h` was called once per ADM
    feature-extractor `init`, i.e. once per worker thread spawned by
    `vmaf_thread_pool_create`, with no synchronisation around the
    65 537-entry static `div_lookup` table. TSan reported the
    overlapping writes on `test_model`, `test_framesync`, and
    `test_pic_preallocation`. Wrapped the populator in a
    `pthread_once_t` guard; the table contents are loop-invariant
    (`div_Q_factor / i`) so once-init preserves bit-exactness.
  - `SAN-FRAMESYNC-MUTEX-DOMAIN` — `core/src/framesync.c`
    mutated the `buf_que` linked-list spine (next pointers, `buf_cnt`,
    FREE/ACQUIRED/RETRIEVED transitions) under `acquire_lock` (M0)
    while `submit_filled_data` and `retrieve_filled_data` walked the
    same spine under `retrieve_lock` (M1) only. TSan flagged the
    inconsistent lock domains as a lock-ordering violation.
    Established a strict M0-before-M1 ordering invariant: every entry
    point that walks the spine takes M0 first, and the producer /
    consumer paths additionally take M1 for the condvar handshake.
    `pthread_cond_wait` releases M1 atomically; M0 is dropped before
    the wait so producers can append. Every `pthread_mutex_*` /
    `pthread_cond_*` return value is now checked or `(void)`-cast.
  - `SAN-MODEL-MALLOC-OOB` + `SAN-PREDICT-METADATA-LEAK` —
    `core/src/svm.cpp` `parse_header()` and `parse_support_vectors()`
    fed unbounded `nr_class` / `total_sv` parsed from the SVM model
    file straight into `Malloc(...)` size calculations and
    `memcpy(_, sv_buffer.data(), sizeof(svm_node) * sv_buffer.size())`
    even when `sv_buffer.empty()`; ASan reported alloc-too-big and
    null-passed-as-argument on a crafted model file. Added a
    `VMAF_SVM_MAX_AXIS_COUNT` sanity bound (1<<24, comfortably above
    Netflix `vmaf_v0.6.1`'s ~6000 SVs) at every parse-time entry
    where `nr_class` / `total_sv` is consumed, with explicit pre-alloc
    `> 0` and `<= MAX` checks via `exceptAssert`. The `sv_buffer`
    empty-after-parse case now throws cleanly instead of feeding 0
    to `Malloc` + `memcpy`. Companion fix in `core/test/test_predict.c`
    closes the metadata-dispatch leak: `test_propagate_metadata`
    populated a local `VmafDictionary *dict` via
    `feature_collector_dispatch_metadata` -> `vmaf_dictionary_set`
    -> `dict_append_new_entry` (`dict.c:121, 124` strdup) and never
    freed it. Added the missing `vmaf_dictionary_free(&dict)` at
    teardown.
