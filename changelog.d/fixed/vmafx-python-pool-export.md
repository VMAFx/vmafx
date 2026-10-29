- **`from vmafx import Pool` works.** The Python package now re-exports `Pool`
  and `PooledScore`, which the published example imported from it, and the
  example loads `libvmafx.so.1` instead of the compatibility library
  ([VMAFx API, Python](docs/api/vmafx/index.md#python)).
