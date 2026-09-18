- **macOS on Apple silicon now computes integer ADM like every other
  platform** (ADR-1257). Since August 2026 the Apple AArch64 build routed the
  first column of every ADM DWT2 row through a compatibility wrapper that
  reproduced a historical three-tap result, to keep a macOS-only expected
  score in the Python quality tests. Upstream Netflix/vmaf fixed the same
  dropped tap in `cba9343ed`, so the wrapper is removed. macOS scores now match
  Linux AArch64 and x86. On the Netflix akiyo fixture, VMAF moves from the
  recorded macOS value 88.030322 to about 88.030459, the value Linux AArch64
  and x86 produce.
