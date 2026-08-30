**Dead code + unused-variable cleanup after r12 PR train** — two defects introduced
by the parallel PR 741/747 merge train:

- `core/src/feature/integer_motion.c`: removed unreachable second dimension guard
  (`if (w < 3 || h < 3)`) at init(). PR 747 already added the authoritative check
  at lines 272-279 using `filter_width / 2 + 1`; the PR 741 duplicate at lines
  289-295 was dead code that could never trigger and carried a stale comment
  ("3-frame SAD kernel") instead of the correct rationale ("5-tap Gaussian filter").
  Removing the dead block also eliminates a latent maintenance hazard: if
  `filter_width` is ever widened, the hardcoded `3` would silently under-check.

- `core/test/test_framesync.c`: removed unused local `prev_val` at line 84.
  PR 741 and PR 746 both fixed the framesync seed comparison; their changes
  landed together producing a `const uint8_t prev_val = ...` that was computed
  but never read — the actual comparison used `expected` (line 98). Compilers
  with `-Wunused-variable` would emit a warning on every build.
