<!-- markdownlint-disable MD041 -->
- **Every job in `tests-and-quality-gates.yml` running `apt-get install` now caches package archives.**
  Package lists are declared per job in `.github/apt/` files, and each install
  step has a timeout cap sized at or above 1.25 times its slowest measured run.
  Job timeout limits cover the sum of their step caps so slow mirrors cannot
  trigger job cancellation.
