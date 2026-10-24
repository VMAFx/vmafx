<!-- markdownlint-disable MD041 -->
- **The Linux base dependency install in `build.yml` now caches apt package archives.**
  Base packages are declared in `.github/apt/build-linux.txt`, and each `apt-get install` step carries a timeout cap sized at or above 1.25 times its slowest measured run.
  The `build-work` job timeout limit is raised to 80 minutes to cover step caps and uncapped step headroom.
