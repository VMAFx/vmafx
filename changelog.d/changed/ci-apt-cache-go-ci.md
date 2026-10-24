<!-- markdownlint-disable MD041 -->
- **The `go-ci` job in `go-ci.yml` now caches apt package archives and caps its install step.**
  Package lists are declared in `.github/apt/go-ci.txt`, and restored `.deb`
  archives are copied into `/var/cache/apt/archives` before install to prevent
  slow package mirrors from triggering job cancellations. The apt install step
  is capped at 9 minutes and the job timeout is set to 90 minutes.
