- The Sanitizers workflow builds again. Both the **ASan + UBSan PR gate** and the
  **TSan master-push** job installed meson from `apt`, and Ubuntu 24.04 ships
  meson 1.3.2 — which predates `c23` support in `c_std` (added in meson 1.4.0).
  Since ADR-0692 raised the project to C23, both jobs died at configure with
  `ERROR: Unknown C std ['c23']` before compiling a single file. Every other
  workflow in the repository already pip-installs meson; these two were the
  exception. They now do the same.
- The failure was invisible in the usual places: it happens during *configure*,
  so there is no sanitizer diagnostic and no test output — just a build error in
  a job whose name implies a runtime finding.
