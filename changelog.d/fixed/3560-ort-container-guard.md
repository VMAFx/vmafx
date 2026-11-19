The ONNX Runtime container guard now reads a `RUN` instruction as simple
commands and accepts only the helper, `export ORT_VERSION`, shell safety
settings, or `ldconfig`; it refuses sourced variables, `--print-pin`, inline
overrides, and module calls without a pinned version, and resolves `ARG`/`ENV`
bindings across the build stage.
