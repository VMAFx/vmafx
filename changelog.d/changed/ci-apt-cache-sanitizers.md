<!-- markdownlint-disable MD041 -->
- **The sanitizers workflow (`sanitizers.yml`) now caches apt package archives and caps every apt install step.**
  Package requirements are declared in `.github/apt/sanitizers-base.txt`, `.github/apt/sanitizers-llvm.txt`, and `.github/apt/fuzz-nightly.txt`.
  Each install step carries a 5-minute timeout cap, and the nightly fuzz job limit is raised to 50 minutes to maintain headroom over measured runs.
