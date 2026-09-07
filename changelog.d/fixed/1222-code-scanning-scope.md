- CodeQL's `paths-ignore` entries now match at any depth (`**/build`,
  `**/build-*`, `**/builddir`). A bare `build` matched only a top-level
  directory, so meson's generated probe files under `core/build/` were
  being analysed and raising alerts. The config also records inline that
  `paths` / `paths-ignore` are **inert for the built C/C++ analysis** —
  every translation unit the CodeQL build compiles is extracted
  regardless — which is why `core/test` is listed and still produces
  alerts.
- Removed a duplicated `_VALID_*` constant block from the MCP server.
  Five names (`_VALID_TINY_DEVICES`, `_VALID_TINY_RESIZES`,
  `_VALID_PIXFMTS`, `_VALID_BITDEPTHS`, `_VALID_BACKENDS`) were defined
  twice with identical values, so editing the first definition was a
  silent no-op; `_VALID_AOM_CTCS` / `_VALID_NFLX_CTCS` were dead
  near-duplicates of the singular `_VALID_AOM_CTC` / `_VALID_NFLX_CTC`
  the validator actually reads. See ADR-1222.
