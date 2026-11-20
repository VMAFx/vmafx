- **The assertion-density gate scans every non-mirror C source the fork's pathspecs list, whatever name its copyright header carries.**
  The gate selected scan candidates by a Lusoris copyright line in the first 20 lines, so a C file an outside
  contributor writes (their own copyright line per ADR-3401) silently dropped out of the check (issue #3347). The
  header grep is gone; the 82 inherited and third-party sources (upstream Netflix and its NVIDIA co-authored CUDA
  files, Xiph, Daala, the IQA library, cJSON, libsvm, dav1d) are exempted by exact path through
  `scripts/ci/assertion-density-exempt-paths.txt`, loaded with the shared `load_manifest()` loader (new
  `pelorus_mirror.py load-manifest` command) that refuses unsorted or duplicate lists, and the gate fails when a
  listed path no longer exists so the manifest cannot go stale.
