### SSIMULACRA2 AVX-512 + NEON IIR blur / `picture_to_linear_rgb` ULP audit (ADR-0467)

Cross-host ULP audit of the SSIMULACRA2 AVX-512 and NEON IIR blur and
`picture_to_linear_rgb` kernels (T3-9b). All 13 byte-exact C unit tests
pass on a Zen 5 host with AVX-512 BW/CD/DQ/VL/IFMA/VBMI; the Python
snapshot gate passes at `places=4`. No residual ULP drift — closed as
clean.
