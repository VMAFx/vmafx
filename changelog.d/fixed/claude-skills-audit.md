# fix(skills): repair ADR-0700 path drift in 4 skill scaffolds

`/add-gpu-backend <name>` would write generated files to
`$repo_root/libvmaf/src/...` — a directory that no longer exists after the
ADR-0700 libvmaf→core rename — so the scaffold has been silently broken on
master since the rename landed. Three other skill files referenced the legacy
`libvmaf/` source path in user-visible workflow text without breaking
execution: `/build-vmaf`'s `build.sh` did `cd "$repo_root/libvmaf"` and
`build-vmaf/SKILL.md` documented the wrong cd target; `/regen-docs`'s SKILL.md
described the unresolvable-link category using the wrong directory name; and
`add-simd-path/templates/simd_feature.c.template` told the generated SIMD
source to point at `libvmaf/test` in its bit-exact-test comment.

Fix: rewrite all four occurrences to `core/` (the post-ADR-0700 source
root). Public install-path references (`core/include/libvmaf/...`,
`libvmaf.so`) are unchanged — they were never affected.

Closes the residual gap that `T-POST-RENAME-DRIFT-SWEEP-2026-05-28` (docs/state.md
row) recorded as fixed but missed inside the scaffold script itself.
