- **Short guide: CPU evidence from a machine the project does not own (#2734).**
  `docs/development/cpu-evidence-offsite.md` gives the contributor with a
  server-class x86 machine, a native Windows build or an Arm laptop the exact
  tester commands (container, Windows zip, macOS bundle, and the container on
  the Apple-silicon machine that the evidence row also asks for) and the
  report path (`docs/hardware-reports/`), and states that these reports are
  extra evidence next to the emulated matrix — the cut does not wait for
  them. The emulation sentence names what the emulated matrix covers and
  names the AVX-512 dispatch as the level it does not. The
  `T-TESTER-WINDOWS-NATIVE-EVIDENCE` and `T-TESTER-APPLE-SILICON-EVIDENCE`
  rows of `docs/state.md` point to it; their opening claims now match the
  tester reports they already record and they carry the reference, branch,
  date and status cells their neighbours have.
