Fixed `adm_csf_mode=1` on fixed-point ADM by normalizing all three CSF bands
with one power-of-two exponent per scale and restoring it after contrast
masking. Full-scale Barten now produces finite CPU scores, and CUDA, SYCL, HIP,
and Metal share the same representation contract; invalid blend-table geometry
continues to fail closed.
