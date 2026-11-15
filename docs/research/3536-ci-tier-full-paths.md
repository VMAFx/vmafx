# Research-3536: Platform launch paths that require full CI

- **Status**: Completed
- **Workstream**: [ADR-3536](../adr/3536-ci-tier-full-paths.md)
- **Last updated**: 2026-11-11

## Question

Which process-launch paths in the shared runner and packaged tester require
native macOS or Windows execution that the Linux light tier cannot provide?
Which files should therefore escalate an own pull request to full CI?

## Sources

- `.github/ci-tier.json` and `scripts/ci/ci_tier.py`: tier authority.
- `.github/workflows/ci-tier.yml`: tier decision precedes platform jobs.
- `scripts/ci/ci_router.py`: delegates to `ci_tier.decide()`.
- `scripts/lib/*.py`: shared runner, native ACL tests, binary discovery.
- `tools/rc1-tester/src/vmaf_rc1_tester/*.py`: packaged tester launch code.
- `tools/rc1-tester/tests/test_safe_subprocess_copy.py`: runner copy parity.

## Method and fingerprint

Read-only audit at commit `e7ded191c76ee03664803ed935fe175bc8f944bd`;
checked 2026-11-11T21:42:34+01:00, from `date --iso-8601=seconds`.
The tables describe that source snapshot plus the resulting configuration
change: `hw_sycl.py` joins the six existing entries. No native hardware or
platform test was executed for this source audit.

Rerun these commands from the repository root:

```bash
git rev-parse HEAD
date --iso-8601=seconds
rg -n 'sys\.platform|os\.name|platform\.system\(' \
  scripts/lib/*.py tools/rc1-tester/src/vmaf_rc1_tester/*.py
rg -n 'subprocess|Popen|run_bounded|_run_cmd|runner\(' \
  scripts/lib/*.py tools/rc1-tester/src/vmaf_rc1_tester/*.py
rg -n -A 24 'def discover_windows|def discover\(|def windows_host' \
  tools/rc1-tester/src/vmaf_rc1_tester/hw_sycl.py
rg -n -A 24 'def collect_host_facts|def collect_darwin_facts' \
  tools/rc1-tester/src/vmaf_rc1_tester/hw_facts.py
rg -n -A 26 'def probe_os|def probe_cpu|def _probe_metal_devices' \
  tools/rc1-tester/src/vmaf_rc1_tester/probe.py
rg -n 'SOURCE|COPY|read_bytes|drift' \
  tools/rc1-tester/tests/test_safe_subprocess_copy.py
```

Intersect platform-guard matches with launch calls, then read each enclosing
function and its callers. Include runner tests and copy parity even when they
do not introduce another launch primitive. Native ACL and process-group
semantics need the actual host; mocking platform values on Linux does not
exercise those host semantics. This audit covers the shared runner and
packaged tester, not every platform conditional elsewhere in the repository.

## Findings

Every `full_paths` entry below names one exact tracked file. The first table
uses paths relative to `scripts/lib/`; the second uses paths relative to
`tools/rc1-tester/`.

| Listed shared-library path | Why full CI |
| --- | --- |
| `safe_subprocess.py` | Darwin ACL validation; Windows process groups. |
| `test_safe_subprocess.py` | Native Darwin ACL tests skip on Linux. |

| Listed tester path | Why full CI |
| --- | --- |
| `src/vmaf_rc1_tester/_safe_subprocess.py` | Same platform launch logic. |
| `tests/test_safe_subprocess_copy.py` | Byte parity with shared runner. |
| `src/vmaf_rc1_tester/hw_facts.py` | Darwin `sysctl`, `system_profiler`. |
| `src/vmaf_rc1_tester/probe.py` | Darwin `sw_vers`, `sysctl`, GPU facts. |
| `src/vmaf_rc1_tester/hw_sycl.py` | Windows packaged-loader probe launch. |

`safe_subprocess._directory_allows_link_retargeting()` calls the native ACL
reader only on Darwin. Process creation sets POSIX sessions or Windows
creation flags according to `os.name`; cleanup also selects POSIX process
groups. The tester copy carries the same code. `hw_facts.collect_host_facts()`
selects `collect_darwin_facts()` only on Darwin. `probe_os()`, `probe_cpu()`,
and `_probe_metal_devices()` similarly gate macOS commands.

The broader `os.name` search finds `hw_sycl.windows_host()` selecting
`discover_windows()`: when the packaged loader exists, it calls
`probe_devices()` with that Windows loader. Linux discovery launches the
probe without this loader argument. This is another platform-specific launch
path, so it is added rather than excluded as a mere library-name switch.

All other platform-guard candidates in the searched roots are excluded below.
The table uses the same two roots; tester entries are relative to
`tools/rc1-tester/src/vmaf_rc1_tester/`.

| Excluded candidate | Why not listed |
| --- | --- |
| Shared `vmaftest.py` | Chooses `vmaf.exe` filename; starts no process. |
| Shared `test_vmaftest.py` | Execute-bit skip; tests lookup, not launch. |
| Tester `hw_cuda.py` | Windows driver file check, not guarded launch. |
| Tester `hw_cudaprobe.py` | DLL selection for in-process `ctypes.CDLL`. |
| Tester `hw_l0probe.py` | DLL selection for in-process `ctypes.CDLL`. |

`hw_cuda.host_access()` selects filesystem access facts on Windows; its
device-probe launch is shared across hosts. The two in-process loader modules
do not spawn children. Those differences do not meet this launch-path rule.

## Alternatives explored

- Keep only the two runner files: misses native command launches and tests.
- Include every platform conditional: includes filename and DLL selection
  that this rule does not target.
- Exact launch paths plus their native tests and copy parity: chosen; narrow
  routing with explicit evidence for each included and excluded candidate.

## Open questions

None within the audited roots. Future platform-specific launch code requires
a new `full_paths` entry and a matching tracked file.

## Related

- [ADR-3536](../adr/3536-ci-tier-full-paths.md): routing and drift hook.
- [ADR-2169](../adr/2169-ci-fewer-runs.md): original tier policy.
- [CI contributor guide](../development/ci.md): adding and checking paths.
