# oom-r20 — #3320 landed test: 12-round rerun + probe A/B + reclamation latency

Date: 2026-11-07 (07:15-08:25 UTC). Build: master `b38635f63`, #3320 test as landed
(unchanged except the stated arm edits), MinGW64 gcc 16.2.0 / nvcc 13.4,
Windows 11, driver 621.01. Devices: CUDA_VISIBLE_DEVICES 0 = RTX 5080,
1 = RTX 4060 Laptop (nvidia-smi -i order is the opposite: 0=4060, 1=5080).
Raw logs: `armA/ armB/ armC/ armD/ ilv/` (per-run .err with rc appended,
timeline.log with nvidia-smi brackets, armD/sampler.log 5 s cadence).

## Arms

| arm | binary | post-release probe | rounds |
| --- | --- | --- | --- |
| A | as-landed | one-shot cuMemAlloc/cuMemFree 2 MiB | 12 x 2 devices |
| B | no-probe | none (only change) | 12 x 2 |
| C | bounded poll | 20 ms poll, 60 s bound, timed | 12 x 2 |
| D | silent ladder | single probes after silent 1/5/15/45/105 s | 6 x 2 |
| ilv | interleaved A/B | as-landed vs no-probe, pairs back-to-back | 12 pairs 4060, 6 pairs 5080 |

## Results

- **A**: 5080 12/12 and 4060 12/12 fail at "device memory not yet returned after
  the hog was freed" (`memory back 0`); retry assertion never fired. Slow window.
- **ilv**: 5080 6/6 pairs both arms fail (as-landed @not-yet-returned, no-probe
  @retry); 4060 12 pairs: as-landed 11/12 pass + 1 transient refusal, no-probe
  12/12 pass. **No systematic probe perturbation.**
- **C**: 4060 12/12 `memory back 1 after 0.0 ms (0 probes)`; 5080 6/6 bound hit
  (`after 60000 ms (1921-1933 probes)`) then 6/6 instant after mid-campaign
  window flip.
- **D**: 4060 6/6 probe OK at the 1 s checkpoint, retry rc 0; 5080 6/6 refused at
  every checkpoint (1/5/15/45/105 s), retry rc -12.
- **armD/sampler.log** (5 s): during 5080 slow-window waits, used ~15.8 GiB /
  free ~150 MiB stays flat -> the driver holds the freed hog until process exit.
  Fresh processes re-take the same 30.8 GiB hog immediately (every run, all arms).

## Conclusion

Windows slow window = the driver holds the released hog's memory for the
lifetime of the freeing process (>= 105 s observed, returned at exit); fast
window = instant return. The in-process one-shot probe is not a perturbation
source; a bounded wait + skip-with-reason is the only design that maps to the
observed behavior. Hog sizes 30.8/22.9 GiB (WDDM oversubscription) vs 710 MiB
on the maintainer's Linux 4090.

Arm edits used (local-only, reverted after): armB/C/D variants of
`check_oom_and_memory_return()` in core/test/test_cuda_oom_pictures_released.c;
binaries saved as build/test/{aslanded,noprobe}.exe.
