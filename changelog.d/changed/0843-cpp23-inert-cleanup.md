## chore(cpp23): drop inert .cpp shadows where meson uses .c (ADR-0843)

Removed 15 dead cpp23 shadow files that existed alongside their `.c` originals
but were not referenced by any meson target:

- `core/src/cpu.c` — superseded by `cpu.cpp` (Wave 7 / ADR-0755; meson uses `.cpp`)
- `core/src/dict.cpp` — Wave 2 / ADR-0727; meson uses `dict.c`
- `core/src/fex_ctx_vector.cpp` — Wave 2 / ADR-0723; meson uses `.c`
- `core/src/log.cpp` — Wave 1 / ADR-0725; meson uses `.c`
- `core/src/mem.cpp` — Wave 1 / ADR-0720; meson uses `.c`
- `core/src/model.cpp` — Wave 3 / ADR-0729; meson uses `.c`
- `core/src/opt.cpp` — Wave 8 / ADR-0721; meson uses `.c` (wave not yet wired)
- `core/src/output.cpp` — Wave 4 / ADR-0733; meson uses `.c`
- `core/src/ref.cpp` — Wave 5 / ADR-0735; meson uses `.c`
- `core/src/thread_locale.cpp` — Wave 5 / ADR-0735; meson uses `.c`
- `core/src/feature/feature_name.cpp` — Wave 3 / ADR-0729; meson uses `.c`
- `core/src/feature/luminance_tools.cpp` — Wave 3 / ADR-0731; meson uses `.c`
- `core/src/feature/mkdirp.cpp` — Wave 3 / ADR-0731; meson uses `.c`
- `core/src/feature/picture_copy.cpp` — Wave 3 / ADR-0729; meson uses `.c`
- `core/src/feature/psnr_tools.cpp` — Wave 3 / ADR-0731; meson uses `.c`

No user-visible behaviour change. The `.c` originals remain the live meson targets.
Future per-wave PRs will wire the cpp23 replacements when ready.
