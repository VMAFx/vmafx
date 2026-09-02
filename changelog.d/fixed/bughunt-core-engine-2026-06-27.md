- Core engine: fix a picture-pool deadlock on the threaded enqueue-failure
  path. When `vmaf_thread_pool_enqueue` fails inside
  `threaded_read_pictures_batch` (`core/src/libvmaf.c`), the function returned
  without unref'ing the caller's `ref`/`dist` pictures. Because
  `vmaf_read_pictures` treats the `done=true` return as "ownership
  transferred" and skips its own `cleanup:` unref, each failed enqueue leaked a
  picture-pool slot; once the always-on pool drained, the next
  `vmaf_picture_pool_fetch` deadlocked in `pthread_cond_wait`. The failure path
  now unref's `ref`/`dist` like the success path.
- Core engine: `aggregate_vector_append` in
  `core/src/feature/feature_collector.c` now returns `-ENOMEM` (was `-EINVAL`)
  when the feature-name `malloc` fails, mirroring the `.cpp` twin so a genuine
  out-of-memory condition is no longer misreported as an invalid argument.
- Core engine: `vmaf_model_collection_append` in `core/src/model.c` no longer
  nulls the caller's `*model_collection` handle (and no longer drops a
  still-valid collection) when the realloc that grows an existing collection
  fails. `realloc` leaves the old buffer intact on failure, so the grow path
  now returns `-ENOMEM` without taking the fresh-allocation `fail` label.
