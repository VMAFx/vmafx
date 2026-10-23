<!-- markdownlint-disable MD041 -->
- **The `Mini Retrain nightly` job in `mini-retrain.yml` now caches apt package archives and caps its apt install step.**
  Package requirements are declared in `.github/apt/mini-retrain.txt`, and the install step has a 5-minute timeout cap sized at 1.25 times its slowest measured run.
  The job timeout limit stays at 40 minutes, covering the step caps sum and uncapped step headroom.
