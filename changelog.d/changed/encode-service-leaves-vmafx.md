- **The encode service moved to its own repository
  ([ADR-3382](docs/adr/3382-encode-service-leaves-vmafx.md)).** The encode-job
  API of ADR-3378 (now superseded) is developed in
  [vidiko/torcul](https://github.com/vidiko/torcul), which calls VMAFx for
  scoring through the `libvmafx.so.1` C ABI; issues #2986 to #2993 continue
  there.
