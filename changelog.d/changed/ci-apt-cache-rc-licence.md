<!-- markdownlint-disable MD041 -->
- **The `companion` job in `published-rc-licence-companions.yml` now caps its apt install step.**
  The "Fetch the corresponding source" step carries a 48-minute timeout cap sized at 1.25 times its slowest measured run (37.7m on 2026-10-23).
  No apt archive cache is used because package installation runs inside the release builder container rather than on the runner; the 240-minute job limit covers the step cap and uncapped step headroom.
