- **thread_pool.c (finding #7)**: `pthread_mutex_init` and two
  `pthread_cond_init` calls in `vmaf_thread_pool_create` now have their
  return values checked.  Under `ENOMEM` the function tears down already-
  initialised primitives, frees `p->workers` and `p`, sets `*pool = NULL`,
  and returns `-ENOMEM` — matching the existing partial-`pthread_create`
  failure pattern.  Previously, a silent init failure led to UB on the first
  `pthread_mutex_lock`. CERT MEM30-C / POSIX ERR33-C.

- **adm_tools.c (finding #8)**: `adm_dwt2_s`, `adm_dwt2_lo_s`,
  `adm_dwt2_d`, and `adm_dwt2_lo_d` now NULL-check each `aligned_malloc`
  and return `-ENOMEM` on failure instead of dereferencing a null pointer.
  Signatures changed from `void` to `int`; callers in `adm.c` propagate
  the error via the existing `goto fail` path.  Power of 10 rule 3,
  CERT MEM30-C.

- **picture.c (finding #10)**: `vmaf_picture_alloc` now rejects `w == 0`,
  `w > 32768`, `h == 0`, `h > 32768` with `-EINVAL` before
  `picture_compute_geometry` runs.  Without this guard, unsigned wrap in
  `(w + DATA_ALIGN - 1u)` produced `aligned_y = 0`, the allocator
  succeeded with a zero-byte buffer, and any pixel read was OOB.
  CERT INT30-C.
