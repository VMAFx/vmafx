<!-- markdownlint-disable MD041 -->
- **The `fuzz` job in `fuzz.yml` now caches apt package archives and caps its apt install step.**
  Package requirements are declared in `.github/apt/fuzz.txt`, and the install step has a 5-minute timeout cap sized at 1.25 times its slowest measured run.
  The job timeout limit is raised from 30 to 40 minutes to cover the step caps sum and uncapped step headroom.
