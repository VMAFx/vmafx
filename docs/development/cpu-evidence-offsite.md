<!-- markdownlint-disable MD013 -->
# CPU evidence from a machine the project does not own

One page for the contributor who has a server-class x86 machine, a native
Windows build or an Arm laptop and a few minutes: run the tester, send the
report. The full walkthroughs — download, checksum, what the run does and does
not do — live in the [tester guide](../usage/tester-image.md); this page is
the short version with the exact commands and the report path.

**These reports are extra evidence next to the emulated matrix; the cut does
not wait for them.** The CI lanes measure the dispatch levels bit-exact
against the scalar code under emulation (Intel SDE for x86, qemu-user for
aarch64) for the levels the emulated matrix covers — the AVX-512 dispatch so
far only on one real Zen 5 machine — so a report from real silicon sharpens
the RC7 CPU capability source of truth
([ADR-1490](../adr/1490-rc3-rc9-candidate-map-cpu-capability.md),
[#1885](https://github.com/VMAFx/vmafx/issues/1885)) and blocks nothing.

## Linux, server-class x86: the tester container

The container runs every CPU extractor at the default dispatch against the
scalar code and against baked references, with no network and no write access
(tester guide [section B](../usage/tester-image.md#b-container-image)):

```sh
docker pull ghcr.io/vmafx/vmafx:<VERSION>-tester
docker run --rm --network none --read-only --cap-drop ALL \
  --security-opt no-new-privileges --tmpfs /tmp \
  ghcr.io/vmafx/vmafx:<VERSION>-tester > report.json
```

## Native Windows, x64 or Arm64: the tester zip

The zip measures the fork's MSVC build natively — its SIMD paths against its
scalar code on your processor (tester guide
[section F](../usage/tester-image.md#f-native-windows-zip-x64-or-arm64)):

```powershell
cd vmafx-tester-windows-<ARCH>-<VERSION>; .\run.cmd
```

`run.cmd` writes `report.json` into the folder you ran it from and prints a
summary to the terminal.

## Arm laptop: Apple silicon (the macOS bundle)

The bundle adds every Metal twin against the CPU on real Apple silicon
(tester guide [section A](../usage/tester-image.md#a-native-macos-bundle-apple-silicon)):

```sh
cd vmafx-tester-macos-arm64-<VERSION> && ./run.sh > report.json
```

The JSON goes to `report.json`; the summary and the file name to use go to
the terminal on standard error, so the redirect hides nothing. The evidence
row closes with an accepted report from each of the bundle and the tester
container, so run the container on the same machine too (Docker pulls the
arm64 image there on its own):

```sh
docker run --rm --network none --read-only --cap-drop ALL \
  --security-opt no-new-privileges --tmpfs /tmp \
  ghcr.io/vmafx/vmafx:<VERSION>-tester > report-container.json
```

## Where the report goes

Reports land under `docs/hardware-reports/<file name>.json`; the run prints
the file name to use (`file name for a pull request: ...`). Open a pull
request with it, or use the
[hardware report form](https://github.com/VMAFx/vmafx/issues/new?template=hardware_report.yml)
without one — both are spelled out in
[Send the report and get credit](../usage/tester-image.md#send-the-report-and-get-credit),
and the [hardware reports page](../hardware-reports/index.md) lists every
accepted report. CI checks the integrity hash the tool wrote, so edit nothing
but the `note` field before sending.

The two open evidence rows that read these reports are
`T-TESTER-WINDOWS-NATIVE-EVIDENCE-2026-10-04` and
`T-TESTER-APPLE-SILICON-EVIDENCE-2026-10-03` in
[`docs/state.md`](../state.md); a verdict `fail` is welcome — it is a finding,
and it gets its own row.
