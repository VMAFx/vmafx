**fix(hip): deterministic wavefront reduction in `integer_vif_hip` horizontal kernels (ADR-0552)**

Per-thread `atomicAdd` calls in the VIF horizontal accumulation kernels were
replaced with a 64-lane XOR-shuffle reduction (`__shfl_xor`) followed by a
single `atomicAdd` per wavefront. This eliminates the non-deterministic
CAS-retry ordering on AMD hardware that caused per-feature divergence of
0.001–0.014 vs CPU. The VMAF SVM amplified this to a 0.031 score-level
divergence — violating ADR-0214's places=4 gate by 200×. After the fix,
HIP VIF output matches CPU within places=4 on the BBB testdata fixture.

Also removes the early `return` for out-of-bounds threads in horizontal kernels
(out-of-bounds threads now carry zero-initialised accumulators through the
wavefront reduce, which is neutral under integer addition and required for
correct wavefront synchronisation).
