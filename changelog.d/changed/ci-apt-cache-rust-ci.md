<!-- markdownlint-disable MD041 -->
- **The `vmafx-sys CI work` job in `rust-ci.yml` now caches apt package archives and caps its apt install step.**
  Package requirements are declared in `.github/apt/rust-vmafx-sys.txt`, and
  the install step has a 16-minute timeout cap sized at 1.25 times its slowest
  measured run. Restored archives prevent slow package mirrors from stalling
  the workflow.
