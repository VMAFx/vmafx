<!-- markdownlint-disable MD041 -->
- **The `Full clang-tidy scan` job in `nightly.yml` now caches apt package archives and caps its apt install step.**
  Package requirements are declared in `.github/apt/clang-tidy-full.txt`, and the install step has a 5-minute timeout cap sized at 1.25 times its slowest measured run.
  The job timeout limit remains 60 minutes, which covers the step caps sum and uncapped step headroom.
