- **The local governance hooks run the pinned engine
  ([ADR-3272](docs/adr/3272-praetor-pin-9bf502ec-engine-launcher.md)).** The hooks ran whatever
  `praetorctl` was first on `PATH`, so a workstation whose copy was newer than the pin could refuse a
  commit that CI accepts. They now call `sh .config/lefthook/engine.sh`, praetor's launcher: it reads
  `PRAETOR_REF` from `.github/workflows/standards-gate.yml`, installs that engine once per pin with
  `go install` and runs it. An unreadable pin or a failed install ends the hook; no engine from
  `PATH` runs in its place. The praetor pin moves from `4ae6247c57d2` to `9bf502ec6ce5`, the commit
  the launcher is taken from. `make verify-all` and a typed `praetorctl` command still use `PATH`
  ([pre-commit hooks](docs/development/pre-commit-hooks.md)).
