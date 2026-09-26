## Added

- **RC1 external tester evidence bundle**: added the runtime-dependency-free
  `tools/rc1-tester/vmaf-rc1-report` launcher and
  `docs/usage/rc1-tester-guide.md`. Testers can probe CPU/CUDA/SYCL/HIP/Metal
  environments, check one or more explicitly requested backend states with a
  pinned-model four-frame run, select accelerator ordinals, and send one
  privacy-redacted archive containing exact argv/environment overrides,
  requested/observed backend state, binary/model/fixture hashes, bounded score
  evidence, logs, JSON, and non-circular integrity checksums. CPU is checked
  against the pinned snapshot, accelerators are checked against an automatic
  CPU reference within `5e-5`, and frame 3 exercises temporal motion. The root
  backend state still does not prove per-feature accelerator execution. The
  inventory keeps performance
  benchmarking/tuning in RC2 and real training in RC3. See
  [ADR-1342](../../docs/adr/1342-rc1-external-tester-report-bundle.md).
