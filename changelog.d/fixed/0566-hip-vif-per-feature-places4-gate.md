**fix(hip): formalise per-feature places=4 gate for HIP VIF, superseding ADR-0537 follow-up (ADR-0566)**

ADR-0537 documented a per-feature places=3 gap in `integer_vif_hip` as an
"acceptable follow-up". This was incorrect: per-feature places=3 produces
VMAF-score places=1 via SVM amplification (VIF scale coefficients 1.2–2.1 × 4
scales; worst-case delta = 0.014 × 6.6 = 0.092 VMAF units, 920× the ADR-0214
places=4 tolerance).

ADR-0566 supersedes the places=3 clause and formalises per-feature places=4 as
the non-negotiable gate for all HIP VIF kernels. ADR-0552's wavefront reduction
fix satisfies this gate.
