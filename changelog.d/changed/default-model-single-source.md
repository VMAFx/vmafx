- **The default VMAF model is now defined in exactly one place.** Thirty-two
  sites across C, Go and Python each spelled `"vmaf_v0.6.1"` as their own
  answer to "which model when the caller names none" — the CLI's `--model`
  fallback, the VPL tool, the C MCP server, eight Go fallbacks and exported
  constants, four Go flag defaults, sixteen Python signature, argparse and `getattr` fallback
  defaults, and two more in `vmaf-roi-score`. Nothing tied them together, so
  changing the fork's default meant finding all thirty-two by hand, and a
  missed one was invisible: a component quietly scoring with a different model
  produces plausible numbers, not an error.
  `VMAF_DEFAULT_MODEL_VERSION` in `core/include/libvmaf/model.h` is now the
  single authoritative definition. C and C++ use the macro; a new public API
  function `vmaf_default_model_version()` lets anything linking libvmaf read it
  from the library actually loaded rather than from a copied constant; the Go
  tree and the two Python tools use gate-checked mirrors
  (`pkg/model.DefaultVersion`, `vmaftune.defaultmodel.DEFAULT_MODEL`,
  `vmafroiscore.defaultmodel.DEFAULT_MODEL`) because they deliberately do not
  link libvmaf. `scripts/ci/check-default-model-single-source.sh` fails the
  build if a mirror drifts from the header or if any component reintroduces a
  hardcoded default; it runs in `make lint` and pre-commit, and is itself
  tested in both directions by
  `scripts/ci/tests/test-default-model-single-source.sh` (22 cases, covering the
  `getattr` / `dict.get` / `or` / Go-flag fallback spellings as well as the
  plain assignment and `return` forms). A line that must pin
  a model regardless of the default marks itself `vmaf-model-pin: <reason>` —
  the AOM CTC preset does, because the CTC specification mandates
  `vmaf_v0.6.1` exactly. **The default's value is unchanged and this release is
  behaviour-identical** (271 Netflix golden tests pass, 0 fail); changing it is
  now a one-line edit. See ADR-1168 and `docs/development/default-model.md`.
- **`vmaf_default_model_version()`** — new public C API entry point returning
  the model version libvmaf scores with when none is named. Returns a
  never-`NULL`, never-freed string owned by the library, valid for the process
  lifetime; thread-safe and allocation-free. Documented in `docs/api/index.md`.
