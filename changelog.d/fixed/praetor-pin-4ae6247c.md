- **Commits pass the local hooks again
  ([ADR-3240](docs/adr/3240-praetor-pin-4ae6247c.md)).** The hooks run the `praetorctl` on `PATH`,
  and once the workstation's copy had moved to praetor `4ae6247c57d2` it refused every commit with
  "target AGENTS.readonly.md missing or unreadable" and "holds an earlier Praetor text". The praetor
  pin moves from `d141862c430b` to `4ae6247c57d2`, and the files that engine writes are
  regenerated: the new read-only context file `AGENTS.readonly.md`, the documentation and API gate
  assets, the three register skills with their licence files, and the agent hook script.
