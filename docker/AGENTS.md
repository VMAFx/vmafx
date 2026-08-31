# Container image invariants

## Node model root

`Dockerfile.node` stages the contents of `model/` with
`cp -r model/. /dist/model/`; do not copy the directory itself. The runtime
copy maps that staging root to `/usr/local/share/vmafx/model`, which is also the
exact `VMAFX_MODEL_DIR`. The builder must continue to assert that
`/dist/model/vmaf_v0.6.1.json` exists so a nested `model/model/` layout fails at
build time instead of producing a worker that cannot resolve its packaged
model.
