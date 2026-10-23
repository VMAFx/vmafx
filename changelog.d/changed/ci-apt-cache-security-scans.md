<!-- markdownlint-disable MD041 -->
- **The `CodeQL (C/C++)` job in `security-scans.yml` now caches apt package archives and caps its apt install step.**
  Package requirements are declared in `.github/apt/codeql-cpp.txt`, and the configure step has a 5-minute timeout cap sized at 1.25 times its slowest measured run.
  The job timeout limit is raised from 35 to 55 minutes to cover the step caps sum and uncapped step headroom.
