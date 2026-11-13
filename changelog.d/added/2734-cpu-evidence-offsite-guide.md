- **Short guide: CPU evidence from a machine the project does not own (#2734).**
  `docs/development/cpu-evidence-offsite.md` gives the contributor with a
  server-class x86 machine, a native Windows build or an Arm laptop the exact
  tester commands (container, Windows zip, macOS bundle) and the report path
  (`docs/hardware-reports/`), and states that these reports are extra evidence
  next to the emulated matrix — the cut does not wait for them. The
  `T-TESTER-WINDOWS-NATIVE-EVIDENCE` and `T-TESTER-APPLE-SILICON-EVIDENCE`
  rows of `docs/state.md` point to it.
