<!-- markdownlint-disable MD041 -->
- **The `Doxygen Public API work` job in `docs.yml` now caches apt package archives and caps its apt install step.**
  Package requirements are declared in `.github/apt/doxygen-work.txt`, and the install step has a 5-minute timeout cap sized at 1.25 times its slowest measured run.
  The job timeout limit is raised from 10 to 20 minutes to cover the step caps sum and uncapped step headroom.
